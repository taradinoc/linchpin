/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_dispatch.c — Extended opcode (EXT / 0x5F) dispatch.
 *
 * The EXT opcode reads one more byte to select a sub-operation:
 *
 *   0x01  XOR
 *   0x02  NOT (bitwise complement)
 *   0x05  STRPOS — search a VM string for a byte character; result is
 *                  1-based position or FALSE_SENTINEL if not found
 *   0x06  STOREG — store TOS into program global[immediate byte]
 *   0x09  LONGJMPR — restore a SETJMP snapshot and push a return value
 *   0x0A  LONGJMP  — restore a SETJMP snapshot (no return value pushed)
 *   0x0B  SETJMP   — snapshot the full VM state; push the snapshot token
 *   0x0C  OPEN     — open a file channel
 *   0x0D  CLOSE    — close a file channel
 *   0x0E  SEQREAD  — sequential byte read from a channel
 *   0x0F  SEQWRITE — sequential byte write to a channel
 *   0x10  READREC  — random-access record read
 *   0x11  WRITEREC — random-access record write
 *   0x12  DISP     — display attribute (style/colour) control
 *   0x13  XDISP    — extended display: scroll, fill, or draw a separator bar
 *                    (sub-opcode in the following byte)
 *   0x14  FSIZE    — query file size
 *   0x15  UNLINK   — delete a file
 *   0x16  DISCARD  — pop and discard the most recent multi-value return
 *   0x17  POLLKEY  — poll host keyboard; push key code or FALSE_SENTINEL
 *   0x18-0x1B  floating-point add/sub/mul/div
 *   0x1C  TUPLE-POP — advance tuple-stack pointer (reclaim a frame)
 *   0x1D  REAL-LOG
 *   0x1E  REAL-EXP
 *   0x1F-0x20  case-insensitive string compare (two variants)
 *   0x21  WPRINTV — windowed print of a substring
 *   0x22  SETWIN  — set window descriptor
 *   0x23  STRCMP  — lexicographic VM-string compare (sort-key aware)
 *   0x24  MEMCMP  — byte-by-byte payload compare
 *   0x25  MEMCMP2 — raw-offset byte compare against payload
 *   0x34  bit-field ops (sub-dispatched)
 *   0x37  structured record ops (sub-dispatched)
 */
#include "lpst_run_internal.h"

