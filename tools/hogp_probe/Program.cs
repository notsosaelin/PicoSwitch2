using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

namespace PicoSwitch.Lab.HogpProbe;

/// <summary>
/// The WINDOWS_PASS.md §14.5 gating experiment, `windows-hogp-bridge-feasibility`.
///
/// ## The one question
///
/// Can a Windows PC present the 161-byte PicoSwitch Bridge HID report descriptor
/// as a BLE HID-over-GATT peripheral, such that the firmware's
/// `android_bridge_identify()` matches it and enables the v2 capability block?
///
/// Phase 6 (Controller Link on Windows) is gated on the answer, and so is a large
/// fraction of the schedule. §14.6 turns each failure mode into a different
/// product decision, which is why this reports WHICH of B1-B6 failed and at what
/// layer rather than a pass/fail.
///
/// ## Staged deliberately
///
/// Each stage answers exactly one of the open questions and stops on the first
/// hard failure, so a negative result names a layer instead of a symptom:
///
///   B1  GattServiceProvider.CreateAsync(0x1812) — does Windows publish HID at all?
///   B2  the 0x2908 Report Reference descriptor — is a reserved descriptor creatable?
///   B3  the Pico's HOGP client without DIS — Windows blocks publishing it
///   B4  will the Pico, as LE central, discover and connect to this peripheral?
///   B5  can the notification rate sustain 125 Hz?
///   B6  does bonding behave, and does it collide with the management bond?
///
/// B1 and B2 need no adapter and no pairing, so they run unattended and are worth
/// having even when nothing is plugged in. B3-B6 need the adapter in controller
/// pairing mode, and their verdict is READ FROM THE FIRMWARE, not inferred here:
/// `bridge` over UART reports calls/matched/rejected/first_mismatch directly.
/// Inferring a match from "the connection stayed up" is exactly the kind of
/// conclusion this project does not accept.
///
/// ## What this is not
///
/// Not part of the companion. Nothing in PicoSwitch.Companion.sln references it.
/// It project-references PicoSwitch.Bridge.Core solely so the report map is the
/// same bytes tools/check_android_descriptor_parity.py verifies across C, Kotlin
/// and C#; a pasted copy would make this experiment prove nothing about the
/// descriptor the product actually ships.
/// </summary>
internal static class Program
{
    /// <summary>Assigned-number UUIDs, expanded from the Bluetooth base UUID.</summary>
    private static Guid Assigned(ushort id) =>
        new(id, 0x0000, 0x1000, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB);

    private static readonly Guid HidService = Assigned(0x1812);
    private static readonly Guid ReportMap = Assigned(0x2A4B);
    private static readonly Guid ReportCharacteristic = Assigned(0x2A4D);
    private static readonly Guid HidInformation = Assigned(0x2A4A);
    private static readonly Guid HidControlPoint = Assigned(0x2A4C);
    private static readonly Guid ProtocolMode = Assigned(0x2A4E);
    private static readonly Guid ReportReference = Assigned(0x2908);

    /// <summary>A meaningless 128-bit service, used only as the advertising control.</summary>
    private static readonly Guid ControlService =
        new("6f9d1a54-2c31-4b8e-9f07-1d3a5c7e9b20");

    private static readonly Findings Findings = new();

