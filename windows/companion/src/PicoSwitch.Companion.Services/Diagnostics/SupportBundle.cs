using System.Runtime.InteropServices;
using System.Text;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Diagnostics;

/// <summary>
/// Everything a support report needs, and nothing that identifies anyone.
///
/// ## The privacy guarantee is part of the artifact (I16)
///
/// The header states what the bundle contains and what it deliberately does not,
/// because the person pasting it into an issue is agreeing to publish it. A
/// guarantee that lives only in documentation is one the reader never sees.
///
/// Redaction rules, applied here rather than trusted to callers:
///
/// - Bluetooth addresses are reduced to a short suffix. An adapter's identity in
///   a bug report only ever needs to distinguish "the same one" from "a different
///   one", and the full address is a durable identifier for a device in someone's
///   home.
/// - User aliases never appear at all. People name adapters after rooms, houses
///   and children.
/// - Controller names DO appear: they are product names, they are the entire
///   point of a compatibility report, and they identify hardware rather than a
///   person.
///
/// ## Why the radio block is not padding
///
/// It is the first thing anyone needs when a Controller Link report says "it does
/// not work on my PC" (§14.5). The peripheral-role flag in particular decides
/// whether that machine can ever act as a controller source, and without it every
/// such report starts with two rounds of questions.
/// </summary>
public static class SupportBundle
{
    public static string Render(
        DiagnosticLog log,
        AdapterSnapshot snapshot,
        AdapterRegistry registry,
        AdapterRelationshipStatus relationship,
        BluetoothRadioCapabilities radio,
        ConnectionState connection,
        string? controllerLinkHostLog = null)
    {
        var text = new StringBuilder();

        text.AppendLine("PicoSwitch2 companion — support bundle");
        text.AppendLine(
            "Contains: app and firmware versions, Bluetooth radio capabilities, adapter and");
        text.AppendLine(
            "capability state, and the diagnostic log. Bluetooth addresses appear only as a");
        text.AppendLine(
            "short suffix, and adapter nicknames are never included. Controller product names");
        text.AppendLine("are included, because compatibility reports are about them.");
        text.AppendLine();

        AppendHost(text);
        AppendRadio(text, radio);
        AppendApp(text, snapshot, connection);
        AppendAdapters(text, registry, relationship);
        AppendCapabilities(text, snapshot.Capabilities);
        AppendControllers(text, snapshot);

        text.AppendLine("── Diagnostic log ─────────────────────────────────────────");
        text.Append(log.Render());
        if (!string.IsNullOrWhiteSpace(controllerLinkHostLog))
        {
            text.AppendLine();
            text.AppendLine("── Controller Link host ───────────────────────────────────");
            text.Append(controllerLinkHostLog.TrimEnd()).AppendLine();
        }
        return text.ToString();
    }

    private static void AppendHost(StringBuilder text)
    {
        text.AppendLine("── Host ───────────────────────────────────────────────────");
        text.AppendLine($"windows       : {Environment.OSVersion.VersionString}");
        text.AppendLine($"architecture  : {RuntimeInformation.ProcessArchitecture}");
        text.AppendLine($"os arch       : {RuntimeInformation.OSArchitecture}");
        text.AppendLine($"runtime       : {RuntimeInformation.FrameworkDescription}");
        text.AppendLine();
    }

    private static void AppendRadio(StringBuilder text, BluetoothRadioCapabilities radio)
    {
        text.AppendLine("── Bluetooth radio ────────────────────────────────────────");
        text.AppendLine($"present       : {radio.RadioPresent}");
        text.AppendLine($"powered       : {radio.RadioOn}");
        text.AppendLine($"low energy    : {radio.LowEnergySupported}");
        text.AppendLine($"central role  : {radio.CentralRoleSupported}");

        // The one that decides whether this PC can ever be a controller source.
        text.AppendLine($"peripheral    : {radio.PeripheralRoleSupported}");
        text.AppendLine($"adv offload   : {radio.AdvertisementOffloadSupported}");
        text.AppendLine($"address       : {Suffix(radio.LocalAddress)}");
        text.AppendLine($"management    : {radio.ManagementBlockedReason ?? "available"}");
        text.AppendLine($"controller link: {radio.PeripheralBlockedReason ?? "radio-capable"}");
        text.AppendLine();
    }

