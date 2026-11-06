#ifndef _CONFIG_SAVE_TRACKER_H_
#define _CONFIG_SAVE_TRACKER_H_

#include <stdbool.h>
#include <stdint.h>

// Session-local monotonic identities for the general settings persistence
// path. A request is complete once completed has reached its identity in
// modulo-uint32 order. The counters intentionally include automatic/internal
// requests so status never claims the persistence engine is idle while work is
// pending.
typedef struct {
    volatile uint32_t requested;
    volatile uint32_t completed;
} config_save_tracker_t;

uint32_t config_save_tracker_request(config_save_tracker_t *tracker);
uint32_t config_save_tracker_requested(const config_save_tracker_t *tracker);
uint32_t config_save_tracker_completed(const config_save_tracker_t *tracker);
void config_save_tracker_complete(config_save_tracker_t *tracker, uint32_t request_id);
bool config_save_tracker_pending(const config_save_tracker_t *tracker);
bool config_save_tracker_reached(uint32_t completed, uint32_t request_id);

#endif
