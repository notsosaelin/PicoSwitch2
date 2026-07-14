/*
 * gcusb -- native Windows USB protocol lab for the NSO GameCube Controller / Pico dongle.
 * See PROMPT.md for the full spec this implements. Built with MinGW-w64 (gcc) against the
 * Windows SetupAPI/WinUSB/HID APIs directly -- no libusb, no driver replacement, no Zadig.
 *
 * This is the "PC-side USB protocol lab" the project owner asked for so hypotheses about the
 * NSO GameCube rumble behavior can be tested by directly driving either the Pico (NSO GameCube
 * personality) or the genuine NSO GameCube Controller over USB from this machine, without a
 * firmware reflash cycle per experiment. The Bluetooth-connected controller still pairs to the
 * Pico as before -- this tool replaces Steam/the console as the USB HOST talking to whichever of
 * the two 057E:2073 devices you point it at.
 *
 * Both the Pico and the genuine controller share VID:PID 057E:2073 -- see gcusb_core.h's own
 * comment for why bcdDevice (0x0111 Pico / 0x0101 genuine) is the required discriminator, and
 * why this tool refuses to guess.
 *
 * Build: tools/gcusb/build.ps1 (invokes gcc directly; see that script for the exact flags).
 */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <usbiodef.h>
#include <devpkey.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "gcusb_core.h"

/* Default WinUSB device-interface class GUID registered by Windows' generic WinUSB co-installer
 * for a device bound via ONLY the MS_COMP_WINUSB compatible ID (no custom "DeviceInterfaceGUIDs"
 * extended property -- switch_gc.c's MS OS descriptor only sends the Compat ID, see its own
 * comment). NOT independently verified against this project's own real hardware yet (no device
 * connected at the time this tool was written) -- if enumeration under this GUID finds nothing,
 * pass --winusb-guid {actual-guid} (read it from Device Manager -> device -> Details ->
 * "Device class guid" or the "WinUsb" driver's own properties) to override. Documented as an
 * open item in the handoff, not silently assumed correct. */
static const GUID k_default_winusb_guid = {
    0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}
};
static GUID g_winusb_guid;

#define GCUSB_VID 0x057E
#define GCUSB_PID 0x2073

/* ------------------------------------------------------------------------------------------- */
/* Global logging + rumble-cleanup state                                                        */
/* ------------------------------------------------------------------------------------------- */

static bool g_ndjson = false;
static FILE *g_log_file = NULL;   /* NULL = stdout only */

/* The single "is a rumble command currently outstanding" flag the Ctrl+C / exit-cleanup path
 * checks. Set just before any nonzero rumble write, cleared immediately after the matching stop
 * completes -- see rumble_scope_enter()/rumble_scope_exit() below. This is deliberately a plain
 * global, not per-thread: this tool is single-threaded by design (one target device per
 * invocation), so there is exactly one "current" rumble scope at a time. */
static volatile bool g_rumble_scope_active = false;
static HANDLE g_active_hid_out = INVALID_HANDLE_VALUE;
static gcusb_target_t g_active_target = GCUSB_TARGET_UNSPECIFIED;

static uint64_t now_us(void) {
    static LARGE_INTEGER freq;
    static bool have_freq = false;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = true; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000LL) / freq.QuadPart);
}

static const char *target_name(gcusb_target_t t) {
    return t == GCUSB_TARGET_PICO ? "pico" : (t == GCUSB_TARGET_GENUINE ? "genuine" : "unspecified");
}

static void hex_encode(const uint8_t *data, uint32_t len, char *out, size_t out_cap) {
    static const char hexch[] = "0123456789abcdef";
    size_t pos = 0;
    for (uint32_t i = 0; i < len && pos + 2 < out_cap; i++) {
        out[pos++] = hexch[data[i] >> 4];
        out[pos++] = hexch[data[i] & 0xF];
    }
    out[pos] = '\0';
}

/* Every transfer this tool performs goes through here -- the single logging chokepoint required
 * by PROMPT.md's "Transport logging" section (monotonic timestamp, target/device/interface,
 * transfer type, direction, setup packet if any, exact request/response bytes, status, elapsed). */
static void log_event(const char *target, const char *iface, const char *xfer_type,
                       const char *direction, const uint8_t *setup, uint32_t setup_len,
                       const uint8_t *req, uint32_t req_len, const uint8_t *resp, uint32_t resp_len,
                       const char *status, uint32_t elapsed_us) {
    char setup_hex[64] = {0}, req_hex[256] = {0}, resp_hex[256] = {0};
    if (setup && setup_len) hex_encode(setup, setup_len, setup_hex, sizeof(setup_hex));
    if (req && req_len) hex_encode(req, req_len, req_hex, sizeof(req_hex));
    if (resp && resp_len) hex_encode(resp, resp_len, resp_hex, sizeof(resp_hex));

    if (g_ndjson) {
        gcusb_log_event_t ev = {0};
        ev.ts_us = now_us();
        ev.target = target;
        ev.iface = iface;
        ev.xfer_type = xfer_type;
        ev.direction = direction;
        ev.setup_hex = (setup && setup_len) ? setup_hex : NULL;
        ev.req_hex = (req && req_len) ? req_hex : NULL;
        ev.resp_hex = (resp && resp_len) ? resp_hex : NULL;
        ev.status = status;
        ev.elapsed_us = elapsed_us;
        char line[1024];
        size_t n = gcusb_format_ndjson_line(&ev, line, sizeof(line));
        if (n > 0) {
            printf("%s\n", line);
            if (g_log_file) fprintf(g_log_file, "%s\n", line);
        }
    } else {
        printf("[%10llu us] %-7s %-11s %-10s %-3s setup=%-16s req=%-40s resp=%-40s %-20s (%u us)\n",
               (unsigned long long)now_us(), target, iface, xfer_type, direction,
               setup_hex[0] ? setup_hex : "-", req_hex[0] ? req_hex : "-",
               resp_hex[0] ? resp_hex : "-", status, elapsed_us);
        if (g_log_file) {
            fprintf(g_log_file, "[%10llu us] %-7s %-11s %-10s %-3s setup=%-16s req=%-40s resp=%-40s %-20s (%u us)\n",
                    (unsigned long long)now_us(), target, iface, xfer_type, direction,
                    setup_hex[0] ? setup_hex : "-", req_hex[0] ? req_hex : "-",
                    resp_hex[0] ? resp_hex : "-", status, elapsed_us);
        }
    }
    fflush(stdout);
    if (g_log_file) fflush(g_log_file);
}

