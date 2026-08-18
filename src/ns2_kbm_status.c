#include "ns2_kbm_status.h"

#include <stdio.h>
#include <string.h>

// Keep every field on its own line, paired with its argument, so an added field
// is a one-line change in two adjacent places rather than an edit that can slip
// a whole argument list out of alignment.
int ns2_kbm_status_format(const ns2_kbm_runtime_status_t *status, char *out,
                          size_t len) {
    if (!out || len == 0) return 0;
    if (!status) {
        out[0] = '\0';
        return 0;
    }
    return snprintf(
        out, len,
        "{"
        "\"mode\":\"%s\","
        "\"override\":\"%s\","
        "\"profile\":\"%s\","
        "\"keyboard\":%s,"
        "\"mouse\":%s,"
        "\"nativeMouse\":%s,"
        "\"keyboardConn\":%u,"
        "\"mouseConn\":%u,"
        "\"group\":%lu,"
        "\"source\":%lu,"
        "\"keyboardReports\":%lu,"
        "\"mouseReports\":%lu,"
        "\"rejectedMode\":%lu,"
        "\"rejectedDuplicate\":%lu,"
        "\"rejectedNotOwner\":%lu,"
        "\"rollover\":%lu,"
        "\"roleLosses\":%lu,"
        "\"mapGeneration\":%lu,"
        "\"neutralizations\":%lu,"
        "\"publishes\":%lu,"
        "\"recenters\":%lu"
        "}",
        ns2_kbm_mode_name((ns2_kbm_mode_t)status->mode),
        ns2_kbm_mode_name((ns2_kbm_mode_t)status->mode_override),
        ns2_kbm_profile_name((ns2_kbm_profile_t)status->profile),
        status->keyboard_connected ? "true" : "false",
        status->mouse_connected ? "true" : "false",
        status->native_mouse_output ? "true" : "false",
        (unsigned)status->keyboard_conn,
        (unsigned)status->mouse_conn,
        (unsigned long)status->group_id,
        (unsigned long)status->source_id,
        (unsigned long)status->keyboard_reports,
        (unsigned long)status->mouse_reports,
        (unsigned long)status->rejected_mode,
        (unsigned long)status->rejected_duplicate,
        (unsigned long)status->rejected_not_owner,
        (unsigned long)status->rollover_reports,
        (unsigned long)status->role_losses,
        (unsigned long)status->config_generation,
        (unsigned long)status->remap_neutralizations,
        (unsigned long)status->publishes,
        (unsigned long)status->stick_recenters);
}

// The limits travel with the values deliberately: a client offering these
// settings must not carry its own copy of the accepted range, because a
// firmware that widened one would then be driven by a client that still
// refuses it.
int ns2_kbm_mouse_format(const ns2_kbm_mouse_config_t *mouse, char *out,
                         size_t len) {
    if (!out || len == 0) return 0;
    if (!mouse) {
        out[0] = '\0';
        return 0;
    }
    return snprintf(out, len,
                    "{\"sensitivityX\":%u,\"sensitivityY\":%u,\"recenterMs\":%u,"
                    "\"invertX\":%s,\"invertY\":%s,\"antiDeadzone\":%u,"
                    "\"sensitivityMin\":%u,\"sensitivityMax\":%u,"
                    "\"recenterMinMs\":%u,\"recenterMaxMs\":%u,"
                    "\"antiDeadzoneMax\":%u}",
                    mouse->sensitivity_x, mouse->sensitivity_y,
                    mouse->recenter_ms, mouse->invert_x ? "true" : "false",
                    mouse->invert_y ? "true" : "false",
                    (unsigned)mouse->anti_deadzone,
                    (unsigned)NS2_KBM_MOUSE_SENS_MIN,
                    (unsigned)NS2_KBM_MOUSE_SENS_MAX,
                    (unsigned)NS2_KBM_MOUSE_RECENTER_MIN_MS,
                    (unsigned)NS2_KBM_MOUSE_RECENTER_MAX_MS,
                    (unsigned)NS2_KBM_MOUSE_ADZ_MAX);
}

bool ns2_kbm_mouse_command_apply(ns2_kbm_mouse_config_t *mouse,
                                 const char *args) {
    if (!mouse || !args) return false;
    char field[16] = {0};
    long value = 0;
    // Trailing text after the value is tolerated, matching what the management
    // and CDC surfaces have always accepted. Tightening it here would silently
    // change which commands an existing client can send.
    if (sscanf(args, "%15s %ld", field, &value) != 2) return false;
    // Representable as the persisted uint16_t at all. Anything narrower is the
    // configured range, which ns2_kbm_runtime_set_mouse() owns.
    if (value < 0 || value > 65535L) return false;

    if (strcmp(field, "sensitivity") == 0) {
        mouse->sensitivity_x = (uint16_t)value;
        mouse->sensitivity_y = (uint16_t)value;
    } else if (strcmp(field, "sensitivityx") == 0) {
        mouse->sensitivity_x = (uint16_t)value;
    } else if (strcmp(field, "sensitivityy") == 0) {
        mouse->sensitivity_y = (uint16_t)value;
    } else if (strcmp(field, "recenter") == 0) {
        mouse->recenter_ms = (uint16_t)value;
    } else if (strcmp(field, "invertx") == 0) {
        if (value > 1) return false;
        mouse->invert_x = (uint8_t)value;
    } else if (strcmp(field, "inverty") == 0) {
        if (value > 1) return false;
        mouse->invert_y = (uint8_t)value;
    } else if (strcmp(field, "antideadzone") == 0) {
        // Only the uint8_t REPRESENTABILITY check belongs here -- without it a
        // value of 256 would truncate to 0 and be accepted as "off", which is a
        // silently wrong setting rather than a rejected one. The configured
        // 0..NS2_KBM_MOUSE_ADZ_MAX range is enforced by
        // ns2_kbm_config_sanitize(), like sensitivity and recenter, so one
        // place decides what a range is.
        if (value > 255) return false;
        mouse->anti_deadzone = (uint8_t)value;
    } else {
        return false;
    }
    return true;
}
