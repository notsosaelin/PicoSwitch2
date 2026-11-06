#include "config_save_tracker.h"

uint32_t config_save_tracker_request(config_save_tracker_t *tracker)
{
    return __atomic_add_fetch(&tracker->requested, 1u, __ATOMIC_RELEASE);
}

uint32_t config_save_tracker_requested(const config_save_tracker_t *tracker)
{
    return __atomic_load_n(&tracker->requested, __ATOMIC_ACQUIRE);
}

uint32_t config_save_tracker_completed(const config_save_tracker_t *tracker)
{
    return __atomic_load_n(&tracker->completed, __ATOMIC_ACQUIRE);
}

void config_save_tracker_complete(config_save_tracker_t *tracker, uint32_t request_id)
{
    __atomic_store_n(&tracker->completed, request_id, __ATOMIC_RELEASE);
}

bool config_save_tracker_pending(const config_save_tracker_t *tracker)
{
    return config_save_tracker_requested(tracker) !=
           config_save_tracker_completed(tracker);
}

bool config_save_tracker_reached(uint32_t completed, uint32_t request_id)
{
    return (int32_t)(completed - request_id) >= 0;
}