/* ------------------------------------------------------------------------------------------- */
/* Rumble stop -- MUST be reachable and correct from every exit path (normal, error, Ctrl+C,     */
/* timeout, device removal). This is the single function every cleanup path calls.              */
/* ------------------------------------------------------------------------------------------- */

static bool hid_write_report(HANDLE hid_out, uint8_t report_id, const uint8_t *data, uint32_t data_len);
static bool hid_write_zlp(HANDLE hid_out);

static void force_stop_rumble(void) {
    if (g_active_hid_out == INVALID_HANDLE_VALUE || g_active_hid_out == NULL) return;
    uint8_t stop[4];
    gcusb_build_rumble_stop_data(stop);
    uint64_t t0 = now_us();
    bool ok_data = hid_write_report(g_active_hid_out, 0x03, stop, 4);
    log_event(target_name(g_active_target), "hid-out", "interrupt", "out", NULL, 0,
              (const uint8_t[]){0x03, 0, 0, 0, 0}, 5, NULL, 0,
              ok_data ? "ok" : "error", (uint32_t)(now_us() - t0));
    t0 = now_us();
    bool ok_zlp = hid_write_zlp(g_active_hid_out);
    log_event(target_name(g_active_target), "hid-out", "interrupt", "out", NULL, 0, NULL, 0, NULL, 0,
              ok_zlp ? "ok(zlp)" : "error(zlp)", (uint32_t)(now_us() - t0));
    g_rumble_scope_active = false;
}

/* Ctrl+C / Ctrl+Break / console-close handler -- PROMPT.md's explicit requirement that a failed
 * experiment can never leave a motor buzzing indefinitely. Registered once at startup. */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    if (g_rumble_scope_active) {
        fprintf(stderr, "\n[gcusb] Interrupted -- forcing rumble stop before exit.\n");
        force_stop_rumble();
    }
    return FALSE;  /* let the default handler terminate the process after we return */
}

/* ------------------------------------------------------------------------------------------- */
/* Device discovery                                                                              */
/* ------------------------------------------------------------------------------------------- */

typedef struct {
    bool found;
    gcusb_target_t target;
    uint16_t bcddevice;
    WCHAR instance_path[512];
    GUID container_id;
    bool have_container_id;
    DEVINST parent_devinst;   /* SP_DEVINFO_DATA.DevInst from the USB_DEVICE enumeration pass --
                                 lets us walk this specific device's children (cfgmgr32) to find
                                 its real, per-device-registered WinUSB interface GUID, rather
                                 than guessing a single hardcoded class GUID for every device. */
    WCHAR hid_device_path[512];
    WCHAR winusb_device_path[512];
    WCHAR product_string[128];
    WCHAR manufacturer_string[128];
    WCHAR serial_string[128];
    bool have_hid;
    bool have_winusb;
} gcusb_device_info_t;

