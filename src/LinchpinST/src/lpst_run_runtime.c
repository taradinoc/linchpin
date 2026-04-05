/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_runtime.c — Global variable access, heap allocation, and call/return.
 *
 * Heap layout (within the 128 KB RAM):
 *   low segment  (0x00000-0x0FFFF):  free-list / next_low_arena_byte grows up
 *   high segment (0x10000-0x1FFFF):  next_high_arena_byte grows up;
 *                                     tuple_stack_byte grows down from 0x1FFFF
 *
 * allocate_vector: checks a single-slot free list first, then low arena,
 *   then high arena, then halts with OOM.
 * allocate_tuple: shrinks the tuple stack downward (used for temporary frames).
 * release_vector: returns the object to the corresponding free-list slot.
 *
 * SETJMP/LONGJMP:
 *   save_jump_snapshot captures the full current VM state (eval stack, call
 *   stack + saved locals, current frame + locals, tuple_stack_byte) and
 *   associates it with a freshly minted token pushed onto the eval stack.
 *   restore_jump_snapshot later looks up that token, restores all state,
 *   and either jumps to the protected region (LONGJMP) or to the landing
 *   offset (LONGJMPR, which also pushes a return value).
 *
 * Near calls target a byte offset within the current module.
 * Far calls use a 16-bit selector: high byte = module_id, low byte = proc_index.
 */
#include "lpst_run_internal.h"

uint16_t load_module_global(const lpst_exec_state *state, uint8_t index)
{
    if (state->current_module_id >= 1 && state->current_module_id <= state->module_count) {
        uint16_t module_index = (uint16_t)(state->current_module_id - 1);
        uint16_t count = state->module_global_counts[module_index];
        if (index < count) {
            return state->module_globals[module_index][index];
        }
    }

    if (index >= 0xCD && index <= 0xD2) {
        uint16_t month = state->system_module_globals[0xCD];
        uint16_t day = state->system_module_globals[0xCE];
        uint16_t year = state->system_module_globals[0xCF];
        uint16_t hour = state->system_module_globals[0xD0];
        uint16_t minute = state->system_module_globals[0xD1];
        uint16_t second = state->system_module_globals[0xD2];

        if (state->host != NULL) {
            if (state->host->get_date != NULL) {
                state->host->get_date(state->host, &month, &day, &year);
            }

            if (state->host->get_time != NULL) {
                state->host->get_time(state->host, &hour, &minute, &second);
            }
        }

        switch (index) {
        case 0xCD:
            return month;
        case 0xCE:
            return day;
        case 0xCF:
            return year;
        case 0xD0:
            return hour;
        case 0xD1:
            return minute;
        case 0xD2:
            return second;
        default:
            break;
        }
    }

    return state->system_module_globals[index];
}

uint8_t clamp_cursor_row_value(uint16_t value)
{
    if (value >= LPST_SCREEN_HEIGHT) {
        return (uint8_t)(LPST_SCREEN_HEIGHT - 1);
    }

    return (uint8_t)value;
}

uint8_t get_visible_screen_width(const lpst_exec_state *state)
{
    uint8_t width;

    if (state == NULL || state->host == NULL || state->host->screen_width == 0) {
        return LPST_SCREEN_WIDTH;
    }

    width = state->host->screen_width;
    if (width > LPST_SCREEN_WIDTH) {
        width = LPST_SCREEN_WIDTH;
    }

    return width;
}

uint8_t clamp_cursor_col_value(const lpst_exec_state *state, uint16_t value)
{
    uint8_t width = get_visible_screen_width(state);

    if (value >= width) {
        return (uint8_t)(width - 1);
    }

    return (uint8_t)value;
}

static void commit_system_cursor(lpst_exec_state *state)
{
    state->cursor_row = clamp_cursor_row_value(state->system_module_globals[0xCA]);
    state->cursor_col = clamp_cursor_col_value(state, state->system_module_globals[0xC9]);
    state->host->set_cursor(state->host, state->cursor_row, state->cursor_col);
}

static void commit_display_style(lpst_exec_state *state)
{
    if (state->host->set_style != NULL) {
        state->host->set_style(state->host, state->system_module_globals[0xD5]);
    }
}

/* Store a value into system slot index.  Slots 0xC9 (cursor column) and
 * 0xD5 (display style) have side effects: writing to 0xC9 commits the
 * cursor position to the host (row was set via slot 0xCA beforehand). */
void store_module_global(lpst_exec_state *state, uint8_t index, uint16_t value)
{
    if (state->current_module_id >= 1 && state->current_module_id <= state->module_count) {
        uint16_t module_index = (uint16_t)(state->current_module_id - 1);
        uint16_t count = state->module_global_counts[module_index];
        if (index < count) {
            state->module_globals[module_index][index] = value;
            return;
        }
    }

    state->system_module_globals[index] = value;

    if (index == 0xC9) {
        commit_system_cursor(state);
    } else if (index == 0xD5) {
        commit_display_style(state);
    }
}

