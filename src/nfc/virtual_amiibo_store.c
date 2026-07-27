#include "virtual_amiibo_store.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"

#include "ns2_amiibo_v3.h"

#define VIRTUAL_AMIIBO_FLASH_BANK0_OFFSET \
    (PICO_FLASH_SIZE_BYTES - 3u * FLASH_SECTOR_SIZE)
#define VIRTUAL_AMIIBO_FLASH_BANK1_OFFSET \
    (PICO_FLASH_SIZE_BYTES - 5u * FLASH_SECTOR_SIZE)
#define VIRTUAL_AMIIBO_RECORD_MAGIC 0x4F424D41u /* "AMBO" little-endian */
#define VIRTUAL_AMIIBO_RECORD_VERSION 2u
// v3 (NTAG I2C 2K) records share the same banks and header layout as the 540
// v2 records but use version 3 and a single 2048-byte payload. Only one amiibo
// (540/572 v2 OR 2048 v3) is ever stored, so the two never coexist.
#define VIRTUAL_AMIIBO_V3_RECORD_VERSION 3u
#define VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE 2048u
// Record must hold the larger of the v2 payload (2*540+32) and the v3 payload
// (2048); 9 flash pages (2304) covers both and fits one 4096-byte bank sector.
#define VIRTUAL_AMIIBO_RECORD_SIZE (9u * FLASH_PAGE_SIZE)
#define VIRTUAL_AMIIBO_HEADER_SIZE 32u
#define VIRTUAL_AMIIBO_PAYLOAD_CLEAN_OFFSET 0u
#define VIRTUAL_AMIIBO_PAYLOAD_USED_OFFSET VIRTUAL_AMIIBO_RAW_SIZE
#define VIRTUAL_AMIIBO_PAYLOAD_SIGNATURE_OFFSET \
    (2u * VIRTUAL_AMIIBO_RAW_SIZE)
#define VIRTUAL_AMIIBO_PAYLOAD_MAX_SIZE \
    (2u * VIRTUAL_AMIIBO_RAW_SIZE + VIRTUAL_AMIIBO_SIGNATURE_SIZE)
#define VIRTUAL_AMIIBO_FLAG_HAS_USED 0x0001u
#define VIRTUAL_AMIIBO_FLAG_USING_USED 0x0002u
#define VIRTUAL_AMIIBO_FLAG_DIRTY 0x0004u

// Version 1 used an append journal in bank 0 with one image per record. Keep a
// read-only migration decoder; the first v2 save goes to bank 1, preserving
// the legacy record until the new snapshot has been programmed and verified.
#define VIRTUAL_AMIIBO_V1_RECORD_VERSION 1u
#define VIRTUAL_AMIIBO_V1_RECORD_SIZE (3u * FLASH_PAGE_SIZE)
#define VIRTUAL_AMIIBO_V1_RECORD_COUNT \
    (FLASH_SECTOR_SIZE / VIRTUAL_AMIIBO_V1_RECORD_SIZE)

_Static_assert(VIRTUAL_AMIIBO_HEADER_SIZE +
                   VIRTUAL_AMIIBO_PAYLOAD_MAX_SIZE <=
                   VIRTUAL_AMIIBO_RECORD_SIZE,
               "virtual amiibo journal record is too small");
_Static_assert(VIRTUAL_AMIIBO_HEADER_SIZE +
                   VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE <=
                   VIRTUAL_AMIIBO_RECORD_SIZE,
               "v3 amiibo journal record is too small");
_Static_assert(VIRTUAL_AMIIBO_RECORD_SIZE <= FLASH_SECTOR_SIZE,
               "journal record exceeds one flash bank sector");
_Static_assert(VIRTUAL_AMIIBO_FLASH_BANK0_OFFSET + FLASH_SECTOR_SIZE <=
                   PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE,
               "virtual amiibo bank 0 overlaps BTstack TLV banks");
_Static_assert(VIRTUAL_AMIIBO_FLASH_BANK1_OFFSET + FLASH_SECTOR_SIZE <=
                   PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE,
               "virtual amiibo bank 1 overlaps config storage");