static void format_guid(const GUID *g, char *out, size_t out_cap) {
    if (!g) { snprintf(out, out_cap, "(none)"); return; }
    snprintf(out, out_cap, "{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
             g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static bool parse_guid_ansi(const char *s, GUID *out) {
    unsigned d1; unsigned d2, d3; unsigned b[8];
    if (sscanf(s, "{%x-%x-%x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
               &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    out->Data1 = d1; out->Data2 = (unsigned short)d2; out->Data3 = (unsigned short)d3;
    for (int k = 0; k < 8; k++) out->Data4[k] = (unsigned char)b[k];
    return true;
}

/* Reads a device's real, per-instance registered device-interface GUID directly from its own
 * "Device Parameters\\DeviceInterfaceGUID" registry value, instead of guessing a fixed class
 * GUID -- see k_default_winusb_guid's own comment for why this project's MS OS descriptor (only
 * a compatible ID, no custom extended-property GUID) means Windows' WinUSB co-installer assigns
 * a GUID this tool cannot predict in advance. Confirmed necessary 2026-07-14 by real hardware
 * testing: the hardcoded default guess did not match either device. `child_hwid_substr` selects
 * which child devnode to inspect (e.g. "MI_01" for this project's vendor/WinUSB interface,
 * interface 1 of the composite device -- interface 0 is HID). */
static bool find_registered_interface_guid(DEVINST parent, const char *child_hwid_substr, GUID *out_guid) {
    DEVINST child;
    if (CM_Get_Child(&child, parent, 0) != CR_SUCCESS) return false;
    WCHAR needle[32];
    MultiByteToWideChar(CP_ACP, 0, child_hwid_substr, -1, needle, 32);
    do {
        WCHAR hwids[1024] = {0};
        ULONG sz = sizeof(hwids);
        if (CM_Get_DevNode_Registry_PropertyW(child, CM_DRP_HARDWAREID, NULL, hwids, &sz, 0) != CR_SUCCESS)
            continue;
        bool matches = false;
        for (WCHAR *p = hwids; *p; p += wcslen(p) + 1) {
            if (wcsstr(p, needle)) { matches = true; break; }
        }
        if (!matches) continue;

        WCHAR instance_id[512];
        if (CM_Get_Device_IDW(child, instance_id, 512, 0) != CR_SUCCESS) continue;

        HDEVINFO hdi = SetupDiCreateDeviceInfoList(NULL, NULL);
        if (hdi == INVALID_HANDLE_VALUE) continue;
        SP_DEVINFO_DATA di = {0};
        di.cbSize = sizeof(di);
        if (SetupDiOpenDeviceInfoW(hdi, instance_id, NULL, 0, &di)) {
            HKEY hkey = SetupDiOpenDevRegKey(hdi, &di, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (hkey != INVALID_HANDLE_VALUE) {
                /* Confirmed 2026-07-14 against this project's own real hardware (via
                 * `Get-ItemProperty HKLM:\...\Device Parameters`): Windows' inbox WinUSB
                 * co-installer stores the per-device interface GUID under the SINGULAR value
                 * name "DeviceInterfaceGUID", not the plural "DeviceInterfaceGUIDs" this function
                 * originally queried (a reasonable-sounding guess that turned out wrong -- the
                 * ClassGuid device property, `{88bae032-...}`, is a DIFFERENT GUID entirely: the
                 * device CLASS, not the device INTERFACE `SetupDiEnumDeviceInterfaces` needs).
                 * Try both names for robustness across driver versions. */
                WCHAR guids[256] = {0};
                DWORD gsz = sizeof(guids), type = 0;
                bool got = RegQueryValueExW(hkey, L"DeviceInterfaceGUID", NULL, &type,
                                           (BYTE *)guids, &gsz) == ERROR_SUCCESS;
                if (!got) {
                    gsz = sizeof(guids);
                    got = RegQueryValueExW(hkey, L"DeviceInterfaceGUIDs", NULL, &type,
                                          (BYTE *)guids, &gsz) == ERROR_SUCCESS;
                }
                if (got) {
                    char guids_a[256];
                    WideCharToMultiByte(CP_ACP, 0, guids, -1, guids_a, sizeof(guids_a), NULL, NULL);
                    if (parse_guid_ansi(guids_a, out_guid)) {
                        RegCloseKey(hkey);
                        SetupDiDestroyDeviceInfoList(hdi);
                        return true;
                    }
                }
                RegCloseKey(hkey);
            }
        }
        SetupDiDestroyDeviceInfoList(hdi);
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
    return false;
}

/* Enumerates the parent composite USB device node (GUID_DEVINTERFACE_USB_DEVICE), matching
 * VID_057E&PID_2073, and reads its bcdDevice straight from the hardware ID's REV_ field (pure
 * string parsing, gcusb_core.c's gcusb_parse_bcddevice_from_hwid -- see that function's own
 * comment). Then separately enumerates HID (GUID_DEVINTERFACE_HID) and WinUSB
 * (g_winusb_guid) device interfaces and matches them to the same VID/PID via HidD_GetAttributes
 * / the WinUSB device path, associating by container ID where available. Fills `out_devices`
 * (caller-supplied array of `max_devices`) and returns the count found (0..max_devices). */
static int enumerate_devices(gcusb_device_info_t *out_devices, int max_devices) {
    int count = 0;
    HDEVINFO hdi = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdi == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA ifdata = {0};
    ifdata.cbSize = sizeof(ifdata);
    for (DWORD i = 0; count < max_devices &&
         SetupDiEnumDeviceInterfaces(hdi, NULL, &GUID_DEVINTERFACE_USB_DEVICE, i, &ifdata); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(hdi, &ifdata, NULL, 0, &needed, NULL);
        if (needed == 0) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devinfo = {0};
        devinfo.cbSize = sizeof(devinfo);
        if (!SetupDiGetDeviceInterfaceDetailW(hdi, &ifdata, detail, needed, NULL, &devinfo)) {
            free(detail);
            continue;
        }

        WCHAR hwid[512] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(hdi, &devinfo, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)hwid, sizeof(hwid), NULL)) {
            free(detail);
            continue;
        }
        char hwid_a[512];
        WideCharToMultiByte(CP_ACP, 0, hwid, -1, hwid_a, sizeof(hwid_a), NULL, NULL);
        if (!strstr(hwid_a, "VID_057E") || !strstr(hwid_a, "PID_2073")) {
            free(detail);
            continue;
        }

        gcusb_device_info_t *dev = &out_devices[count];
        memset(dev, 0, sizeof(*dev));
        dev->found = true;
        wcsncpy(dev->instance_path, detail->DevicePath, sizeof(dev->instance_path) / sizeof(WCHAR) - 1);
        dev->parent_devinst = devinfo.DevInst;

        uint16_t bcd = 0;
        if (gcusb_parse_bcddevice_from_hwid(hwid_a, &bcd)) {
            dev->bcddevice = bcd;
            if (bcd == GCUSB_BCDDEVICE_PICO) dev->target = GCUSB_TARGET_PICO;
            else if (bcd == GCUSB_BCDDEVICE_GENUINE) dev->target = GCUSB_TARGET_GENUINE;
        }

        DEVPROPTYPE ptype = 0;
        if (SetupDiGetDevicePropertyW(hdi, &devinfo, &DEVPKEY_Device_ContainerId, &ptype,
                                      (PBYTE)&dev->container_id, sizeof(GUID), NULL, 0) &&
            ptype == DEVPROP_TYPE_GUID) {
            dev->have_container_id = true;
        }

        count++;
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(hdi);

    /* Second pass: find the HID child interface for each discovered device and read bcdDevice
     * directly from HIDD_ATTRIBUTES.VersionNumber (authoritative, no string parsing needed) --
     * also cross-checks the parent-hardware-ID-derived value above. Matched by container ID. */
    HDEVINFO hid_hdi = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hid_hdi != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA hid_ifdata = {0};
        hid_ifdata.cbSize = sizeof(hid_ifdata);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hid_hdi, NULL, &GUID_DEVINTERFACE_HID, i,
                                                       &hid_ifdata); i++) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(hid_hdi, &hid_ifdata, NULL, 0, &needed, NULL);
            if (needed == 0) continue;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
                (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
            if (!detail) continue;
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devinfo = {0};
            devinfo.cbSize = sizeof(devinfo);
            if (!SetupDiGetDeviceInterfaceDetailW(hid_hdi, &hid_ifdata, detail, needed, NULL, &devinfo)) {
                free(detail);
                continue;
            }

            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr = {0};
                attr.Size = sizeof(attr);
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == GCUSB_VID &&
                    attr.ProductID == GCUSB_PID) {
                    GUID cid = {0};
                    DEVPROPTYPE ptype = 0;
                    bool have_cid = SetupDiGetDevicePropertyW(hid_hdi, &devinfo, &DEVPKEY_Device_ContainerId,
                                                              &ptype, (PBYTE)&cid, sizeof(GUID), NULL, 0) &&
                                    ptype == DEVPROP_TYPE_GUID;
                    for (int j = 0; j < count; j++) {
                        if (have_cid && out_devices[j].have_container_id &&
                            memcmp(&out_devices[j].container_id, &cid, sizeof(GUID)) == 0) {
                            out_devices[j].have_hid = true;
                            wcsncpy(out_devices[j].hid_device_path, detail->DevicePath,
                                    sizeof(out_devices[j].hid_device_path) / sizeof(WCHAR) - 1);
                            /* HIDD_ATTRIBUTES.VersionNumber IS bcdDevice -- authoritative,
                             * cross-checking the hardware-ID-string-derived value above. */
                            out_devices[j].bcddevice = attr.VersionNumber;
                            HidD_GetProductString(h, out_devices[j].product_string,
                                                  sizeof(out_devices[j].product_string));
                            HidD_GetManufacturerString(h, out_devices[j].manufacturer_string,
                                                       sizeof(out_devices[j].manufacturer_string));
                            HidD_GetSerialNumberString(h, out_devices[j].serial_string,
                                                       sizeof(out_devices[j].serial_string));
                            break;
                        }
                    }
                }
                CloseHandle(h);
            }
            free(detail);
        }
        SetupDiDestroyDeviceInfoList(hid_hdi);
    }

    /* Third pass: find the WinUSB (vendor, interface 1 = "MI_01") child interface. For each
     * discovered device, first try to read its OWN real registered device-interface GUID
     * directly from the registry (find_registered_interface_guid() -- see its own comment for
     * why the hardcoded g_winusb_guid default cannot be relied on for this project's MS OS
     * descriptor, which only sends a compatible ID, no custom extended-property GUID). Falls
     * back to g_winusb_guid (the CLI-overridable default) only if that per-device lookup fails. */
    for (int j = 0; j < count; j++) {
        GUID guid_to_try = g_winusb_guid;
        bool discovered = find_registered_interface_guid(out_devices[j].parent_devinst, "MI_01", &guid_to_try);

        HDEVINFO wu_hdi = SetupDiGetClassDevsW(&guid_to_try, NULL, NULL,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (wu_hdi == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA wu_ifdata = {0};
        wu_ifdata.cbSize = sizeof(wu_ifdata);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(wu_hdi, NULL, &guid_to_try, i, &wu_ifdata); i++) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(wu_hdi, &wu_ifdata, NULL, 0, &needed, NULL);
            if (needed == 0) continue;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
                (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
            if (!detail) continue;
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devinfo = {0};
            devinfo.cbSize = sizeof(devinfo);
            if (SetupDiGetDeviceInterfaceDetailW(wu_hdi, &wu_ifdata, detail, needed, NULL, &devinfo)) {
                GUID cid = {0};
                DEVPROPTYPE ptype = 0;
                bool have_cid = SetupDiGetDevicePropertyW(wu_hdi, &devinfo, &DEVPKEY_Device_ContainerId,
                                                          &ptype, (PBYTE)&cid, sizeof(GUID), NULL, 0) &&
                                ptype == DEVPROP_TYPE_GUID;
                /* When the GUID was discovered per-device (not the shared default), it's already
                 * scoped to this exact device instance's own registration, so container-ID
                 * matching is a redundant-but-harmless extra check rather than the only check. */
                if (have_cid && out_devices[j].have_container_id &&
                    memcmp(&out_devices[j].container_id, &cid, sizeof(GUID)) == 0) {
                    out_devices[j].have_winusb = true;
                    wcsncpy(out_devices[j].winusb_device_path, detail->DevicePath,
                            sizeof(out_devices[j].winusb_device_path) / sizeof(WCHAR) - 1);
                } else if (discovered && !out_devices[j].have_winusb) {
                    /* Per-device-discovered GUID with no comparable container ID on either side --
                     * still trust it (it came from walking THIS device's own children), rather
                     * than discard a real find over a missing cross-check. */
                    out_devices[j].have_winusb = true;
                    wcsncpy(out_devices[j].winusb_device_path, detail->DevicePath,
                            sizeof(out_devices[j].winusb_device_path) / sizeof(WCHAR) - 1);
                }
            }
            free(detail);
        }
        SetupDiDestroyDeviceInfoList(wu_hdi);
    }

    return count;
}

static void print_device_info(const gcusb_device_info_t *d, bool ndjson) {
    char product_a[128] = {0}, mfg_a[128] = {0}, serial_a[128] = {0}, container_a[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, d->product_string, -1, product_a, sizeof(product_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, d->manufacturer_string, -1, mfg_a, sizeof(mfg_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, d->serial_string, -1, serial_a, sizeof(serial_a), NULL, NULL);
    format_guid(d->have_container_id ? &d->container_id : NULL, container_a, sizeof(container_a));
    char instance_a[512] = {0};
    WideCharToMultiByte(CP_ACP, 0, d->instance_path, -1, instance_a, sizeof(instance_a), NULL, NULL);

    if (ndjson) {
        printf("{\"instance_path\":\"%s\",\"container_id\":\"%s\",\"vid\":\"057E\",\"pid\":\"2073\","
               "\"bcddevice\":\"0x%04X\",\"resolved_target\":\"%s\",\"product\":\"%s\","
               "\"manufacturer\":\"%s\",\"serial\":\"%s\",\"have_hid\":%s,\"have_winusb\":%s}\n",
               instance_a, container_a, d->bcddevice, target_name(d->target), product_a, mfg_a,
               serial_a, d->have_hid ? "true" : "false", d->have_winusb ? "true" : "false");
    } else {
        printf("Device instance path : %s\n", instance_a);
        printf("Container ID         : %s\n", container_a);
        printf("VID:PID              : 057E:2073\n");
        printf("bcdDevice            : 0x%04X  (resolved target: %s)\n", d->bcddevice, target_name(d->target));
        printf("Product string       : %s\n", product_a[0] ? product_a : "(unavailable)");
        printf("Manufacturer string  : %s\n", mfg_a[0] ? mfg_a : "(unavailable)");
        printf("Serial string        : %s\n", serial_a[0] ? serial_a : "(unavailable)");
        printf("HID interface found  : %s\n", d->have_hid ? "yes" : "no");
        printf("WinUSB interface found: %s%s\n", d->have_winusb ? "yes" : "no",
               d->have_winusb ? "" : "  (see g_default_winusb_guid's comment -- pass --winusb-guid to override)");
        printf("\n");
    }
}

/* The one safety gate every mutating command must pass through: resolve the requested target to
 * an ACTUAL connected device, refuse on any mismatch or ambiguity, never guess. */
static bool resolve_target(gcusb_target_t target, gcusb_device_info_t *out) {
    if (target == GCUSB_TARGET_UNSPECIFIED) {
        fprintf(stderr, "[gcusb] ERROR: --target pico|genuine is required. Refusing to guess.\n");
        return false;
    }
    gcusb_device_info_t devices[8];
    int n = enumerate_devices(devices, 8);
    gcusb_device_info_t *match = NULL;
    for (int i = 0; i < n; i++) {
        if (devices[i].target == target) {
            if (match) {
                fprintf(stderr, "[gcusb] ERROR: multiple devices resolved to target '%s' -- "
                                "refusing to pick one. Run 'gcusb list' and disambiguate.\n",
                        target_name(target));
                return false;
            }
            match = &devices[i];
        }
    }
    if (!match) {
        fprintf(stderr, "[gcusb] ERROR: no connected device matches --target %s "
                        "(VID:PID 057E:2073, bcdDevice 0x%04X). Run 'gcusb list' to see what's "
                        "actually connected.\n",
                target_name(target), gcusb_expected_bcddevice(target));
        return false;
    }
    if (!gcusb_bcddevice_matches(target, match->bcddevice)) {
        /* Should be unreachable given how `match` was selected above, but this is the exact
         * safety invariant PROMPT.md calls out explicitly -- check it again, structurally,
         * rather than trusting the selection logic implicitly. */
        fprintf(stderr, "[gcusb] ERROR: resolved device's bcdDevice 0x%04X does not match target "
                        "'%s''s expected 0x%04X. Refusing.\n",
                match->bcddevice, target_name(target), gcusb_expected_bcddevice(target));
        return false;
    }
    *out = *match;
    return true;
}

/* ------------------------------------------------------------------------------------------- */
/* Transfer primitives                                                                          */
/* ------------------------------------------------------------------------------------------- */

static bool open_winusb(const gcusb_device_info_t *dev, HANDLE *out_file, WINUSB_INTERFACE_HANDLE *out_winusb) {
    if (!dev->have_winusb) {
        fprintf(stderr, "[gcusb] ERROR: no WinUSB interface known for this device. See "
                        "k_default_winusb_guid's comment in gcusb_win.c -- pass --winusb-guid.\n");
        return false;
    }
    HANDLE f = CreateFileW(dev->winusb_device_path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    WINUSB_INTERFACE_HANDLE wu;
    if (!WinUsb_Initialize(f, &wu)) {
        CloseHandle(f);
        return false;
    }
    *out_file = f;
    *out_winusb = wu;
    return true;
}

static bool open_hid_out(const gcusb_device_info_t *dev, HANDLE *out_hid) {
    if (!dev->have_hid) return false;
    HANDLE h = CreateFileW(dev->hid_device_path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    *out_hid = h;
    return true;
}

/* Vendor bulk OUT 0x02 write + IN 0x82 read-response, matching switch_gc_vendor_dispatch()'s own
 * request/response shape. Logs both directions. Returns response length (0 on failure/timeout). */
static uint32_t vendor_bulk_command(const char *target, WINUSB_INTERFACE_HANDLE wu,
                                    const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t resp_cap, DWORD timeout_ms) {
    uint64_t t0 = now_us();
    ULONG written = 0;
    WinUsb_SetPipePolicy(wu, 0x02, PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms);
    BOOL ok = WinUsb_WritePipe(wu, 0x02, (PUCHAR)req, req_len, &written, NULL);
    log_event(target, "vendor-bulk", "bulk", "out", NULL, 0, req, req_len, NULL, 0,
              ok ? "ok" : "error", (uint32_t)(now_us() - t0));
    if (!ok) return 0;

    t0 = now_us();
    ULONG got = 0;
    WinUsb_SetPipePolicy(wu, 0x82, PIPE_TRANSFER_TIMEOUT, sizeof(timeout_ms), &timeout_ms);
    ok = WinUsb_ReadPipe(wu, 0x82, resp, resp_cap, &got, NULL);
    log_event(target, "vendor-bulk", "bulk", "in", NULL, 0, NULL, 0, resp, ok ? got : 0,
              ok ? "ok" : "error", (uint32_t)(now_us() - t0));
    return ok ? (uint32_t)got : 0;
}

static bool hid_write_report(HANDLE hid_out, uint8_t report_id, const uint8_t *data, uint32_t data_len) {
    uint8_t buf[64] = {0};
    buf[0] = report_id;
    if (data && data_len) memcpy(&buf[1], data, data_len > 63 ? 63 : data_len);
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(hid_out, buf, sizeof(buf), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(hid_out, &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
    return ok != 0;
}

static bool hid_write_zlp(HANDLE hid_out) {
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(hid_out, NULL, 0, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(hid_out, &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
    return ok != 0;
}

static uint32_t hid_read_report(HANDLE hid_in, uint8_t *buf, uint32_t buf_cap, DWORD timeout_ms) {
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    BOOL ok = ReadFile(hid_in, buf, buf_cap, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hid_in, &ov, &got, FALSE);
        } else {
            CancelIo(hid_in);
            got = 0;
        }
    }
    CloseHandle(ov.hEvent);
    return got;
}

/* ------------------------------------------------------------------------------------------- */
/* Rumble scope guard -- wraps every rumble-capable operation so a stop is always attempted      */
/* ------------------------------------------------------------------------------------------- */

static void rumble_scope_enter(gcusb_target_t target, HANDLE hid_out) {
    g_active_target = target;
    g_active_hid_out = hid_out;
    g_rumble_scope_active = true;
}

static void rumble_scope_exit(void) {
    force_stop_rumble();
}

/* ------------------------------------------------------------------------------------------- */
/* Subcommands                                                                                   */
/* ------------------------------------------------------------------------------------------- */

static int cmd_list(bool ndjson) {
    gcusb_device_info_t devices[8];
    int n = enumerate_devices(devices, 8);
    if (n == 0) {
        fprintf(stderr, "[gcusb] No 057E:2073 devices found.\n");
        return 1;
    }
    for (int i = 0; i < n; i++) print_device_info(&devices[i], ndjson);
    return 0;
}

static int cmd_describe(gcusb_target_t target, bool ndjson) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    print_device_info(&dev, ndjson);
    return 0;
}

/* Sends the minimum safe init sequence: 0x03/0x0D (Initialise USB), 0x03/0x0A (Select Input
 * Report, value 0x0A for --profile console-capture-shaped tests, 0x05 for --profile steam,
 * matching Steam's own observed behavior -- docs/experiments/gc-stage-d-steam-diagnosis-2026-07-13.md).
 * Deliberately does NOT send 0x15 (pairing) unless --profile console-capture is explicit, and
 * NEVER sends any HID OUT rumble report -- this is Experiment 1's exact "no host rumble writes"
 * requirement. */
static int cmd_init(gcusb_target_t target, const char *profile) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    HANDLE f;
    WINUSB_INTERFACE_HANDLE wu;
    if (!open_winusb(&dev, &f, &wu)) {
        fprintf(stderr, "[gcusb] ERROR: could not open WinUSB interface.\n");
        return 1;
    }
    const char *tn = target_name(target);
    uint8_t resp[64];

    uint8_t init_usb[] = {0x03, 0x91, 0x00, 0x0D, 0x00, 0x08, 0x00, 0x00,
                          0x01, 0x00, 0xF3, 0xB9, 0x34, 0x8C, 0x81, 0x78};
    vendor_bulk_command(tn, wu, init_usb, sizeof(init_usb), resp, sizeof(resp), 1000);

    uint8_t report_id = (profile && strcmp(profile, "console-capture") == 0) ? 0x0A : 0x05;
    uint8_t select_report[] = {0x03, 0x91, 0x00, 0x0A, 0x00, 0x04, 0x00, 0x00, report_id, 0, 0, 0};
    vendor_bulk_command(tn, wu, select_report, sizeof(select_report), resp, sizeof(resp), 1000);

    printf("[gcusb] init complete for target=%s profile=%s (report_id=0x%02X). No rumble reports "
           "were sent -- this satisfies Experiment 1's baseline.\n",
           tn, profile ? profile : "(default)", report_id);

    WinUsb_Free(wu);
    CloseHandle(f);
    return 0;
}

static int cmd_read_input(gcusb_target_t target, int count, int duration_s) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    HANDLE hid_in;
    if (!open_hid_out(&dev, &hid_in)) {
        fprintf(stderr, "[gcusb] ERROR: could not open HID interface.\n");
        return 1;
    }
    const char *tn = target_name(target);
    uint64_t deadline = duration_s > 0 ? now_us() + (uint64_t)duration_s * 1000000ull : 0;
    int seen = 0;
    while ((count <= 0 || seen < count) && (deadline == 0 || now_us() < deadline)) {
        uint8_t buf[64];
        uint64_t t0 = now_us();
        uint32_t got = hid_read_report(hid_in, buf, sizeof(buf), 500);
        if (got > 0) {
            log_event(tn, "hid-in", "interrupt", "in", NULL, 0, NULL, 0, buf, got, "ok",
                      (uint32_t)(now_us() - t0));
            seen++;
        }
    }
    CloseHandle(hid_in);
    return 0;
}

static int cmd_send_command(gcusb_target_t target, const char *hex, bool allow_unsafe) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;

    uint8_t bytes[64];
    uint32_t len = 0;
    for (const char *p = hex; *p && len < sizeof(bytes); ) {
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) break;
        bytes[len++] = (uint8_t)v;
        p += 2;
    }

    gcusb_cmd_class_t cls = gcusb_classify_command(bytes, len);
    if (cls == GCUSB_CMD_REJECTED && !allow_unsafe) {
        fprintf(stderr, "[gcusb] REFUSED: '%s' is not in the default allowlist. Use --unsafe to "
                        "override (not recommended without independent evidence it's safe).\n",
                gcusb_command_name(bytes, len));
        return 1;
    }
    if (cls == GCUSB_CMD_REQUIRES_CONSOLE_CAPTURE_PROFILE && !allow_unsafe) {
        fprintf(stderr, "[gcusb] REFUSED: '%s' requires --profile console-capture (real pairing "
                        "crypto state) or --unsafe.\n", gcusb_command_name(bytes, len));
        return 1;
    }

    HANDLE f;
    WINUSB_INTERFACE_HANDLE wu;
    if (!open_winusb(&dev, &f, &wu)) {
        fprintf(stderr, "[gcusb] ERROR: could not open WinUSB interface.\n");
        return 1;
    }
    uint8_t resp[64];
    vendor_bulk_command(target_name(target), wu, bytes, len, resp, sizeof(resp), 1000);
    WinUsb_Free(wu);
    CloseHandle(f);
    return 0;
}

static int cmd_stop_rumble(gcusb_target_t target) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    HANDLE hid_out;
    if (!open_hid_out(&dev, &hid_out)) {
        fprintf(stderr, "[gcusb] ERROR: could not open HID interface.\n");
        return 1;
    }
    g_active_target = target;
    g_active_hid_out = hid_out;
    force_stop_rumble();
    CloseHandle(hid_out);
    printf("[gcusb] stop-rumble: sent both candidate stop mechanisms (zero-data report 0x03, "
          "and a zero-length interrupt OUT) to target=%s.\n", target_name(target));
    return 0;
}

static int cmd_rumble(gcusb_target_t target, uint8_t amplitude, uint32_t duration_ms,
                      bool allow_unsafe, bool confirmed_genuine) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    if (target == GCUSB_TARGET_GENUINE && !confirmed_genuine) {
        fprintf(stderr, "[gcusb] REFUSED: this would drive the GENUINE controller's physical "
                        "motor. Re-run with --confirm-genuine-motor to proceed.\n");
        return 1;
    }
    HANDLE hid_out;
    if (!open_hid_out(&dev, &hid_out)) {
        fprintf(stderr, "[gcusb] ERROR: could not open HID interface.\n");
        return 1;
    }

    // `amplitude` here is legacy CLI naming (kept for continuity) -- per the corrected,
    // kernel-sourced protocol model (2026-07-14, see gc_rumble_state_t in switch_gc.c), the wire
    // format only has a 3-state ON/OFF/STOP field, not a real amplitude. Any nonzero value here
    // just means "ON"; gcusb_build_rumble_data() does the actual OFF/ON mapping.
    uint8_t amp = gcusb_clamp_rumble_amplitude(amplitude, allow_unsafe);
    uint32_t dur = gcusb_clamp_rumble_duration_ms(duration_ms, allow_unsafe);
    if (amp != amplitude || dur != duration_ms) {
        printf("[gcusb] Safety clamp applied: amplitude 0x%02X->0x%02X, duration %ums->%ums "
              "(use --unsafe to bypass).\n", amplitude, amp, duration_ms, dur);
    }

    rumble_scope_enter(target, hid_out);
    uint8_t data[4];
    gcusb_build_rumble_data(amp, data);
    uint64_t t0 = now_us();
    bool ok = hid_write_report(hid_out, 0x03, data, 4);
    log_event(target_name(target), "hid-out", "interrupt", "out", NULL, 0,
              (const uint8_t[]){0x03, data[0], data[1], data[2], data[3]}, 5, NULL, 0,
              ok ? "ok" : "error", (uint32_t)(now_us() - t0));
    Sleep(dur);
    rumble_scope_exit();
    CloseHandle(hid_out);
    printf("[gcusb] rumble pulse complete: state=%s duration=%ums, then stopped.\n",
           amp != 0 ? "ON" : "OFF", dur);
    return 0;
}

