/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_dispatch_bits.c — Bit-field extraction and replacement helpers.
 *
 * The 16-bit control word passed to these operations encodes the field
 * geometry and the target word within an aggregate:
 *
 *   bits 15-12  shift   — how many bits to shift the field right before
 *                          masking (for extract) or left before ORing (for replace)
 *   bits 11-8   width   — number of bits in the field; a width of 0 means
 *                          "empty field" and always produces 0
 *   bits 7-0    word index  — which word within the aggregate to access
 *                            (the "local" variants use only the low nibble,
 *                             bits 3-0, to index a local-variable aggregate)
 *
 * The "single-bit" variants (replace_single_bit / replace_single_bit_in_local)
 * use bits 15-12 as the bit number and bit 8 as the desired bit value (0 or 1).
 */
#include "lpst_run_internal.h"

static uint16_t extract_bit_field_core(const lpst_exec_state *state,
    uint16_t handle,
    uint16_t word_index,
    uint16_t control_word)
{
    uint16_t shift = (uint16_t)((control_word >> 12) & 0x0Fu);
    uint16_t width = (uint16_t)((control_word >> 8) & 0x0Fu);
    uint16_t value_mask;
    uint16_t word;

    value_mask = width == 0 ? 0 : (uint16_t)((1u << width) - 1u);
    word = read_aggregate_word(state, handle, word_index);
    return (uint16_t)((word >> shift) & value_mask);
}

uint16_t extract_bit_field(const lpst_exec_state *state, uint16_t handle, uint16_t control_word)
{
    return extract_bit_field_core(state, handle, (uint16_t)(control_word & 0xFFu), control_word);
}

uint16_t extract_bit_field_from_local(const lpst_exec_state *state, uint16_t handle, uint16_t control_word)
{
    return extract_bit_field_core(state, handle, (uint16_t)(control_word & 0x0Fu), control_word);
}

static void replace_bit_field_core(lpst_exec_state *state,
    uint16_t handle,
    uint16_t word_index,
    uint16_t control_word,
    uint16_t value)
{
    uint16_t shift = (uint16_t)((control_word >> 12) & 0x0Fu);
    uint16_t width = (uint16_t)((control_word >> 8) & 0x0Fu);
    uint16_t value_mask;
    uint16_t word_mask;
    uint16_t word;
    uint16_t updated;

    value_mask = width == 0 ? 0 : (uint16_t)((1u << width) - 1u);
    word_mask = (uint16_t)(value_mask << shift);
    word = read_aggregate_word(state, handle, word_index);
    updated = (uint16_t)((word & ~word_mask) | ((value & value_mask) << shift));
    write_aggregate_word(state, handle, word_index, updated);
}

void replace_bit_field(lpst_exec_state *state, uint16_t handle, uint16_t control_word, uint16_t value)
{
    replace_bit_field_core(state, handle, (uint16_t)(control_word & 0xFFu), control_word, value);
}

void replace_bit_field_in_local(lpst_exec_state *state, uint16_t handle, uint16_t control_word, uint16_t value)
{
    replace_bit_field_core(state, handle, (uint16_t)(control_word & 0x0Fu), control_word, value);
}

static void replace_single_bit_core(lpst_exec_state *state,
    uint16_t handle,
    uint16_t word_index,
    uint16_t control_word)
{
    uint16_t bit_number = (uint16_t)((control_word >> 12) & 0x0Fu);
    bool bit_value = ((control_word >> 8) & 0x01u) != 0;
    uint16_t mask = (uint16_t)(1u << bit_number);
    uint16_t word = read_aggregate_word(state, handle, word_index);

    word = bit_value ? (uint16_t)(word | mask) : (uint16_t)(word & ~mask);
    write_aggregate_word(state, handle, word_index, word);
}

void replace_single_bit(lpst_exec_state *state, uint16_t handle, uint16_t control_word)
{
    replace_single_bit_core(state, handle, (uint16_t)(control_word & 0xFFu), control_word);
}

void replace_single_bit_in_local(lpst_exec_state *state, uint16_t handle, uint16_t control_word)
{
    replace_single_bit_core(state, handle, (uint16_t)(control_word & 0x0Fu), control_word);
}