static virtual_amiibo_t tag;
static critical_section_t tag_lock;
static volatile bool persist_requested;
static volatile bool clear_requested;
static volatile bool loaded_snapshot;
static volatile bool presented_snapshot;
static int active_bank = -1;
static uint8_t record_buffer[VIRTUAL_AMIIBO_RECORD_SIZE];

// NTAG I2C 2K (v3) slot. It shares the journal banks with the 540/572 store
// (mutually exclusive — only one amiibo is ever stored) and persists across
// power cycles, cleared only by Eject. Declared here because init() loads it.
static uint8_t v3_image[NS2_AMIIBO_V3_SIZE];
static uint8_t v3_upload_buf[NS2_AMIIBO_V3_SIZE];
static uint16_t v3_upload_size;
static uint16_t v3_upload_received;
static uint32_t v3_upload_crc;
static volatile bool v3_upload_in_progress;
static volatile bool v3_slot_loaded;
static uint32_t v3_generation;

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t bank_offset(unsigned bank)
{
    return bank == 0u ? VIRTUAL_AMIIBO_FLASH_BANK0_OFFSET
                      : VIRTUAL_AMIIBO_FLASH_BANK1_OFFSET;
}

static const uint8_t *bank_record(unsigned bank)
{
    return (const uint8_t *)(XIP_BASE + bank_offset(bank));
}

static size_t v2_payload_size(uint16_t source_size)
{
    return 2u * VIRTUAL_AMIIBO_RAW_SIZE +
           (source_size == VIRTUAL_AMIIBO_EXTENDED_SIZE
                ? VIRTUAL_AMIIBO_SIGNATURE_SIZE : 0u);
}

static bool record_v2_valid(const uint8_t *record, uint32_t *generation,
                            uint16_t *size, uint16_t *flags)
{
    if (read_u32le(record) != VIRTUAL_AMIIBO_RECORD_MAGIC ||
        read_u16le(record + 4) != VIRTUAL_AMIIBO_RECORD_VERSION ||
        read_u16le(record + 6) != VIRTUAL_AMIIBO_HEADER_SIZE)
        return false;
    if (read_u32le(record + 20) != virtual_amiibo_crc32(record, 20))
        return false;

    const uint16_t stored_size = read_u16le(record + 12);
    if (!virtual_amiibo_supported_size(stored_size)) return false;
    const uint16_t stored_flags = read_u16le(record + 14);
    if ((stored_flags & ~(VIRTUAL_AMIIBO_FLAG_HAS_USED |
                          VIRTUAL_AMIIBO_FLAG_USING_USED |
                          VIRTUAL_AMIIBO_FLAG_DIRTY)) != 0u)
        return false;
    if ((stored_flags & VIRTUAL_AMIIBO_FLAG_USING_USED) != 0u &&
        (stored_flags & VIRTUAL_AMIIBO_FLAG_HAS_USED) == 0u)
        return false;

    const uint8_t *payload = record + VIRTUAL_AMIIBO_HEADER_SIZE;
    if (virtual_amiibo_crc32(payload, v2_payload_size(stored_size)) !=
        read_u32le(record + 16))
        return false;
    const uint8_t *clean =
        payload + VIRTUAL_AMIIBO_PAYLOAD_CLEAN_OFFSET;
    const uint8_t *used =
        payload + VIRTUAL_AMIIBO_PAYLOAD_USED_OFFSET;
    if (virtual_amiibo_validate_raw(clean, VIRTUAL_AMIIBO_RAW_SIZE) !=
        VIRTUAL_AMIIBO_OK)
        return false;
    if ((stored_flags & VIRTUAL_AMIIBO_FLAG_HAS_USED) != 0u &&
        (virtual_amiibo_validate_raw(used, VIRTUAL_AMIIBO_RAW_SIZE) !=
             VIRTUAL_AMIIBO_OK ||
         memcmp(clean, used, 16) != 0))
        return false;

    *generation = read_u32le(record + 8);
    *size = stored_size;
    *flags = stored_flags;
    return true;
}