// Repurposed 2026-07-14: this personality has no continuous amplitude to sweep (see
// gc_rumble_data's own comment) -- the real open question now is toggling cadence, since the
// genuine protocol simulates intensity by how fast/often the host alternates ON/OFF (delta-sigma
// duty-cycle modulation, per the kernel source), and this project's own downstream Xbox-bridge
// envelope (bthid_gamepad.c's pulse_sustain_10ms) needs toggling faster than its hold time to
// read as a texture rather than a smear. Sends a bounded ON/OFF/ON/OFF sequence at progressively
// shorter intervals so the owner can observe where (if anywhere) rapid toggling stops feeling
// like distinct pulses.
static int cmd_rumble_sweep(gcusb_target_t target, bool allow_unsafe, bool confirmed_genuine) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    if (target == GCUSB_TARGET_GENUINE && !confirmed_genuine) {
        fprintf(stderr, "[gcusb] REFUSED: this would drive the GENUINE controller's physical "
                        "motor. Re-run with --confirm-genuine-motor to proceed.\n");
        return 1;
    }
    HANDLE hid_out;
    if (!open_hid_out(&dev, &hid_out)) {
        fprintf(stderr, "[gcusb] ERROR: could not open HID interface.\n");
        return 1;
    }
    // Each entry is an ON-then-OFF interval in ms, progressively shorter -- bounded, safe (all
    // well under GCUSB_RUMBLE_SWEEP_MAX_STEP_MS), never starting at the fastest/most aggressive.
    static const uint32_t k_intervals_ms[] = {200, 100, 50};
    rumble_scope_enter(target, hid_out);
    for (size_t i = 0; i < sizeof(k_intervals_ms) / sizeof(k_intervals_ms[0]); i++) {
        uint32_t interval = k_intervals_ms[i];
        printf("[gcusb] toggle step %zu/%zu: ON/OFF every %ums -- observe motor texture now.\n",
               i + 1, sizeof(k_intervals_ms) / sizeof(k_intervals_ms[0]), interval);
        for (int rep = 0; rep < 3; rep++) {
            uint8_t data[4];
            gcusb_build_rumble_data(1, data);  // ON
            uint64_t t0 = now_us();
            bool ok = hid_write_report(hid_out, 0x03, data, 4);
            log_event(target_name(target), "hid-out", "interrupt", "out", NULL, 0,
                      (const uint8_t[]){0x03, data[0], data[1], data[2], data[3]}, 5, NULL, 0,
                      ok ? "ok" : "error", (uint32_t)(now_us() - t0));
            Sleep(interval);
            gcusb_build_rumble_data(0, data);  // OFF
            t0 = now_us();
            ok = hid_write_report(hid_out, 0x03, data, 4);
            log_event(target_name(target), "hid-out", "interrupt", "out", NULL, 0,
                      (const uint8_t[]){0x03, data[0], data[1], data[2], data[3]}, 5, NULL, 0,
                      ok ? "ok" : "error", (uint32_t)(now_us() - t0));
            Sleep(interval);
        }
    }
    rumble_scope_exit();
    CloseHandle(hid_out);
    printf("[gcusb] rumble-sweep (toggle-cadence test) complete, motor stopped.\n");
    return 0;
}