static uint16_t encode_handle(uint32_t byte_offset)
{
    uint16_t segment_bit = (byte_offset >= LPST_SEGMENT_SIZE_BYTES) ? 0x8000u : 0u;
    uint16_t word_addr = (uint16_t)((byte_offset & 0xFFFFu) / 2u);
    return segment_bit | word_addr;
}

/* Allocate a word_count-word aggregate from the heap.
 *
 * Strategy:
 *  1. If a free-list slot exists for word_count, reuse it.
 *  2. Otherwise try the low arena (bumps next_low_arena_byte upward).
 *  3. Then try the high arena (bumps next_high_arena_byte upward, capped by
 *     the tuple stack floor).
 *  4. If all fail, the VM is halted with an OOM error. */
uint16_t allocate_vector(lpst_exec_state *state, uint16_t word_count)
{
    uint32_t byte_size = (uint32_t)(word_count > 0 ? word_count : 1) * 2u;
    uint32_t offset;
    uint16_t handle;

    if (word_count > 0 && word_count <= LPST_FREE_LIST_BUCKETS &&
        state->free_list[word_count - 1] != 0) {
        offset = state->free_list[word_count - 1];
        state->free_list[word_count - 1] = 0;
        zero_ram_bytes(state, offset, byte_size);
        handle = encode_handle(offset);
        return handle;
    }

    if (state->next_low_arena_byte + byte_size <= LPST_SEGMENT_SIZE_BYTES) {
        offset = state->next_low_arena_byte;
        state->next_low_arena_byte += byte_size;
        zero_ram_bytes(state, offset, byte_size);
        handle = encode_handle(offset);
        return handle;
    }

    if (state->next_high_arena_byte + byte_size <= state->tuple_stack_floor_byte) {
        offset = state->next_high_arena_byte;
        state->next_high_arena_byte += byte_size;
        zero_ram_bytes(state, offset, byte_size);
        handle = encode_handle(offset);
        return handle;
    }

    fprintf(stderr, "out of memory allocating %u words\n", word_count);
    return LPST_FALSE_SENTINEL;
}

/* Allocate a tuple (temporary frame) aggregate from the high segment by
 * growing the tuple stack downward.  Unlike vector allocation this never
 * grows upward; a stack underflow or collision with the high arena is fatal. */
uint16_t allocate_tuple(lpst_exec_state *state, uint16_t word_count)
{
    uint32_t byte_size = (uint32_t)(word_count > 0 ? word_count : 1) * 2u;
    uint32_t new_top;

    if (state->tuple_stack_byte < byte_size) {
        fprintf(stderr, "tuple stack underflow\n");
        return LPST_FALSE_SENTINEL;
    }

    new_top = state->tuple_stack_byte - byte_size;
    if (new_top < state->tuple_stack_floor_byte) {
        fprintf(stderr, "tuple stack overflow\n");
        return LPST_FALSE_SENTINEL;
    }

    state->tuple_stack_byte = new_top;
    zero_ram_bytes(state, new_top, byte_size);
    return encode_handle(new_top);
}

/* Return a vector to the per-size free list if the slot is empty.
 * Only one object per size is cached; there is no linked-list free list. */
void release_vector(lpst_exec_state *state, uint16_t handle, uint16_t word_count)
{
    uint32_t offset;

    if (handle == LPST_FALSE_SENTINEL || word_count == 0) {
        return;
    }

    offset = handle_to_byte_offset(handle);

    if (word_count <= LPST_FREE_LIST_BUCKETS && state->free_list[word_count - 1] == 0) {
        state->free_list[word_count - 1] = offset;
    }
}

static const lpst_procedure *find_procedure_by_offset(
    const lpst_module *mod, uint16_t code_offset)
{
    uint16_t i;
    for (i = 0; i < mod->procedure_count; i++) {
        if (mod->procedures[i].code_offset == code_offset ||
            mod->procedures[i].start_offset == code_offset) {
            return &mod->procedures[i];
        }
    }

    return NULL;
}

static uint16_t compute_upper_bound(const lpst_module *mod, uint16_t start_offset)
{
    uint16_t best = (uint16_t)mod->length;
    uint16_t i;
    for (i = 0; i < mod->procedure_count; i++) {
        uint16_t so = mod->procedures[i].start_offset;
        if (so > start_offset && so < best) {
            best = so;
        }
    }

    return best;
}

static uint16_t parse_private_header(
    const lpst_exec_state *state,
    const lpst_module *mod,
    uint16_t offset,
    lpst_proc_header *out_header)
{
    lpst_exec_state *mutable_state = (lpst_exec_state *)state;
    uint8_t header_byte = read_code_byte_at(mutable_state, mod, offset);
    uint32_t cursor = (uint32_t)offset + 1u;

    out_header->local_count = header_byte & 0x7Fu;
    out_header->initializer_count = 0;

    if ((header_byte & 0x80u) != 0) {
        uint8_t count = 0;
        for (;;) {
            uint8_t marker = read_code_byte_at(mutable_state, mod, (uint16_t)cursor++);
            uint8_t local_idx = marker & 0x3Fu;
            bool is_byte = (marker & 0x40u) != 0;
            bool is_last = (marker & 0x80u) != 0;
            uint16_t value;

            if (is_byte) {
                value = (uint16_t)(int16_t)(int8_t)read_code_byte_at(
                    mutable_state, mod, (uint16_t)cursor++);
            } else {
                value = (uint16_t)(read_code_byte_at(mutable_state, mod, (uint16_t)cursor) |
                                    (read_code_byte_at(mutable_state, mod, (uint16_t)(cursor + 1u)) << 8));
                cursor += 2;
            }

            if (count < LPST_MAX_INITIALIZERS_PER_PROC) {
                out_header->initializers[count].local_index = local_idx;
                out_header->initializers[count].value = value;
                count++;
            }

            if (is_last) {
                break;
            }
        }

        out_header->initializer_count = count;
    }

    out_header->header_size = (uint8_t)(cursor - offset);
    return (uint16_t)(offset + out_header->header_size);
}

