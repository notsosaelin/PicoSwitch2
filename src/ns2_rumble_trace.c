#include "ns2_rumble_trace.h"

#include <string.h>

static volatile ns2_rumble_trace_t s_trace;

void ns2_rumble_trace_console(uint8_t left, uint8_t right)
{
    s_trace.console_reports++;
    s_trace.console_left = left;
    s_trace.console_right = right;
    if (left != 0u || right != 0u) s_trace.console_nonzero++;
}

void ns2_rumble_trace_bridge(uint8_t left, uint8_t right, uint8_t player,
                             bool motion_wanted, bool sent)
{
    s_trace.bridge_left = left;
    s_trace.bridge_right = right;
    s_trace.bridge_player = player;
    s_trace.bridge_motion_wanted = motion_wanted ? 1u : 0u;
    if (left != 0u || right != 0u) s_trace.bridge_nonzero++;
    if (sent) s_trace.bridge_sent++;
    else s_trace.bridge_failed++;
}

void ns2_rumble_trace_get(ns2_rumble_trace_t *out)
{
    if (!out) return;
    // One shallow copy of plain scalars. See the header on why this needs no
    // critical section.
    ns2_rumble_trace_t snapshot;
    memcpy(&snapshot, (const void *)&s_trace, sizeof(snapshot));
    *out = snapshot;
}

void ns2_rumble_trace_reset(void)
{
    memset((void *)&s_trace, 0, sizeof(s_trace));
}