void dispatch_extended(lpst_exec_state *state)
{
    uint8_t ext_op = fetch_byte(state);
    uint8_t operand_u8;
    uint16_t val, val2;
    uint16_t ext_pc = (uint16_t)(state->program_counter - 2);

    if (state->trace_enabled) {
        if ((state->current_module_id == 8 && ext_pc >= 0x0F60 && ext_pc <= 0x12C0)
            || (state->current_module_id == 7 && ext_pc >= 0x0800 && ext_pc <= 0x0850)
            || (state->current_module_id == 4 && ext_pc >= 0x05D0 && ext_pc <= 0x0640)) {
            fprintf(stderr,
                    "[EXT_ENTER] mod=%u pc=0x%04X ext=0x%02X stk=%u top=[0x%04X,0x%04X,0x%04X,0x%04X]\n",
                    state->current_module_id,
                    ext_pc,
                    ext_op,
                    state->eval_stack_top,
                    state->eval_stack_top > 0 ? state->eval_stack[state->eval_stack_top - 1] : 0,
                    state->eval_stack_top > 1 ? state->eval_stack[state->eval_stack_top - 2] : 0,
                    state->eval_stack_top > 2 ? state->eval_stack[state->eval_stack_top - 3] : 0,
                    state->eval_stack_top > 3 ? state->eval_stack[state->eval_stack_top - 4] : 0);
        }
    }

    switch (ext_op) {
    case 0x01: /* XOR */
        val = lpst_exec_pop(state);
        val2 = lpst_exec_pop(state);
        lpst_exec_push(state, val ^ val2);
        break;

    case 0x02: /* NOT (bitwise complement) */
        val = lpst_exec_pop(state);
        lpst_exec_push(state, (uint16_t)~val);
        break;

    case 0x05:
    /* STRPOS: search VM string aggregate for a byte character.
     * Stack: [handle, search_char].  Result: 1-based position, or
     * FALSE_SENTINEL if the character is not found. */
    {
        uint16_t handle = lpst_exec_pop(state);
        uint16_t search_char = lpst_exec_pop(state);
        if (handle == LPST_FALSE_SENTINEL) {
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
        } else {
            uint16_t str_len = read_aggregate_word(state, handle, 0);
            uint16_t found = LPST_FALSE_SENTINEL;
            uint8_t target = (uint8_t)(search_char & 0xFFu);
            uint16_t si;
            for (si = 0; si < str_len; si++) {
                uint8_t current = read_aggregate_payload_byte(state, handle, (int)si);
                if (current == 0) {
                    break;
                }

                if (current == target) {
                    found = (uint16_t)(si + 1);
                    break;
                }
            }
            lpst_exec_push(state, found);
        }
        break;
    }

    case 0x06: /* STOREG: pop value, store in program global[immediate byte] */
        operand_u8 = fetch_byte(state);
        val = lpst_exec_pop(state);
        if (operand_u8 < state->program_global_count) {
            state->program_globals[operand_u8] = val;
        }
        break;

    case 0x09:
    /* LONGJMPR: unwind to a saved snapshot and push a return value.
     * Stack: [return_value, token]. */
    {
        uint16_t return_val = lpst_exec_pop(state);
        uint16_t token = lpst_exec_pop(state);
        restore_jump_snapshot(state, token, true, return_val);
        break;
    }

    case 0x0A:
    /* LONGJMP: unwind to a saved snapshot (no return value pushed).
     * Stack: [token]. */
    {
        uint16_t token = lpst_exec_pop(state);
        restore_jump_snapshot(state, token, false, 0);
        break;
    }

    case 0x0B:
    /* SETJMP: snapshot the full VM state now and push the token.
     * The two inline 16-bit operands are passed to save_jump_snapshot. */
    {
        uint16_t op0 = fetch_le16(state);
        uint16_t op1 = fetch_le16(state);
        save_jump_snapshot(state, op0, op1);
        break;
    }

    case 0x0C: /* OPEN: open a file; push channel ID or FALSE_SENTINEL */
        handle_ext_open(state);
        break;

    case 0x0D: /* CLOSE: close a file channel */
        handle_ext_close(state);
        break;

    case 0x0E: /* SEQREAD: read next byte from active channel */
        handle_ext_seqread(state);
        break;

    case 0x0F: /* SEQWRITE: write a byte to the active channel */
        handle_ext_seqwrite(state);
        break;

    case 0x10: /* READREC: random-access record read */
        handle_ext_readrec(state);
        break;

    case 0x11: /* WRITEREC: random-access record write */
        handle_ext_writerec(state);
        break;

    case 0x12: /* DISP: set display style/colour attribute */
        operand_u8 = fetch_byte(state);
        host_apply_disp(state, operand_u8);
        break;

    case 0x13:
    /* XDISP: extended display operation.  The sub-opcode in the next byte
     * selects the operation; val is a line (or character) count:
     *   0x00  scroll up   from cur_row to scroll_bot by val lines
     *   0x01  scroll down from cur_row to scroll_bot by val lines
     *   0x02-0x03  (no-op)
     *   0x04  scroll up   the full scroll region by val lines
     *   0x05  scroll down the full scroll region by val lines
     *   0x06  draw a horizontal separator bar of val dashes starting at cursor */
        operand_u8 = fetch_byte(state);
        val = lpst_exec_pop(state);
        {
            lpst_host *host = state->host;
            uint8_t scroll_top = clamp_cursor_row_value(load_module_global(state, 0xC5));
            uint8_t scroll_bot = clamp_cursor_row_value(load_module_global(state, 0xC4));
            uint8_t cur_row = state->cursor_row;
            uint8_t lines = (uint8_t)(val & 0xFFu);

            switch (operand_u8) {
            case 0x00:
                if (lines > 0 && cur_row <= scroll_bot) {
                    host->scroll_up(host, cur_row, scroll_bot, lines);
                }
                break;
            case 0x01:
                if (lines > 0 && cur_row <= scroll_bot) {
                    host->scroll_down(host, cur_row, scroll_bot, lines);
                }
                break;
            case 0x02:
            case 0x03:
                break;
            case 0x04:
                if (lines > 0 && scroll_top <= scroll_bot) {
                    host->scroll_up(host, scroll_top, scroll_bot, lines);
                }
                break;
            case 0x05:
                if (lines > 0 && scroll_top <= scroll_bot) {
                    host->scroll_down(host, scroll_top, scroll_bot, lines);
                }
                break;
            case 0x06:
            {
                uint8_t col = state->cursor_col;
                uint8_t i;
                uint8_t width = get_visible_screen_width(state);
                for (i = 0; i < lines && col + i < width; i++) {
                    host->put_char(host, '-', cur_row, (uint8_t)(col + i));
                }
                state->cursor_col = (uint8_t)((col + lines < width)
                    ? (col + lines)
                    : (width - 1));
                break;
            }
            default:
                fprintf(stderr, "XDISP subop 0x%02X value 0x%04X (unhandled)\n",
                        operand_u8, val);
                break;
            }

            state->cursor_row = cur_row;
            host->set_cursor(host, state->cursor_row, state->cursor_col);
        }
        lpst_exec_push(state, val);
        break;

    case 0x14: /* FSIZE: push the size of an open channel's file */
        handle_ext_fsize(state);
        break;

    case 0x15: /* UNLINK: delete a file by name */
        handle_ext_unlink(state);
        break;

    case 0x16:
    /* DISCARD: pop and throw away the last multi-value return.
     * last_return_word_count was recorded by do_return; one pop per word. */
        {
            uint16_t count = state->last_return_word_count;
            uint16_t i;
            for (i = 0; i < count; i++) {
                lpst_exec_pop(state);
            }
        }
        break;

    case 0x17:
    {
        uint16_t mg_c0 = state->system_module_globals[0xC0];
        if (mg_c0 != 0) {
            val = UINT16_C(0x8001);
        } else {
            val = state->host->poll_key(state->host);
        }
        if (val != LPST_FALSE_SENTINEL) {
            record_recent_word_event(
                state->recent_kbinput,
                LPST_RECENT_INPUT_EVENTS,
                &state->recent_kbinput_next,
                &state->recent_kbinput_count,
                state->current_module_id,
                (uint16_t)(state->program_counter - 2),
                val);
        }
        lpst_exec_push(state, val);
        break;
    }

    case 0x18:
        lpst_exec_push(state, execute_real_add(state));
        break;

    case 0x19:
        lpst_exec_push(state, execute_real_subtract(state));
        break;

    case 0x1A:
        lpst_exec_push(state, execute_real_multiply(state));
        break;

    case 0x1B:
        lpst_exec_push(state, execute_real_divide(state));
        break;

    case 0x1C:
    {
        uint16_t word_count = lpst_exec_pop(state);
        uint32_t byte_size = (uint32_t)(word_count > 0 ? word_count : 1) * 2u;
        state->tuple_stack_byte += byte_size;
        if (state->tuple_stack_byte > LPST_FULL_RAM_BYTES) {
            state->tuple_stack_byte = LPST_FULL_RAM_BYTES;
        }
        break;
    }

    case 0x1D:
        lpst_exec_push(state, execute_real_log(state));
        break;

    case 0x1E:
        lpst_exec_push(state, execute_real_exp(state));
        break;

    case 0x1F:
    {
        uint16_t first_length = lpst_exec_pop(state);
        uint16_t first_offset = lpst_exec_pop(state);
        uint16_t second_length = lpst_exec_pop(state);
        uint16_t first_handle = lpst_exec_pop(state);
        uint16_t second_handle = lpst_exec_pop(state);
        lpst_exec_push(state, compare_vm_strings_ignore_case(
            state,
            first_handle,
            first_offset,
            first_length,
            second_handle,
            second_length));
        break;
    }

    case 0x20:
    {
        uint16_t first_length = lpst_exec_pop(state);
        uint16_t first_offset = lpst_exec_pop(state);
        uint16_t second_length = lpst_exec_pop(state);
        uint16_t first_handle = (uint16_t)(lpst_exec_pop(state) - 1);
        uint16_t second_handle = lpst_exec_pop(state);
        lpst_exec_push(state, compare_vm_strings_ignore_case(
            state,
            first_handle,
            first_offset,
            first_length,
            second_handle,
            second_length));
        break;
    }

    case 0x21:
    {
        uint16_t src_offset = lpst_exec_pop(state);
        uint16_t char_count = lpst_exec_pop(state);
        uint16_t src_handle = lpst_exec_pop(state);
        uint16_t desc_handle = lpst_exec_pop(state);
        lpst_exec_push(state, execute_wprintv(state, desc_handle, src_handle, char_count, src_offset));
        break;
    }

    case 0x22:
    {
        uint16_t desc_handle = lpst_exec_pop(state);
        lpst_exec_push(state, execute_setwin(state, desc_handle));
        break;
    }

    case 0x23:
    {
        uint16_t second_handle = lpst_exec_pop(state);
        uint16_t first_handle = lpst_exec_pop(state);
        lpst_exec_push(state, compare_vm_strings(state, first_handle, second_handle));
        break;
    }

    case 0x24:
    {
        uint16_t byte_count = lpst_exec_pop(state);
        uint16_t first_handle = lpst_exec_pop(state);
        uint16_t second_handle = lpst_exec_pop(state);
        int cmp_result = 0;
        uint16_t ci;
        for (ci = 0; ci < byte_count; ci++) {
            uint8_t a = read_aggregate_payload_byte(state, first_handle, ci);
            uint8_t b = read_aggregate_payload_byte(state, second_handle, ci);
            if (a < b) { cmp_result = 1; break; }
            if (a > b) { cmp_result = -1; break; }
        }
        lpst_exec_push(state, (uint16_t)(int16_t)cmp_result);
        break;
    }

    case 0x25:
    {
        uint16_t first_offset = lpst_exec_pop(state);
        uint16_t byte_count = lpst_exec_pop(state);
        uint16_t first_handle = lpst_exec_pop(state);
        uint16_t second_handle = lpst_exec_pop(state);
        uint32_t base1 = handle_to_byte_offset(first_handle);
        int cmp_result = 0;
        uint16_t ci;
        for (ci = 0; ci < byte_count; ci++) {
            uint8_t a = read_ram_byte(state, base1 + first_offset + ci);
            uint8_t b = read_aggregate_payload_byte(state, second_handle, ci);
            if (a < b) { cmp_result = 1; break; }
            if (a > b) { cmp_result = -1; break; }
        }
        lpst_exec_push(state, (uint16_t)(int16_t)cmp_result);
        break;
    }

    case 0x26:
    {
        uint16_t offset = lpst_exec_pop(state);
        uint16_t handle = lpst_exec_pop(state);
        uint8_t key_length = read_aggregate_raw_byte(state, handle, (int)(offset - 1));
        uint8_t data_length = read_aggregate_raw_byte(state, handle, (int)(offset + key_length + 1));
        uint16_t advance = (uint16_t)(key_length + data_length + 3);
        lpst_exec_push(state, (uint16_t)(offset + advance));
        break;
    }

    case 0x27:
        operand_u8 = fetch_byte(state);
        val = (uint16_t)(load_module_global(state, operand_u8) - 1);
        store_module_global(state, operand_u8, val);
        lpst_exec_push(state, val);
        break;

    case 0x28:
        operand_u8 = fetch_byte(state);
        if (operand_u8 < state->program_global_count) {
            val = (uint16_t)(state->program_globals[operand_u8] - 1);
            state->program_globals[operand_u8] = val;
            lpst_exec_push(state, val);
        }
        break;

    case 0x29:
        operand_u8 = fetch_byte(state);
        {
            uint8_t i;
            for (i = 0; i < operand_u8; i++) {
                lpst_exec_pop(state);
            }
        }
        break;

    case 0x2A:
        operand_u8 = fetch_byte(state);
        if (operand_u8 < state->program_global_count) {
            val = (uint16_t)(state->program_globals[operand_u8] + 1);
            state->program_globals[operand_u8] = val;
            lpst_exec_push(state, val);
        }
        break;

    case 0x2B:
        operand_u8 = fetch_byte(state);
        val = (uint16_t)(load_module_global(state, operand_u8) + 1);
        store_module_global(state, operand_u8, val);
        lpst_exec_push(state, val);
        break;

    case 0x2C:
        operand_u8 = fetch_byte(state);
        val = lpst_exec_peek(state);
        store_module_global(state, operand_u8, val);
        break;

    case 0x2D:
        operand_u8 = fetch_byte(state);
        val = lpst_exec_peek(state);
        if (operand_u8 < state->program_global_count) {
            state->program_globals[operand_u8] = val;
        }
        break;

    case 0x2E:
        operand_u8 = fetch_byte(state);
        {
            uint16_t results[LPST_MAX_LOCALS];
            int i;
            for (i = operand_u8 - 1; i >= 0; i--) {
                results[i] = lpst_exec_pop(state);
            }
            do_return(state, results, operand_u8);
        }
        break;

    case 0x2F:
        val = lpst_exec_pop(state);
        host_print_char(state, (uint8_t)(val & 0xFFu));
        break;

    case 0x33:
    {
        uint16_t in_val = lpst_exec_pop(state);
        uint8_t ch = (uint8_t)(in_val & 0xFFu);
        lpst_exec_push(state, (ch >= 0x20 && ch < 0x7F) ? (uint16_t)ch : (uint16_t)' ');
        break;
    }

    case 0x36:
    {
        uint16_t packed_key = lpst_exec_pop(state);
        uint16_t root_handle = load_module_global(state, 0xD8);
        int low_index = packed_key & 0x7F;
        int high_index = (packed_key >> 8) & 0xFF;
        uint16_t primary_table_handle;
        uint16_t secondary_table_handle;
        uint16_t relative_descriptor;

        if (state->trace_enabled) {
            fprintf(stderr,
                "[LOOKUP_ENTER] key=0x%04X root=0x%04X pc=0x%04X\n",
                packed_key,
                root_handle,
                state->program_counter - 2);
        }

        record_recent_word_event(
            state->recent_ext36,
            LPST_RECENT_EXT36_EVENTS,
            &state->recent_ext36_next,
            &state->recent_ext36_count,
            state->current_module_id,
            (uint16_t)(state->program_counter - 2),
            packed_key);

        if (root_handle == LPST_FALSE_SENTINEL || root_handle == 0) {
            fprintf(stderr,
                    "LOOKUP requires initialized system slot 0xD8 at PC 0x%04X\n",
                    state->program_counter - 2);
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            break;
        }

        if (low_index <= 0 || high_index <= 0) {
            fprintf(stderr,
                    "LOOKUP received invalid packed key 0x%04X at PC 0x%04X\n",
                    packed_key,
                    state->program_counter - 2);
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            break;
        }

        primary_table_handle = read_aggregate_word(state, root_handle, 0);
        secondary_table_handle = read_aggregate_word(state, primary_table_handle, 2 * (low_index - 1));

        if (secondary_table_handle == 0 || secondary_table_handle == LPST_FALSE_SENTINEL) {
            uint16_t resolver_selector = read_aggregate_word(state, root_handle, 3);
            uint16_t return_pc = state->program_counter;

            if (state->trace_enabled) {
                fprintf(stderr,
                        "[LOOKUP] key=0x%04X root=0x%04X table0=0x%04X table1=0x%04X -> fallback selector 0x%04X\n",
                        packed_key,
                        root_handle,
                        primary_table_handle,
                        secondary_table_handle,
                        resolver_selector);
            }

            lpst_exec_push(state, packed_key);
            if (!enter_far_call(state, resolver_selector, 1, return_pc)) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            }
            break;
        }

        relative_descriptor = (uint16_t)(read_aggregate_word(state, secondary_table_handle, high_index - 1) & 0x3FFFu);

        if (relative_descriptor != 0x3FFFu) {
            uint16_t descriptor_base_handle = read_aggregate_word(state, root_handle, 2);
            uint16_t result = (uint16_t)(descriptor_base_handle + relative_descriptor);

            if (state->trace_enabled) {
                fprintf(stderr,
                        "[LOOKUP] key=0x%04X root=0x%04X table0=0x%04X table1=0x%04X rel=0x%04X -> 0x%04X\n",
                        packed_key,
                        root_handle,
                        primary_table_handle,
                        secondary_table_handle,
                        relative_descriptor,
                        result);
            }

            lpst_exec_push(state, result);
            break;
        }

        {
            uint16_t resolver_selector = read_aggregate_word(state, root_handle, 3);
            uint16_t return_pc = state->program_counter;

            if (state->trace_enabled) {
                fprintf(stderr,
                        "[LOOKUP] key=0x%04X root=0x%04X -> fallback selector 0x%04X\n",
                        packed_key,
                        root_handle,
                        resolver_selector);
            }

            lpst_exec_push(state, packed_key);
            if (!enter_far_call(state, resolver_selector, 1, return_pc)) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            }
        }
        break;
    }

    case 0x37:
        handle_ext37(state);
        break;

    case 0x34:
        handle_ext34(state);
        break;

    case 0x30:
    {
        uint16_t dst_offset, word_count, dst_handle, src_handle;
        int wi;

        if (state->eval_stack_top < 4) {
            if (state->trace_enabled) {
                fprintf(stderr, "[UNPACK] stack depth %u < 4 -> FALSE\n",
                        state->eval_stack_top);
            }
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
            break;
        }

        dst_offset  = lpst_exec_pop(state);
        word_count  = lpst_exec_pop(state);
        dst_handle  = lpst_exec_pop(state);
        src_handle  = lpst_exec_pop(state);

        for (wi = 0; wi < (int)word_count; wi++) {
            uint16_t packed = read_aggregate_word(state, src_handle, wi);
            uint8_t first  = (uint8_t)((packed >> 10) & 0x1F);
            uint8_t second = (uint8_t)((packed >> 5)  & 0x1F);
            uint8_t third  = (uint8_t)(packed & 0x1F);

            write_aggregate_payload_byte(state, dst_handle, dst_offset++, first);
            write_aggregate_payload_byte(state, dst_handle, dst_offset++, second);

            if (packed & 0x8000u) {
                write_aggregate_payload_byte(state, dst_handle, dst_offset++, (uint8_t)(third | 0x80));
                if (state->trace_enabled) {
                    fprintf(stderr, "[UNPACK] src=0x%04X dst=0x%04X words=%u terminated at word %d offset=0x%04X\n",
                            src_handle, dst_handle, word_count, wi, dst_offset);
                }
                lpst_exec_push(state, LPST_FALSE_SENTINEL);
                goto unpack_done;
            }

            write_aggregate_payload_byte(state, dst_handle, dst_offset++, third);
        }

        if (state->trace_enabled) {
            fprintf(stderr, "[UNPACK] src=0x%04X dst=0x%04X words=%u -> offset=0x%04X\n",
                    src_handle, dst_handle, word_count, dst_offset);
        }
        lpst_exec_push(state, dst_offset);
        unpack_done:
        break;
    }

    default:
        fprintf(stderr, "unhandled extended opcode 0x5F 0x%02X at PC 0x%04X\n",
                ext_op, state->program_counter - 2);
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        break;
    }
}