static lpst_jump_snapshot *find_jump_snapshot(lpst_exec_state *state, uint16_t token)
{
    uint16_t i;

    for (i = 0; i < LPST_JUMP_SNAPSHOT_CAPACITY; i++) {
        if (state->jump_snapshots[i].in_use && state->jump_snapshots[i].token == token) {
            return &state->jump_snapshots[i];
        }
    }

    return NULL;
}

static void free_jump_snapshot_storage(lpst_jump_snapshot *snapshot)
{
    uint16_t i;

    if (snapshot == NULL) {
        return;
    }

    free(snapshot->frame_locals);
    snapshot->frame_locals = NULL;
    free(snapshot->eval_stack);
    snapshot->eval_stack = NULL;

    if (snapshot->call_stack != NULL) {
        for (i = 0; i < snapshot->call_stack_top; i++) {
            free(snapshot->call_stack[i].saved_locals);
            snapshot->call_stack[i].saved_locals = NULL;
        }

        free(snapshot->call_stack);
        snapshot->call_stack = NULL;
    }

    snapshot->call_stack_top = 0;
    snapshot->eval_stack_top = 0;
    memset(&snapshot->frame, 0, sizeof(snapshot->frame));
}

static void release_jump_snapshot(lpst_jump_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    free_jump_snapshot_storage(snapshot);
    memset(snapshot, 0, sizeof(*snapshot));
}

static void release_jump_snapshots_for_frame(
    lpst_exec_state *state,
    uint16_t module_id,
    uint16_t procedure_index,
    uint16_t procedure_start_offset,
    uint16_t local_count,
    uint16_t call_stack_top)
{
    uint16_t i;

    for (i = 0; i < LPST_JUMP_SNAPSHOT_CAPACITY; i++) {
        lpst_jump_snapshot *snapshot = &state->jump_snapshots[i];

        if (!snapshot->in_use) {
            continue;
        }

        if (snapshot->frame.module_id != module_id
            || snapshot->frame.procedure_index != procedure_index
            || snapshot->frame.procedure_start_offset != procedure_start_offset
            || snapshot->frame.local_count != local_count
            || snapshot->call_stack_top != call_stack_top) {
            continue;
        }

        release_jump_snapshot(snapshot);
    }
}

static lpst_jump_snapshot *alloc_jump_snapshot(lpst_exec_state *state, uint16_t token)
{
    uint16_t i;
    lpst_jump_snapshot *snapshot = find_jump_snapshot(state, token);

    if (snapshot != NULL) {
        free_jump_snapshot_storage(snapshot);
        return snapshot;
    }

    for (i = 0; i < LPST_JUMP_SNAPSHOT_CAPACITY; i++) {
        if (!state->jump_snapshots[i].in_use) {
            memset(&state->jump_snapshots[i], 0, sizeof(state->jump_snapshots[i]));
            state->jump_snapshots[i].in_use = true;
            state->jump_snapshots[i].token = token;
            return &state->jump_snapshots[i];
        }
    }

    return NULL;
}

static bool clone_call_continuation(lpst_call_continuation *dst, const lpst_call_continuation *src)
{
    memset(dst, 0, sizeof(*dst));
    *dst = *src;

    if (src->saved_local_count > 0) {
        dst->saved_locals = (uint16_t *)malloc(src->saved_local_count * sizeof(uint16_t));
        if (dst->saved_locals == NULL) {
            return false;
        }

        memcpy(dst->saved_locals, src->saved_locals, src->saved_local_count * sizeof(uint16_t));
    }

    return true;
}

/* After restoring locals (e.g. via a STOREL opcode in the current frame),
 * update any live SETJMP snapshots that belong to the same frame so they
 * will restore to the latest local values rather than stale ones. */
static void refresh_current_frame_jump_snapshots(lpst_exec_state *state)
{
    uint16_t i;

    for (i = 0; i < LPST_JUMP_SNAPSHOT_CAPACITY; i++) {
        lpst_jump_snapshot *snapshot = &state->jump_snapshots[i];

        if (!snapshot->in_use) {
            continue;
        }

        if (snapshot->frame.module_id != state->current_frame.module_id
            || snapshot->frame.procedure_index != state->current_frame.procedure_index
            || snapshot->frame.procedure_start_offset != state->current_frame.procedure_start_offset
            || snapshot->frame.local_count != state->current_frame.local_count
            || snapshot->call_stack_top != state->call_stack_top
            || snapshot->frame_locals == NULL) {
            continue;
        }

        memcpy(snapshot->frame_locals,
               state->local_storage,
               state->current_frame.local_count * sizeof(uint16_t));
    }
}

