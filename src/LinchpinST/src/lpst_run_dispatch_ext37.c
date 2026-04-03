/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_dispatch_ext37.c — EXTRACT opcode structured-record operations.
 *
 * Cornerstone stores data records as variable-length packed structures inside
 * aggregate (heap) objects.  A record consists of a one-word header followed
 * by a run of packed fields ("components"):
 *
 *   header word:
 *     high byte  = component count (0 means an untyped single-field record)
 *     low  byte  = record tag (used to search for a record by type)
 *
 *   packed fields:  each field begins with a length byte:
 *     bit 7 set  = 8-byte (4-word) floating-point field
 *     bit 7 clear = (length & 0x7F) bytes of ASCII data (length+1 bytes total,
 *                   including the length byte itself)
 *
 * The EXTRACT dispatch lets the VM navigate and project these structures
 * without knowing their layout at compile time, similar to a runtime-
 * polymorphic record accessor.
 */
#include "lpst_run_internal.h"

/* Describes a selected record within an aggregate.  word_offset and
 * word_length are relative to the aggregate base (word_index 0 = header);
 * payload_byte_offset and payload_byte_length describe the field data that
 * follows the record header. */
typedef struct lpst_ext37_record_selection {
    int word_offset;
    int word_length;
    int payload_byte_offset;
    int payload_byte_length;
    uint8_t tag;
    uint8_t component_count;
} lpst_ext37_record_selection;

/* Describes a selected sub-field within a record.  byte_length is the
 * logical content length; data_byte_offset is where the content starts;
 * encoded_byte_length includes the length-byte overhead. */
typedef struct lpst_ext37_subfield_selection {
    uint8_t byte_length;
    int data_byte_offset;
    int encoded_byte_length;
    int word_length;
} lpst_ext37_subfield_selection;

static int try_measure_ext37_packed_field(const lpst_exec_state *state,
    uint16_t base_handle,
    int field_byte_offset,
    int remaining_field_words,
    int *field_byte_length,
    int *field_word_length)
{
    int logical_byte_length;

    *field_byte_length = 0;
    *field_word_length = 0;
    if (remaining_field_words <= 0) {
        return 0;
    }

    logical_byte_length = read_aggregate_payload_byte(state, base_handle, field_byte_offset);
    if ((logical_byte_length & 0x80) != 0) {
        *field_byte_length = 8;
        *field_word_length = 4;
    } else {
        *field_byte_length = logical_byte_length + 1;
        *field_word_length = (*field_byte_length + 1) / 2;
    }

    return *field_word_length <= remaining_field_words;
}

static int try_measure_ext37_payload(const lpst_exec_state *state,
    uint16_t base_handle,
    int payload_byte_offset,
    int remaining_payload_words,
    uint8_t component_count,
    int *payload_byte_length,
    int *payload_word_length)
{
    int byte_cursor;
    int words_used;
    int bytes_used;
    int index;

    *payload_byte_length = 0;
    *payload_word_length = 0;

    if (remaining_payload_words < 0) {
        return 0;
    }

    if (component_count == 0) {
        return try_measure_ext37_packed_field(state,
            base_handle,
            payload_byte_offset,
            remaining_payload_words,
            payload_byte_length,
            payload_word_length);
    }

    byte_cursor = payload_byte_offset;
    words_used = 0;
    bytes_used = 0;
    for (index = 0; index < component_count; index++) {
        int field_byte_length;
        int field_word_length;

        if (!try_measure_ext37_packed_field(state,
                base_handle,
                byte_cursor,
                remaining_payload_words - words_used,
                &field_byte_length,
                &field_word_length)) {
            return 0;
        }

        bytes_used += field_byte_length;
        words_used += field_word_length;
        byte_cursor += field_word_length * 2;
    }

    *payload_byte_length = bytes_used;
    *payload_word_length = words_used;
    return 1;
}