// v3 (NTAG I2C 2K) record: same 32-byte header (magic/version/header_size/
// generation/size/flags/payload_crc/header_crc) followed by a single 2048-byte
// image payload.
static bool record_v3_valid(const uint8_t *record, uint32_t *generation)
{
    if (read_u32le(record) != VIRTUAL_AMIIBO_RECORD_MAGIC ||
        read_u16le(record + 4) != VIRTUAL_AMIIBO_V3_RECORD_VERSION ||
        read_u16le(record + 6) != VIRTUAL_AMIIBO_HEADER_SIZE)
        return false;
    if (read_u32le(record + 20) != virtual_amiibo_crc32(record, 20))
        return false;
    if (read_u16le(record + 12) != VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE)
        return false;
    const uint8_t *payload = record + VIRTUAL_AMIIBO_HEADER_SIZE;
    if (virtual_amiibo_crc32(payload, VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE) !=
        read_u32le(record + 16))
        return false;
    if (!ns2_amiibo_v3_valid(payload, VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE))
        return false;
    *generation = read_u32le(record + 8);
    return true;
}

static const uint8_t *v1_record_at(unsigned index)
{
    return (const uint8_t *)(XIP_BASE + VIRTUAL_AMIIBO_FLASH_BANK0_OFFSET +
                             index * VIRTUAL_AMIIBO_V1_RECORD_SIZE);
}

static bool record_v1_valid(const uint8_t *record, uint32_t *generation,
                            uint16_t *size)
{
    if (read_u32le(record) != VIRTUAL_AMIIBO_RECORD_MAGIC ||
        read_u16le(record + 4) != VIRTUAL_AMIIBO_V1_RECORD_VERSION ||
        read_u16le(record + 6) != VIRTUAL_AMIIBO_HEADER_SIZE)
        return false;
    if (read_u32le(record + 20) != virtual_amiibo_crc32(record, 20))
        return false;
    const uint16_t stored_size = read_u16le(record + 12);
    if (!virtual_amiibo_supported_size(stored_size)) return false;
    const uint8_t *payload = record + VIRTUAL_AMIIBO_HEADER_SIZE;
    if (virtual_amiibo_crc32(payload, stored_size) !=
            read_u32le(record + 16) ||
        virtual_amiibo_validate_raw(payload, VIRTUAL_AMIIBO_RAW_SIZE) !=
            VIRTUAL_AMIIBO_OK)
        return false;
    *generation = read_u32le(record + 8);
    *size = stored_size;
    return true;
}