/* Snapshot the full VM state for later LONGJMP recovery.
 *
 * protected_offset  = the PC to jump to on a plain LONGJMP
 * landing_offset    = the PC to jump to on a LONGJMPR (with a return value)
 *
 * The snapshot token is pushed onto the eval stack so the program can
 * pass it down through procedure calls to the eventual LONGJMP site. */
bool save_jump_snapshot(lpst_exec_state *state, uint16_t protected_offset, uint16_t landing_offset)
{
    lpst_jump_snapshot *snapshot;
    uint16_t token = state->next_jump_token++;
    uint16_t i;

    if (token == LPST_FALSE_SENTINEL) {
        token = state->next_jump_token++;
    }

    snapshot = alloc_jump_snapshot(state, token);
    if (snapshot == NULL) {
        fprintf(stderr, "jump snapshot table full\n");
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return false;
    }

    snapshot->module_id = state->current_module_id;
    snapshot->longjmp_program_counter = protected_offset;
    snapshot->longjmpr_program_counter = landing_offset;
    snapshot->tuple_stack_byte = state->tuple_stack_byte;
    snapshot->frame = state->current_frame;
    snapshot->frame_upper_bound = state->frame_upper_bound;

    if (state->current_frame.local_count > 0) {
        snapshot->frame_locals = (uint16_t *)malloc(state->current_frame.local_count * sizeof(uint16_t));
        if (snapshot->frame_locals == NULL) {
            fprintf(stderr, "allocation failure saving SETJMP locals\n");
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            return false;
        }

        memcpy(snapshot->frame_locals, state->local_storage,
               state->current_frame.local_count * sizeof(uint16_t));
    }

    snapshot->frame.locals = snapshot->frame_locals;

    snapshot->eval_stack_top = state->eval_stack_top;
    if (state->eval_stack_top > 0) {
        snapshot->eval_stack = (uint16_t *)malloc(state->eval_stack_top * sizeof(uint16_t));
        if (snapshot->eval_stack == NULL) {
            fprintf(stderr, "allocation failure saving SETJMP stack\n");
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            return false;
        }

        memcpy(snapshot->eval_stack, state->eval_stack, state->eval_stack_top * sizeof(uint16_t));
    }

    snapshot->call_stack_top = state->call_stack_top;
    if (state->call_stack_top > 0) {
        snapshot->call_stack = (lpst_call_continuation *)calloc(
            state->call_stack_top, sizeof(lpst_call_continuation));
        if (snapshot->call_stack == NULL) {
            fprintf(stderr, "allocation failure saving SETJMP callers\n");
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            return false;
        }

        for (i = 0; i < state->call_stack_top; i++) {
            if (!clone_call_continuation(&snapshot->call_stack[i], &state->call_stack[i])) {
                fprintf(stderr, "allocation failure cloning SETJMP caller\n");
                state->is_halted = true;
                state->halt_code = 0xFFFF;
                return false;
            }
        }
    }

    if (state->trace_enabled) {
        fprintf(stderr,
                "[SETJMP] token=0x%04X module=%u restart=0x%04X exit=0x%04X stk=%u call=%u\n",
                token, state->current_module_id, protected_offset, landing_offset,
                state->eval_stack_top, state->call_stack_top);
    }

    lpst_exec_push(state, token);
    return true;
}

/* Restore a previously saved snapshot identified by token.
 *
 * If use_return_target is true (LONGJMPR), jump to longjmpr_program_counter
 * and push return_value onto the restored eval stack.  Otherwise (LONGJMP),
 * jump to longjmp_program_counter without pushing anything.
 *
 * The snapshot is released after restoration so its token cannot be reused. */
