#include "ns2_motion_hybrid.h"

#include <string.h>

#include "ns2_motion_pdu.h"

typedef struct {
    uint16_t offset;
    uint16_t width;
} bit_range_t;

typedef enum {
    LAYOUT_HIGH_RATE = 0,
    LAYOUT_NORMAL,
    LAYOUT_CATCHUP,
} motion40_layout_t;

static uint16_t elapsed40(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH])
{
    return (uint16_t)((pdu[1] >> 4) | ((uint16_t)pdu[2] << 4));
}

static motion40_layout_t layout40(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH])
{
    const uint16_t elapsed = elapsed40(pdu);
    if (elapsed >= 15u) return LAYOUT_CATCHUP;
    if (elapsed >= 11u) return LAYOUT_NORMAL;
    return LAYOUT_HIGH_RATE;
}

static bool is_mode3(const uint8_t pdu[NS2_MOTION_PDU40_LENGTH])
{
    return (pdu[4] & 0x03u) == 3u;
}

static void copy_range(uint8_t *out, const uint8_t *donor,
                       bit_range_t range)
{
    for (uint16_t bit = range.offset; bit < range.offset + range.width; ++bit) {
        const uint8_t mask = (uint8_t)(1u << (bit & 7u));
        const uint16_t byte = bit >> 3;
        if (donor[byte] & mask)
            out[byte] |= mask;
        else
            out[byte] &= (uint8_t)~mask;
    }
}

static void copy_ranges(uint8_t *out, const uint8_t *donor,
                        const bit_range_t *ranges, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i)
        copy_range(out, donor, ranges[i]);
}

static const bit_range_t k30_timing[] = {{0, 16}};
static const bit_range_t k30_temperature[] = {{16, 16}};
static const bit_range_t k30_prefix[] = {
    {32, 2}, {40, 24}, {64, 2}, {72, 24}, {96, 2}, {104, 24},
};
static const bit_range_t k30_flags[] = {{34, 6}, {66, 6}, {98, 6}};
static const bit_range_t k30_accel[] = {{128, 96}};
static const bit_range_t k30_tail[] = {{224, 16}};

static const bit_range_t k40_timing[] = {{0, 24}};
static const bit_range_t k40_status[] = {{24, 8}};
static const bit_range_t k40_packing[] = {{32, 2}};
static const bit_range_t k40_high_prefix[] = {{34, 24}, {58, 23}, {81, 25}};
static const bit_range_t k40_high_accel[] = {{106, 66}, {238, 66}};
static const bit_range_t k40_high_gyro[] = {{172, 66}};
static const bit_range_t k40_high_tail[] = {{304, 16}};
static const bit_range_t k40_normal_prefix[] = {{34, 22}, {56, 21}, {77, 23}};
static const bit_range_t k40_normal_accel[] = {
    {100, 42}, {181, 39}, {262, 42},
};
static const bit_range_t k40_normal_gyro[] = {{142, 39}, {220, 42}};
static const bit_range_t k40_normal_tail[] = {{304, 16}};
static const bit_range_t k40_catchup_prefix[] = {{34, 22}, {56, 21}, {77, 23}};
static const bit_range_t k40_catchup_accel[] = {
    {100, 42}, {190, 39}, {277, 42},
};
static const bit_range_t k40_catchup_gyro[] = {{142, 48}, {229, 48}};
static const bit_range_t k40_catchup_tail[] = {{319, 1}};

#define COUNT_OF(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

uint32_t ns2_motion_hybrid_available_groups(const uint8_t *pdu,
                                            uint8_t length)
{
    if (!pdu) return 0u;
    if (length == NS2_MOTION_PDU30_LENGTH) {
        return NS2_MOTION_HYBRID_TIMING |
               NS2_MOTION_HYBRID_TEMPERATURE |
               NS2_MOTION_HYBRID_PREFIX |
               NS2_MOTION_HYBRID_FLAGS_RESERVED |
               NS2_MOTION_HYBRID_ACCEL |
               NS2_MOTION_HYBRID_TAIL;
    }
    if (length == NS2_MOTION_PDU40_LENGTH && is_mode3(pdu)) {
        const motion40_layout_t layout = layout40(pdu);
        (void)layout;
        return NS2_MOTION_HYBRID_TIMING |
               NS2_MOTION_HYBRID_STATUS |
               NS2_MOTION_HYBRID_PACKING |
               NS2_MOTION_HYBRID_PREFIX |
               NS2_MOTION_HYBRID_ACCEL |
               NS2_MOTION_HYBRID_GYRO |
               NS2_MOTION_HYBRID_TAIL;
    }
    return 0u;
}

static void splice30(uint8_t *out, const uint8_t *donor, uint32_t groups)
{
    if (groups & NS2_MOTION_HYBRID_TIMING)
        copy_ranges(out, donor, k30_timing, COUNT_OF(k30_timing));
    if (groups & NS2_MOTION_HYBRID_TEMPERATURE)
        copy_ranges(out, donor, k30_temperature, COUNT_OF(k30_temperature));
    if (groups & NS2_MOTION_HYBRID_PREFIX)
        copy_ranges(out, donor, k30_prefix, COUNT_OF(k30_prefix));
    if (groups & NS2_MOTION_HYBRID_FLAGS_RESERVED)
        copy_ranges(out, donor, k30_flags, COUNT_OF(k30_flags));
    if (groups & NS2_MOTION_HYBRID_ACCEL)
        copy_ranges(out, donor, k30_accel, COUNT_OF(k30_accel));
    if (groups & NS2_MOTION_HYBRID_TAIL)
        copy_ranges(out, donor, k30_tail, COUNT_OF(k30_tail));
}