void virtual_amiibo_store_init(void)
{
    critical_section_init(&tag_lock);
    virtual_amiibo_init(&tag);
    persist_requested = false;
    clear_requested = false;
    __atomic_store_n(&loaded_snapshot, false, __ATOMIC_RELEASE);
    __atomic_store_n(&presented_snapshot, false, __ATOMIC_RELEASE);
    v3_slot_loaded = false;
    v3_generation = 0;
    active_bank = -1;

    // A v3 (NTAG I2C 2K) record takes precedence: the type switch that writes it
    // erases both banks first, so a valid v3 record means no v2 record survives.
    const uint8_t *best_v3 = NULL;
    uint32_t best_v3_generation = 0;
    for (unsigned bank = 0; bank < 2u; ++bank) {
        uint32_t generation;
        const uint8_t *record = bank_record(bank);
        if (!record_v3_valid(record, &generation)) continue;
        if (!best_v3 || (int32_t)(generation - best_v3_generation) > 0) {
            best_v3 = record;
            best_v3_generation = generation;
            active_bank = (int)bank;
        }
    }
    if (best_v3) {
        memcpy(v3_image, best_v3 + VIRTUAL_AMIIBO_HEADER_SIZE,
               VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE);
        v3_generation = best_v3_generation;
        __atomic_store_n(&v3_slot_loaded, true, __ATOMIC_RELEASE);
        __atomic_store_n(&presented_snapshot, true, __ATOMIC_RELEASE);
        return;
    }

    const uint8_t *best = NULL;
    uint32_t best_generation = 0;
    uint16_t best_size = 0;
    uint16_t best_flags = 0;
    for (unsigned bank = 0; bank < 2u; ++bank) {
        uint32_t generation;
        uint16_t size;
        uint16_t flags;
        const uint8_t *record = bank_record(bank);
        if (!record_v2_valid(record, &generation, &size, &flags)) continue;
        if (!best || (int32_t)(generation - best_generation) > 0) {
            best = record;
            best_generation = generation;
            best_size = size;
            best_flags = flags;
            active_bank = (int)bank;
        }
    }
    if (best) {
        const uint8_t *payload = best + VIRTUAL_AMIIBO_HEADER_SIZE;
        uint8_t clean[VIRTUAL_AMIIBO_EXTENDED_SIZE];
        memcpy(clean,
               payload + VIRTUAL_AMIIBO_PAYLOAD_CLEAN_OFFSET,
               VIRTUAL_AMIIBO_RAW_SIZE);
        if (best_size == VIRTUAL_AMIIBO_EXTENDED_SIZE) {
            memcpy(clean + VIRTUAL_AMIIBO_RAW_SIZE,
                   payload + VIRTUAL_AMIIBO_PAYLOAD_SIGNATURE_OFFSET,
                   VIRTUAL_AMIIBO_SIGNATURE_SIZE);
        }
        (void)virtual_amiibo_load(&tag, clean, best_size, true);
        if ((best_flags & VIRTUAL_AMIIBO_FLAG_HAS_USED) != 0u) {
            memcpy(tag.raw,
                   payload + VIRTUAL_AMIIBO_PAYLOAD_USED_OFFSET,
                   VIRTUAL_AMIIBO_RAW_SIZE);
            tag.has_used_copy = true;
        }
        tag.using_used_copy =
            (best_flags & VIRTUAL_AMIIBO_FLAG_USING_USED) != 0u;
        tag.dirty = (best_flags & VIRTUAL_AMIIBO_FLAG_DIRTY) != 0u;
        tag.persisted = true;
        tag.generation = best_generation;
        __atomic_store_n(&loaded_snapshot, true, __ATOMIC_RELEASE);
        __atomic_store_n(&presented_snapshot, true, __ATOMIC_RELEASE);
        return;
    }

    // Migrate the newest valid v1 append record as an Unused baseline.
    for (unsigned i = 0; i < VIRTUAL_AMIIBO_V1_RECORD_COUNT; ++i) {
        uint32_t generation;
        uint16_t size;
        const uint8_t *record = v1_record_at(i);
        if (!record_v1_valid(record, &generation, &size)) continue;
        if (!best || (int32_t)(generation - best_generation) > 0) {
            best = record;
            best_generation = generation;
            best_size = size;
        }
    }
    if (best) {
        (void)virtual_amiibo_load(
            &tag, best + VIRTUAL_AMIIBO_HEADER_SIZE, best_size, true);
        tag.generation = best_generation;
        __atomic_store_n(&loaded_snapshot, true, __ATOMIC_RELEASE);
        __atomic_store_n(&presented_snapshot, true, __ATOMIC_RELEASE);
    }
}

bool virtual_amiibo_store_loaded(void)
{
    return __atomic_load_n(&loaded_snapshot, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&presented_snapshot, __ATOMIC_ACQUIRE);
}

virtual_amiibo_result_t virtual_amiibo_store_set_presented(bool presented)
{
    if (presented &&
        !__atomic_load_n(&loaded_snapshot, __ATOMIC_ACQUIRE))
        return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    __atomic_store_n(&presented_snapshot, presented, __ATOMIC_RELEASE);
    return VIRTUAL_AMIIBO_OK;
}

