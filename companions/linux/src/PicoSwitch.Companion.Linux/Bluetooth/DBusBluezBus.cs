using Tmds.DBus.Protocol;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// <see cref="IBluezBus"/> over the system D-Bus, using BlueZ's documented
/// object API (<c>org.bluez.Adapter1</c>, <c>Device1</c>, <c>GattService1</c>,
/// <c>GattCharacteristic1</c>; see BlueZ <c>doc/org.bluez.*.rst</c>).
///
/// No state of its own beyond the bus connection: every call reads BlueZ's
/// current object tree, so nothing here can go stale across a bluetoothd
/// restart. The connection is opened lazily, so constructing the platform on a
/// machine without a system bus (a build agent, a container) costs nothing
/// until Bluetooth is actually used, and then fails as a <see cref="BluezException"/>.
/// </summary>
internal sealed class DBusBluezBus : IBluezBus
{
    private const string Service = "org.bluez";
    private const string ObjectManager = "org.freedesktop.DBus.ObjectManager";
    private const string Properties = "org.freedesktop.DBus.Properties";
    private const string Adapter1 = "org.bluez.Adapter1";
    private const string Device1 = "org.bluez.Device1";
    private const string GattService1 = "org.bluez.GattService1";
    private const string GattCharacteristic1 = "org.bluez.GattCharacteristic1";

    private readonly SemaphoreSlim connectGate = new(1, 1);
    private Connection? connection;

    /* ------------------------------------------------------------- adapter */

    public async Task<BluezAdapter?> GetAdapterAsync(CancellationToken cancellationToken = default)
    {
        var objects = await GetManagedObjectsAsync(cancellationToken).ConfigureAwait(false);
        foreach (var (path, interfaces) in objects.OrderBy(entry => entry.Key, StringComparer.Ordinal))
        {
            if (!interfaces.TryGetValue(Adapter1, out var properties))
            {
                continue;
            }

            return new BluezAdapter(
                path,
                StringOf(properties, "Address"),
                BoolOf(properties, "Powered"),
                properties.TryGetValue("Roles", out var roles) ? roles.GetArray<string>() : null);
        }

        return null;
    }

    public async Task<BluezDevice?> FindDeviceAsync(
        string adapterPath,
        ulong address,
        CancellationToken cancellationToken = default)
    {
        var objects = await GetManagedObjectsAsync(cancellationToken).ConfigureAwait(false);
        var path = BluezPaths.DevicePath(adapterPath, address);
        return objects.TryGetValue(path, out var interfaces) &&
               interfaces.TryGetValue(Device1, out var properties)
            ? DeviceOf(path, properties)
            : null;
    }

    /* ----------------------------------------------------------- discovery */

    public async Task<IAsyncDisposable> StartDiscoveryAsync(
        string adapterPath,
        string serviceUuid,
        Action<BluezAdvertisement> found,
        CancellationToken cancellationToken = default)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        var uuid = serviceUuid.ToLowerInvariant();

        // Devices BlueZ already holds (paired, or cached from an earlier scan)
        // do not produce InterfacesAdded again. Their advertisements arrive as
        // RSSI property changes instead, so the snapshot is kept to recognise
        // which of those carry the management service. The snapshot itself is
        // NOT reported: a cached object is not evidence the adapter is on.
        //
        // The exception is a device BlueZ is CONNECTED to: a connected LE
        // peripheral stops advertising, so it can never be "found" by a scan,
        // yet the live link is better evidence than any advertisement. Seen on
        // the Ally (2026-11-06): BlueZ kept a link from a failed attempt open,
        // the adapter reported advertising=false, and every later Pair timed
        // out in discovery. Identity is still verified after connecting.
        var known = new Dictionary<string, (ulong Address, string? Name, bool Matches)>(StringComparer.Ordinal);
        var connectedMatches = new List<BluezAdvertisement>();
        foreach (var (path, interfaces) in await GetManagedObjectsAsync(cancellationToken).ConfigureAwait(false))
        {
            if (BluezPaths.IsChildOf(path, adapterPath) &&
                interfaces.TryGetValue(Device1, out var properties) &&
                BluezPaths.TryParseAddress(StringOf(properties, "Address"), out var address))
            {
                known[path] = (address, StringOf(properties, "Name") ?? StringOf(properties, "Alias"),
                               HasUuid(properties, uuid));
                if (known[path].Matches && BoolOf(properties, "Connected"))
                {
                    connectedMatches.Add(new BluezAdvertisement(path, address, known[path].Name, null));
                }
            }
        }