bool restore_jump_snapshot(
    lpst_exec_state *state,
    uint16_t token,
    bool use_return_target,
    uint16_t return_value)
{
    lpst_jump_snapshot *snapshot = find_jump_snapshot(state, token);
    uint16_t i;

    if (snapshot == NULL) {
        fprintf(stderr, "LONGJMP references unknown SETJMP token 0x%04X\n", token);
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return false;
    }

    state->current_module_id = snapshot->module_id;
    state->program_counter = use_return_target
        ? snapshot->longjmpr_program_counter
        : snapshot->longjmp_program_counter;
    state->tuple_stack_byte = snapshot->tuple_stack_byte;
    state->current_frame = snapshot->frame;
    state->current_frame.locals = state->local_storage;
    state->frame_upper_bound = snapshot->frame_upper_bound;

    if (snapshot->frame.local_count > 0 && snapshot->frame_locals != NULL) {
        memcpy(state->local_storage, snapshot->frame_locals,
               snapshot->frame.local_count * sizeof(uint16_t));
    }

    state->eval_stack_top = snapshot->eval_stack_top;
    if (snapshot->eval_stack_top > 0) {
        memcpy(state->eval_stack, snapshot->eval_stack,
               snapshot->eval_stack_top * sizeof(uint16_t));
    }

    for (i = 0; i < state->call_stack_top; i++) {
        free(state->call_stack[i].saved_locals);
        state->call_stack[i].saved_locals = NULL;
    }

    state->call_stack_top = 0;
    for (i = 0; i < snapshot->call_stack_top; i++) {
        if (!clone_call_continuation(&state->call_stack[i], &snapshot->call_stack[i])) {
            fprintf(stderr, "allocation failure restoring LONGJMP caller\n");
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            return false;
        }
    }

    state->call_stack_top = snapshot->call_stack_top;
    load_code_page(state, current_module(state), state->program_counter);

    if (state->trace_enabled) {
        fprintf(stderr, "[%s] token=0x%04X module=%u target=0x%04X\n",
                use_return_target ? "LONGJMPR" : "LONGJMP",
                token, state->current_module_id, state->program_counter);
    }

    if (use_return_target) {
        lpst_exec_push(state, return_value);
    }

    release_jump_snapshot(snapshot);

    return true;
}

static void pop_arguments(lpst_exec_state *state, uint16_t *args, uint8_t argc)
{
    int i;
    for (i = argc - 1; i >= 0; i--) {
        args[i] = lpst_exec_pop(state);
    }
}

static bool enter_procedure(
    lpst_exec_state *state,
    uint16_t target_module_id,
    const lpst_proc_header *header,
    uint16_t proc_start_offset,
    uint16_t proc_code_offset,
    uint16_t proc_upper_bound,
    int exported_index,
    uint16_t return_pc,
    uint8_t argc,
    uint16_t *args)
{
    lpst_call_continuation *cont;
    uint16_t *new_locals;
    uint16_t copy_count;
    uint16_t i;

    if (state->call_stack_top >= LPST_CALL_STACK_CAPACITY) {
        fprintf(stderr, "call stack overflow\n");
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return false;
    }

    refresh_current_frame_jump_snapshots(state);

    cont = &state->call_stack[state->call_stack_top];
    cont->module_id = state->current_module_id;
    cont->return_pc = return_pc;
    cont->procedure_start_offset = state->current_frame.procedure_start_offset;
    cont->procedure_index = (uint16_t)state->current_frame.procedure_index;
    cont->frame_upper_bound = state->frame_upper_bound;

    cont->saved_local_count = (uint16_t)state->current_frame.local_count;
    if (cont->saved_local_count > 0) {
        cont->saved_locals = (uint16_t *)malloc(cont->saved_local_count * sizeof(uint16_t));
        if (cont->saved_locals == NULL) {
            fprintf(stderr, "allocation failure saving locals\n");
            state->is_halted = true;
            state->halt_code = 0xFFFF;
            return false;
        }

        memcpy(cont->saved_locals, state->local_storage,
               cont->saved_local_count * sizeof(uint16_t));
    } else {
        cont->saved_locals = NULL;
    }

    cont->saved_eval_stack_depth = state->eval_stack_top;
    state->call_stack_top++;

    new_locals = state->local_storage;
    for (i = 0; i < header->local_count && i < LPST_MAX_LOCALS; i++) {
        new_locals[i] = LPST_FALSE_SENTINEL;
    }

    for (i = 0; i < header->initializer_count; i++) {
        uint8_t idx = header->initializers[i].local_index;
        if (idx < header->local_count && idx < LPST_MAX_LOCALS) {
            new_locals[idx] = header->initializers[i].value;
        }
    }

    copy_count = argc;
    if (copy_count > header->local_count) {
        copy_count = header->local_count;
    }

    for (i = 0; i < copy_count; i++) {
        new_locals[i] = args[i];
    }

    state->current_module_id = target_module_id;
    state->program_counter = proc_code_offset;
    state->current_frame.module_id = target_module_id;
    state->current_frame.procedure_index = (uint16_t)exported_index;
    state->current_frame.procedure_start_offset = proc_start_offset;
    state->current_frame.code_offset = proc_code_offset;
    state->current_frame.locals = new_locals;
    state->current_frame.local_count = header->local_count;
    state->frame_upper_bound = proc_upper_bound;

    return true;
}

/* Enter a call to a procedure at target_offset within the current module.
 * argc arguments are popped from the eval stack in reverse order and passed
 * as the first argc locals.  The current module does not change.
 * Private (non-exported) procedures are located by parsing their header
 * from bytecode if they are not found in the procedure table. */
