#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "config_save_tracker.h"

int main(void)
{
    config_save_tracker_t tracker = {0};
    assert(!config_save_tracker_pending(&tracker));

    uint32_t first = config_save_tracker_request(&tracker);
    uint32_t second = config_save_tracker_request(&tracker);
    assert(first == 1u);
    assert(second == 2u);
    assert(config_save_tracker_pending(&tracker));

    // Completing the snapshot taken for the first write must not hide a newer
    // request that arrived while that write was in progress.
    config_save_tracker_complete(&tracker, first);
    assert(config_save_tracker_pending(&tracker));
    assert(config_save_tracker_reached(
        config_save_tracker_completed(&tracker), first));
    assert(!config_save_tracker_reached(
        config_save_tracker_completed(&tracker), second));

    config_save_tracker_complete(&tracker, second);
    assert(!config_save_tracker_pending(&tracker));

    // IDs wrap naturally; signed modulo comparison still recognizes a request
    // completed immediately after UINT32_MAX.
    tracker.requested = UINT32_MAX;
    tracker.completed = UINT32_MAX;
    uint32_t wrapped = config_save_tracker_request(&tracker);
    assert(wrapped == 0u);
    assert(config_save_tracker_pending(&tracker));
    config_save_tracker_complete(&tracker, wrapped);
    assert(config_save_tracker_reached(0u, wrapped));
    assert(!config_save_tracker_pending(&tracker));

    puts("config save tracker tests passed");
    return 0;
}