/* Shared engine for replay/compare: runs a script file's SEND/SLEEP/STOP-RUMBLE lines against
 * one target, honoring the allowlist and rumble scope guarantees. */
/* Hardware-free script validation: parses every line and reports syntax/allowlist status without
 * opening any device at all -- PROMPT.md's explicit "replay script validation without touching
 * hardware" requirement. Returns 0 if every SEND line is syntactically valid, and (unless
 * `allow_unsafe`) every SEND line is allowlisted; nonzero otherwise. Safe to run with zero
 * devices connected, which is exactly the point. */
static int validate_script_file(const char *script_path, bool allow_unsafe) {
    FILE *f = fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "[gcusb] ERROR: cannot open script '%s'.\n", script_path);
        return 1;
    }
    char line[256];
    int line_no = 0, problems = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        gcusb_script_line_t parsed;
        gcusb_parse_script_line(line, strlen(line), &parsed);
        switch (parsed.kind) {
            case GCUSB_SCRIPT_LINE_BLANK:
            case GCUSB_SCRIPT_LINE_SLEEP:
            case GCUSB_SCRIPT_LINE_STOP_RUMBLE:
                printf("[gcusb] line %3d: OK\n", line_no);
                break;
            case GCUSB_SCRIPT_LINE_SEND:
                if (parsed.allowlisted || allow_unsafe) {
                    printf("[gcusb] line %3d: OK   SEND %s\n", line_no,
                           gcusb_command_name(parsed.bytes, parsed.byte_count));
                } else {
                    printf("[gcusb] line %3d: FAIL SEND %s -- not in default allowlist "
                           "(class=%d)\n", line_no, gcusb_command_name(parsed.bytes, parsed.byte_count),
                           (int)parsed.cmd_class);
                    problems++;
                }
                break;
            case GCUSB_SCRIPT_LINE_INVALID:
                printf("[gcusb] line %3d: FAIL unparseable: %s", line_no, line);
                problems++;
                break;
        }
    }
    fclose(f);
    printf("[gcusb] validate-only: %d problem(s) found in '%s'.\n", problems, script_path);
    return problems == 0 ? 0 : 1;
}

