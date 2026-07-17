using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32;

internal static class UltimateFlashHarness
{
    private const string DllName = "8BitDoFirmwareUpdaterTools.dll";
    private const string StockSha256 =
        "1030145FEC364ACEB55CEAED221396131DCF02EAAEEB8BD9AD4044BA5596074D";
    private const string PatchedSha256 =
        "8A561682AD6174322C95E70A53EDD2C0AB080A41D826DCB1694A55CFDE53167C";

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void CallbackDelegate();

    // Match the vendor application's delegate exactly. Its CallbackString
    // import is Unicode, but the delegate itself has the runtime defaults.
    private delegate void CallbackStringDelegate(string message);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void CallbackPidDelegate(int firmwarePid, int gamepadPid);

    [DllImport(DllName, EntryPoint = "FindHid")]
    private static extern bool FindHid(ushort vendorId, ushort productId);

    [DllImport(DllName, EntryPoint = "updateGampeWithFile",
        CharSet = CharSet.Unicode)]
    private static extern void UpdateGameWithFile(
        [MarshalAs(UnmanagedType.LPWStr)] string path);

    [DllImport(DllName, EntryPoint = "dll_getProgress")]
    private static extern int GetProgress();

    [DllImport(DllName, EntryPoint = "dll_getType")]
    private static extern uint GetBootType();

    [DllImport(DllName, EntryPoint = "dll_getBootVersion")]
    private static extern float GetBootVersion();

    [DllImport(DllName, EntryPoint = "dll_getBootVersion_beta")]
    private static extern float GetBootVersionBeta();

    [DllImport(DllName, EntryPoint = "usbrr_for_ps_classic")]
    private static extern int SetPsClassicMode(uint enabled);

    [DllImport(DllName, EntryPoint = "usbrr_changeXinputMode")]
    private static extern int SetXinputMode(uint enabled);

    [DllImport(DllName, EntryPoint = "Callback")]
    private static extern void SetCallback(CallbackDelegate callback);

    [DllImport(DllName, EntryPoint = "CallbackString",
        CharSet = CharSet.Unicode)]
    private static extern void SetStringCallback(CallbackStringDelegate callback);

    [DllImport(DllName, EntryPoint = "CallbackChangeErrorPID")]
    private static extern void SetPidCallback(CallbackPidDelegate callback);

    private static CallbackDelegate unsupportedCallback;
    private static CallbackStringDelegate stringCallback;
    private static CallbackPidDelegate pidCallback;

    private static string Sha256(byte[] data)
    {
        using (SHA256 algorithm = SHA256.Create())
        {
            return BitConverter.ToString(algorithm.ComputeHash(data))
                .Replace("-", "");
        }
    }

    private static uint ReadUInt32(byte[] data, int offset)
    {
        return BitConverter.ToUInt32(data, offset);
    }