static int try_measure_ext37_record(const lpst_exec_state *state,
    uint16_t base_handle,
    int word_offset,
    int remaining_words,
    lpst_ext37_record_selection *selection)
{
    uint16_t header_word;
    int payload_byte_offset;
    int payload_byte_length;
    int payload_word_length;

    memset(selection, 0, sizeof(*selection));
    if (remaining_words <= 0 || word_offset < 0) {
        return 0;
    }

    header_word = read_aggregate_word(state, base_handle, word_offset);
    payload_byte_offset = word_offset * 2 + 2;

    if (!try_measure_ext37_payload(state,
            base_handle,
            payload_byte_offset,
            remaining_words - 1,
            (uint8_t)(header_word >> 8),
            &payload_byte_length,
            &payload_word_length)) {
        return 0;
    }

    selection->word_offset = word_offset;
    selection->word_length = 1 + payload_word_length;
    selection->payload_byte_offset = payload_byte_offset;
    selection->payload_byte_length = payload_byte_length;
    selection->tag = (uint8_t)(header_word & 0xFFu);
    selection->component_count = (uint8_t)(header_word >> 8);
    return selection->word_length <= remaining_words;
}

static int try_measure_ext37_typed_field(const lpst_exec_state *state,
    uint16_t base_handle,
    int word_offset,
    int remaining_words,
    lpst_ext37_record_selection *selection)
{
    uint16_t header_word;
    uint8_t field_kind;
    uint8_t component_count;
    int payload_byte_offset;
    int payload_byte_length;
    int field_word_length;
    int component_word_length;

    memset(selection, 0, sizeof(*selection));
    if (remaining_words <= 0 || word_offset < 0) {
        return 0;
    }

    header_word = read_aggregate_word(state, base_handle, word_offset);
    field_kind = (uint8_t)(header_word & 0x00FFu);
    component_count = (uint8_t)(header_word >> 8);
    payload_byte_offset = word_offset * 2;

    if (header_word == 0) {
        field_word_length = 1;
        payload_byte_length = 0;
    } else if (field_kind == 0) {
        if (remaining_words < 2) {
            return 0;
        }

        field_word_length = read_aggregate_word(state, base_handle, word_offset + 1);
        if (field_word_length <= 0) {
            return 0;
        }

        payload_byte_length = (field_word_length - 1) * 2;
    } else {
        component_word_length = (field_kind + 1) / 2;
        field_word_length = 1 + component_count * component_word_length;
        payload_byte_length = component_count * field_kind;
    }

    if (field_word_length > remaining_words) {
        return 0;
    }

    selection->word_offset = word_offset;
    selection->word_length = field_word_length;
    selection->payload_byte_offset = payload_byte_offset;
    selection->payload_byte_length = payload_byte_length;
    selection->tag = field_kind;
    selection->component_count = component_count;
    return 1;
}

static int try_select_ext37_record(const lpst_exec_state *state,
    uint16_t base_handle,
    uint16_t available_span,
    uint16_t initial_word_offset,
    uint16_t record_tag_selector,
    uint16_t record_skip_count,
    lpst_ext37_record_selection *selection)
{
    int current_word_offset = initial_word_offset == LPST_FALSE_SENTINEL ? 0 : initial_word_offset;
    int remaining_words = available_span;

    memset(selection, 0, sizeof(*selection));
    if (current_word_offset < 0 || remaining_words <= 0) {
        return 0;
    }

    if (record_skip_count != LPST_FALSE_SENTINEL) {
        uint16_t index;

        for (index = 0; index < record_skip_count; index++) {
            lpst_ext37_record_selection skipped;

            if (!try_measure_ext37_record(state, base_handle, current_word_offset, remaining_words, &skipped)) {
                return 0;
            }

            current_word_offset += skipped.word_length;
            remaining_words -= skipped.word_length;
        }
    }

    if (record_tag_selector != LPST_FALSE_SENTINEL) {
        uint8_t requested_tag = (uint8_t)(record_tag_selector & 0xFFu);

        while (remaining_words > 0) {
            lpst_ext37_record_selection candidate;

            if (!try_measure_ext37_record(state, base_handle, current_word_offset, remaining_words, &candidate)) {
                return 0;
            }

            if (candidate.tag == requested_tag) {
                *selection = candidate;
                return 1;
            }

            current_word_offset += candidate.word_length;
            remaining_words -= candidate.word_length;
        }

        return 0;
    }

    return try_measure_ext37_record(state, base_handle, current_word_offset, remaining_words, selection);
}