static void splice40(uint8_t *out, const uint8_t *donor,
                     motion40_layout_t layout, uint32_t groups)
{
    if (groups & NS2_MOTION_HYBRID_TIMING)
        copy_ranges(out, donor, k40_timing, COUNT_OF(k40_timing));
    if (groups & NS2_MOTION_HYBRID_STATUS)
        copy_ranges(out, donor, k40_status, COUNT_OF(k40_status));
    if (groups & NS2_MOTION_HYBRID_PACKING)
        copy_ranges(out, donor, k40_packing, COUNT_OF(k40_packing));

    const bit_range_t *prefix;
    const bit_range_t *accel;
    const bit_range_t *gyro;
    const bit_range_t *tail;
    uint8_t prefix_count;
    uint8_t accel_count;
    uint8_t gyro_count;
    uint8_t tail_count;
    if (layout == LAYOUT_HIGH_RATE) {
        prefix = k40_high_prefix; prefix_count = COUNT_OF(k40_high_prefix);
        accel = k40_high_accel; accel_count = COUNT_OF(k40_high_accel);
        gyro = k40_high_gyro; gyro_count = COUNT_OF(k40_high_gyro);
        tail = k40_high_tail; tail_count = COUNT_OF(k40_high_tail);
    } else if (layout == LAYOUT_NORMAL) {
        prefix = k40_normal_prefix; prefix_count = COUNT_OF(k40_normal_prefix);
        accel = k40_normal_accel; accel_count = COUNT_OF(k40_normal_accel);
        gyro = k40_normal_gyro; gyro_count = COUNT_OF(k40_normal_gyro);
        tail = k40_normal_tail; tail_count = COUNT_OF(k40_normal_tail);
    } else {
        prefix = k40_catchup_prefix; prefix_count = COUNT_OF(k40_catchup_prefix);
        accel = k40_catchup_accel; accel_count = COUNT_OF(k40_catchup_accel);
        gyro = k40_catchup_gyro; gyro_count = COUNT_OF(k40_catchup_gyro);
        tail = k40_catchup_tail; tail_count = COUNT_OF(k40_catchup_tail);
    }
    if (groups & NS2_MOTION_HYBRID_PREFIX)
        copy_ranges(out, donor, prefix, prefix_count);
    if (groups & NS2_MOTION_HYBRID_ACCEL)
        copy_ranges(out, donor, accel, accel_count);
    if (groups & NS2_MOTION_HYBRID_GYRO)
        copy_ranges(out, donor, gyro, gyro_count);
    if (groups & NS2_MOTION_HYBRID_TAIL)
        copy_ranges(out, donor, tail, tail_count);
}

ns2_motion_hybrid_result_t ns2_motion_hybrid_splice(
    const uint8_t *base, const uint8_t *donor, uint8_t length,
    uint32_t groups, uint8_t *out)
{
    if (!base || !donor || !out)
        return NS2_MOTION_HYBRID_BAD_ARGUMENT;
    if (length != NS2_MOTION_PDU30_LENGTH &&
        length != NS2_MOTION_PDU40_LENGTH)
        return NS2_MOTION_HYBRID_BAD_LENGTH;
    const uint32_t available = ns2_motion_hybrid_available_groups(base, length);
    if (available == 0u)
        return length == NS2_MOTION_PDU40_LENGTH
            ? NS2_MOTION_HYBRID_BAD_MODE : NS2_MOTION_HYBRID_BAD_LENGTH;
    if (groups == 0u || (groups & ~available) != 0u)
        return NS2_MOTION_HYBRID_BAD_GROUP;

    motion40_layout_t layout = LAYOUT_HIGH_RATE;
    if (length == NS2_MOTION_PDU40_LENGTH) {
        if (!is_mode3(donor)) return NS2_MOTION_HYBRID_BAD_MODE;
        layout = layout40(base);
        if (layout40(donor) != layout)
            return NS2_MOTION_HYBRID_LAYOUT_MISMATCH;
        // Status is controller-authored opaque state, not a layout constant.
        // Five genuine mode-3 high-rate packets carry 0x00 rather than 0x0D.
        // Require structural equality, but never rewrite or reject a genuine
        // non-modal value merely because it is uncommon.
        if (base[3] != donor[3])
            return NS2_MOTION_HYBRID_STATUS_MISMATCH;
    }

    uint8_t candidate[NS2_MOTION_PDU40_LENGTH];
    memcpy(candidate, base, length);
    if (length == NS2_MOTION_PDU30_LENGTH)
        splice30(candidate, donor, groups);
    else
        splice40(candidate, donor, layout, groups);

    if (length == NS2_MOTION_PDU40_LENGTH &&
        (!is_mode3(candidate) || layout40(candidate) != layout ||
         candidate[3] != base[3]))
        return NS2_MOTION_HYBRID_OUTPUT_INVALID;
    memcpy(out, candidate, length);
    return NS2_MOTION_HYBRID_OK;
}

const char *ns2_motion_hybrid_result_name(ns2_motion_hybrid_result_t result)
{
    switch (result) {
        case NS2_MOTION_HYBRID_OK: return "ok";
        case NS2_MOTION_HYBRID_BAD_ARGUMENT: return "bad_argument";
        case NS2_MOTION_HYBRID_BAD_LENGTH: return "bad_length";
        case NS2_MOTION_HYBRID_BAD_MODE: return "bad_mode";
        case NS2_MOTION_HYBRID_LAYOUT_MISMATCH: return "layout_mismatch";
        case NS2_MOTION_HYBRID_STATUS_MISMATCH: return "status_mismatch";
        case NS2_MOTION_HYBRID_BAD_GROUP: return "bad_group";
        case NS2_MOTION_HYBRID_OUTPUT_INVALID: return "output_invalid";
        default: return "unknown";
    }
}