    private static async Task<int> Main(string[] args)
    {
        var seconds = ArgInt(args, "--seconds", 120);
        var output = ArgString(args, "--out", null);
        var advertiseOnly = args.Contains("--probe-only", StringComparer.Ordinal);

        // CONTROLS FOR THE ADVERTISING QUESTION, one variable each.
        //
        // B1 and B2 passed on this radio and advertising still reported Aborted,
        // which is a failure §14.5 did not anticipate and which does not belong to
        // any of B1-B6 as written. These exist to say WHICH property Windows
        // objected to rather than guessing:
        //
        //   --control-service   a custom 128-bit service instead of 0x1812, so
        //                       "this radio cannot advertise at all" is separable
        //                       from "it will not advertise HID"
        //   --plain             drop EncryptionRequired from every characteristic
        //   --no-discoverable   connectable but not discoverable
        var controlService = args.Contains("--control-service", StringComparer.Ordinal);
        var plain = args.Contains("--plain", StringComparer.Ordinal);
        var discoverable = !args.Contains("--no-discoverable", StringComparer.Ordinal);
        var connectable = !args.Contains("--not-connectable", StringComparer.Ordinal);
        var protection = plain
            ? GattProtectionLevel.Plain
            : GattProtectionLevel.EncryptionRequired;

        Findings.Answers["mode.controlService"] = controlService;
        Findings.Answers["mode.plain"] = plain;
        Findings.Answers["mode.discoverable"] = discoverable;
        Findings.Answers["mode.connectable"] = connectable;

        Banner();

        if (args.Contains("--advertiser-control", StringComparer.Ordinal))
        {
            // THE CONTROL FOR "CAN THIS RADIO ADVERTISE AT ALL".
            //
            // BluetoothLEAdvertisementPublisher is the simplest possible LE
            // advertiser: no GATT service, no characteristics, no HID. If this
            // cannot start either, the machine cannot act as an LE advertiser and
            // nothing about B1-B6 has been measured -- the experiment is
            // inconclusive on this radio rather than negative.
            return await AdvertiserControlAsync(output);
        }

        // ---- Environment ---------------------------------------------------
        // §14.5: "record the outcome per radio". Peripheral-role support and LE
        // peripheral quality vary by controller and driver, so a result without
        // its radio is not a result.
        var adapter = await Step("environment", async () =>
        {
            var value = await BluetoothAdapter.GetDefaultAsync();
            if (value is null)
            {
                throw new InvalidOperationException("No Bluetooth adapter is present.");
            }

            Findings.Environment["osVersion"] = Environment.OSVersion.VersionString;
            Findings.Environment["radioAddress"] = FormatAddress(value.BluetoothAddress);
            Findings.Environment["isLowEnergySupported"] = value.IsLowEnergySupported;
            Findings.Environment["isCentralRoleSupported"] = value.IsCentralRoleSupported;
            Findings.Environment["isPeripheralRoleSupported"] = value.IsPeripheralRoleSupported;
            Findings.Environment["isAdvertisementOffloadSupported"] =
                value.IsAdvertisementOffloadSupported;
            Findings.Environment["maxAdvertisementDataLength"] = value.MaxAdvertisementDataLength;
            Findings.Environment["descriptorBytes"] = BridgeHidDescriptor.Bytes.Length;
            Findings.Environment["descriptorSha256"] = Sha256(BridgeHidDescriptor.ToArray());
            Findings.Environment["bridgeContract"] = BridgeContract.Version;
            return value;
        });

        if (adapter is null)
        {
            return Finish(output, 2);
        }

        Report("radio", $"address={Findings.Environment["radioAddress"]} " +
            $"le={adapter.IsLowEnergySupported} central={adapter.IsCentralRoleSupported} " +
            $"peripheral={adapter.IsPeripheralRoleSupported}");
        Report("descriptor",
            $"{BridgeHidDescriptor.Bytes.Length} bytes, sha256 " +
            $"{((string)Findings.Environment["descriptorSha256"])[..16]}…, contract {BridgeContract.Version}");

        if (!adapter.IsPeripheralRoleSupported)
        {
            // Not a B1 failure. The radio simply cannot take the role, which is a
            // per-machine fact and a documented product gate, not a Windows API
            // verdict. Recording it as B1 would poison the schedule decision.
            Fail("precondition", "B0",
                "This radio does not support the LE peripheral role. Re-run on a radio that does; " +
                "this says nothing about B1-B6.");
            return Finish(output, 3);
        }

        // ---- B1: can Windows publish the HID service at all? ----------------
        // The control service is a random 128-bit UUID with no assigned meaning, so
        // Windows has no policy opinion about it. If THAT advertises and HID does
        // not, the objection is to the service; if neither does, the objection is
        // to peripheral advertising on this radio and has nothing to do with HOGP.
        var serviceUuid = controlService ? ControlService : HidService;

        var provider = await Step("B1", async () =>
        {
            var result = await GattServiceProvider.CreateAsync(serviceUuid);
            Findings.Answers["B1.error"] = result.Error.ToString();
            if (result.Error != BluetoothError.Success)
            {
                throw new BluetoothProbeException(
                    $"GattServiceProvider.CreateAsync(0x1812) returned {result.Error}");
            }

            return result.ServiceProvider;
        });

        if (provider is null)
        {
            Fail("B1", "B1",
                $"Windows refused to publish the HID service (0x1812): {Findings.Answers.GetValueOrDefault("B1.error")}. " +
                "Path B is dead; §14.6 escalates to Path C as a separate joint firmware + Windows pass.");
            return Finish(output, 4);
        }

        Pass("B1", $"Windows published service {serviceUuid}.");

        // ---- B2: the Report Reference descriptor ---------------------------
        // Report Map first: it is a plain read characteristic, so a failure here
        // is unambiguous and separates "Windows will not take these bytes" from
        // "Windows will not take a reserved descriptor".
        var reportMap = await Step("B2.reportMap", () => CreateAsync(
            provider, ReportMap,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties = GattCharacteristicProperties.Read,
                ReadProtectionLevel = protection,
                StaticValue = Buffer(BridgeHidDescriptor.ToArray()),
            }));