static int try_select_ext37_typed_field_fallback(const lpst_exec_state *state,
    uint16_t base_handle,
    uint16_t available_span,
    uint16_t initial_word_offset,
    uint16_t record_tag_selector,
    uint16_t record_skip_count,
    lpst_ext37_record_selection *selection)
{
    int current_word_offset;
    int remaining_words;
    uint16_t ordinal;

    memset(selection, 0, sizeof(*selection));
    if (initial_word_offset != LPST_FALSE_SENTINEL || record_tag_selector == LPST_FALSE_SENTINEL) {
        return 0;
    }

    if (record_skip_count != LPST_FALSE_SENTINEL) {
        lpst_ext37_record_selection skipped_field;

        current_word_offset = record_skip_count;
        if (current_word_offset < 0 || current_word_offset >= available_span) {
            return 0;
        }

        if (!try_measure_ext37_typed_field(state,
                base_handle,
                current_word_offset,
                available_span - current_word_offset,
                &skipped_field)) {
            return 0;
        }

        current_word_offset += skipped_field.word_length;
        remaining_words = available_span - current_word_offset;

        for (ordinal = 1; ordinal <= record_tag_selector; ordinal++) {
            lpst_ext37_record_selection candidate;

            if (!try_measure_ext37_typed_field(state,
                    base_handle,
                    current_word_offset,
                    remaining_words,
                    &candidate)) {
                return 0;
            }

            if (ordinal == record_tag_selector) {
                *selection = candidate;
                return 1;
            }

            current_word_offset += candidate.word_length;
            remaining_words -= candidate.word_length;
        }

        return 0;
    }

    current_word_offset = record_tag_selector;
    if (current_word_offset < 0 || current_word_offset >= available_span) {
        return 0;
    }

    return try_measure_ext37_typed_field(state,
        base_handle,
        current_word_offset,
        available_span - current_word_offset,
        selection);
}

static int try_resolve_ext37_subfield(const lpst_exec_state *state,
    uint16_t base_handle,
    const lpst_ext37_record_selection *selection,
    uint16_t mode,
    lpst_ext37_subfield_selection *subfield)
{
    int byte_cursor;
    int remaining_words;
    uint16_t index;

    memset(subfield, 0, sizeof(*subfield));
    if (selection->component_count == 0 || mode > selection->component_count) {
        return 0;
    }

    byte_cursor = selection->payload_byte_offset;
    remaining_words = selection->word_length - 1;
    for (index = 1; index <= mode; index++) {
        int field_byte_length;
        int field_word_length;

        if (!try_measure_ext37_packed_field(state,
                base_handle,
                byte_cursor,
                remaining_words,
                &field_byte_length,
                &field_word_length)) {
            return 0;
        }

        if (index == mode) {
            subfield->byte_length = read_aggregate_payload_byte(state, base_handle, byte_cursor);
            subfield->data_byte_offset = byte_cursor + 1;
            subfield->encoded_byte_length = field_byte_length;
            subfield->word_length = field_word_length;
            return 1;
        }

        byte_cursor += field_word_length * 2;
        remaining_words -= field_word_length;
    }

    return 0;
}

static int try_resolve_ext37_typed_subfield(const lpst_exec_state *state,
    uint16_t base_handle,
    const lpst_ext37_record_selection *selection,
    uint16_t mode,
    lpst_ext37_subfield_selection *subfield)
{
    int byte_cursor;
    int remaining_words;
    int component_word_length;
    int component_byte_offset;
    uint16_t index;

    memset(subfield, 0, sizeof(*subfield));
    if (selection->component_count == 0 || mode == 0 || mode > selection->component_count) {
        return 0;
    }

    if (selection->tag == 0) {
        byte_cursor = selection->payload_byte_offset;
        remaining_words = selection->word_length - 2;
        for (index = 1; index <= mode; index++) {
            int field_byte_length;
            int field_word_length;

            if (!try_measure_ext37_packed_field(state,
                    base_handle,
                    byte_cursor,
                    remaining_words,
                    &field_byte_length,
                    &field_word_length)) {
                return 0;
            }

            if (index == mode) {
                subfield->byte_length = (uint8_t)(field_byte_length - 1);
                subfield->data_byte_offset = byte_cursor + 1;
                subfield->encoded_byte_length = field_byte_length;
                subfield->word_length = field_word_length;
                return 1;
            }

            byte_cursor += field_word_length * 2;
            remaining_words -= field_word_length;
        }

        return 0;
    }

    component_word_length = (selection->tag + 1) / 2;
    component_byte_offset = selection->payload_byte_offset + (mode - 1) * component_word_length * 2;
    subfield->byte_length = selection->tag;
    subfield->data_byte_offset = component_byte_offset;
    subfield->encoded_byte_length = selection->tag;
    subfield->word_length = component_word_length;
    return 1;
}

