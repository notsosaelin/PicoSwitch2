/*
 * Host regression coverage for the pure BOOTSEL gesture recognizer.
 *
 * Production calls the same recognizer through bootsel_poll() from both
 * bthid_on_report_boundary() and the 30 ms control timer. These tests model
 * those two call sources independently so a sustained HID report stream can
 * be proven to advance gestures even when timer callbacks are starved.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
 *       tools/test_bootsel_gesture.c src/bootsel_gesture.c \
 *       -o build/test_bootsel_gesture.exe
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bootsel_gesture.h"

typedef enum {
    SERVICE_TIMER,
    SERVICE_REPORT,
} service_source_t;

typedef struct {
    bootsel_gesture_state_t gesture;
    unsigned timer_calls;
    unsigned report_calls;
} fixture_t;

static int failures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("FAIL: %s\n", message);                                   \
            failures++;                                                       \
        } else {                                                               \
            printf("OK:   %s\n", message);                                   \
        }                                                                      \
    } while (0)

static bootsel_gesture_t service(fixture_t *fixture, service_source_t source,
                                 bool sample_valid, bool pressed,
                                 uint32_t now_ms) {
    if (source == SERVICE_TIMER)
        fixture->timer_calls++;
    else
        fixture->report_calls++;
    return bootsel_gesture_update(&fixture->gesture, sample_valid, pressed, now_ms);
}

static void test_unknown_sample_and_single_tap(void) {
    fixture_t fixture = {0};

    CHECK(service(&fixture, SERVICE_TIMER, false, true, 100) == BOOTSEL_NONE,
          "unknown sample cannot fabricate a press edge");
    CHECK(service(&fixture, SERVICE_TIMER, true, false, 130) == BOOTSEL_NONE,
          "first valid released sample cannot fabricate a release edge");

    CHECK(service(&fixture, SERVICE_TIMER, true, true, 200) == BOOTSEL_NONE,
          "single tap press is not immediately classified");
    CHECK(service(&fixture, SERVICE_TIMER, true, false, 230) == BOOTSEL_NONE,
          "single tap release waits for the tap window");
    CHECK(service(&fixture, SERVICE_TIMER, true, false,
                  230 + BOOTSEL_TAP_WINDOW_MS) == BOOTSEL_SINGLE_TAP,
          "single tap is classified after the tap window");
}

static void test_timer_fallback_double_tap(void) {
    fixture_t fixture = {0};

    service(&fixture, SERVICE_TIMER, true, false, 0);
    service(&fixture, SERVICE_TIMER, true, true, 30);
    service(&fixture, SERVICE_TIMER, true, false, 60);
    service(&fixture, SERVICE_TIMER, true, true, 180);
    service(&fixture, SERVICE_TIMER, true, false, 210);

    CHECK(service(&fixture, SERVICE_TIMER, true, false, 690) == BOOTSEL_NONE,
          "timer fallback does not classify before the 500 ms deadline");
    CHECK(service(&fixture, SERVICE_TIMER, true, false, 720) == BOOTSEL_DOUBLE_TAP,
          "timer fallback classifies a double tap while reports are quiet");
    CHECK(fixture.report_calls == 0,
          "quiet/disconnected double tap needs no report-boundary calls");
}

static void test_report_flood_triple_tap_without_timer(void) {
    fixture_t fixture = {0};
    bootsel_gesture_t result = BOOTSEL_NONE;
    unsigned premature_gestures = 0;

    service(&fixture, SERVICE_REPORT, true, false, 0);

    const uint32_t press_times[] = {100, 220, 340};
    const uint32_t release_times[] = {140, 260, 380};
    for (unsigned tap = 0; tap < 3; tap++) {
        for (uint32_t now = press_times[tap]; now < release_times[tap]; now += 4) {
            result = service(&fixture, SERVICE_REPORT, true, true, now);
            if (result != BOOTSEL_NONE)
                premature_gestures++;
        }
        result = service(&fixture, SERVICE_REPORT, true, false, release_times[tap]);
        if (result != BOOTSEL_NONE)
            premature_gestures++;
    }

    for (uint32_t now = 384; now < 880; now += 4) {
        result = service(&fixture, SERVICE_REPORT, true, false, now);
        if (result != BOOTSEL_NONE)
            premature_gestures++;
    }
    result = service(&fixture, SERVICE_REPORT, true, false, 880);

    CHECK(premature_gestures == 0,
          "report flood emits nothing before the final tap-window deadline");
    CHECK(result == BOOTSEL_TRIPLE_TAP,
          "report boundaries classify a triple tap when the timer is fully starved");
    CHECK(fixture.timer_calls == 0,
          "starvation regression uses zero timer callbacks");
    CHECK(fixture.report_calls > 100,
          "starvation regression exercises a sustained high-rate report stream");
}

static void test_report_driven_hold_fires_once(void) {
    fixture_t fixture = {0};
    unsigned hold_count = 0;

    service(&fixture, SERVICE_REPORT, true, false, 900);
    service(&fixture, SERVICE_REPORT, true, true, 1000);
    for (uint32_t now = 1004; now <= 3200; now += 4) {
        if (service(&fixture, SERVICE_REPORT, true, true, now) == BOOTSEL_HOLD)
            hold_count++;
    }

    CHECK(hold_count == 1,
          "report-driven two-second hold fires exactly once under a report flood");
    CHECK(service(&fixture, SERVICE_REPORT, true, false, 3300) == BOOTSEL_NONE,
          "release after a fired hold does not become a tap");
    CHECK(service(&fixture, SERVICE_REPORT, true, false,
                  3300 + BOOTSEL_TAP_WINDOW_MS) == BOOTSEL_NONE,
          "completed hold cannot later become a delayed tap gesture");
    CHECK(fixture.timer_calls == 0,
          "report-driven hold remains independent of the timer fallback");
}

static void test_mixed_sources_do_not_duplicate(void) {
    fixture_t fixture = {0};

    service(&fixture, SERVICE_REPORT, true, true, 100);
    service(&fixture, SERVICE_REPORT, true, false, 130);
    service(&fixture, SERVICE_REPORT, true, true, 240);
    service(&fixture, SERVICE_REPORT, true, false, 270);

    CHECK(service(&fixture, SERVICE_TIMER, true, false, 770) == BOOTSEL_DOUBLE_TAP,
          "timer may finish a gesture whose edges arrived at report boundaries");
    CHECK(service(&fixture, SERVICE_REPORT, true, false, 771) == BOOTSEL_NONE,
          "report service cannot emit the timer-completed gesture twice");
    CHECK(service(&fixture, SERVICE_TIMER, true, false, 800) == BOOTSEL_NONE,
          "later timer fallback also sees the gesture as consumed");
}

static void test_timestamp_wrap(void) {
    fixture_t fixture = {0};
    uint32_t start = UINT32_MAX - 1000u;

    service(&fixture, SERVICE_REPORT, true, true, start);
    CHECK(service(&fixture, SERVICE_REPORT, true, true,
                  start + BOOTSEL_HOLD_MS - 1u) == BOOTSEL_NONE,
          "hold stays pending one millisecond before deadline across wrap");
    CHECK(service(&fixture, SERVICE_REPORT, true, true,
                  start + BOOTSEL_HOLD_MS) == BOOTSEL_HOLD,
          "unsigned timing preserves the hold deadline across uint32 wrap");
}

int main(void) {
    test_unknown_sample_and_single_tap();
    test_timer_fallback_double_tap();
    test_report_flood_triple_tap_without_timer();
    test_report_driven_hold_fires_once();
    test_mixed_sources_do_not_duplicate();
    test_timestamp_wrap();

    if (failures) {
        printf("bootsel_gesture: %d failure(s)\n", failures);
        return 1;
    }
    puts("bootsel_gesture: all tests passed");
    return 0;
}