        if (reportMap is null)
        {
            Fail("B2", "B2", "The Report Map characteristic could not be created.");
            return Finish(output, 5);
        }

        var input = await Step("B2.inputReport", () => CreateAsync(
            provider, ReportCharacteristic,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties =
                    GattCharacteristicProperties.Read | GattCharacteristicProperties.Notify,
                ReadProtectionLevel = protection,
            }));

        var outputReport = await Step("B2.outputReport", () => CreateAsync(
            provider, ReportCharacteristic,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties =
                    GattCharacteristicProperties.Read |
                    GattCharacteristicProperties.Write |
                    GattCharacteristicProperties.WriteWithoutResponse,
                ReadProtectionLevel = protection,
                WriteProtectionLevel = protection,
            }));

        if (input is null || outputReport is null)
        {
            Fail("B2", "B2", "A Report characteristic could not be created.");
            return Finish(output, 5);
        }

        // THE ACTUAL B2 QUESTION. 0x2908 is a reserved descriptor and is not in
        // the auto-generated set, so it *should* be creatable — but WinRT throws
        // for reserved descriptors it owns, and there is no documented list.
        var inputReference = await Step("B2.inputReference", () => CreateDescriptorAsync(
            input, ReportReference, [0x01, 0x01]));   // report id 1, Input
        var outputReference = await Step("B2.outputReference", () => CreateDescriptorAsync(
            outputReport, ReportReference, [0x02, 0x02]));  // report id 2, Output

        if (inputReference is null || outputReference is null)
        {
            Fail("B2", "B2",
                "The 0x2908 Report Reference descriptor could not be created. Without it a HOGP " +
                "client cannot map a Report characteristic to a report id, so Path B is dead; " +
                "§14.6 escalates to Path C.");
            return Finish(output, 5);
        }

        Pass("B2", "Report Map, both Report characteristics and both 0x2908 descriptors were created.");

        // The rest of the mandatory HOGP set. Not separately gated: if these fail
        // the service is incomplete and B3 cannot be asked honestly.
        //
        // HID Information: bcdHID 0x0111, country 0x00, flags 0x02
        // (NormallyConnectable). Protocol Mode reports Report mode (0x01) — boot
        // mode has no vendor extension and would defeat the whole point.
        var hidInfo = await Step("hidInformation", () => CreateAsync(
            provider, HidInformation,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties = GattCharacteristicProperties.Read,
                ReadProtectionLevel = protection,
                StaticValue = Buffer([0x11, 0x01, 0x00, 0x02]),
            }));

        var controlPoint = await Step("hidControlPoint", () => CreateAsync(
            provider, HidControlPoint,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties = GattCharacteristicProperties.WriteWithoutResponse,
            }));

        var protocolMode = await Step("protocolMode", () => CreateAsync(
            provider, ProtocolMode,
            new GattLocalCharacteristicParameters
            {
                CharacteristicProperties =
                    GattCharacteristicProperties.Read |
                    GattCharacteristicProperties.WriteWithoutResponse,
                StaticValue = Buffer([0x01]),
            }));

        if (hidInfo is null || controlPoint is null || protocolMode is null)
        {
            Fail("hogp-set", "B2", "The mandatory HOGP characteristic set is incomplete.");
            return Finish(output, 5);
        }

        Report("service", "HID Information, HID Control Point and Protocol Mode created.");

        // DIS IS DELIBERATELY ABSENT, and that absence is the experiment.
        //
        // HOGP mandates the Device Information Service with a PnP ID, and Windows
        // blocks publishing DIS from an application. B3 asks whether BTstack's
        // hids_client proceeds without it. If it does not, Path B is dead on
        // Windows without a firmware change that is out of scope for this pass.
        Findings.Answers["deviceInformationServicePublished"] = false;

        var subscribers = 0;
        input.SubscribedClientsChanged += (sender, _) =>
        {
            subscribers = sender.SubscribedClients.Count;
            Report("subscribers", $"input report subscribers = {subscribers}");
        };

        outputReport.WriteRequested += async (_, eventArgs) =>
        {
            using var deferral = eventArgs.GetDeferral();
            var request = await eventArgs.GetRequestAsync();
            var bytes = new byte[request.Value.Length];
            DataReader.FromBuffer(request.Value).ReadBytes(bytes);
            Findings.OutputReports.Add(Convert.ToHexString(bytes));
            Report("output", $"report 2 <- {Convert.ToHexString(bytes)}");
            if (request.Option == GattWriteOption.WriteWithResponse)
            {
                request.Respond();
            }
        };

        // A GET_REPORT poll must be answered synchronously with a freshly composed
        // report (§14.1 item 5), so this is not optional politeness.
        input.ReadRequested += async (_, eventArgs) =>
        {
            using var deferral = eventArgs.GetDeferral();
            var request = await eventArgs.GetRequestAsync();
            request.RespondWithValue(Buffer(ControllerReportEncoder.Encode(ControllerState.Neutral)));
        };

        // ---- B4: advertise connectably and let the Pico find us -------------
        // THE EVENT ARGS CARRY THE REASON. An earlier version logged only the
        // status, so `Aborted` arrived with no explanation and three rounds of
        // controls were spent narrowing what the error byte says outright.
        var advertisingTransitions = new List<string>();
        provider.AdvertisementStatusChanged += (sender, eventArgs) =>
        {
            advertisingTransitions.Add($"{sender.AdvertisementStatus}/{eventArgs.Error}");
            Report("advertising", $"{sender.AdvertisementStatus} error={eventArgs.Error}");
        };

        // RETRIED, because `Aborted` with `Error = Success` is what an Intel radio
        // reports when it cannot take the advertisement RIGHT NOW rather than
        // when it cannot take it at all. This machine holds several live LE links
        // (two Xbox controllers, a keyboard, the adapter itself), and a
        // one-shot attempt cannot tell a resource conflict from a refusal.
        //
        // If every attempt aborts, that is a much stronger claim than one abort.
        var parameters = new GattServiceProviderAdvertisingParameters
        {
            IsConnectable = connectable,
            IsDiscoverable = discoverable,
        };

        var settled = GattServiceProviderAdvertisementStatus.Created;
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            provider.StartAdvertising(parameters);
            settled = await SettleAsync(provider, TimeSpan.FromSeconds(5));
            Report("advertising", $"attempt {attempt}: {settled}");
            if (settled is GattServiceProviderAdvertisementStatus.Started or
                           GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData)
            {
                break;
            }

            provider.StopAdvertising();
            await Task.Delay(1000);
        }

        Findings.Answers["advertisingAttempts"] = advertisingTransitions.Count;

        // WAIT FOR THE STATUS TO SETTLE. StartAdvertising is asynchronous and
        // leaves the property at `Created` for a moment, so reading it straight
        // afterwards records the state before Windows has decided anything. The
        // first run of this probe reported `Created` as though it were a result.
        Findings.Answers["advertisementStatus"] = settled.ToString();
        Findings.Answers["advertisementTransitions"] = advertisingTransitions;

        if (settled is not (GattServiceProviderAdvertisementStatus.Started or
                            GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData))
        {
            Fail("advertising", "B4",
                $"Advertising did not start: {settled}. Nothing downstream of this can be " +
                "measured, and the failure is in the peripheral advertiser rather than in HOGP.");
            return Finish(output, 6);
        }

        if (settled == GattServiceProviderAdvertisementStatus.StartedWithoutAllAdvertisementData)
        {
            // Worth recording rather than treating as success: if the service UUID
            // was the field that got dropped, the Pico's scan filter has nothing
            // to match on and a B4 failure would be an artefact of this, not a
            // property of the firmware.
            Report("advertising",
                "WARNING: some advertisement data was dropped; a B4 failure here may be an " +
                "artefact of the truncated advertisement rather than the firmware's scan.");
        }

        if (advertiseOnly)
        {
            Report("done", "--probe-only: B1 and B2 answered; stopping before B3-B6.");
            provider.StopAdvertising();
            return Finish(output, 0);
        }

        Console.WriteLine();
        Console.WriteLine("  Put the adapter into CONTROLLER pairing mode now (double-tap BOOTSEL,");
        Console.WriteLine("  or `pairing start` over management). Then read the firmware's verdict:");
        Console.WriteLine();
        Console.WriteLine("      pwsh -File tools/read_uart_diag.ps1 -Command 'bridge'");
        Console.WriteLine();

        // ---- B5: 125 Hz with a slow A toggle --------------------------------
        // The A toggle is what distinguishes "reports are being accepted" from
        // "reports are reaching a matched profile" when watched on the console.
        var clock = Stopwatch.StartNew();
        var sent = 0;
        var failed = 0;
        var toggles = 0;
        var worstGapMicroseconds = 0L;
        var previousTick = clock.ElapsedTicks;
        var deadline = TimeSpan.FromSeconds(seconds);
        var interval = TimeSpan.FromMilliseconds(8);

        while (clock.Elapsed < deadline)
        {
            var pressed = (int)(clock.Elapsed.TotalSeconds / 2) % 2 == 1;
            var state = ControllerState.Neutral with
            {
                Buttons = ControllerButtonSet.Empty.With(ControllerButton.A, pressed),
            };

            if (subscribers > 0)
            {
                var status = await input.NotifyValueAsync(
                    Buffer(ControllerReportEncoder.Encode(state)));
                if (status.Count > 0 &&
                    status[0].Status == GattCommunicationStatus.Success)
                {
                    sent++;
                }
                else if (status.Count > 0)
                {
                    failed++;
                }

                var now = clock.ElapsedTicks;
                var gap = (now - previousTick) * 1_000_000L / Stopwatch.Frequency;
                previousTick = now;
                if (gap > worstGapMicroseconds)
                {
                    worstGapMicroseconds = gap;
                }
            }
            else
            {
                previousTick = clock.ElapsedTicks;
            }

            if (pressed != (toggles % 2 == 1))
            {
                toggles++;
            }

            await Task.Delay(interval);
        }

        Findings.Answers["notificationsSent"] = sent;
        Findings.Answers["notificationsFailed"] = failed;
        Findings.Answers["subscribersAtEnd"] = subscribers;
        Findings.Answers["worstNotifyGapMicroseconds"] = worstGapMicroseconds;
        Findings.Answers["outputReportsReceived"] = Findings.OutputReports.Count;

        if (subscribers == 0)
        {
            // Not a verdict on B3 by itself: nothing subscribed could equally be
            // B4 (the Pico never connected). The UART trace separates them —
            // `calls == 0` with no connection is B4, `calls == 0` with a
            // connection is B3.
            Fail("B3/B4", "B3/B4",
                "No client ever subscribed to the input report. Read `bridge` and `btstate` over " +
                "UART: a connection with calls == 0 is B3 (the HOGP client refused, most likely " +
                "over the absent DIS); no connection at all is B4.");
        }
        else
        {
            Pass("B4", $"A client subscribed and {sent} notifications were accepted.");
        }

        provider.StopAdvertising();
        Report("advertising", "stopped");

        Console.WriteLine();
        Console.WriteLine("  Now read the firmware's own verdict — this experiment does NOT");
        Console.WriteLine("  conclude a match from the connection staying up:");
        Console.WriteLine();
        Console.WriteLine("      pwsh -File tools/read_uart_diag.ps1 -Command 'bridge'");
        Console.WriteLine();

        return Finish(output, 0);
    }

    private static async Task<int> AdvertiserControlAsync(string? output)
    {
        var publisher = new BluetoothLEAdvertisementPublisher();

        // MANUFACTURER DATA ONLY, deliberately. A first attempt put the HID
        // service UUID in ServiceUuids and Start() threw ArgumentException:
        // BluetoothLEAdvertisementPublisher restricts which AD sections an
        // application may compose, and this control is not asking about AD
        // contents. It is asking the narrower question "can this radio transmit
        // an LE advertisement at all", so it advertises the least contentious
        // payload there is.
        var manufacturer = new BluetoothLEManufacturerData { CompanyId = 0xFFFF };
        var writer = new DataWriter();
        writer.WriteBytes([0x50, 0x53, 0x32]);
        manufacturer.Data = writer.DetachBuffer();
        publisher.Advertisement.ManufacturerData.Add(manufacturer);

        var seen = new List<string>();
        publisher.StatusChanged += (sender, eventArgs) =>
        {
            seen.Add($"{sender.Status}/{eventArgs.Error}");
            Report("publisher", $"status={sender.Status} error={eventArgs.Error}");
        };

        try
        {
            publisher.Start();
        }
        catch (Exception error)
        {
            Findings.Answers["publisher.exception"] = error.GetType().Name;
            Findings.Answers["publisher.message"] = error.Message;
            Fail("advertiser-control", "B0",
                $"BluetoothLEAdvertisementPublisher.Start() threw: {error.Message}");
            return Finish(output, 7);
        }
        // Both Created AND Waiting are pre-decision states. An earlier version
        // waited only while Waiting, exited immediately because the status was
        // still Created, and recorded "cannot advertise" from a status that meant
        // "has not tried yet".
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < TimeSpan.FromSeconds(10) &&
               publisher.Status is BluetoothLEAdvertisementPublisherStatus.Created
                                or BluetoothLEAdvertisementPublisherStatus.Waiting)
        {
            await Task.Delay(50);
        }

        Findings.Answers["publisherStatus"] = publisher.Status.ToString();
        Findings.Answers["publisherTransitions"] = seen;
        Report("publisher", $"settled status={publisher.Status}");

        if (publisher.Status == BluetoothLEAdvertisementPublisherStatus.Started)
        {
            Pass("advertiser-control", "This radio can advertise; the failure is specific to " +
                "GattServiceProvider advertising.");
            await Task.Delay(2000);
            publisher.Stop();
            return Finish(output, 0);
        }

        Fail("advertiser-control", "B0",
            $"This radio cannot start an LE advertisement at all ({publisher.Status}). B1-B6 are " +
            "NOT answered by this run; re-run on a radio that can advertise.");
        publisher.Stop();
        return Finish(output, 7);
    }

    /// <summary>
    /// Wait for the advertiser to reach a terminal state, or give up.
    ///
    /// `Created` is the transient state before Windows has acted, so treating it
    /// as an answer records "we did not wait" as though it were "it worked".
    /// </summary>
    private static async Task<GattServiceProviderAdvertisementStatus> SettleAsync(
        GattServiceProvider provider, TimeSpan budget)
    {
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < budget &&
               provider.AdvertisementStatus is GattServiceProviderAdvertisementStatus.Created
                                            or GattServiceProviderAdvertisementStatus.Stopped)
        {
            await Task.Delay(50);
        }

        return provider.AdvertisementStatus;
    }

    /* ------------------------------------------------------------- plumbing */

    private static async Task<GattLocalCharacteristic?> CreateAsync(
        GattServiceProvider provider, Guid uuid, GattLocalCharacteristicParameters parameters)
    {
        var result = await provider.Service.CreateCharacteristicAsync(uuid, parameters);
        if (result.Error != BluetoothError.Success)
        {
            throw new BluetoothProbeException(
                $"CreateCharacteristicAsync({uuid}) returned {result.Error}");
        }

        return result.Characteristic;
    }

    private static async Task<GattLocalDescriptor?> CreateDescriptorAsync(
        GattLocalCharacteristic characteristic, Guid uuid, byte[] value)
    {
        var result = await characteristic.CreateDescriptorAsync(uuid,
            new GattLocalDescriptorParameters
            {
                ReadProtectionLevel = GattProtectionLevel.Plain,
                StaticValue = Buffer(value),
            });

        if (result.Error != BluetoothError.Success)
        {
            throw new BluetoothProbeException(
                $"CreateDescriptorAsync({uuid}) returned {result.Error}");
        }

        return result.Descriptor;
    }

    /// <summary>
    /// Run one stage and record what it did, including the exact failure.
    ///
    /// Returning null rather than throwing is deliberate: §14.6 needs to know
    /// WHICH stage failed and with what error, and an unhandled exception at the
    /// top level throws that away along with everything already learned.
    /// </summary>
    private static async Task<T?> Step<T>(string name, Func<Task<T?>> block)
        where T : class
    {
        try
        {
            return await block();
        }
        catch (Exception error)
        {
            Findings.Answers[$"{name}.exception"] = error.GetType().Name;
            Findings.Answers[$"{name}.message"] = error.Message;
            if (error is not BluetoothProbeException)
            {
                Findings.Answers[$"{name}.hresult"] =
                    "0x" + error.HResult.ToString("X8", CultureInfo.InvariantCulture);
            }

            Report("fail", $"{name}: {error.Message}");
            return null;
        }
    }

    private static IBuffer Buffer(byte[] value)
    {
        var writer = new DataWriter();
        writer.WriteBytes(value);
        return writer.DetachBuffer();
    }

    private static string Sha256(byte[] value) =>
        Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(value)).ToLowerInvariant();

    private static string FormatAddress(ulong value) =>
        string.Join(':', BitConverter.GetBytes(value).Take(6).Reverse()
            .Select(b => b.ToString("X2", CultureInfo.InvariantCulture)));

    private static void Banner()
    {
        Console.WriteLine();
        Console.WriteLine("  windows-hogp-bridge-feasibility — WINDOWS_PASS.md §14.5");
        Console.WriteLine("  Can Windows present the PicoSwitch Bridge descriptor over HOGP?");
        Console.WriteLine();
    }

    private static void Report(string tag, string message) =>
        Console.WriteLine($"  {DateTime.Now:HH:mm:ss.fff}  {tag,-12} {message}");

    private static void Pass(string tag, string message)
    {
        Findings.Verdicts[tag] = "pass";
        Report("PASS", $"{tag}: {message}");
    }

    private static void Fail(string tag, string question, string message)
    {
        Findings.Verdicts[tag] = "fail";
        Findings.Answers["failedQuestion"] = question;
        Report("FAIL", $"{tag}: {message}");
    }

    private static int Finish(string? path, int code)
    {
        Findings.ExitCode = code;
        var json = JsonSerializer.Serialize(Findings,
            new JsonSerializerOptions { WriteIndented = true });

        Console.WriteLine();
        Console.WriteLine(json);

        if (path is not null)
        {
            File.WriteAllText(path, json, Encoding.UTF8);
            Console.WriteLine();
            Console.WriteLine($"  written: {path}");
        }

        return code;
    }

    private static int ArgInt(string[] args, string name, int fallback)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length &&
               int.TryParse(args[index + 1], CultureInfo.InvariantCulture, out var value)
            ? value
            : fallback;
    }

    private static string? ArgString(string[] args, string name, string? fallback)
    {
        var index = Array.IndexOf(args, name);
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : fallback;
    }
}

internal sealed class BluetoothProbeException(string message) : Exception(message);

internal sealed class Findings
{
    public Dictionary<string, object> Environment { get; } = [];

    public Dictionary<string, object> Answers { get; } = [];

    public Dictionary<string, string> Verdicts { get; } = [];

    public List<string> OutputReports { get; } = [];

    public int ExitCode { get; set; }
}