static int aggregate_byte_span_contains_non_zero(const lpst_exec_state *state,
    uint16_t handle,
    int byte_offset,
    int byte_length)
{
    int index;

    for (index = 0; index < byte_length; index++) {
        if (read_aggregate_payload_byte(state, handle, byte_offset + index) != 0) {
            return 1;
        }
    }

    return 0;
}

static int aggregate_raw_byte_span_contains_non_zero(const lpst_exec_state *state,
    uint16_t handle,
    int raw_byte_offset,
    int byte_length)
{
    int index;

    for (index = 0; index < byte_length; index++) {
        if (read_aggregate_raw_byte(state, handle, raw_byte_offset + index) != 0) {
            return 1;
        }
    }

    return 0;
}

static int can_write_ext37_destination(const lpst_exec_state *state, uint16_t destination_handle, uint16_t byte_length)
{
    return is_usable_aggregate_handle(destination_handle)
        && read_aggregate_word(state, destination_handle, 0) >= byte_length;
}

static uint16_t compute_ext37_skip_offset(const lpst_exec_state *state,
    uint16_t base_handle,
    uint16_t available_span,
    uint16_t record_tag_selector,
    uint16_t record_skip_count)
{
    int current_word_offset = record_skip_count;
    int records_to_walk = record_tag_selector;
    int index;

    for (index = 0; index < records_to_walk; index++) {
        int remaining_words = available_span - current_word_offset;
        lpst_ext37_record_selection field;

        if (!try_measure_ext37_typed_field(state, base_handle, current_word_offset, remaining_words, &field)) {
            break;
        }

        current_word_offset += field.word_length;
    }

    return (uint16_t)current_word_offset;
}

static uint16_t execute_ext37_leading_packed_field(lpst_exec_state *state,
    uint16_t effective_base,
    uint16_t destination_handle)
{
    uint16_t field_length = (uint16_t)(read_aggregate_word(state, effective_base, 0) & 0x00FFu);
    uint16_t data_base = (uint16_t)(effective_base + 1u);

    if (field_length == 0) {
        return LPST_FALSE_SENTINEL;
    }

    if (can_write_ext37_destination(state, destination_handle, field_length)) {
        copy_aggregate_raw_bytes_to_payload(state,
            data_base,
            0,
            destination_handle,
            0,
            field_length);
    }

    return field_length;
}