        var gate = new Lock();
        var subscriptions = new List<IDisposable>();
        try
        {
            subscriptions.Add(await bus.AddMatchAsync(
                new MatchRule
                {
                    Type = MessageType.Signal,
                    Path = "/",
                    Interface = ObjectManager,
                    Member = "InterfacesAdded",
                },
                static (Message message, object? _) =>
                {
                    var reader = message.GetBodyReader();
                    var path = reader.ReadObjectPathAsString();
                    return (path, ReadInterfaces(ref reader));
                },
                (error, value, _, _) =>
                {
                    if (error is not null ||
                        !BluezPaths.IsChildOf(value.path, adapterPath) ||
                        !value.Item2.TryGetValue(Device1, out var properties) ||
                        !BluezPaths.TryParseAddress(StringOf(properties, "Address"), out var address))
                    {
                        return;
                    }

                    var name = StringOf(properties, "Name") ?? StringOf(properties, "Alias");
                    var matches = HasUuid(properties, uuid);
                    lock (gate)
                    {
                        known[value.path] = (address, name, matches);
                    }

                    if (matches)
                    {
                        found(new BluezAdvertisement(value.path, address, name, Int16Of(properties, "RSSI")));
                    }
                },
                ObserverFlags.None,
                readerState: null,
                handlerState: null,
                emitOnCapturedContext: false).ConfigureAwait(false));

            subscriptions.Add(await bus.AddMatchAsync(
                // NO PathNamespace: Tmds.DBus.Protocol 0.21.3 serialises it as
                // "pathNamespace=" instead of the spec's "path_namespace=", and the
                // bus rejects the whole rule (MatchRuleInvalid; confirmed on Bazzite
                // / dbus-broker, 2026-11-06). The handler drops paths not in `known`,
                // which only holds this adapter's devices, so the filter is redundant.
                new MatchRule
                {
                    Type = MessageType.Signal,
                    Interface = Properties,
                    Member = "PropertiesChanged",
                    Arg0 = Device1,
                },
                static (Message message, object? _) =>
                {
                    var reader = message.GetBodyReader();
                    _ = reader.ReadString();
                    return (message.PathAsString ?? string.Empty, reader.ReadDictionaryOfStringToVariantValue());
                },
                (error, value, _, _) =>
                {
                    if (error is not null)
                    {
                        return;
                    }

                    var (path, changed) = value;
                    (ulong Address, string? Name, bool Matches) device;
                    lock (gate)
                    {
                        if (!known.TryGetValue(path, out device))
                        {
                            return;
                        }

                        if (changed.ContainsKey("UUIDs"))
                        {
                            device.Matches = HasUuid(changed, uuid);
                            known[path] = device;
                        }
                    }

                    // An RSSI update is BlueZ relaying a fresh advertisement.
                    if (device.Matches && changed.ContainsKey("RSSI"))
                    {
                        found(new BluezAdvertisement(path, device.Address, device.Name, Int16Of(changed, "RSSI")));
                    }
                },
                ObserverFlags.None,
                readerState: null,
                handlerState: null,
                emitOnCapturedContext: false).ConfigureAwait(false));

            await CallAsync(adapterPath, Adapter1, "SetDiscoveryFilter", "a{sv}", (ref MessageWriter writer) =>
                writer.WriteDictionary(new Dictionary<string, VariantValue>
                {
                    ["UUIDs"] = VariantValue.Array(new[] { uuid }),
                    ["Transport"] = VariantValue.String("le"),

                    // Repeated advertisements are what keep RSSI updating for
                    // a device BlueZ already knows, which is the only signal a
                    // paired adapter gives that it is actually powered.
                    ["DuplicateData"] = VariantValue.Bool(true),
                }), cancellationToken).ConfigureAwait(false);

            await CallAsync(adapterPath, Adapter1, "StartDiscovery", null, null, cancellationToken)
                .ConfigureAwait(false);

            foreach (var connected in connectedMatches)
            {
                found(connected);
            }
        }
        catch
        {
            foreach (var subscription in subscriptions)
            {
                subscription.Dispose();
            }

            throw;
        }