// --- NTAG I2C 2K (v3) upload/slot API (state declared with the top statics) ---
virtual_amiibo_result_t virtual_amiibo_store_v3_upload_begin(
    size_t size, uint32_t expected_crc)
{
    if (size != NS2_AMIIBO_V3_SIZE) return VIRTUAL_AMIIBO_ERROR_SIZE;
    v3_upload_size = (uint16_t)size;
    v3_upload_received = 0;
    v3_upload_crc = expected_crc;
    memset(v3_upload_buf, 0, sizeof(v3_upload_buf));
    v3_upload_in_progress = true;
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_store_v3_upload_chunk(
    size_t offset, const uint8_t *data, size_t size)
{
    if (!v3_upload_in_progress) return VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE;
    if (!data || size == 0) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    if (offset > v3_upload_size || size > (size_t)v3_upload_size - offset)
        return VIRTUAL_AMIIBO_ERROR_RANGE;
    memcpy(v3_upload_buf + offset, data, size);
    if (offset + size > v3_upload_received)
        v3_upload_received = (uint16_t)(offset + size);
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_store_v3_upload_commit(void)
{
    if (!v3_upload_in_progress) return VIRTUAL_AMIIBO_ERROR_UPLOAD_STATE;
    if (v3_upload_received != v3_upload_size)
        return VIRTUAL_AMIIBO_ERROR_INCOMPLETE;
    if (virtual_amiibo_crc32(v3_upload_buf, v3_upload_size) != v3_upload_crc)
        return VIRTUAL_AMIIBO_ERROR_CRC;
    if (!ns2_amiibo_v3_valid(v3_upload_buf, v3_upload_size))
        return VIRTUAL_AMIIBO_ERROR_BCC;
    memcpy(v3_image, v3_upload_buf, NS2_AMIIBO_V3_SIZE);
    v3_upload_in_progress = false;
    // Mutual exclusion: a v3 tag replaces any loaded 540/572 tag. The journal
    // write in service_save erases both banks on the type switch.
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_init(&tag);
    critical_section_exit(&tag_lock);
    __atomic_store_n(&loaded_snapshot, false, __ATOMIC_RELEASE);
    v3_generation++;
    __atomic_store_n(&v3_slot_loaded, true, __ATOMIC_RELEASE);
    __atomic_store_n(&presented_snapshot, true, __ATOMIC_RELEASE);
    persist_requested = true;
    return VIRTUAL_AMIIBO_OK;
}

bool virtual_amiibo_store_v3_upload_active(void)
{
    return v3_upload_in_progress;
}

bool virtual_amiibo_store_v3_loaded(void)
{
    return __atomic_load_n(&v3_slot_loaded, __ATOMIC_ACQUIRE);
}

void virtual_amiibo_store_v3_clear(void)
{
    v3_upload_in_progress = false;
    __atomic_store_n(&v3_slot_loaded, false, __ATOMIC_RELEASE);
}

bool virtual_amiibo_store_v3_copy(uint8_t out[2048])
{
    if (!out || !virtual_amiibo_store_v3_loaded()) return false;
    memcpy(out, v3_image, NS2_AMIIBO_V3_SIZE);
    return true;
}

void virtual_amiibo_store_status(virtual_amiibo_status_t *out)
{
    if (!out) return;
    critical_section_enter_blocking(&tag_lock);
    memset(out, 0, sizeof(*out));
    out->loaded = tag.loaded;
    out->presented =
        __atomic_load_n(&presented_snapshot, __ATOMIC_ACQUIRE);
    out->dirty = tag.dirty;
    out->persisted = tag.persisted;
    out->has_originality_signature = tag.has_originality_signature;
    out->has_used_copy = tag.has_used_copy;
    out->using_used_copy = tag.using_used_copy;
    out->upload_active = tag.upload_active;
    out->size = tag.source_size;
    out->upload_size = tag.upload_size;
    out->upload_received = tag.upload_received;
    out->generation = tag.generation;
    virtual_amiibo_uid(&tag, out->uid);
    critical_section_exit(&tag_lock);
}

virtual_amiibo_result_t virtual_amiibo_store_upload_begin(
    size_t size, uint32_t expected_crc)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_upload_begin(&tag, size, expected_crc);
    critical_section_exit(&tag_lock);
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_upload_chunk(
    size_t offset, const uint8_t *data, size_t size)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_upload_chunk(&tag, offset, data, size);
    critical_section_exit(&tag_lock);
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_upload_commit(void)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result = virtual_amiibo_upload_commit(&tag);
    if (result == VIRTUAL_AMIIBO_OK)
        __atomic_store_n(&loaded_snapshot, true, __ATOMIC_RELEASE);
    if (result == VIRTUAL_AMIIBO_OK)
        __atomic_store_n(&presented_snapshot, true, __ATOMIC_RELEASE);
    critical_section_exit(&tag_lock);
    if (result == VIRTUAL_AMIIBO_OK) {
        // Mutual exclusion: a 540/572 tag replaces any loaded v3 tag; the
        // journal write erases the v3 record on the type switch.
        __atomic_store_n(&v3_slot_loaded, false, __ATOMIC_RELEASE);
        persist_requested = true;
    }
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_upload_commit_used(void)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_upload_commit_used(&tag);
    critical_section_exit(&tag_lock);
    if (result == VIRTUAL_AMIIBO_OK) persist_requested = true;
    return result;
}

void virtual_amiibo_store_upload_cancel(void)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_upload_cancel(&tag);
    critical_section_exit(&tag_lock);
}