static int run_script_on_target(gcusb_target_t target, const char *script_path, bool allow_unsafe) {
    gcusb_device_info_t dev;
    if (!resolve_target(target, &dev)) return 1;
    FILE *f = fopen(script_path, "r");
    if (!f) {
        fprintf(stderr, "[gcusb] ERROR: cannot open script '%s'.\n", script_path);
        return 1;
    }
    HANDLE wu_file;
    WINUSB_INTERFACE_HANDLE wu;
    if (!open_winusb(&dev, &wu_file, &wu)) {
        fprintf(stderr, "[gcusb] ERROR: could not open WinUSB interface.\n");
        fclose(f);
        return 1;
    }
    HANDLE hid_out = INVALID_HANDLE_VALUE;
    open_hid_out(&dev, &hid_out);  /* optional -- only needed for STOP-RUMBLE lines */
    if (hid_out != INVALID_HANDLE_VALUE) { g_active_target = target; g_active_hid_out = hid_out; }

    char line[256];
    int line_no = 0;
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        gcusb_script_line_t parsed;
        gcusb_parse_script_line(line, strlen(line), &parsed);
        switch (parsed.kind) {
            case GCUSB_SCRIPT_LINE_BLANK:
                break;
            case GCUSB_SCRIPT_LINE_SLEEP:
                Sleep(parsed.sleep_ms);
                break;
            case GCUSB_SCRIPT_LINE_STOP_RUMBLE:
                if (hid_out != INVALID_HANDLE_VALUE) force_stop_rumble();
                break;
            case GCUSB_SCRIPT_LINE_SEND:
                if (!parsed.allowlisted && !allow_unsafe) {
                    fprintf(stderr, "[gcusb] REFUSED at script line %d: '%s' not in the default "
                                    "allowlist (use --unsafe to override).\n",
                            line_no, gcusb_command_name(parsed.bytes, parsed.byte_count));
                    rc = 1;
                } else {
                    uint8_t resp[64];
                    vendor_bulk_command(target_name(target), wu, parsed.bytes, parsed.byte_count,
                                        resp, sizeof(resp), 1000);
                }
                break;
            case GCUSB_SCRIPT_LINE_INVALID:
                fprintf(stderr, "[gcusb] WARNING: script line %d unparseable, skipped: %s", line_no, line);
                break;
        }
        if (rc != 0) break;
    }
    if (hid_out != INVALID_HANDLE_VALUE) { force_stop_rumble(); CloseHandle(hid_out); }
    WinUsb_Free(wu);
    CloseHandle(wu_file);
    fclose(f);
    return rc;
}

