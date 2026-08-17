#include "ns2_kbm_status.h"

#include <stdio.h>

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