    private static bool HasSamePortUltimateApplicationHistory(out string location)
    {
        location = null;
        const string usbEnum = @"SYSTEM\CurrentControlSet\Enum\USB\";
        using (RegistryKey bootRoot = Registry.LocalMachine.OpenSubKey(
            usbEnum + "VID_2DC8&PID_3208"))
        using (RegistryKey applicationRoot = Registry.LocalMachine.OpenSubKey(
            usbEnum + "VID_2DC8&PID_6007"))
        {
            if (bootRoot == null || applicationRoot == null)
                return false;

            foreach (string bootName in bootRoot.GetSubKeyNames())
            {
                using (RegistryKey boot = bootRoot.OpenSubKey(bootName))
                {
                    string bootLocation = boot == null
                        ? null
                        : boot.GetValue("LocationInformation") as string;
                    if (String.IsNullOrEmpty(bootLocation))
                        continue;

                    foreach (string applicationName in applicationRoot.GetSubKeyNames())
                    {
                        using (RegistryKey application =
                            applicationRoot.OpenSubKey(applicationName))
                        {
                            string applicationLocation = application == null
                                ? null
                                : application.GetValue("LocationInformation") as string;
                            if (String.Equals(
                                bootLocation, applicationLocation,
                                StringComparison.OrdinalIgnoreCase))
                            {
                                location = bootLocation;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    private static void ValidateImage(string path, byte[] image, string hash)
    {
        if (image.Length != 104988)
            throw new InvalidDataException("Expected a 104,988-byte type-41 image.");
        if (hash != StockSha256 &&
            hash != PatchedSha256)
            throw new InvalidDataException(
                "Refusing an image whose SHA-256 is not an explicitly approved image.");
        if (ReadUInt32(image, 0) != 111 ||
            ReadUInt32(image, 4) != 0x01018000 ||
            ReadUInt32(image, 8) != 0x00019A00 ||
            ReadUInt32(image, 12) != 0x00006007)
            throw new InvalidDataException("Unexpected type-41 firmware header.");
        for (int offset = 16; offset < 28; offset++)
            if (image[offset] != 0)
                throw new InvalidDataException("Unexpected nonzero reserved header byte.");

        Console.WriteLine("Image: " + Path.GetFullPath(path));
        Console.WriteLine("SHA-256: " + hash);
        if (hash == StockSha256)
            Console.WriteLine(
                "Approved image: untouched official 1.11 recovery firmware");
        else if (hash == PatchedSha256)
            Console.WriteLine(
                "Approved image: PicoSwitch2 independent-paddle firmware");
        else
            Console.WriteLine(
                "Approved image: PicoSwitch2 independent-paddle firmware");
    }

    [STAThread]
    public static int Main(string[] args)
    {
        try
        {
            if (args.Length != 2 ||
                (args[1] != "--validate" &&
                 args[1] != "--probe-boot" &&
                 args[1] != "--flash-approved-image"))
            {
                Console.Error.WriteLine(
                    "Usage: UltimateFlashHarness.exe IMAGE.dat --validate");
                Console.Error.WriteLine(
                    "   or: UltimateFlashHarness.exe IMAGE.dat --probe-boot");
                Console.Error.WriteLine(
                    "   or: UltimateFlashHarness.exe IMAGE.dat --flash-approved-image");
                Console.Error.WriteLine(
                    "No device operation was attempted. The explicit gate is required.");
                return 2;
            }

            string path = Path.GetFullPath(args[0]);
            byte[] image = File.ReadAllBytes(path);
            string hash = Sha256(image);
            ValidateImage(path, image, hash);
            if (args[1] == "--validate")
            {
                Console.WriteLine("Validation complete. No device operation was attempted.");
                return 0;
            }

            string dllPath = Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory, DllName);
            if (!File.Exists(dllPath))
                throw new FileNotFoundException(
                    "Place the official updater DLL beside this harness.", dllPath);
            string dataDirectory = Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory, "data");
            if (!Directory.Exists(dataDirectory))
                Directory.CreateDirectory(dataDirectory);

            // Manual boot mode is a separate recovery environment. Refuse the
            // updater's automatic application-to-boot transition so a broken
            // application can always be replaced by the approved stock image.
            if (!FindHid(0x2DC8, 0x3208))
            {
                Console.Error.WriteLine(
                    "Refusing to flash: manual bootloader 2DC8:3208 is not present.");
                return 3;
            }

            // The official updater performs these queries after finding a
            // manual boot device. Besides identifying the selected product,
            // they initialize the native updater's protocol state.
            uint bootType = GetBootType();
            Console.WriteLine("Bootloader firmware type: " + bootType);
            float bootVersion = GetBootVersion();
            Console.WriteLine("Bootloader version: " + bootVersion);
            float bootVersionBeta = GetBootVersionBeta();
            Console.WriteLine("Bootloader beta version: " + bootVersionBeta);
            if (bootType != 41)
            {
                string continuityLocation = null;
                bool manualRecoveryWithoutIdentity =
                    bootType == 0 &&
                    bootVersion == 0.0f &&
                    bootVersionBeta == 0.0f &&
                    HasSamePortUltimateApplicationHistory(
                        out continuityLocation);
                if (!manualRecoveryWithoutIdentity)
                {
                    Console.Error.WriteLine(
                        "Refusing to flash: expected Ultimate Bluetooth type 41.");
                    return 5;
                }
                Console.WriteLine(
                    "Manual-recovery identity fallback: prior Ultimate " +
                    "application PID 6007 matches boot PID 3208 at " +
                    continuityLocation + ".");
            }
            if (args[1] == "--probe-boot")
            {
                Console.WriteLine(
                    "Bootloader probe complete. No firmware write was attempted.");
                return 0;
            }

            // The probe path does not need callbacks. Register them only after
            // the bootloader identity and version queries have succeeded so a
            // callback ABI problem cannot weaken the pre-write gate.
            unsupportedCallback = delegate
            {
                Console.Error.WriteLine("Updater callback: unsupported update path.");
            };
            stringCallback = delegate(string message)
            {
                Console.WriteLine("Updater: " + message);
            };
            pidCallback = delegate(int firmwarePid, int gamepadPid)
            {
                Console.Error.WriteLine(
                    "Updater PID mismatch: firmware=0x" + firmwarePid.ToString("X4") +
                    " device=0x" + gamepadPid.ToString("X4"));
            };
            SetCallback(unsupportedCallback);
            SetStringCallback(stringCallback);
            SetPidCallback(pidCallback);

            // Match UpdateView.startUpdate_new() for a normal controller image.
            SetPsClassicMode(0);
            SetXinputMode(0);
            Console.WriteLine("Manual bootloader found. Starting approved image write.");
            UpdateGameWithFile(path);

            int progress = GetProgress();
            Console.WriteLine("Updater progress: " + progress);
            if (progress != 100)
            {
                Console.Error.WriteLine(
                    "Updater did not report completion; manual recovery remains available.");
                return 4;
            }
            Console.WriteLine("Approved image write completed.");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.ToString());
            return 1;
        }
    }
}