virtual_amiibo_result_t virtual_amiibo_store_read(
    size_t offset, uint8_t *out, size_t size)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_read(&tag, offset, out, size);
    critical_section_exit(&tag_lock);
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_read_copy(
    bool used, size_t offset, uint8_t *out, size_t size)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_read_copy(&tag, used, offset, out, size);
    critical_section_exit(&tag_lock);
    return result;
}

void virtual_amiibo_store_acknowledge_download(void)
{
    critical_section_enter_blocking(&tag_lock);
    const uint32_t generation = tag.generation;
    virtual_amiibo_acknowledge_download(&tag);
    const bool changed = tag.generation != generation;
    critical_section_exit(&tag_lock);
    if (changed) persist_requested = true;
}

virtual_amiibo_result_t virtual_amiibo_store_select_used(bool used)
{
    critical_section_enter_blocking(&tag_lock);
    const uint32_t generation = tag.generation;
    virtual_amiibo_result_t result =
        virtual_amiibo_select_used(&tag, used);
    const bool changed =
        result == VIRTUAL_AMIIBO_OK && tag.generation != generation;
    critical_section_exit(&tag_lock);
    if (changed) persist_requested = true;
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_copy_raw(uint8_t out[540])
{
    if (!out) return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_copy_active_raw(&tag, out);
    critical_section_exit(&tag_lock);
    return result;
}

virtual_amiibo_result_t virtual_amiibo_store_copy_image(
    uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint8_t signature[VIRTUAL_AMIIBO_SIGNATURE_SIZE],
    bool *has_signature, uint32_t *generation)
{
    if (!raw || !signature)
        return VIRTUAL_AMIIBO_ERROR_ARGUMENT;
    critical_section_enter_blocking(&tag_lock);
    if (!tag.loaded) {
        critical_section_exit(&tag_lock);
        return VIRTUAL_AMIIBO_ERROR_NOT_LOADED;
    }
    (void)virtual_amiibo_copy_active_raw(&tag, raw);
    memcpy(signature, tag.signature, VIRTUAL_AMIIBO_SIGNATURE_SIZE);
    if (has_signature)
        *has_signature = tag.has_originality_signature;
    if (generation)
        *generation = tag.generation;
    critical_section_exit(&tag_lock);
    return VIRTUAL_AMIIBO_OK;
}

virtual_amiibo_result_t virtual_amiibo_store_apply_console_write(
    const uint8_t raw[VIRTUAL_AMIIBO_RAW_SIZE],
    uint32_t expected_generation)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_result_t result =
        virtual_amiibo_apply_console_write(&tag, raw, expected_generation);
    critical_section_exit(&tag_lock);
    if (result == VIRTUAL_AMIIBO_OK) {
        // Console commits must survive power loss. Core0 never writes flash;
        // the existing core1 config-service point performs the snapshot.
        persist_requested = true;
    }
    return result;
}

void virtual_amiibo_store_mark_dirty(void)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_mark_dirty(&tag);
    critical_section_exit(&tag_lock);
}

void virtual_amiibo_store_request_persist(void)
{
    virtual_amiibo_status_t status;
    virtual_amiibo_store_status(&status);
    // `status.loaded` describes only the 540/572 store. A 2 KB (v3) tag lives in
    // its own slot and leaves `loaded` false by design (the two are mutually
    // exclusive), so gating solely on it made `amiibo persist` a silent no-op for
    // v3 -- the record was only ever written if service_save happened to run
    // before power was lost, which is why a USB-mode upload did not survive a
    // power blip while a BLE upload (dongle stays powered) usually did.
    if (status.loaded || virtual_amiibo_store_v3_loaded())
        persist_requested = true;
}