static uint16_t extract_ext37_selection(lpst_exec_state *state,
    uint16_t base_handle,
    uint16_t destination_handle,
    const lpst_ext37_record_selection *selection,
    uint16_t non_zero_guard,
    uint16_t mode,
    int used_typed_field_fallback)
{
    lpst_ext37_subfield_selection subfield;
    uint16_t result;
    int resolved_subfield;

    memset(&subfield, 0, sizeof(subfield));

    if (used_typed_field_fallback) {
        if (mode == LPST_FALSE_SENTINEL) {
            if (non_zero_guard != LPST_FALSE_SENTINEL
                && !aggregate_byte_span_contains_non_zero(state,
                    base_handle,
                    selection->payload_byte_offset,
                    selection->payload_byte_length)) {
                return LPST_FALSE_SENTINEL;
            }

            result = (uint16_t)selection->payload_byte_length;
            {
                uint16_t raw_byte_length = (uint16_t)(result + 2u);

                if (is_usable_aggregate_handle(destination_handle)
                    && read_aggregate_word(state, destination_handle, 0) >= raw_byte_length) {
                    copy_aggregate_bytes(state,
                        base_handle,
                        selection->word_offset * 2,
                        destination_handle,
                        0,
                        raw_byte_length);
                }
            }

            return result;
        }

        if (mode == 0 || mode == 0xFFFFu) {
            return selection->component_count;
        }

        if (!try_resolve_ext37_typed_subfield(state, base_handle, selection, mode, &subfield)) {
            return LPST_FALSE_SENTINEL;
        }

        if (non_zero_guard != LPST_FALSE_SENTINEL
            && !aggregate_byte_span_contains_non_zero(state,
                base_handle,
                subfield.data_byte_offset,
                subfield.byte_length)) {
            return LPST_FALSE_SENTINEL;
        }

        result = subfield.byte_length;
        if (can_write_ext37_destination(state, destination_handle, result)) {
            copy_aggregate_bytes(state,
                base_handle,
                subfield.data_byte_offset,
                destination_handle,
                0,
                result);
        }

        return result;
    }

    if (mode == LPST_FALSE_SENTINEL) {
        if (non_zero_guard != LPST_FALSE_SENTINEL
            && !aggregate_byte_span_contains_non_zero(state,
                base_handle,
                selection->payload_byte_offset,
                selection->payload_byte_length)) {
            return LPST_FALSE_SENTINEL;
        }

        result = (uint16_t)selection->payload_byte_length;
        if (can_write_ext37_destination(state, destination_handle, result)) {
            copy_aggregate_bytes(state,
                base_handle,
                selection->payload_byte_offset,
                destination_handle,
                0,
                result);
        }

        return result;
    }

    if (mode == 0 || mode == 0xFFFFu) {
        return selection->component_count;
    }

    resolved_subfield = try_resolve_ext37_subfield(state, base_handle, selection, mode, &subfield);
    if (!resolved_subfield) {
        return LPST_FALSE_SENTINEL;
    }

    result = subfield.byte_length;
    if (can_write_ext37_destination(state, destination_handle, result)) {
        copy_aggregate_bytes(state,
            base_handle,
            subfield.data_byte_offset,
            destination_handle,
            0,
            result);
    }

    return result;
}