bool enter_near_call(
    lpst_exec_state *state,
    uint16_t target_offset,
    uint8_t argc,
    uint16_t return_pc)
{
    const lpst_module *mod = current_module(state);
    const lpst_procedure *proc = find_procedure_by_offset(mod, target_offset);
    lpst_proc_header priv_header;
    const lpst_proc_header *header;
    uint16_t code_offset;
    uint16_t start_offset;
    int exported_index;
    uint16_t args[LPST_MAX_LOCALS];

    if (argc > LPST_MAX_LOCALS) {
        argc = LPST_MAX_LOCALS;
    }

    pop_arguments(state, args, argc);

    if (proc != NULL) {
        fill_cached_proc_header(proc, &priv_header);
        code_offset = proc->code_offset;
        header = &priv_header;
        start_offset = proc->start_offset;
        exported_index = proc->exported_index;
    } else {
        code_offset = parse_private_header(state, mod, target_offset, &priv_header);
        header = &priv_header;
        start_offset = target_offset;
        exported_index = -1;
    }

    return enter_procedure(
        state, mod->module_id, header, start_offset, code_offset,
        proc != NULL ? proc->upper_bound : compute_upper_bound(mod, start_offset),
        exported_index, return_pc, argc, args);
}

/* Enter a call to an exported procedure in any module.
 * selector encodes the destination as (module_id << 8) | proc_index.
 * argc arguments are popped from the eval stack in reverse order. */
bool enter_far_call(
    lpst_exec_state *state,
    uint16_t selector,
    uint8_t argc,
    uint16_t return_pc)
{
    uint16_t target_module_id = selector >> 8;
    uint16_t proc_index = selector & 0xFFu;
    const lpst_module *target_mod;
    const lpst_procedure *proc;
    uint16_t args[LPST_MAX_LOCALS];

    if (argc > LPST_MAX_LOCALS) {
        argc = LPST_MAX_LOCALS;
    }

    pop_arguments(state, args, argc);

    if (target_module_id < 1 || target_module_id > state->image->module_count) {
        fprintf(stderr, "far call to invalid module %u\n", target_module_id);
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return false;
    }

    target_mod = &state->image->modules[target_module_id - 1];
    if (proc_index >= target_mod->procedure_count) {
        fprintf(stderr, "far call to invalid proc %u in module %u\n",
                proc_index, target_module_id);
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return false;
    }

    proc = &target_mod->procedures[proc_index];
    {
        lpst_proc_header header;
        fill_cached_proc_header(proc, &header);

        return enter_procedure(
            state, target_module_id, &header, proc->start_offset,
            proc->code_offset, proc->upper_bound, proc->exported_index, return_pc, argc, args);
    }
}

/* Return from the current procedure.
 *
 * Pushes result_count values onto the eval stack, releases SETJMP snapshots
 * that belong to the exiting frame, then pops the call stack and restores
 * the caller's module, PC, frame, and locals.  When the call stack is empty
 * the VM halts with the first result (or 0) as the halt code. */
void do_return(lpst_exec_state *state, uint16_t *results, uint16_t result_count)
{
    lpst_call_continuation *cont;
    uint16_t i;
    uint16_t exiting_module_id = state->current_module_id;
    uint16_t exiting_procedure_index = (uint16_t)state->current_frame.procedure_index;
    uint16_t exiting_start_offset = state->current_frame.procedure_start_offset;
    uint16_t exiting_local_count = (uint16_t)state->current_frame.local_count;
    uint16_t exiting_call_stack_top = state->call_stack_top;

    state->last_return_word_count = result_count;

    release_jump_snapshots_for_frame(
        state,
        exiting_module_id,
        exiting_procedure_index,
        exiting_start_offset,
        exiting_local_count,
        exiting_call_stack_top);

    if (state->call_stack_top == 0) {
        state->is_halted = true;
        state->halt_code = (result_count > 0) ? results[0] : 0;
        return;
    }

    state->call_stack_top--;
    cont = &state->call_stack[state->call_stack_top];

    state->current_module_id = cont->module_id;
    state->program_counter = cont->return_pc;
    state->current_frame.module_id = cont->module_id;
    state->current_frame.procedure_index = cont->procedure_index;
    state->current_frame.procedure_start_offset = cont->procedure_start_offset;
    state->frame_upper_bound = cont->frame_upper_bound;

    if (cont->saved_locals != NULL) {
        memcpy(state->local_storage, cont->saved_locals,
               cont->saved_local_count * sizeof(uint16_t));
        free(cont->saved_locals);
        cont->saved_locals = NULL;
    }

    state->current_frame.locals = state->local_storage;
    state->current_frame.local_count = cont->saved_local_count;

    {
        const lpst_module *mod = current_module(state);
        const lpst_procedure *proc = find_procedure_by_offset(
            mod, cont->procedure_start_offset);
        if (proc != NULL) {
            state->current_frame.code_offset = proc->code_offset;
        } else {
            state->current_frame.code_offset = cont->procedure_start_offset;
        }
    }

    state->eval_stack_top = cont->saved_eval_stack_depth;
    for (i = 0; i < result_count; i++) {
        lpst_exec_push(state, results[i]);
    }
}

void host_print_char(lpst_exec_state *state, uint8_t ch)
{
    lpst_host *host = state->host;

    if (ch == '\r' || ch == '\n') {
        uint8_t row = state->cursor_row;
        uint8_t col = state->cursor_col;

        row++;
        col = 0;
        if (row >= LPST_SCREEN_HEIGHT) {
            host->scroll_up(host, 0, LPST_SCREEN_HEIGHT - 1, 1);
            row = LPST_SCREEN_HEIGHT - 1;
        }

        state->cursor_row = row;
        state->cursor_col = col;
        host->set_cursor(host, row, col);

        return;
    }

    {
        uint8_t row = state->cursor_row;
        uint8_t col = state->cursor_col;
        uint8_t width = get_visible_screen_width(state);

        host->put_char(host, ch, row, col);
        if (col + 1 < width) {
            col++;
        }

        state->cursor_row = row;
        state->cursor_col = col;
        host->set_cursor(host, row, col);
    }
}