void virtual_amiibo_store_request_clear(void)
{
    critical_section_enter_blocking(&tag_lock);
    virtual_amiibo_init(&tag);
    // A pending image snapshot is superseded: the user asked for the image
    // to be gone, and programming it after the erase would resurrect it.
    persist_requested = false;
    critical_section_exit(&tag_lock);
    // Discard any v3 (NTAG I2C 2K) tag too — the clear erases both banks, which
    // hold whichever format was stored.
    v3_upload_in_progress = false;
    __atomic_store_n(&v3_slot_loaded, false, __ATOMIC_RELEASE);
    __atomic_store_n(&presented_snapshot, false, __ATOMIC_RELEASE);
    __atomic_store_n(&loaded_snapshot, false, __ATOMIC_RELEASE);
    clear_requested = true;
}

bool virtual_amiibo_store_clear_pending(void)
{
    return clear_requested;
}

bool virtual_amiibo_store_persist_pending(void)
{
    return persist_requested;
}

void virtual_amiibo_store_service_save(void)
{
    if (clear_requested) {
        // Erase both journal banks so a power cycle cannot resurrect the
        // discarded tag. The prior-snapshot alternation guarantee is
        // intentionally void here; v1 migration records live in bank 0 and
        // are removed by the same erase.
        for (unsigned bank = 0; bank < 2u; ++bank) {
            const uint32_t destination_offset = bank_offset(bank);
            multicore_lockout_start_blocking();
            uint32_t interrupts = save_and_disable_interrupts();
            flash_range_erase(destination_offset, FLASH_SECTOR_SIZE);
            restore_interrupts(interrupts);
            multicore_lockout_end_blocking();
        }
        active_bank = -1;
        clear_requested = false;
    }

    if (!persist_requested) return;

    // v3 (NTAG I2C 2K) persist: a single 2048-byte image record. It replaces any
    // 540 tag, so both banks are erased first (no stale v2 record can survive the
    // type switch), then bank 0 is programmed. A power loss here leaves no tag —
    // never a wrong one — and the user re-uploads.
    if (__atomic_load_n(&v3_slot_loaded, __ATOMIC_ACQUIRE)) {
        memset(record_buffer, 0xFF, sizeof(record_buffer));
        uint8_t *v3payload = record_buffer + VIRTUAL_AMIIBO_HEADER_SIZE;
        memcpy(v3payload, v3_image, VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE);
        write_u32le(record_buffer, VIRTUAL_AMIIBO_RECORD_MAGIC);
        write_u16le(record_buffer + 4, VIRTUAL_AMIIBO_V3_RECORD_VERSION);
        write_u16le(record_buffer + 6, VIRTUAL_AMIIBO_HEADER_SIZE);
        write_u32le(record_buffer + 8, v3_generation);
        write_u16le(record_buffer + 12, VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE);
        write_u16le(record_buffer + 14, 0u);
        write_u32le(record_buffer + 16,
                    virtual_amiibo_crc32(v3payload,
                                         VIRTUAL_AMIIBO_V3_PAYLOAD_SIZE));
        write_u32le(record_buffer + 20,
                    virtual_amiibo_crc32(record_buffer, 20));
        multicore_lockout_start_blocking();
        uint32_t interrupts = save_and_disable_interrupts();
        flash_range_erase(bank_offset(0), FLASH_SECTOR_SIZE);
        flash_range_erase(bank_offset(1), FLASH_SECTOR_SIZE);
        flash_range_program(bank_offset(0), record_buffer,
                            sizeof(record_buffer));
        restore_interrupts(interrupts);
        multicore_lockout_end_blocking();
        uint32_t verified_v3_generation;
        if (record_v3_valid(bank_record(0), &verified_v3_generation) &&
            verified_v3_generation == v3_generation) {
            active_bank = 0;
            persist_requested = false;
        }
        return;
    }

    uint16_t size;
    uint16_t flags;
    uint32_t generation;
    memset(record_buffer, 0xFF, sizeof(record_buffer));
    uint8_t *payload = record_buffer + VIRTUAL_AMIIBO_HEADER_SIZE;
    critical_section_enter_blocking(&tag_lock);
    if (!tag.loaded) {
        persist_requested = false;
        critical_section_exit(&tag_lock);
        return;
    }
    size = tag.source_size;
    generation = tag.generation;
    flags = (tag.has_used_copy ? VIRTUAL_AMIIBO_FLAG_HAS_USED : 0u) |
            (tag.using_used_copy ? VIRTUAL_AMIIBO_FLAG_USING_USED : 0u) |
            (tag.dirty ? VIRTUAL_AMIIBO_FLAG_DIRTY : 0u);
    memcpy(payload + VIRTUAL_AMIIBO_PAYLOAD_CLEAN_OFFSET,
           tag.clean_raw, VIRTUAL_AMIIBO_RAW_SIZE);
    memcpy(payload + VIRTUAL_AMIIBO_PAYLOAD_USED_OFFSET,
           tag.has_used_copy ? tag.raw : tag.clean_raw,
           VIRTUAL_AMIIBO_RAW_SIZE);
    if (tag.has_originality_signature) {
        memcpy(payload + VIRTUAL_AMIIBO_PAYLOAD_SIGNATURE_OFFSET,
               tag.signature, VIRTUAL_AMIIBO_SIGNATURE_SIZE);
    }
    critical_section_exit(&tag_lock);

    // Type switch from a previously-stored v3 tag: erase both banks (outside the
    // tag lock) so a stale v3 record cannot out-rank the new 540 record at init.
    {
        uint32_t stale_v3_generation;
        bool had_v3 = false;
        for (unsigned bank = 0; bank < 2u; ++bank)
            if (record_v3_valid(bank_record(bank), &stale_v3_generation))
                had_v3 = true;
        if (had_v3) {
            multicore_lockout_start_blocking();
            uint32_t interrupts = save_and_disable_interrupts();
            flash_range_erase(bank_offset(0), FLASH_SECTOR_SIZE);
            flash_range_erase(bank_offset(1), FLASH_SECTOR_SIZE);
            restore_interrupts(interrupts);
            multicore_lockout_end_blocking();
            active_bank = -1;
        }
    }

    write_u32le(record_buffer, VIRTUAL_AMIIBO_RECORD_MAGIC);
    write_u16le(record_buffer + 4, VIRTUAL_AMIIBO_RECORD_VERSION);
    write_u16le(record_buffer + 6, VIRTUAL_AMIIBO_HEADER_SIZE);
    write_u32le(record_buffer + 8, generation);
    write_u16le(record_buffer + 12, size);
    write_u16le(record_buffer + 14, flags);
    write_u32le(
        record_buffer + 16,
        virtual_amiibo_crc32(payload, v2_payload_size(size)));
    write_u32le(
        record_buffer + 20, virtual_amiibo_crc32(record_buffer, 20));

    // Alternate two sectors. The prior snapshot remains valid until the new
    // bank has been fully programmed, so a power loss cannot destroy both.
    const unsigned destination =
        active_bank < 0 ? 1u : (active_bank == 0 ? 1u : 0u);
    const uint32_t destination_offset = bank_offset(destination);
    multicore_lockout_start_blocking();
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(destination_offset, FLASH_SECTOR_SIZE);
    flash_range_program(destination_offset, record_buffer,
                        sizeof(record_buffer));
    restore_interrupts(interrupts);
    multicore_lockout_end_blocking();

    uint32_t verified_generation;
    uint16_t verified_size;
    uint16_t verified_flags;
    const bool verified = record_v2_valid(
        bank_record(destination), &verified_generation,
        &verified_size, &verified_flags) &&
        verified_generation == generation &&
        verified_size == size && verified_flags == flags;
    if (verified)
        active_bank = (int)destination;

    critical_section_enter_blocking(&tag_lock);
    if (verified && tag.loaded && tag.generation == generation) {
        tag.persisted = true;
        persist_requested = false;
    } else {
        // Verification failure or a newer in-RAM generation keeps the request
        // live. The other bank still contains the last valid snapshot.
        persist_requested = tag.loaded;
    }
    critical_section_exit(&tag_lock);
}