        return new AsyncCallback(async () =>
        {
            foreach (var subscription in subscriptions)
            {
                subscription.Dispose();
            }

            try
            {
                await CallAsync(adapterPath, Adapter1, "StopDiscovery", null, null, CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (BluezException)
            {
                // Already stopped (another client, or bluetoothd restarted). The
                // scan is going away either way.
            }
        });
    }

    /* ---------------------------------------------------------- connection */

    public Task ConnectAsync(string devicePath, CancellationToken cancellationToken = default) =>
        CallAsync(devicePath, Device1, "Connect", null, null, cancellationToken);

    public async Task DisconnectAsync(string devicePath)
    {
        try
        {
            await CallAsync(devicePath, Device1, "Disconnect", null, null, CancellationToken.None)
                .WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
        }
        catch (Exception error) when (error is BluezException or TimeoutException)
        {
            // Best effort: a link that is already gone cannot be disconnected.
        }
    }

    public async Task<bool> WaitServicesResolvedAsync(
        string devicePath,
        CancellationToken cancellationToken = default)
    {
        var completion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var watch = await WatchPropertiesAsync(devicePath, Device1, changed =>
        {
            if (changed.TryGetValue("ServicesResolved", out var resolved) && resolved.GetBool())
            {
                completion.TrySetResult(true);
            }
            else if (changed.TryGetValue("Connected", out var connected) && !connected.GetBool())
            {
                completion.TrySetResult(false);
            }
        }, cancellationToken).ConfigureAwait(false);

        // Read AFTER subscribing, so a resolution between the two cannot be missed.
        var current = await GetPropertyAsync(devicePath, Device1, "ServicesResolved", cancellationToken)
            .ConfigureAwait(false);
        if (current.GetBool())
        {
            return true;
        }

        using (cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken)))
        {
            return await completion.Task.ConfigureAwait(false);
        }
    }

    public async Task<BluezGattService?> FindServiceAsync(
        string devicePath,
        string serviceUuid,
        CancellationToken cancellationToken = default)
    {
        var objects = await GetManagedObjectsAsync(cancellationToken).ConfigureAwait(false);
        var uuid = serviceUuid.ToLowerInvariant();
        foreach (var (path, interfaces) in objects)
        {
            if (!BluezPaths.IsChildOf(path, devicePath) ||
                !interfaces.TryGetValue(GattService1, out var service) ||
                !string.Equals(StringOf(service, "UUID"), uuid, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var characteristics = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var (childPath, childInterfaces) in objects)
            {
                if (BluezPaths.IsChildOf(childPath, path) &&
                    childInterfaces.TryGetValue(GattCharacteristic1, out var characteristic) &&
                    StringOf(characteristic, "UUID") is { } characteristicUuid)
                {
                    characteristics[characteristicUuid.ToLowerInvariant()] = childPath;
                }
            }

            return new BluezGattService(path, characteristics);
        }

        return null;
    }

    /* ----------------------------------------------------------------- GATT */

    public Task WriteAsync(
        string characteristicPath,
        byte[] value,
        bool withResponse,
        CancellationToken cancellationToken = default) =>
        CallAsync(characteristicPath, GattCharacteristic1, "WriteValue", "aya{sv}", (ref MessageWriter writer) =>
        {
            writer.WriteArray(value);
            writer.WriteDictionary(new Dictionary<string, VariantValue>
            {
                ["type"] = VariantValue.String(withResponse ? "request" : "command"),
            });
        }, cancellationToken);

    public async Task<IAsyncDisposable> SubscribeAsync(
        string characteristicPath,
        Action<byte[]> notification,
        CancellationToken cancellationToken = default)
    {
        var watch = await WatchPropertiesAsync(characteristicPath, GattCharacteristic1, changed =>
        {
            if (changed.TryGetValue("Value", out var value))
            {
                notification(value.GetArray<byte>());
            }
        }, cancellationToken).ConfigureAwait(false);

        try
        {
            await CallAsync(characteristicPath, GattCharacteristic1, "StartNotify", null, null, cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            watch.Dispose();
            throw;
        }

        return new AsyncCallback(async () =>
        {
            watch.Dispose();
            try
            {
                await CallAsync(characteristicPath, GattCharacteristic1, "StopNotify", null, null,
                    CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
            }
            catch (Exception error) when (error is BluezException or TimeoutException)
            {
                // Best effort, exactly like the Windows CCC clear on dispose: the
                // link may already be gone, and the unsubscribe must never block
                // the teardown that actually closes it.
            }
        });
    }

    public async Task<int?> GetMtuAsync(string characteristicPath, CancellationToken cancellationToken = default)
    {
        try
        {
            var mtu = await GetPropertyAsync(characteristicPath, GattCharacteristic1, "MTU", cancellationToken)
                .ConfigureAwait(false);
            return mtu.GetUInt16();
        }
        catch (BluezException)
        {
            // Older BlueZ (before 5.62) has no MTU property.
            return null;
        }
    }

    public Task<IDisposable> WatchDisconnectAsync(
        string devicePath,
        Action disconnected,
        CancellationToken cancellationToken = default)
    {
        var raised = 0;
        return WatchPropertiesAsync(devicePath, Device1, changed =>
        {
            if (changed.TryGetValue("Connected", out var connected) &&
                !connected.GetBool() &&
                Interlocked.Exchange(ref raised, 1) == 0)
            {
                disconnected();
            }
        }, cancellationToken);
    }

    public async Task<IDisposable> WatchDisconnectReasonAsync(
        string devicePath,
        Action<string, string> disconnected,
        CancellationToken cancellationToken = default)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        return await bus.AddMatchAsync(
            new MatchRule
            {
                Type = MessageType.Signal,
                Path = devicePath,
                Interface = Device1,
                Member = "Disconnected",
            },
            static (Message message, object? _) =>
            {
                var reader = message.GetBodyReader();
                return (reader.ReadString(), reader.ReadString());
            },
            (error, value, _, _) =>
            {
                if (error is null)
                {
                    disconnected(value.Item1, value.Item2);
                }
            },
            ObserverFlags.None,
            readerState: null,
            handlerState: null,
            emitOnCapturedContext: false).ConfigureAwait(false);
    }

    /* -------------------------------------------------------------- pairing */

    public Task PairAsync(string devicePath, CancellationToken cancellationToken = default) =>
        CallAsync(devicePath, Device1, "Pair", null, null, cancellationToken);

    public Task SetTrustedAsync(string devicePath, bool trusted, CancellationToken cancellationToken = default) =>
        CallAsync(devicePath, Properties, "Set", "ssv", (ref MessageWriter writer) =>
        {
            writer.WriteString(Device1);
            writer.WriteString("Trusted");
            writer.WriteVariant(VariantValue.Bool(trusted));
        }, cancellationToken);

    public Task RemoveDeviceAsync(
        string adapterPath,
        string devicePath,
        CancellationToken cancellationToken = default) =>
        CallAsync(adapterPath, Adapter1, "RemoveDevice", "o", (ref MessageWriter writer) =>
            writer.WriteObjectPath(devicePath), cancellationToken);

    public async ValueTask DisposeAsync()
    {
        await connectGate.WaitAsync().ConfigureAwait(false);
        try
        {
            connection?.Dispose();
            connection = null;
        }
        finally
        {
            connectGate.Release();
        }
    }

    /* -------------------------------------------------------------- helpers */

    private delegate void BodyWriter(ref MessageWriter writer);

    private async Task<Connection> BusAsync(CancellationToken cancellationToken)
    {
        await connectGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (connection is { } existing)
            {
                return existing;
            }

            if (Address.System is not { } address)
            {
                throw new BluezException(BluezErrors.ServiceUnknown, "No system D-Bus address is available.");
            }

            var opened = new Connection(address);
            try
            {
                await opened.ConnectAsync().ConfigureAwait(false);
            }
            catch (Exception error)
            {
                opened.Dispose();
                throw new BluezException(
                    BluezErrors.ServiceUnknown,
                    $"Could not connect to the system D-Bus: {error.Message}",
                    error);
            }

            connection = opened;
            return opened;
        }
        finally
        {
            connectGate.Release();
        }
    }

    private async Task CallAsync(
        string path,
        string @interface,
        string member,
        string? signature,
        BodyWriter? body,
        CancellationToken cancellationToken)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        var message = Build(bus, path, @interface, member, signature, body);
        await Guard(bus.CallMethodAsync(message), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Build one method call. The ONLY place a MessageWriter is used.
    ///
    /// MessageWriter is a struct whose buffer <c>CreateMessage()</c> detaches
    /// only from the instance it is called on. The first version of this file
    /// copied the writer (<c>var w = writer;</c>, to pass a <c>using</c>
    /// variable by ref) and created the message from the copy — so disposing
    /// the original returned the buffer the outgoing message still owned to the
    /// pool. bluetoothd received a corrupt message and never replied, and every
    /// call hung forever (found by SystemBusTests on WSL, 2026-11-06). Write,
    /// create and dispose on ONE instance; do not reintroduce the copy.
    /// </summary>
    private static MessageBuffer Build(
        Connection bus,
        string path,
        string @interface,
        string member,
        string? signature,
        BodyWriter? body)
    {
        var writer = bus.GetMessageWriter();
        try
        {
            writer.WriteMethodCallHeader(destination: Service, path: path, @interface: @interface, member: member,
                signature: signature);
            body?.Invoke(ref writer);
            return writer.CreateMessage();
        }
        finally
        {
            writer.Dispose();
        }
    }

    private async Task<VariantValue> GetPropertyAsync(
        string path,
        string @interface,
        string name,
        CancellationToken cancellationToken)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        var message = Build(bus, path, Properties, "Get", "ss", (ref MessageWriter w) =>
        {
            w.WriteString(@interface);
            w.WriteString(name);
        });

        return await Guard(
            bus.CallMethodAsync(message, static (Message reply, object? _) => reply.GetBodyReader().ReadVariantValue()),
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<Dictionary<string, Dictionary<string, Dictionary<string, VariantValue>>>>
        GetManagedObjectsAsync(CancellationToken cancellationToken)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        var message = Build(bus, "/", ObjectManager, "GetManagedObjects", signature: null, body: null);

        return await Guard(
            bus.CallMethodAsync(message, static (Message reply, object? _) =>
            {
                var reader = reply.GetBodyReader();
                var result = new Dictionary<string, Dictionary<string, Dictionary<string, VariantValue>>>(
                    StringComparer.Ordinal);
                var objects = reader.ReadDictionaryStart();
                while (reader.HasNext(objects))
                {
                    var path = reader.ReadObjectPathAsString();
                    result[path] = ReadInterfaces(ref reader);
                }

                return result;
            }),
            cancellationToken).ConfigureAwait(false);
    }

    private static Dictionary<string, Dictionary<string, VariantValue>> ReadInterfaces(ref Reader reader)
    {
        var interfaces = new Dictionary<string, Dictionary<string, VariantValue>>(StringComparer.Ordinal);
        var entries = reader.ReadDictionaryStart();
        while (reader.HasNext(entries))
        {
            var name = reader.ReadString();
            interfaces[name] = reader.ReadDictionaryOfStringToVariantValue();
        }

        return interfaces;
    }

    private async Task<IDisposable> WatchPropertiesAsync(
        string path,
        string @interface,
        Action<Dictionary<string, VariantValue>> changed,
        CancellationToken cancellationToken)
    {
        var bus = await BusAsync(cancellationToken).ConfigureAwait(false);
        return await bus.AddMatchAsync(
            new MatchRule
            {
                Type = MessageType.Signal,
                Path = path,
                Interface = Properties,
                Member = "PropertiesChanged",
                Arg0 = @interface,
            },
            static (Message message, object? _) =>
            {
                var reader = message.GetBodyReader();
                _ = reader.ReadString();
                return reader.ReadDictionaryOfStringToVariantValue();
            },
            (error, value, _, _) =>
            {
                if (error is null)
                {
                    changed(value);
                }
            },
            ObserverFlags.None,
            readerState: null,
            handlerState: null,
            emitOnCapturedContext: false).ConfigureAwait(false);
    }

    private async Task<T> Guard<T>(Task<T> call, CancellationToken cancellationToken)
    {
        try
        {
            return await call.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (DBusException error)
        {
            throw new BluezException(error.ErrorName, error.ErrorMessage, error);
        }
        catch (DisconnectedException error)
        {
            throw LostBus(error);
        }
    }

    private async Task Guard(Task call, CancellationToken cancellationToken)
    {
        try
        {
            await call.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (DBusException error)
        {
            throw new BluezException(error.ErrorName, error.ErrorMessage, error);
        }
        catch (DisconnectedException error)
        {
            throw LostBus(error);
        }
    }

    /// <summary>
    /// The system bus connection itself went away (dbus-daemon restarted, the
    /// socket closed). Forget it so the next call reconnects instead of failing
    /// on a dead connection for the rest of the session, and report it as
    /// ServiceUnknown: a local bus loss says nothing about the ADAPTER, so it
    /// must classify as a neutral protocol error — never as link-failure or
    /// bond evidence that the shared recovery policy would act on.
    /// </summary>
    private BluezException LostBus(DisconnectedException error)
    {
        var dead = Interlocked.Exchange(ref connection, null);
        dead?.Dispose();
        return new BluezException(BluezErrors.ServiceUnknown, $"The system D-Bus connection was lost: {error.Message}", error);
    }

    private static BluezDevice? DeviceOf(string path, Dictionary<string, VariantValue> properties) =>
        BluezPaths.TryParseAddress(StringOf(properties, "Address"), out var address)
            ? new BluezDevice(
                path,
                address,
                StringOf(properties, "Name") ?? StringOf(properties, "Alias"),
                BoolOf(properties, "Paired"),
                BoolOf(properties, "Connected"),
                BoolOf(properties, "ServicesResolved"))
            : null;

    private static bool HasUuid(Dictionary<string, VariantValue> properties, string uuid) =>
        properties.TryGetValue("UUIDs", out var uuids) &&
        uuids.GetArray<string>().Any(value => string.Equals(value, uuid, StringComparison.OrdinalIgnoreCase));

    private static string? StringOf(Dictionary<string, VariantValue> properties, string name) =>
        properties.TryGetValue(name, out var value) && value.Type == VariantValueType.String
            ? value.GetString()
            : null;

    private static bool BoolOf(Dictionary<string, VariantValue> properties, string name) =>
        properties.TryGetValue(name, out var value) && value.Type == VariantValueType.Bool && value.GetBool();

    private static short? Int16Of(Dictionary<string, VariantValue> properties, string name) =>
        properties.TryGetValue(name, out var value) && value.Type == VariantValueType.Int16
            ? value.GetInt16()
            : null;

    private sealed class AsyncCallback(Func<Task> dispose) : IAsyncDisposable
    {
        private int disposed;

        public async ValueTask DisposeAsync()
        {
            if (Interlocked.Exchange(ref disposed, 1) == 0)
            {
                await dispose().ConfigureAwait(false);
            }
        }
    }
}