static int cmd_replay(gcusb_target_t target, const char *script_path, bool allow_unsafe) {
    return run_script_on_target(target, script_path, allow_unsafe);
}

static int cmd_compare(const char *script_path, bool allow_unsafe) {
    printf("[gcusb] compare: running '%s' against genuine, then pico. Diff the two NDJSON logs "
          "yourself (or redirect each run with --log-file) -- see PROMPT.md Experiment 3.\n",
          script_path);
    int rc1 = run_script_on_target(GCUSB_TARGET_GENUINE, script_path, allow_unsafe);
    int rc2 = run_script_on_target(GCUSB_TARGET_PICO, script_path, allow_unsafe);
    return (rc1 != 0 || rc2 != 0) ? 1 : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* CLI                                                                                           */
/* ------------------------------------------------------------------------------------------- */

static void print_usage(void) {
    printf(
        "gcusb -- native Windows USB protocol lab for the NSO GameCube Controller / Pico dongle\n"
        "\n"
        "  gcusb list [--ndjson]\n"
        "  gcusb describe --target pico|genuine [--ndjson]\n"
        "  gcusb init --target pico|genuine [--profile steam|console-capture]\n"
        "  gcusb read-input --target pico|genuine [--count N] [--duration S]\n"
        "  gcusb send-command --target pico|genuine --hex <bytes> [--unsafe]\n"
        "  gcusb rumble --target pico|genuine --amplitude <0=off, nonzero=on> --duration-ms <N>\n"
        "                [--unsafe] [--confirm-genuine-motor]\n"
        "                (GC rumble is ON/OFF/STOP only, not a real amplitude -- see\n"
        "                 src/switch_gc/switch_gc.c's gc_rumble_state_t comment)\n"
        "  gcusb rumble-sweep --target pico|genuine [--unsafe] [--confirm-genuine-motor]\n"
        "                (toggle-cadence test, not an amplitude sweep -- see cmd_rumble_sweep)\n"
        "  gcusb stop-rumble --target pico|genuine\n"
        "  gcusb replay --target pico|genuine --script <file> [--unsafe]\n"
        "  gcusb replay --script <file> --validate-only [--unsafe]   (no device needed)\n"
        "  gcusb compare --script <file> [--unsafe]\n"
        "\n"
        "Global: --ndjson (machine-readable log lines), --log-file <path> (also write there).\n"
        "Default operation is read-only (list/describe/read-input). All mutating operations "
        "require an explicit --target and refuse on any bcdDevice mismatch.\n");
}

int main(int argc, char **argv) {
    g_winusb_guid = k_default_winusb_guid;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    if (argc < 2) { print_usage(); return 1; }
    const char *cmd = argv[1];

    gcusb_target_t target = GCUSB_TARGET_UNSPECIFIED;
    const char *hex = NULL, *script = NULL, *profile = NULL, *log_file_path = NULL;
    bool validate_only = false;
    int count = 0, duration_s = 0;
    uint32_t amplitude = 0, duration_ms = 0;
    bool unsafe = false, confirm_genuine = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = gcusb_parse_target(argv[++i]);
        else if (strcmp(argv[i], "--ndjson") == 0) g_ndjson = true;
        else if (strcmp(argv[i], "--unsafe") == 0) unsafe = true;
        else if (strcmp(argv[i], "--confirm-genuine-motor") == 0) confirm_genuine = true;
        else if (strcmp(argv[i], "--hex") == 0 && i + 1 < argc) hex = argv[++i];
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) script = argv[++i];
        else if (strcmp(argv[i], "--validate-only") == 0) validate_only = true;
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) profile = argv[++i];
        else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) log_file_path = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) duration_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--amplitude") == 0 && i + 1 < argc) amplitude = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--duration-ms") == 0 && i + 1 < argc) duration_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--winusb-guid") == 0 && i + 1 < argc) {
            /* Escape hatch documented in k_default_winusb_guid's comment. */
            unsigned d1; unsigned short d2, d3; unsigned b[8];
            if (sscanf(argv[++i], "{%x-%hx-%hx-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                       &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) == 11) {
                g_winusb_guid.Data1 = d1; g_winusb_guid.Data2 = d2; g_winusb_guid.Data3 = d3;
                for (int k = 0; k < 8; k++) g_winusb_guid.Data4[k] = (unsigned char)b[k];
            }
        }
    }

    if (log_file_path) g_log_file = fopen(log_file_path, "a");

    int rc;
    if (strcmp(cmd, "list") == 0) rc = cmd_list(g_ndjson);
    else if (strcmp(cmd, "describe") == 0) rc = cmd_describe(target, g_ndjson);
    else if (strcmp(cmd, "init") == 0) rc = cmd_init(target, profile);
    else if (strcmp(cmd, "read-input") == 0) rc = cmd_read_input(target, count, duration_s);
    else if (strcmp(cmd, "send-command") == 0) {
        if (!hex) { fprintf(stderr, "[gcusb] --hex is required.\n"); rc = 1; }
        else rc = cmd_send_command(target, hex, unsafe);
    }
    else if (strcmp(cmd, "stop-rumble") == 0) rc = cmd_stop_rumble(target);
    else if (strcmp(cmd, "rumble") == 0) rc = cmd_rumble(target, (uint8_t)amplitude, duration_ms, unsafe, confirm_genuine);
    else if (strcmp(cmd, "rumble-sweep") == 0) rc = cmd_rumble_sweep(target, unsafe, confirm_genuine);
    else if (strcmp(cmd, "replay") == 0) {
        if (!script) { fprintf(stderr, "[gcusb] --script is required.\n"); rc = 1; }
        else if (validate_only) rc = validate_script_file(script, unsafe);
        else rc = cmd_replay(target, script, unsafe);
    }
    else if (strcmp(cmd, "compare") == 0) {
        if (!script) { fprintf(stderr, "[gcusb] --script is required.\n"); rc = 1; }
        else rc = cmd_compare(script, unsafe);
    }
    else { print_usage(); rc = 1; }

    if (g_log_file) fclose(g_log_file);
    return rc;
}
