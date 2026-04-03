/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run.c — Main VM fetch-decode-execute loop.
 *
 * Opcodes are dispatched in four ranges before the general switch:
 *
 *   0xE0-0xFF  STOREL-peek(n):  store top-of-stack into local n, keep TOS
 *   0xC0-0xDF  STOREL-pop(n):   pop TOS into local n
 *   0xA0-0xBF  LOADL(n):        push local n (or FALSE_SENTINEL) onto stack
 *   0x60-0x9F  VLOADW:          push word_index from aggregate held in local n
 *                                (opcode bits [5:4] = word_index, [3:0] = local n)
 *
 * All other opcodes go through the switch statement below.
 *
 * The trace branches that print specific PC ranges are diagnostic aids for
 * investigating Cornerstone behaviour; they do not affect correctness.
 */
#include "lpst_run_internal.h"

lpst_result lpst_run(lpst_exec_state *state)
{
    unsigned ti;

    if (state == NULL) {
        return LPST_ERR_NULL_ARG;
    }

    while (!state->is_halted) {
        uint16_t instruction_start;
        uint8_t opcode;
        uint16_t val, val2;
        int16_t sval, sval2;
        uint8_t operand_u8;
        uint16_t operand_u16;
        uint16_t handle;
        uint16_t target;

        if (state->instruction_limit > 0 &&
            state->instruction_count >= state->instruction_limit) {
            fprintf(stderr, "instruction limit reached (%u)\n",
                    state->instruction_limit);
            return LPST_OK;
        }

        instruction_start = state->program_counter;
        opcode = fetch_byte(state);
        state->instruction_count++;

        record_recent_m1_startup_event(state, instruction_start, opcode);

        if (state->current_module_id == 2
            && (instruction_start == 0x04DD || instruction_start == 0x0556)) {
            record_recent_p201_event(state, instruction_start);
        }

        record_m5p128_trace_event(state, instruction_start);

        if (state->trace_enabled) {
            fprintf(stderr, "[%u] mod=%u pc=0x%04X op=0x%02X stk=%u call=%u top=[",
                    state->instruction_count,
                    state->current_module_id,
                    instruction_start,
                    opcode,
                    state->eval_stack_top,
                    state->call_stack_top);
            for (ti = 0; ti < 4 && ti < state->eval_stack_top; ti++) {
                if (ti) fprintf(stderr, ",");
                fprintf(stderr, "0x%04X",
                        state->eval_stack[state->eval_stack_top - 1 - ti]);
            }
            fprintf(stderr, "]\n");

            if ((state->current_module_id == 7 &&
                 (instruction_start == 0x0334 ||
                  instruction_start == 0x0183 ||
                  instruction_start == 0x0192 ||
                  instruction_start == 0x00CB ||
                  instruction_start == 0x012A ||
                  instruction_start == 0x0817 ||
                  instruction_start == 0x0825))
                || (state->current_module_id == 4 &&
                    (instruction_start == 0x05DC ||
                     instruction_start == 0x05EE ||
                     instruction_start == 0x0625 ||
                     instruction_start == 0x0631))
                || (state->current_module_id == 8 &&
                    (instruction_start == 0x6A2B ||
                     instruction_start == 0x6B68 ||
                     instruction_start == 0x69E3))) {
                unsigned li;

                fprintf(stderr, "    locals(");
                for (li = 0; li < state->current_frame.local_count; li++) {
                    if (li) {
                        fprintf(stderr, ",");
                    }
                    fprintf(stderr, "L%u=0x%04X", li, state->local_storage[li]);
                }
                fprintf(stderr, ")\n");
            }
        }

        /* STOREL-peek: store top-of-stack in local[n] without consuming it. */
        if (opcode >= 0xE0u) {
            operand_u8 = opcode & 0x1Fu;
            val = lpst_exec_peek(state);
            if (operand_u8 < state->current_frame.local_count) {
                state->local_storage[operand_u8] = val;
            }
            continue;
        }

        /* STOREL-pop: pop top-of-stack into local[n]. */
        if (opcode >= 0xC0u) {
            operand_u8 = opcode & 0x1Fu;
            val = lpst_exec_pop(state);
            if (operand_u8 < state->current_frame.local_count) {
                state->local_storage[operand_u8] = val;
            }
            continue;
        }

        /* LOADL: push local[n] (FALSE_SENTINEL if n is out of range). */
        if (opcode >= 0xA0u) {
            operand_u8 = opcode & 0x1Fu;
            val = (operand_u8 < state->current_frame.local_count)
                ? state->local_storage[operand_u8]
                : LPST_FALSE_SENTINEL;
            lpst_exec_push(state, val);
            continue;
        }

        /* VLOADW: pop an aggregate handle from local n and push word[word_index].
         * Halts with error 0xFFFF if the handle is FALSE_SENTINEL. */
        if (opcode >= 0x60u) {
            uint8_t word_index = (opcode >> 4) & 0x03u;
            uint8_t local_index = opcode & 0x0Fu;
            uint16_t packed_handle;

            packed_handle = (local_index < state->current_frame.local_count)
                ? state->local_storage[local_index]
                : LPST_FALSE_SENTINEL;

            if (packed_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "VLOADW from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
                continue;
            }

            {
                uint16_t pv_value = read_aggregate_word(state, packed_handle, word_index);
                lpst_exec_push(state, pv_value);
            }
            continue;
        }

        switch (opcode) {
        case 0x00: /* BREAK — unconditional halt (debug breakpoint) */
            fprintf(stderr, "BREAK at PC 0x%04X\n", instruction_start);
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            break;

        case 0x01: /* ADD */
        case 0x53: /* ADD alias — same implementation as 0x01 */
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            lpst_exec_push(state, (uint16_t)(val + val2));
            break;

        case 0x02: /* SUB */
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            lpst_exec_push(state, (uint16_t)(val - val2));
            break;

        case 0x03: /* IMUL */
            sval2 = (int16_t)lpst_exec_pop(state);
            sval = (int16_t)lpst_exec_pop(state);
            lpst_exec_push(state, (uint16_t)(sval * sval2));
            break;

        case 0x04: /* IDIV — truncates toward zero */
            sval2 = (int16_t)lpst_exec_pop(state);
            sval = (int16_t)lpst_exec_pop(state);
            lpst_exec_push(state, sval2 == 0 ? 0 : (uint16_t)(sval / sval2));
            break;

        case 0x05: /* FLOORMOD — floored modulus matching MME behaviour */
        {
            /* MME uses IDIV then adjusts a negative remainder by adding
               the divisor, producing a floored/Euclidean modulus. */
            int16_t mod_divisor = (int16_t)lpst_exec_pop(state);
            int16_t mod_dividend = (int16_t)lpst_exec_pop(state);
            if (mod_divisor == 0) {
                lpst_exec_push(state, 0);
            } else {
                int16_t mod_rem = (int16_t)(mod_dividend % mod_divisor);
                if (mod_rem < 0)
                    mod_rem = (int16_t)(mod_rem + mod_divisor);
                lpst_exec_push(state, (uint16_t)mod_rem);
            }
            break;
        }

        case 0x06: /* NEGATE — arithmetic negation */
            sval = (int16_t)lpst_exec_pop(state);
            lpst_exec_push(state, (uint16_t)(-sval));
            break;

        case 0x07: /* SHIFT — positive count left-shifts, negative arithmetic right-shifts */
            sval2 = (int16_t)lpst_exec_pop(state);
            sval = (int16_t)lpst_exec_pop(state);
            if (sval2 >= 0) {
                lpst_exec_push(state, (uint16_t)(sval << sval2));
            } else {
                lpst_exec_push(state, (uint16_t)(sval >> (-sval2)));
            }
            break;

        case 0x08: /* INCL — increment local[operand] and push new value */
            operand_u8 = fetch_byte(state);
            if (operand_u8 < state->current_frame.local_count) {
                val = (uint16_t)(state->local_storage[operand_u8] + 1);
                state->local_storage[operand_u8] = val;
                lpst_exec_push(state, val);
            }
            break;

        case 0x0B: /* DECL — decrement local[operand] and push new value */
            operand_u8 = fetch_byte(state);
            if (operand_u8 < state->current_frame.local_count) {
                val = (uint16_t)(state->local_storage[operand_u8] - 1);
                state->local_storage[operand_u8] = val;
                lpst_exec_push(state, val);
            }
            break;

        case 0x0E: /* AND — bitwise AND */
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            lpst_exec_push(state, val & val2);
            break;

        case 0x0F: /* OR — bitwise OR */
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            lpst_exec_push(state, val | val2);
            break;

        case 0x10: /* USHFT — unsigned shift (positive = left, negative = unsigned right) */
            sval2 = (int16_t)lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            if (sval2 >= 0) {
                lpst_exec_push(state, (uint16_t)(val << sval2));
            } else {
                lpst_exec_push(state, (uint16_t)(val >> (-sval2)));
            }
            break;

        case 0x11: /* ALLOCV — allocate a heap vector of word_count words */
        {
            uint16_t word_count = lpst_exec_pop(state);
            uint16_t allocated_handle = allocate_vector(state, word_count);
            if (allocated_handle == LPST_FALSE_SENTINEL) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            }
            lpst_exec_push(state, allocated_handle);
            break;
        }

        case 0x12: /* ALLOCV+FILL — allocate vector and pop word_count values into it */
        {
            uint16_t word_count = lpst_exec_pop(state);
            uint16_t allocated_handle;
            int vi;

            allocated_handle = allocate_vector(state, word_count);
            if (allocated_handle == LPST_FALSE_SENTINEL) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
                lpst_exec_push(state, allocated_handle);
                break;
            }

            for (vi = (int)word_count - 1; vi >= 0; vi--) {
                write_aggregate_word(state, allocated_handle, vi, lpst_exec_pop(state));
            }

            lpst_exec_push(state, allocated_handle);
            break;
        }

        case 0x13: /* FREEV — return vector to the free list */
        {
            uint16_t size_words = lpst_exec_pop(state);
            uint16_t allocated_handle = lpst_exec_pop(state);
            release_vector(state, allocated_handle, size_words);
            break;
        }

        case 0x14: /* ALLOC-TUPLE — allocate a tuple (high-segment) vector */
        {
            uint16_t word_count = lpst_exec_pop(state);
            uint16_t allocated_handle = allocate_tuple(state, word_count);
            if (allocated_handle == LPST_FALSE_SENTINEL) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            }
            lpst_exec_push(state, allocated_handle);
            break;
        }

        case 0x15: /* ALLOC-TUPLE+FILL — allocate tuple and pop word_count values into it */
        {
            uint16_t word_count = lpst_exec_pop(state);
            uint16_t allocated_handle;
            int vi;

            allocated_handle = allocate_tuple(state, word_count);
            if (allocated_handle == LPST_FALSE_SENTINEL) {
                state->is_halted = true;
                state->halt_code = 0xFFFF;
                lpst_exec_push(state, allocated_handle);
                break;
            }

            for (vi = (int)word_count - 1; vi >= 0; vi--) {
                write_aggregate_word(state, allocated_handle, vi, lpst_exec_pop(state));
            }

            lpst_exec_push(state, allocated_handle);
            break;
        }

        case 0x16: /* VLOADW-dynamic — pop handle and 1-based index, push word */
        {
            uint16_t index = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            int resolved_index = (int)index;
            if (dynamic_handle != 0 && dynamic_handle != LPST_FALSE_SENTINEL && index > 0) {
                resolved_index = (int)(index - 1);
            }
            if (dynamic_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "VLOADW from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            } else {
                lpst_exec_push(state, read_aggregate_word(state, dynamic_handle, resolved_index));
            }
            break;
        }

        case 0x17: /* VLOADB-dynamic — pop handle and 1-based byte index, push byte */
        {
            int16_t index = (int16_t)lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "VLOADB from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            } else {
                lpst_exec_push(state, read_aggregate_payload_byte(state, dynamic_handle, index - 1));
            }
            break;
        }

        case 0x18: /* VLOADW-imm — pop handle, push word[immediate_index] */
            operand_u8 = fetch_byte(state);
        {
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "VLOADW_ from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            } else {
                uint16_t vw_val = read_aggregate_word(state, dynamic_handle, operand_u8);
                lpst_exec_push(state, vw_val);
            }
            break;
        }

        case 0x19: /* VLOADB-imm — pop handle, push byte[immediate_offset] */
            operand_u8 = fetch_byte(state);
        {
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "VLOADB_ from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            } else {
                lpst_exec_push(state, read_aggregate_payload_byte(state, dynamic_handle, operand_u8));
            }
            break;
        }

        case 0x1A: /* VSTOREW-dynamic — pop value, 1-based index, handle; write word */
        {
            uint16_t wval = lpst_exec_pop(state);
            uint16_t index = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle != LPST_FALSE_SENTINEL) {
                int resolved_index = (int)index;
                if (dynamic_handle != 0 && index > 0) {
                    resolved_index = (int)(index - 1);
                }
                write_aggregate_word(state, dynamic_handle, resolved_index, wval);
            }
            break;
        }

        case 0x1B: /* VSTOREB-dynamic — pop value, 1-based byte index, handle; write byte */
        {
            uint16_t bval = lpst_exec_pop(state);
            int16_t index = (int16_t)lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle != LPST_FALSE_SENTINEL) {
                write_aggregate_payload_byte(state, dynamic_handle, index - 1, (uint8_t)(bval & 0xFFu));
            }
            break;
        }

        case 0x1C: /* VSTOREW-imm — pop value and handle, store at word[immediate_index] */
            operand_u8 = fetch_byte(state);
        {
            uint16_t wval = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle != LPST_FALSE_SENTINEL) {
                write_aggregate_word(state, dynamic_handle, operand_u8, wval);
            }
            break;
        }

        case 0x1D: /* VSTOREB-imm — pop value and handle, store at raw byte[immediate_offset] */
            operand_u8 = fetch_byte(state);
        {
            uint16_t bval = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            if (dynamic_handle != LPST_FALSE_SENTINEL) {
                uint32_t base = handle_to_byte_offset(dynamic_handle);
                write_ram_byte(state, base + 2u + operand_u8, (uint8_t)(bval & 0xFFu));
            }
            break;
        }

        case 0x1E: /* VFILLW — fill word_count words of aggregate with fill_val, push handle */
        {
            uint16_t fill_val = lpst_exec_pop(state);
            uint16_t count = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            uint16_t vi;
            for (vi = 0; vi < count; vi++) {
                write_aggregate_word(state, dynamic_handle, vi, fill_val);
            }
            lpst_exec_push(state, dynamic_handle);
            break;
        }

        case 0x1F: /* VFILLB — fill count bytes of aggregate with fill_val, push handle */
        {
            uint16_t fill_val = lpst_exec_pop(state);
            uint16_t count = lpst_exec_pop(state);
            uint16_t dynamic_handle = lpst_exec_pop(state);
            uint16_t vi;
            for (vi = 0; vi < count; vi++) {
                write_aggregate_payload_byte(state, dynamic_handle, vi, (uint8_t)(fill_val & 0xFFu));
            }
            lpst_exec_push(state, dynamic_handle);
            break;
        }

        case 0x20: /* COPYW — copy count words from src_handle to dst_handle */
        {
            uint16_t dst_handle = lpst_exec_pop(state);
            uint16_t count = lpst_exec_pop(state);
            uint16_t src_handle = lpst_exec_pop(state);

            move_ram_bytes(
                state,
                handle_to_byte_offset(dst_handle),
                handle_to_byte_offset(src_handle),
                (size_t)count * 2u);
            lpst_exec_push(state, dst_handle);
            break;
        }

        case 0x21: /* COPYB-off — copy count bytes at src+src_off to dst+dst_off */
        {
            int16_t dst_off = (int16_t)lpst_exec_pop(state);
            int16_t src_off = (int16_t)lpst_exec_pop(state);
            uint16_t dst_handle = lpst_exec_pop(state);
            uint16_t count = lpst_exec_pop(state);
            uint16_t src_handle = lpst_exec_pop(state);

            move_ram_bytes(
                state,
                handle_to_byte_offset(dst_handle) + 2u + (uint32_t)dst_off,
                handle_to_byte_offset(src_handle) + 2u + (uint32_t)src_off,
                count);
            lpst_exec_push(state, dst_handle);
            break;
        }

        case 0x22: /* LOADG — push program global[operand] */
            operand_u8 = fetch_byte(state);
            if (operand_u8 < state->program_global_count) {
                lpst_exec_push(state, state->program_globals[operand_u8]);
            } else {
                lpst_exec_push(state, LPST_FALSE_SENTINEL);
            }
            break;

        case 0x23: /* LOADMG — push module global[operand] */
            operand_u8 = fetch_byte(state);
            lpst_exec_push(state, load_module_global(state, operand_u8));
            break;


        /* Small integer constants pushed directly by opcode. */
        case 0x09: lpst_exec_push(state, 8); break; /* PUSH 8 */
        case 0x0A: lpst_exec_push(state, 4); break; /* PUSH 4 */
        case 0x0C: lpst_exec_push(state, 0xFFFFu); break; /* PUSH 0xFFFF */
        case 0x0D: lpst_exec_push(state, 3); break; /* PUSH 3 */
        case 0x24: lpst_exec_push(state, 2); break; /* PUSH 2 */

        case 0x25: /* PUSH16 — push an immediate 16-bit LE word */
            operand_u16 = fetch_le16(state);
            lpst_exec_push(state, operand_u16);
            break;

        case 0x26: /* PUSH8 — push an immediate unsigned byte */
            operand_u8 = fetch_byte(state);
            lpst_exec_push(state, (uint16_t)operand_u8);
            break;

        case 0x27: lpst_exec_push(state, LPST_FALSE_SENTINEL); break; /* PUSH FALSE */
        case 0x28: lpst_exec_push(state, 0); break; /* PUSH 0 */

        case 0x29: /* DUP — push a copy of the top-of-stack */
            val = lpst_exec_peek(state);
            lpst_exec_push(state, val);
            break;

        case 0x2A: lpst_exec_push(state, (uint16_t)(int16_t)-8); break; /* PUSH -8 */
        case 0x2B: lpst_exec_push(state, 5); break; /* PUSH 5 */
        case 0x2C: lpst_exec_push(state, 1); break; /* PUSH 1 */

        case 0x2D: /* STOREMG — pop value, store into module global[operand] */
            operand_u8 = fetch_byte(state);
            val = lpst_exec_pop(state);
            store_module_global(state, operand_u8, val);
            break;

        case 0x2E: lpst_exec_push(state, 0x00FFu); break; /* PUSH 0x00FF */

        case 0x2F: /* DROP — discard the top-of-stack value */
            lpst_exec_pop(state);
            break;

        case 0x30: /* JUMP — unconditional branch */
            target = decode_jump_target(state, instruction_start);
            state->program_counter = target;
            break;

        case 0x31: /* JZ — branch if TOS == 0 */
            target = decode_jump_target(state, instruction_start);
            val = lpst_exec_pop(state);
            if (val == 0) { state->program_counter = target; }
            break;

        case 0x32: /* JNZ — branch if TOS != 0 */
            target = decode_jump_target(state, instruction_start);
            val = lpst_exec_pop(state);
            if (val != 0) { state->program_counter = target; }
            break;

        case 0x33: /* JFALSE — branch if TOS == FALSE_SENTINEL */
            target = decode_jump_target(state, instruction_start);
            val = lpst_exec_pop(state);
            if (val == LPST_FALSE_SENTINEL) { state->program_counter = target; }
            break;

        case 0x34: /* JNOTFALSE — branch if TOS != FALSE_SENTINEL */
            target = decode_jump_target(state, instruction_start);
            val = lpst_exec_pop(state);
            if (val != LPST_FALSE_SENTINEL) { state->program_counter = target; }
            break;

        case 0x35: /* JGT — branch if (int16)TOS > 0 */
            target = decode_jump_target(state, instruction_start);
            sval = (int16_t)lpst_exec_pop(state);
            if (sval > 0) { state->program_counter = target; }
            break;

        case 0x36: /* JLE — branch if (int16)TOS <= 0 */
            target = decode_jump_target(state, instruction_start);
            sval = (int16_t)lpst_exec_pop(state);
            if (sval <= 0) { state->program_counter = target; }
            break;

        case 0x37: /* JLT — branch if (int16)TOS < 0 */
            target = decode_jump_target(state, instruction_start);
            sval = (int16_t)lpst_exec_pop(state);
            if (sval < 0) { state->program_counter = target; }
            break;

        case 0x38: /* JGE — branch if (int16)TOS >= 0 */
            target = decode_jump_target(state, instruction_start);
            sval = (int16_t)lpst_exec_pop(state);
            if (sval >= 0) { state->program_counter = target; }
            break;

        case 0x39: /* JLT2 — branch if stack[-1] < stack[-2] (signed) */
            target = decode_jump_target(state, instruction_start);
			sval2 = (int16_t)lpst_exec_pop(state);
			sval = (int16_t)lpst_exec_pop(state);
            if (sval2 < sval) { state->program_counter = target; }
            break;

        case 0x3A: /* JLE2 — branch if stack[-1] <= stack[-2] (signed) */
            target = decode_jump_target(state, instruction_start);
			sval2 = (int16_t)lpst_exec_pop(state);
			sval = (int16_t)lpst_exec_pop(state);
            if (sval2 <= sval) { state->program_counter = target; }
            break;

        case 0x3B: /* JGE2 — branch if stack[-1] >= stack[-2] (signed) */
            target = decode_jump_target(state, instruction_start);
			sval2 = (int16_t)lpst_exec_pop(state);
			sval = (int16_t)lpst_exec_pop(state);
            if (sval2 >= sval) { state->program_counter = target; }
            break;

        case 0x3C: /* JGT2 — branch if stack[-1] > stack[-2] (signed) */
            target = decode_jump_target(state, instruction_start);
			sval2 = (int16_t)lpst_exec_pop(state);
			sval = (int16_t)lpst_exec_pop(state);
            if (sval2 > sval) { state->program_counter = target; }
            break;

        case 0x3D: /* JEQ — branch if TOS == TOS-1 (bitwise equal) */
            target = decode_jump_target(state, instruction_start);
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            if (val == val2) { state->program_counter = target; }
            break;

        case 0x3E: /* JNE — branch if TOS != TOS-1 (bitwise not equal) */
            target = decode_jump_target(state, instruction_start);
            val2 = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            if (val != val2) { state->program_counter = target; }
            break;


        /* Near calls: target is a byte offset within the current module. */
        case 0x3F: /* CALL0 near — fetch 16-bit target, call with 0 return values */
            operand_u16 = fetch_le16(state);
            enter_near_call(state, operand_u16, 0, state->program_counter);
            break;

        case 0x40: /* CALL1 near — call with 1 return value */
            operand_u16 = fetch_le16(state);
            enter_near_call(state, operand_u16, 1, state->program_counter);
            break;

        case 0x41: /* CALL2 near — call with 2 return values */
            operand_u16 = fetch_le16(state);
            enter_near_call(state, operand_u16, 2, state->program_counter);
            break;

        case 0x42: /* CALL3 near — call with 3 return values */
            operand_u16 = fetch_le16(state);
            enter_near_call(state, operand_u16, 3, state->program_counter);
            break;

        case 0x43: /* CALLN near — return count in immediate byte, target on stack */
            operand_u8 = fetch_byte(state);
            val = lpst_exec_pop(state);
            enter_near_call(state, val, operand_u8, state->program_counter);
            break;


        /* Far calls: target is a module selector (module_id << 8 | proc_index). */
        case 0x44: /* CALL0 far — fetch selector, far call with 0 return values */
            operand_u16 = fetch_le16(state);
            enter_far_call(state, operand_u16, 0, state->program_counter);
            break;

        case 0x45: /* CALL1 far — far call with 1 return value */
            operand_u16 = fetch_le16(state);
            enter_far_call(state, operand_u16, 1, state->program_counter);
            break;

        case 0x46: /* CALL2 far — far call with 2 return values */
            operand_u16 = fetch_le16(state);
            enter_far_call(state, operand_u16, 2, state->program_counter);
            break;

        case 0x47: /* CALL3 far — far call with 3 return values */
            operand_u16 = fetch_le16(state);
            enter_far_call(state, operand_u16, 3, state->program_counter);
            break;

        case 0x48: /* CALLN far — return count in immediate byte, selector on stack */
            operand_u8 = fetch_byte(state);
            val = lpst_exec_pop(state);
            enter_far_call(state, val, operand_u8, state->program_counter);
            break;

        case 0x49: /* RET1 — pop one value and return it */
        {
            uint16_t result = lpst_exec_pop(state);
            do_return(state, &result, 1);
            break;
        }

        case 0x4A: /* RETFALSE — return FALSE_SENTINEL as the one result */
        {
            uint16_t result = LPST_FALSE_SENTINEL;
            do_return(state, &result, 1);
            break;
        }

        case 0x4B: /* RETZERO — return 0 as the one result */
        {
            uint16_t result = 0;
            do_return(state, &result, 1);
            break;
        }

        case 0x4C: lpst_exec_push(state, 6); break; /* PUSH 6 */

        case 0x4D: /* HALT — stop execution with the given halt code */
            operand_u16 = fetch_le16(state);
            state->is_halted = true;
            state->halt_code = operand_u16;
            break;

        case 0x4E: /* NEXTPAGE — skip to the start of the next 256-byte code page */
            state->program_counter = (uint16_t)((instruction_start & ~0xFFu) + 0x100u);
            load_code_page(state, current_module(state), state->program_counter);
            break;

        case 0x4F: lpst_exec_push(state, 7); break; /* PUSH 7 */

        case 0x50: /* PRINTS — print substring of a vector aggregate */
        {
            uint16_t start_off = lpst_exec_pop(state);
            uint16_t length = lpst_exec_pop(state);
            uint16_t print_handle = lpst_exec_pop(state);
            host_print_vector(state, print_handle, length, start_off);
            break;
        }

        case 0x51: /* LOADVB2 — push byte at raw offset index-1 from aggregate base */
        {
            int16_t index = (int16_t)lpst_exec_pop(state);
            uint16_t raw_handle = lpst_exec_pop(state);
            if (raw_handle == LPST_FALSE_SENTINEL) {
                fprintf(stderr, "LOADVB2 from FALSE at PC 0x%04X\n",
                        instruction_start);
                state->is_halted = true;
                state->halt_code = 0xFFFF;
            } else {
                uint32_t base = handle_to_byte_offset(raw_handle);
                uint16_t raw_value = read_ram_byte(state, base + (uint32_t)(index - 1));
                lpst_exec_push(state, raw_value);
            }
            break;
        }

        case 0x52: /* STOREVB2 — store byte at raw offset index-1 from aggregate base */
        {
            uint16_t bval = lpst_exec_pop(state);
            int16_t index = (int16_t)lpst_exec_pop(state);
            uint16_t raw_handle = lpst_exec_pop(state);
            if (raw_handle != LPST_FALSE_SENTINEL) {
                uint32_t base = handle_to_byte_offset(raw_handle);
                write_ram_byte(state, base + (uint32_t)(index - 1), (uint8_t)(bval & 0xFFu));
            }
            break;
        }

        case 0x54: /* INCL-safe — increment local only if value != FALSE_SENTINEL */
            operand_u8 = fetch_byte(state);
            if (operand_u8 < state->current_frame.local_count) {
                val = state->local_storage[operand_u8];
                if (val != LPST_FALSE_SENTINEL) {
                    state->local_storage[operand_u8] = (uint16_t)(val + 1);
                }
            }
            break;

        case 0x55: /* RETVOID — return from current procedure with no value */
            do_return(state, NULL, 0);
            break;

        case 0x56: /* STOREL — pop TOS into local[operand] (extended byte-operand form) */
            operand_u8 = fetch_byte(state);
            val = lpst_exec_pop(state);
            if (operand_u8 < state->current_frame.local_count) {
                state->local_storage[operand_u8] = val;
            }
            break;

        case 0x57: /* LOADL — push local[operand] (extended byte-operand form) */
            operand_u8 = fetch_byte(state);
            val = (operand_u8 < state->current_frame.local_count)
                ? state->local_storage[operand_u8]
                : LPST_FALSE_SENTINEL;
            lpst_exec_push(state, val);
            break;

        case 0x58: /* STOREL-peek — store TOS in local[operand] without consuming */
            operand_u8 = fetch_byte(state);
            val = lpst_exec_peek(state);
            if (operand_u8 < state->current_frame.local_count) {
                state->local_storage[operand_u8] = val;
            }
            break;

        case 0x59: /* BITSV-local — extract control_word bit field from aggregate in local */
            operand_u16 = fetch_le16(state);
            operand_u8 = (uint8_t)((operand_u16 >> 4) & 0x0Fu);
            handle = (operand_u8 < state->current_frame.local_count)
                ? state->local_storage[operand_u8]
                : LPST_FALSE_SENTINEL;
            lpst_exec_push(state, extract_bit_field_from_local(state, handle, operand_u16));
            break;

        case 0x5A: /* BITSV — extract control_word bit field from aggregate on stack */
            operand_u16 = fetch_le16(state);
            handle = lpst_exec_pop(state);
            {
                uint16_t bitsv_result = extract_bit_field(state, handle, operand_u16);
                lpst_exec_push(state, bitsv_result);
            }
            break;

        case 0x5B: /* BITSREP-local — replace control_word bit field in local aggregate */
            operand_u16 = fetch_le16(state);
            operand_u8 = (uint8_t)((operand_u16 >> 4) & 0x0Fu);
            handle = (operand_u8 < state->current_frame.local_count)
                ? state->local_storage[operand_u8]
                : LPST_FALSE_SENTINEL;
            val = lpst_exec_pop(state);
            replace_bit_field_in_local(state, handle, operand_u16, val);
            break;

        case 0x5C: /* BITSREP — replace control_word bit field in stack aggregate */
            operand_u16 = fetch_le16(state);
            handle = lpst_exec_pop(state);
            val = lpst_exec_pop(state);
            replace_bit_field(state, handle, operand_u16, val);
            break;

        case 0x5D: /* BITSET-local — set single bit in local aggregate */
            operand_u16 = fetch_le16(state);
            operand_u8 = (uint8_t)((operand_u16 >> 4) & 0x0Fu);
            handle = (operand_u8 < state->current_frame.local_count)
                ? state->local_storage[operand_u8]
                : LPST_FALSE_SENTINEL;
            replace_single_bit_in_local(state, handle, operand_u16);
            break;

        case 0x5E: /* BITSET — set single bit in stack aggregate */
            operand_u16 = fetch_le16(state);
            handle = lpst_exec_pop(state);
            replace_single_bit(state, handle, operand_u16);
            break;

        case 0x5F: /* EXT — dispatch to extended opcode handler */
            dispatch_extended(state);
            break;

        default:
            fprintf(stderr, "unhandled opcode 0x%02X at PC 0x%04X\n",
                    opcode, instruction_start);
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            break;
        }
    }

    return LPST_OK;
}