void handle_ext37(lpst_exec_state *state)
{
    uint16_t mode;
    uint16_t non_zero_guard;
    uint16_t initial_word_offset;
    uint16_t record_skip_count;
    uint16_t record_tag_selector;
    uint16_t destination_handle;
    uint16_t available_span;
    uint16_t base_handle;
    uint16_t word_offset;
    uint16_t effective_base;
    uint16_t effective_span;
    lpst_ext37_record_selection selection;
    int used_typed_field_fallback = 0;
    uint16_t result;

    if (state->eval_stack_top < 8) {
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    mode = lpst_exec_pop(state);
    non_zero_guard = lpst_exec_pop(state);
    initial_word_offset = lpst_exec_pop(state);
    record_skip_count = lpst_exec_pop(state);
    record_tag_selector = lpst_exec_pop(state);
    destination_handle = lpst_exec_pop(state);
    available_span = lpst_exec_pop(state);
    base_handle = lpst_exec_pop(state);

    if (state->trace_enabled) {
        fprintf(stderr,
                "[EXTRACT_ENTER] base=0x%04X span=0x%04X dst=0x%04X tag=0x%04X skip=0x%04X off=0x%04X guard=0x%04X mode=0x%04X pc=0x%04X\n",
                base_handle,
                available_span,
                destination_handle,
                record_tag_selector,
                record_skip_count,
                initial_word_offset,
                non_zero_guard,
                mode,
                state->program_counter - 2);
    }

    memset(&selection, 0, sizeof(selection));

    if (!is_usable_aggregate_handle(base_handle)) {
        if (state->trace_enabled) {
            fprintf(stderr,
                    "[EXTRACT_FAIL] unusable base=0x%04X\n",
                    base_handle);
        }
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    if (record_skip_count != LPST_FALSE_SENTINEL) {
        word_offset = compute_ext37_skip_offset(state,
            base_handle,
            available_span,
            record_tag_selector,
            record_skip_count);
    } else {
        word_offset = record_tag_selector;
    }

    effective_base = (uint16_t)(base_handle + word_offset);
    effective_span = (uint16_t)(available_span - word_offset);

    if (initial_word_offset != LPST_FALSE_SENTINEL) {
        if (!try_select_ext37_record(state,
                effective_base,
                effective_span,
                initial_word_offset,
                LPST_FALSE_SENTINEL,
                LPST_FALSE_SENTINEL,
                &selection)) {
            used_typed_field_fallback = try_select_ext37_typed_field_fallback(state,
                effective_base,
                effective_span,
                initial_word_offset,
                LPST_FALSE_SENTINEL,
                LPST_FALSE_SENTINEL,
                &selection);
        }

        if (selection.word_length == 0 && !used_typed_field_fallback) {
            if (state->trace_enabled) {
                fprintf(stderr,
                        "[EXTRACT_FAIL] withOffset effectiveBase=0x%04X off=0x%04X\n",
                        effective_base,
                        initial_word_offset);
            }
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
            return;
        }

        result = extract_ext37_selection(state,
            effective_base,
            destination_handle,
            &selection,
            non_zero_guard,
            mode,
            used_typed_field_fallback);
        if (result == LPST_FALSE_SENTINEL && state->trace_enabled) {
            fprintf(stderr,
                    "[EXTRACT_FAIL] extract effectiveBase=0x%04X mode=0x%04X selTag=0x%02X selCount=0x%02X selOff=0x%04X\n",
                    effective_base,
                    mode,
                    selection.tag,
                    selection.component_count,
                    selection.word_offset);
        }
        lpst_exec_push(state, result);
        return;
    }

    if (mode != LPST_FALSE_SENTINEL) {
        if (!try_select_ext37_record(state,
                effective_base,
                effective_span,
                LPST_FALSE_SENTINEL,
                LPST_FALSE_SENTINEL,
                LPST_FALSE_SENTINEL,
                &selection)) {
            used_typed_field_fallback = try_measure_ext37_typed_field(state,
                effective_base,
                0,
                effective_span,
                &selection);
        }

        if (selection.word_length == 0 && !used_typed_field_fallback) {
            if (state->trace_enabled) {
                fprintf(stderr,
                        "[EXTRACT_FAIL] fieldWalker effectiveBase=0x%04X mode=0x%04X\n",
                        effective_base,
                        mode);
            }
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
            return;
        }

        result = extract_ext37_selection(state,
            effective_base,
            destination_handle,
            &selection,
            non_zero_guard,
            mode,
            used_typed_field_fallback);
        if (result == LPST_FALSE_SENTINEL && state->trace_enabled) {
            fprintf(stderr,
                    "[EXTRACT_FAIL] extract effectiveBase=0x%04X mode=0x%04X selTag=0x%02X selCount=0x%02X selOff=0x%04X\n",
                    effective_base,
                    mode,
                    selection.tag,
                    selection.component_count,
                    selection.word_offset);
        }
        lpst_exec_push(state, result);
        return;
    }

    if (non_zero_guard != LPST_FALSE_SENTINEL) {
        if (!aggregate_raw_byte_span_contains_non_zero(state, effective_base, 0, non_zero_guard)) {
            if (state->trace_enabled) {
                fprintf(stderr,
                        "[EXTRACT_FAIL] guard-direct effectiveBase=0x%04X guard=0x%04X\n",
                        effective_base,
                        non_zero_guard);
            }
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
            return;
        }

        if (can_write_ext37_destination(state, destination_handle, non_zero_guard)) {
            copy_aggregate_raw_bytes_to_payload(state,
                effective_base,
                0,
                destination_handle,
                0,
                non_zero_guard);
        }

        result = non_zero_guard;
    } else {
        result = execute_ext37_leading_packed_field(state, effective_base, destination_handle);
    }

    if (state->trace_enabled) {
        fprintf(stderr,
                "[EXTRACT] base=0x%04X span=0x%04X dst=0x%04X tag=0x%04X skip=0x%04X off=0x%04X guard=0x%04X mode=0x%04X effBase=0x%04X effSpan=0x%04X selOff=0x%04X selTag=0x%02X selCount=0x%02X payloadBytes=0x%04X -> 0x%04X\n",
                base_handle,
                available_span,
                destination_handle,
                record_tag_selector,
                record_skip_count,
                initial_word_offset,
                non_zero_guard,
                mode,
                effective_base,
                effective_span,
                selection.word_offset,
                selection.tag,
                selection.component_count,
                selection.payload_byte_length,
                result);
    }

    lpst_exec_push(state, result);
}