void host_print_vector(lpst_exec_state *state, uint16_t handle,
                       uint16_t length, uint16_t start_offset)
{
    uint16_t i;
    for (i = 0; i < length; i++) {
        uint8_t ch = read_aggregate_payload_byte(state, handle, start_offset + i);
        uint8_t printable = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
        host_print_char(state, printable);
    }
}

static bool try_resolve_display_window(lpst_exec_state *state,
    uint16_t descriptor_handle,
    uint16_t logical_column,
    uint16_t logical_row,
    uint8_t *out_host_row,
    uint8_t *out_host_column,
    uint16_t *out_remaining_width)
{
    uint16_t geometry_handle;
    int col;
    int row;
    int origin_col;
    int origin_row;
    int physical_col;
    int physical_row;
    int col_extent;
    int row_extent;
    int delta_col;
    int delta_row;

    geometry_handle = read_aggregate_word(state, descriptor_handle, 3);
    if (geometry_handle == LPST_FALSE_SENTINEL || geometry_handle == 0) {
        return false;
    }

    col = (int16_t)logical_column;
    row = (int16_t)logical_row;
    origin_col = (int16_t)read_aggregate_word(state, geometry_handle, 0);
    origin_row = (int16_t)read_aggregate_word(state, geometry_handle, 1);
    physical_col = (int16_t)read_aggregate_word(state, geometry_handle, 2);
    physical_row = (int16_t)read_aggregate_word(state, geometry_handle, 3);
    col_extent = read_aggregate_word(state, geometry_handle, 4);
    row_extent = read_aggregate_word(state, geometry_handle, 5);

    delta_col = col - origin_col;
    delta_row = row - origin_row;
    if (delta_col < 0 || delta_row < 0 || delta_col >= col_extent || delta_row >= row_extent) {
        return false;
    }

    physical_col += delta_col;
    physical_row += delta_row;
    if (physical_row < 0 || physical_row >= LPST_SCREEN_HEIGHT ||
        physical_col < 0 || physical_col >= get_visible_screen_width(state)) {
        return false;
    }

    if (out_host_row != NULL) {
        *out_host_row = (uint8_t)physical_row;
    }

    if (out_host_column != NULL) {
        *out_host_column = (uint8_t)physical_col;
    }

    if (out_remaining_width != NULL) {
        *out_remaining_width = (uint16_t)((col_extent > delta_col) ? (col_extent - delta_col) : 0);
    }

    return true;
}

uint16_t execute_setwin(lpst_exec_state *state, uint16_t descriptor_handle)
{
    uint16_t logical_column;
    uint16_t logical_row;
    uint16_t attribute_bits;
    uint8_t host_row;
    uint8_t host_column;

    if (descriptor_handle == LPST_FALSE_SENTINEL || descriptor_handle == 0) {
        store_module_global(state, 0xD3, LPST_FALSE_SENTINEL);
        if (state->trace_enabled) {
            fprintf(stderr, "[SETWIN] desc=0x%04X -> false\n", descriptor_handle);
        }
        return LPST_FALSE_SENTINEL;
    }

    logical_column = read_aggregate_word(state, descriptor_handle, 0);
    logical_row = read_aggregate_word(state, descriptor_handle, 1);
    if (!try_resolve_display_window(state, descriptor_handle, logical_column, logical_row,
            &host_row, &host_column, NULL)) {
        store_module_global(state, 0xD3, LPST_FALSE_SENTINEL);
        if (state->trace_enabled) {
            fprintf(stderr, "[SETWIN] desc=0x%04X logical=(c=%u,r=%u) -> false\n",
                    descriptor_handle, logical_column, logical_row);
        }
        return LPST_FALSE_SENTINEL;
    }

    store_module_global(state, 0xD3, descriptor_handle);
    attribute_bits = extract_bit_field(state, descriptor_handle, UINT16_C(0x0802));
    store_module_global(state, 0xD5, attribute_bits);
    state->cursor_row = host_row;
    state->cursor_col = host_column;
    state->host->set_cursor(state->host, host_row, host_column);
    if (state->trace_enabled) {
        fprintf(stderr,
                "[SETWIN] desc=0x%04X logical=(c=%u,r=%u) host=(%u,%u) attr=0x%04X -> 0x%04X\n",
                descriptor_handle,
                logical_column,
                logical_row,
                host_row,
                host_column,
                attribute_bits,
                logical_column);
    }
    return logical_column;
}

