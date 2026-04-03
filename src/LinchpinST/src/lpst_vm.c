/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_vm.c — Core VM memory helpers: RAM bank access, arena seeding, frame
 * initialisation, and local-variable bootstrap.
 */
#include "lpst_vm.h"

#include <string.h>

static uint8_t *lpst_ram_ptr(uint8_t *ram_banks[LPST_RAM_BANK_COUNT], size_t offset)
{
    size_t bank_index;

    if (ram_banks == NULL || offset >= LPST_FULL_RAM_BYTES) {
        return NULL;
    }

    bank_index = offset / LPST_RAM_BANK_BYTES;
    if (bank_index >= LPST_RAM_BANK_COUNT || ram_banks[bank_index] == NULL) {
        return NULL;
    }

    return &ram_banks[bank_index][offset % LPST_RAM_BANK_BYTES];
}

static size_t lpst_align_even(size_t value)
{
    return (value + 1u) & ~(size_t)1u;
}

static bool lpst_write_byte(uint8_t *ram_banks[LPST_RAM_BANK_COUNT], size_t offset, uint8_t value)
{
    uint8_t *dest = lpst_ram_ptr(ram_banks, offset);

    if (dest == NULL) {
        return false;
    }

    *dest = value;
    return true;
}

static bool lpst_write_word(uint8_t *ram_banks[LPST_RAM_BANK_COUNT], size_t offset, uint16_t value)
{
    return lpst_write_byte(ram_banks, offset, (uint8_t)(value & 0xFFu))
        && lpst_write_byte(ram_banks, offset + 1u, (uint8_t)(value >> 8));
}

static bool lpst_fill_zero(uint8_t *ram_banks[LPST_RAM_BANK_COUNT], size_t offset, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (!lpst_write_byte(ram_banks, offset + index, 0u)) {
            return false;
        }
    }

    return true;
}

static bool lpst_copy_bytes(
    uint8_t *ram_banks[LPST_RAM_BANK_COUNT],
    size_t offset,
    const uint8_t *source,
    size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (!lpst_write_byte(ram_banks, offset + index, source[index])) {
            return false;
        }
    }

    return true;
}

/*
 * Write the arena-allocator header block at arena_offset within the RAM banks.
 * The VM's heap allocator expects a specific 10-byte structure at the base of
 * each allocatable region: a free-list anchor and a size word.  segment_extent_bytes
 * is the total usable size of the segment, and the size word is set to
 * (segment_extent_bytes - 8) to exclude the header itself.
 */
static bool lpst_seed_arena(
    uint8_t *ram_banks[LPST_RAM_BANK_COUNT],
    size_t arena_offset,
    size_t segment_extent_bytes)
{
    size_t segment_base;
    size_t intra_segment_offset;
    uint16_t arena_handle;

    if (arena_offset + 10u > LPST_FULL_RAM_BYTES) {
        return false;
    }

    segment_base = arena_offset & ~(LPST_SEGMENT_SIZE_BYTES - 1u);
    intra_segment_offset = arena_offset - segment_base;
    if (intra_segment_offset + 10u > segment_extent_bytes) {
        return false;
    }

    arena_handle = (uint16_t)(((segment_base == 0u) ? 0u : 0x8000u) | (uint16_t)(intra_segment_offset / 2u));
    return lpst_write_word(ram_banks, arena_offset + 0u, 0x0000u)
        && lpst_write_word(ram_banks, arena_offset + 2u, (uint16_t)(arena_handle + 3u))
        && lpst_write_word(ram_banks, arena_offset + 6u, (uint16_t)(arena_handle + 3u))
        && lpst_write_word(
            ram_banks,
            arena_offset + 8u,
            (uint16_t)(segment_extent_bytes - (intra_segment_offset + 8u)));
}

/*
 * Initialise the locals array for a procedure call.  All slots are first set
 * to LPST_FALSE_SENTINEL, mirroring the behaviour of MME when a new frame is
 * entered.  Named initializers from the procedure header are then overlaid.
 */
bool lpst_bootstrap_locals(
    uint16_t *locals,
    size_t local_count,
    const lpst_proc_initializer *initializers,
    size_t initializer_count)
{
    size_t index;

    if (locals == NULL) {
        return false;
    }

    for (index = 0; index < local_count; ++index) {
        locals[index] = LPST_FALSE_SENTINEL;
    }

    for (index = 0; index < initializer_count; ++index) {
        const lpst_proc_initializer *initializer = &initializers[index];
        if (initializer->local_index >= local_count) {
            return false;
        }

        locals[initializer->local_index] = initializer->value;
    }

    return true;
}

/*
 * Load the initial RAM snapshot from the MME file into the low segment and
 * set up arena-allocator headers for both the low and high segments.
 *
 * Layout after this call:
 *   [0 .. initial_ram_size)       — initial RAM data from the MME file
 *   [aligned_end .. 0xFFFF]      — low-segment heap (upward-growing)
 *   [0x10000 .. 0x1F800)         — high-segment heap (upward-growing)
 *   [0x1F800 .. 0x1FFFF]         — reserved for the tuple (downward) stack
 */
bool lpst_seed_initial_ram(
    uint8_t *ram_banks[LPST_RAM_BANK_COUNT],
    const uint8_t *initial_ram_bytes,
    size_t initial_ram_size)
{
    size_t low_arena_offset;
    size_t high_arena_offset;

    if (ram_banks == NULL) {
        return false;
    }

    if (initial_ram_size > LPST_SEGMENT_SIZE_BYTES || initial_ram_bytes == NULL) {
        return false;
    }

    if (!lpst_fill_zero(ram_banks, 0u, LPST_FULL_RAM_BYTES)) {
        return false;
    }

    if (!lpst_copy_bytes(ram_banks, 0u, initial_ram_bytes, initial_ram_size)) {
        return false;
    }

    low_arena_offset = lpst_align_even(initial_ram_size);
    if (!lpst_seed_arena(ram_banks, low_arena_offset, LPST_SEGMENT_SIZE_BYTES)) {
        return false;
    }

    high_arena_offset = LPST_SEGMENT_SIZE_BYTES;
    return lpst_seed_arena(ram_banks, high_arena_offset, LPST_SEGMENT_SIZE_BYTES - LPST_TUPLE_STACK_RESERVE_BYTES);
}

void lpst_initialize_frame(
    lpst_vm_frame *frame,
    uint16_t module_id,
    uint16_t procedure_index,
    uint16_t procedure_start_offset,
    uint16_t code_offset,
    uint16_t *locals,
    size_t local_count)
{
    if (frame == NULL) {
        return;
    }

    frame->module_id = module_id;
    frame->procedure_index = procedure_index;
    frame->procedure_start_offset = procedure_start_offset;
    frame->code_offset = code_offset;
    frame->locals = locals;
    frame->local_count = local_count;
}