    private static void AppendApp(StringBuilder text, AdapterSnapshot snapshot, ConnectionState connection)
    {
        var firmware = snapshot.Firmware;
        var contract = BridgeContract.Evaluate(
            firmware.BridgeContract,
            connection.Connected,
            !string.IsNullOrWhiteSpace(firmware.Id));

        text.AppendLine("── Versions ───────────────────────────────────────────────");
        text.AppendLine($"app           : {AppVersion()}");
        text.AppendLine($"configuration : {Configuration}");
        text.AppendLine($"mgmt protocol : {ManagementProtocolVersion}");
        text.AppendLine($"bridge (app)  : {BridgeContract.Version}");
        text.AppendLine(
            $"firmware      : {Or(firmware.Version)} build {Or(firmware.Build)} " +
            $"product {Or(firmware.Product)}");
        text.AppendLine($"bridge (fw)   : {firmware.BridgeContract}");
        text.AppendLine($"contract      : {contract.Summary}");
        text.AppendLine($"personality   : {ControllerModeSection.Label(snapshot.Personality.Current)}");
        text.AppendLine();
    }

    private static void AppendAdapters(
        StringBuilder text,
        AdapterRegistry registry,
        AdapterRelationshipStatus relationship)
    {
        text.AppendLine("── Adapters ───────────────────────────────────────────────");
        text.AppendLine($"remembered    : {registry.Records.Count}");
        text.AppendLine($"active        : {Suffix(registry.ActiveId?.Value)}");
        text.AppendLine($"phase         : {relationship.Phase}");
        text.AppendLine($"generation    : {relationship.Generation}");
        text.AppendLine($"reason        : {relationship.Reason?.ToString() ?? "none"}");
        text.AppendLine($"pairing       : {relationship.Pairing}");

        foreach (var record in registry.Records)
        {
            // Suffix and facts only. No alias: people name adapters after rooms,
            // houses and children.
            text.AppendLine(
                $"  adapter …{Suffix(record.Address)} repair={record.RepairRequired} " +
                $"fw={Or(record.LastFirmwareVersion)} personality={Or(record.LastPersonality)}");
        }

        text.AppendLine();
    }

    private static void AppendCapabilities(StringBuilder text, AdapterCapabilities capabilities)
    {
        text.AppendLine("── Capabilities ───────────────────────────────────────────");
        text.AppendLine($"core          : {capabilities.Core}");
        text.AppendLine($"personality   : {capabilities.Personality}");
        text.AppendLine($"colors        : {capabilities.Colors}");
        text.AppendLine($"peers         : {capabilities.Peers}");
        text.AppendLine($"peer forget   : {capabilities.PeerForget}");
        text.AppendLine($"remote pairing: {capabilities.RemotePairing}");
        text.AppendLine($"active input  : {capabilities.ActiveInput}");
        text.AppendLine($"amiibo        : {capabilities.Amiibo}");
        text.AppendLine($"kbm           : {capabilities.Kbm}");
        text.AppendLine();
    }

    private static void AppendControllers(StringBuilder text, AdapterSnapshot snapshot)
    {
        text.AppendLine("── Controllers ────────────────────────────────────────────");
        text.AppendLine($"attached      : {snapshot.Controller.Name}");
        text.AppendLine(
            $"vid/pid       : {snapshot.Controller.Vid:X4}:{snapshot.Controller.Pid:X4}");
        text.AppendLine($"peers         : {snapshot.Peers.Peers.Count} (complete={snapshot.Peers.Complete})");

        foreach (var peer in snapshot.Peers.Peers)
        {
            // Product names stay; addresses do not.
            text.AppendLine(
                $"  peer …{Suffix(peer.Address)} role={peer.Role.WireName()} " +
                $"bonded={peer.Bonded} connected={peer.Connected} name={Or(peer.Name)}");
        }

        text.AppendLine();
    }

    /// <summary>
    /// The last three octets, which is enough to tell two adapters apart in a bug
    /// report and not enough to identify a device in someone's home.
    /// </summary>
    private static string Suffix(string? address)
    {
        if (string.IsNullOrWhiteSpace(address))
        {
            return "none";
        }

        var clean = address.Replace(":", string.Empty, StringComparison.Ordinal);
        return clean.Length <= 6 ? clean : clean[^6..];
    }

    private static string Or(string? value) =>
        string.IsNullOrWhiteSpace(value) ? "unknown" : value;

    private static string AppVersion() =>
        typeof(SupportBundle).Assembly.GetName().Version?.ToString() ?? "unknown";

    private const string ManagementProtocolVersion = "1";

#if DEBUG
    private const string Configuration = "Debug";
#else
    private const string Configuration = "Release";
#endif
}