uint16_t execute_wprintv(lpst_exec_state *state,
    uint16_t descriptor_handle,
    uint16_t source_handle,
    uint16_t char_count,
    uint16_t source_offset)
{
    uint16_t start_col;
    uint16_t geometry_handle;
    uint16_t source_limit;
    int col_origin;
    int col_span;
    int start;
    int end_req;
    int end_src;
    int end;
    int win_min;
    int win_max;
    int start_clipped;
    int src_start;
    int end_clipped;
    int count;
    uint16_t setwin_result;
    char preview[101];
    int preview_len;

    if (descriptor_handle == LPST_FALSE_SENTINEL || descriptor_handle == 0 ||
        source_handle == LPST_FALSE_SENTINEL || source_handle == 0) {
        if (state->trace_enabled) {
            fprintf(stderr, "[WPRINTV] desc=0x%04X src=0x%04X N=%u O=%u -> false\n",
                    descriptor_handle, source_handle, char_count, source_offset);
        }
        return LPST_FALSE_SENTINEL;
    }

    start_col = read_aggregate_word(state, descriptor_handle, 0);
    geometry_handle = read_aggregate_word(state, descriptor_handle, 3);
    if (geometry_handle == LPST_FALSE_SENTINEL || geometry_handle == 0) {
        if (state->trace_enabled) {
            fprintf(stderr, "[WPRINTV] desc=0x%04X -> false (no geometry)\n", descriptor_handle);
        }
        return LPST_FALSE_SENTINEL;
    }

    source_limit = read_aggregate_word(state, source_handle, 0);
    col_origin = (int16_t)read_aggregate_word(state, geometry_handle, 0);
    col_span = read_aggregate_word(state, geometry_handle, 4);
    start = (int16_t)start_col;
    end_req = start + char_count;

    if ((uint32_t)source_offset + (uint32_t)char_count > (uint32_t)source_limit) {
        int available = (int)source_limit - (int)source_offset;
        if (available < 0) {
            available = 0;
        }
        end_src = start + available;
    } else {
        end_src = end_req;
    }

    end = (end_req < end_src) ? end_req : end_src;
    win_min = col_origin;
    win_max = col_origin + col_span;
    start_clipped = start;
    src_start = source_offset;

    if (start_clipped < win_min) {
        int advance = win_min - start_clipped;
        src_start += advance;
        start_clipped = win_min;
    }

    end_clipped = (end < win_max) ? end : win_max;
    count = end_clipped - start_clipped;
    if (count < 0) {
        count = 0;
    }
    if (count > 100) {
        count = 100;
    }

    write_aggregate_word(state, descriptor_handle, 0, (uint16_t)start_clipped);
    setwin_result = execute_setwin(state, descriptor_handle);
    if (setwin_result == LPST_FALSE_SENTINEL) {
        write_aggregate_word(state, descriptor_handle, 0, (uint16_t)end_req);
        if (state->trace_enabled) {
            fprintf(stderr,
                    "[WPRINTV] desc=0x%04X src=0x%04X N=%u O=%u -> false (SETWIN)\n",
                    descriptor_handle, source_handle, char_count, source_offset);
        }
        return LPST_FALSE_SENTINEL;
    }

    if (count > 0) {
        host_print_vector(state, source_handle, (uint16_t)count, (uint16_t)src_start);
    }

    preview_len = 0;
    if (count > 0) {
        int preview_count = count < 100 ? count : 100;
        int preview_index;

        for (preview_index = 0; preview_index < preview_count; preview_index++) {
            uint8_t ch = read_aggregate_payload_byte(state, source_handle, src_start + preview_index);
            preview[preview_len++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
        }
    }
    preview[preview_len] = '\0';

    write_aggregate_word(state, descriptor_handle, 0, (uint16_t)end_req);
    execute_setwin(state, descriptor_handle);
    if (state->trace_enabled) {
        fprintf(stderr,
                "[WPRINTV] desc=0x%04X src=0x%04X N=%u O=%u count=%d -> 0x%04X \"%s\"\n",
                descriptor_handle,
                source_handle,
                char_count,
                source_offset,
                count,
                setwin_result,
                preview);
    }
    return setwin_result;
}

void host_apply_disp(lpst_exec_state *state, uint8_t subop)
{
    lpst_host *host = state->host;
    uint8_t row = state->cursor_row;
    uint8_t col = state->cursor_col;
    uint8_t width = get_visible_screen_width(state);

    switch (subop) {
    case 0x00:
        if (col + 1 < width) { col++; }
        break;
    case 0x01:
        if (col > 0) { col--; }
        break;
    case 0x02:
        if (row < LPST_SCREEN_HEIGHT - 1) { row++; }
        break;
    case 0x03:
        if (row > 0) { row--; }
        break;
    case 0x04:
        host->erase_to_eol(host, row, col);
        break;
    case 0x05:
        host->erase_to_eol(host, row, col);
        if (row + 1 < LPST_SCREEN_HEIGHT) {
            host->clear_rows(host, (uint8_t)(row + 1), LPST_SCREEN_HEIGHT - 1);
        }
        break;
    default:
        fprintf(stderr, "unhandled DISP subop 0x%02X\n", subop);
        break;
    }

    state->cursor_row = row;
    state->cursor_col = col;
    host->set_cursor(host, row, col);
}
