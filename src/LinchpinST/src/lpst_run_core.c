/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_core.c — Code-cache, RAM access, and aggregate helpers.
 *
 * The LRU code cache sits between the execution loop and the OBJ file.  Each
 * slot holds a 256-byte page of bytecode; pages are evicted least-recently-
 * used when all slots are occupied.  The cache tick counter ages slots so
 * that the victim selection can compare only integer timestamps.
 *
 * RAM is divided into four 32 KB banks (two banks per logical segment):
 *   low segment  = banks[0] + banks[1]  (handles with bit 15 clear)
 *   high segment = banks[2] + banks[3]  (handles with bit 15 set)
 *
 * An aggregate handle is a 16-bit word identifier for a heap object:
 *   bit 15  = segment selector (0=low, 1=high)
 *   bits 14:0 = word address within the segment
 * The four bytes at that address are a header word; the payload starts two
 * bytes later (word index 0 in the aggregate reads the header word itself).
 */
#include "lpst_run_internal.h"

/* Update the three fast-path fields so the execution loop can fetch bytes
 * within this page without going through load_code_page on every instruction. */
static void bind_current_code_page(lpst_exec_state *state, const lpst_code_cache_entry *entry)
{
    if (entry == NULL || !entry->valid) {
        state->current_code_page_bytes = NULL;
        state->current_code_page_base = 0;
        state->current_code_page_limit = 0;
        return;
    }

    state->current_code_page_bytes = entry->bytes;
    state->current_code_page_base = entry->page_base;
    state->current_code_page_limit = entry->page_base + (uint32_t)entry->page_size;
}

const lpst_module *current_module(const lpst_exec_state *state)
{
    return &state->image->modules[state->current_module_id - 1];
}

void fill_cached_proc_header(const lpst_procedure *proc, lpst_proc_header *out_header)
{
    out_header->local_count = proc->local_count;
    out_header->header_size = (uint8_t)(proc->code_offset - proc->start_offset);
    out_header->initializer_count = proc->initializer_count;

    if (proc->initializer_count > 0 && proc->initializers != NULL) {
        memcpy(out_header->initializers,
               proc->initializers,
               (size_t)proc->initializer_count * sizeof(lpst_proc_initializer));
    }
}

/* Increment the global tick counter (wrapping safely by resetting all slots
 * to 0 and starting at 1 if it overflows) and stamp entry with the new tick.
 * Entries with higher ticks are considered more recently used. */
static void touch_code_cache_entry(lpst_exec_state *state, lpst_code_cache_entry *entry)
{
    state->code_cache_tick++;
    if (state->code_cache_tick == 0) {
        size_t index;

        state->code_cache_tick = 1;
        for (index = 0; index < state->code_cache_entry_count; index++) {
            if (state->code_cache_entries[index].valid) {
                state->code_cache_entries[index].last_used_tick = 0;
            }
        }
    }

    entry->last_used_tick = state->code_cache_tick;
}

static lpst_code_cache_entry *find_code_cache_entry(lpst_exec_state *state, uint32_t page_base)
{
    size_t index;

    for (index = 0; index < state->code_cache_entry_count; index++) {
        lpst_code_cache_entry *entry = &state->code_cache_entries[index];
        if (entry->valid && entry->page_base == page_base) {
            return entry;
        }
    }

    return NULL;
}

/* Choose the LRU victim: the first empty slot wins; otherwise the slot with
 * the smallest last_used_tick is evicted. */
static lpst_code_cache_entry *select_code_cache_victim(lpst_exec_state *state)
{
    size_t index;
    lpst_code_cache_entry *victim = NULL;

    for (index = 0; index < state->code_cache_entry_count; index++) {
        lpst_code_cache_entry *entry = &state->code_cache_entries[index];
        if (!entry->valid) {
            return entry;
        }

        if (victim == NULL || entry->last_used_tick < victim->last_used_tick) {
            victim = entry;
        }
    }

    return victim;
}

/* Load the 256-byte page that contains module_offset into the cache.
 *
 * The absolute OBJ-file offset is (mod->object_offset + module_offset); the
 * page boundary is aligned down to the nearest 256 bytes.  After loading,
 * the page is bound as the current page so subsequent fetch_byte calls can
 * use the inline fast path instead of calling back into this function. */
lpst_code_cache_entry *load_code_page(lpst_exec_state *state, const lpst_module *mod, uint16_t module_offset)
{
    uint32_t abs = mod->object_offset + module_offset;
    uint32_t page_base = abs & ~0xFFu;
    uint32_t module_end = mod->object_offset + mod->length;
    lpst_code_cache_entry *entry;
    size_t to_read;

    if (state->code_fp == NULL || abs >= module_end || state->code_cache_entries == NULL
        || state->code_cache_entry_count == 0) {
        return NULL;
    }

    entry = find_code_cache_entry(state, page_base);
    if (entry != NULL) {
        state->code_cache_hits++;
        touch_code_cache_entry(state, entry);
        bind_current_code_page(state, entry);
        return entry;
    }

    state->code_cache_misses++;

    to_read = (size_t)(module_end - page_base);
    if (to_read > LPST_CODE_PAGE_SIZE) {
        to_read = LPST_CODE_PAGE_SIZE;
    }

    entry = select_code_cache_victim(state);
    if (entry == NULL) {
        return NULL;
    }

    if (entry->valid) {
        state->code_cache_evictions++;
    }

    if (fseek(state->code_fp, (long)page_base, SEEK_SET) != 0) {
        return NULL;
    }

    if (fread(entry->bytes, 1, to_read, state->code_fp) != to_read) {
        return NULL;
    }

    if (to_read < LPST_CODE_PAGE_SIZE) {
        memset(&entry->bytes[to_read], 0, LPST_CODE_PAGE_SIZE - to_read);
    }

    entry->page_base = page_base;
    entry->page_size = to_read;
    entry->valid = true;
    touch_code_cache_entry(state, entry);
    bind_current_code_page(state, entry);
    return entry;
}

uint8_t read_code_byte_at(lpst_exec_state *state, const lpst_module *mod, uint16_t module_offset)
{
    uint32_t abs = mod->object_offset + module_offset;
    uint32_t page_offset = abs & 0xFFu;
    lpst_code_cache_entry *entry = load_code_page(state, mod, module_offset);

    if (entry == NULL || page_offset >= entry->page_size) {
        fprintf(stderr, "code page load failed at module %u offset 0x%04X\n",
                mod->module_id, module_offset);
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return 0;
    }

    return entry->bytes[page_offset];
}

uint8_t fetch_byte(lpst_exec_state *state)
{
    const lpst_module *mod = current_module(state);
    uint32_t abs = mod->object_offset + state->program_counter;
    uint8_t value;

    if (state->current_code_page_bytes != NULL
        && abs >= state->current_code_page_base
        && abs < state->current_code_page_limit) {
        value = state->current_code_page_bytes[abs - state->current_code_page_base];
    } else {
        value = read_code_byte_at(state, mod, state->program_counter);
    }

    state->program_counter++;
    return value;
}

uint16_t fetch_le16(lpst_exec_state *state)
{
    uint8_t lo = fetch_byte(state);
    uint8_t hi = fetch_byte(state);
    return (uint16_t)(lo | (hi << 8));
}

/* Decode a conditional/unconditional branch target.
 *
 * Encoding: the first byte of the target operand selects the form:
 *   byte != 0  → short form: target = instruction_start + 2 + (int8_t)byte
 *   byte == 0  → long form:  target = the following 16-bit LE word
 *
 * The +2 accounts for the opcode byte and the selector byte already consumed. */
uint16_t decode_jump_target(lpst_exec_state *state, uint16_t instruction_start)
{
    uint8_t selector = fetch_byte(state);
    if (selector != 0) {
        int16_t disp = (int16_t)(int8_t)selector;
        return (uint16_t)(instruction_start + 2 + disp);
    }

    return fetch_le16(state);
}

/* Convert a 16-bit aggregate handle to an absolute byte offset in the RAM
 * banks.  Bit 15 selects the segment: clear = low (0x00000–0xFFFF), set =
 * high (0x10000–0x1FFFF).  The remaining 15 bits are the word address, so
 * multiply by 2 to get a byte offset within the segment. */
uint32_t handle_to_byte_offset(uint16_t handle)
{
    uint32_t segment_base = (handle & 0x8000u) ? 0x10000u : 0u;
    uint32_t word_addr = handle & 0x7FFFu;
    return segment_base + word_addr * 2u;
}

static uint32_t ram_bank_index_for_offset(uint32_t byte_offset)
{
    return byte_offset >> LPST_RAM_BANK_SHIFT;
}

static uint32_t ram_bank_offset_for_offset(uint32_t byte_offset)
{
    return byte_offset & LPST_RAM_BANK_MASK;
}

uint8_t read_ram_byte(const lpst_exec_state *state, uint32_t byte_offset)
{
    uint32_t bank_index = ram_bank_index_for_offset(byte_offset);
    uint32_t bank_offset = ram_bank_offset_for_offset(byte_offset);

    return state->ram_banks[bank_index][bank_offset];
}

void write_ram_byte(lpst_exec_state *state, uint32_t byte_offset, uint8_t value)
{
    uint32_t bank_index = ram_bank_index_for_offset(byte_offset);
    uint32_t bank_offset = ram_bank_offset_for_offset(byte_offset);

    state->ram_banks[bank_index][bank_offset] = value;
}

void zero_ram_bytes(lpst_exec_state *state, uint32_t byte_offset, size_t byte_count)
{
    while (byte_count > 0) {
        uint32_t bank_index = ram_bank_index_for_offset(byte_offset);
        uint32_t bank_offset = ram_bank_offset_for_offset(byte_offset);
        size_t chunk = LPST_RAM_BANK_BYTES - bank_offset;

        if (chunk > byte_count) {
            chunk = byte_count;
        }

        memset(&state->ram_banks[bank_index][bank_offset], 0, chunk);
        byte_offset += (uint32_t)chunk;
        byte_count -= chunk;
    }
}

void move_ram_bytes(lpst_exec_state *state, uint32_t destination_offset, uint32_t source_offset, size_t byte_count)
{
    if (byte_count == 0 || destination_offset == source_offset) {
        return;
    }

    if (destination_offset < source_offset) {
        while (byte_count > 0) {
            uint32_t source_bank_index = ram_bank_index_for_offset(source_offset);
            uint32_t source_bank_offset = ram_bank_offset_for_offset(source_offset);
            uint32_t destination_bank_index = ram_bank_index_for_offset(destination_offset);
            uint32_t destination_bank_offset = ram_bank_offset_for_offset(destination_offset);
            size_t chunk = LPST_RAM_BANK_BYTES - source_bank_offset;
            size_t destination_chunk = LPST_RAM_BANK_BYTES - destination_bank_offset;

            if (chunk > destination_chunk) {
                chunk = destination_chunk;
            }

            if (chunk > byte_count) {
                chunk = byte_count;
            }

            memmove(
                &state->ram_banks[destination_bank_index][destination_bank_offset],
                &state->ram_banks[source_bank_index][source_bank_offset],
                chunk);
            destination_offset += (uint32_t)chunk;
            source_offset += (uint32_t)chunk;
            byte_count -= chunk;
        }
    } else {
        while (byte_count > 0) {
            uint32_t source_end_offset = source_offset + (uint32_t)byte_count;
            uint32_t destination_end_offset = destination_offset + (uint32_t)byte_count;
            uint32_t source_bank_index = ram_bank_index_for_offset(source_end_offset - 1u);
            uint32_t source_bank_offset = ram_bank_offset_for_offset(source_end_offset - 1u);
            uint32_t destination_bank_index = ram_bank_index_for_offset(destination_end_offset - 1u);
            uint32_t destination_bank_offset = ram_bank_offset_for_offset(destination_end_offset - 1u);
            size_t chunk = source_bank_offset + 1u;
            size_t destination_chunk = destination_bank_offset + 1u;

            if (chunk > destination_chunk) {
                chunk = destination_chunk;
            }

            if (chunk > byte_count) {
                chunk = byte_count;
            }

            memmove(
                &state->ram_banks[destination_bank_index][destination_bank_offset + 1u - chunk],
                &state->ram_banks[source_bank_index][source_bank_offset + 1u - chunk],
                chunk);
            byte_count -= chunk;
        }
    }
}

int lpst_ascii_stricmp(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        int left_folded = tolower((unsigned char)*left);
        int right_folded = tolower((unsigned char)*right);

        if (left_folded != right_folded) {
            return left_folded - right_folded;
        }

        left++;
        right++;
    }

    return tolower((unsigned char)*left) - tolower((unsigned char)*right);
}

static uint16_t read_ram_word(const lpst_exec_state *state, uint32_t byte_offset)
{
    return (uint16_t)(read_ram_byte(state, byte_offset) |
                      (read_ram_byte(state, byte_offset + 1u) << 8));
}

static void write_ram_word(lpst_exec_state *state, uint32_t byte_offset, uint16_t value)
{
    write_ram_byte(state, byte_offset, (uint8_t)(value & 0xFFu));
    write_ram_byte(state, byte_offset + 1u, (uint8_t)(value >> 8));
}

/* Read a 16-bit word from an aggregate at word_index.
 * word_index 0 reads the two-byte header (the aggregate size/type word);
 * word_index >= 1 reads from the payload.  All internal aggregate helpers
 * use word_index 0 for the header to stay consistent with the VM's own
 * convention. */
uint16_t read_aggregate_word(const lpst_exec_state *state, uint16_t handle, int word_index)
{
    uint32_t base = handle_to_byte_offset(handle);
    return read_ram_word(state, base + (uint32_t)word_index * 2u);
}

void write_aggregate_word(lpst_exec_state *state, uint16_t handle, int word_index, uint16_t value)
{
    uint32_t base = handle_to_byte_offset(handle);

    write_ram_word(state, base + (uint32_t)word_index * 2u, value);
}

/* Read a payload byte from an aggregate.  byte_index 0 is the first byte
 * after the two-byte header word, i.e., at absolute offset base+2. */
uint8_t read_aggregate_payload_byte(const lpst_exec_state *state, uint16_t handle, int byte_index)
{
    uint32_t base = handle_to_byte_offset(handle);
    return read_ram_byte(state, base + 2u + (uint32_t)byte_index);
}

/* Read a raw byte at raw_byte_offset from the aggregate base (no +2 skip).
 * Used when callers need to access the header bytes directly. */
uint8_t read_aggregate_raw_byte(const lpst_exec_state *state, uint16_t handle, int raw_byte_offset)
{
    uint32_t base = handle_to_byte_offset(handle);
    return read_ram_byte(state, base + (uint32_t)raw_byte_offset);
}

void write_aggregate_payload_byte(lpst_exec_state *state, uint16_t handle, int byte_index, uint8_t value)
{
    uint32_t base = handle_to_byte_offset(handle);
    write_ram_byte(state, base + 2u + (uint32_t)byte_index, value);
}

/* Copy payload bytes between two aggregates, adding the +2 header skip on
 * both sides so callers can reason in terms of payload offsets. */
void copy_aggregate_bytes(lpst_exec_state *state,
    uint16_t source_handle,
    int source_offset,
    uint16_t destination_handle,
    int destination_offset,
    uint16_t count)
{
    move_ram_bytes(
        state,
        handle_to_byte_offset(destination_handle) + 2u + (uint32_t)destination_offset,
        handle_to_byte_offset(source_handle) + 2u + (uint32_t)source_offset,
        count);
}

/* Copy raw bytes from source (no +2 skip) into the destination payload
 * (+2 skip).  Used when the source region starts at the aggregate base rather
 * than at the payload. */
void copy_aggregate_raw_bytes_to_payload(lpst_exec_state *state,
    uint16_t source_handle,
    int source_offset,
    uint16_t destination_handle,
    int destination_offset,
    uint16_t count)
{
    move_ram_bytes(
        state,
        handle_to_byte_offset(destination_handle) + 2u + (uint32_t)destination_offset,
        handle_to_byte_offset(source_handle) + (uint32_t)source_offset,
        count);
}

int is_usable_aggregate_handle(uint16_t handle)
{
    return handle != 0 && handle != LPST_FALSE_SENTINEL;
}

void record_recent_word_event(
    lpst_recent_word_event *events,
    uint16_t capacity,
    uint16_t *next_index,
    uint16_t *count,
    uint16_t module_id,
    uint16_t pc,
    uint16_t value)
{
    uint16_t slot;

    if (capacity == 0) {
        return;
    }

    slot = *next_index;
    events[slot].module_id = module_id;
    events[slot].pc = pc;
    events[slot].value = value;

    *next_index = (uint16_t)((slot + 1u) % capacity);
    if (*count < capacity) {
        (*count)++;
    }
}

void record_recent_p201_event(lpst_exec_state *state, uint16_t instruction_start)
{
    lpst_recent_p201_event *event;
    uint16_t slot;

    slot = state->recent_p201_next;
    event = &state->recent_p201[slot];
    event->pc = instruction_start;
    event->l0 = state->current_frame.local_count > 0 ? state->local_storage[0] : LPST_FALSE_SENTINEL;
    event->l1 = state->current_frame.local_count > 1 ? state->local_storage[1] : LPST_FALSE_SENTINEL;
    event->l2 = state->current_frame.local_count > 2 ? state->local_storage[2] : LPST_FALSE_SENTINEL;
    event->l3 = state->current_frame.local_count > 3 ? state->local_storage[3] : LPST_FALSE_SENTINEL;
    event->l4 = state->current_frame.local_count > 4 ? state->local_storage[4] : LPST_FALSE_SENTINEL;

    state->recent_p201_next = (uint16_t)((slot + 1u) % LPST_RECENT_P201_EVENTS);
    if (state->recent_p201_count < LPST_RECENT_P201_EVENTS) {
        state->recent_p201_count++;
    }
}

void record_m5p128_trace_event(lpst_exec_state *state, uint16_t instruction_start)
{
    lpst_m5p128_trace_event *event;
    uint16_t slot;

    if (state->current_module_id != 5) {
        return;
    }
    if (instruction_start < 0x3D97u || instruction_start >= 0x3DA2u) {
        return;
    }

    slot = state->m5p128_trace_next;
    event = &state->m5p128_trace[slot];
    event->pc = instruction_start;
    event->stk_depth = state->eval_stack_top;
    event->stk0 = state->eval_stack_top > 0
        ? state->eval_stack[state->eval_stack_top - 1] : 0;
    event->stk1 = state->eval_stack_top > 1
        ? state->eval_stack[state->eval_stack_top - 2] : 0;
    event->l8 = state->current_frame.local_count > 8 ? state->local_storage[8] : LPST_FALSE_SENTINEL;
    event->l9 = state->current_frame.local_count > 9 ? state->local_storage[9] : LPST_FALSE_SENTINEL;

    state->m5p128_trace_next = (uint16_t)((slot + 1u) % LPST_M5P128_TRACE_EVENTS);
    if (state->m5p128_trace_count < LPST_M5P128_TRACE_EVENTS) {
        state->m5p128_trace_count++;
    }
}

void record_recent_m8_priv0f28_event(lpst_exec_state *state, uint16_t caller_pc, const uint16_t *args, uint8_t argc)
{
    lpst_recent_m8_priv0f28_event *event;
    uint16_t slot;

    slot = state->recent_m8_priv0f28_next;
    event = &state->recent_m8_priv0f28[slot];
    event->caller_module_id = state->current_module_id;
    event->caller_procedure_index = (uint16_t)state->current_frame.procedure_index;
    event->caller_pc = caller_pc;
    event->arg0 = argc > 0 ? args[0] : LPST_FALSE_SENTINEL;
    event->arg1 = argc > 1 ? args[1] : LPST_FALSE_SENTINEL;
    event->arg2 = argc > 2 ? args[2] : LPST_FALSE_SENTINEL;
    event->arg3 = argc > 3 ? args[3] : LPST_FALSE_SENTINEL;
    event->arg4 = argc > 4 ? args[4] : LPST_FALSE_SENTINEL;
    event->arg5 = argc > 5 ? args[5] : LPST_FALSE_SENTINEL;

    state->recent_m8_priv0f28_next = (uint16_t)((slot + 1u) % LPST_RECENT_M8_PRIV0F28_EVENTS);
    if (state->recent_m8_priv0f28_count < LPST_RECENT_M8_PRIV0F28_EVENTS) {
        state->recent_m8_priv0f28_count++;
    }
}

static uint16_t load_module_global_for_module(const lpst_exec_state *state, uint16_t module_id, uint8_t index)
{
    if (module_id >= 1 && module_id <= state->module_count) {
        uint16_t module_index = (uint16_t)(module_id - 1);
        uint16_t count = state->module_global_counts[module_index];

        if (index < count) {
            return state->module_globals[module_index][index];
        }
    }

    return state->system_module_globals[index];
}

void record_recent_m1_startup_event(lpst_exec_state *state, uint16_t instruction_start, uint8_t opcode)
{
    lpst_recent_m1_startup_event *event;
    uint16_t slot;

    if (state->current_module_id != 1) {
        return;
    }

    if (state->current_frame.procedure_index != 2 && state->current_frame.procedure_index != 7) {
        return;
    }

    slot = state->recent_m1_startup_next;
    event = &state->recent_m1_startup[slot];
    event->procedure_index = (uint16_t)state->current_frame.procedure_index;
    event->pc = instruction_start;
    event->opcode = opcode;
    event->g0 = state->program_global_count > 0 ? state->program_globals[0] : LPST_FALSE_SENTINEL;
    event->g1 = state->program_global_count > 1 ? state->program_globals[1] : LPST_FALSE_SENTINEL;
    event->m1g0 = load_module_global_for_module(state, 1, 0);
    event->m8g59 = load_module_global_for_module(state, 8, 0x59);

    state->recent_m1_startup_next = (uint16_t)((slot + 1u) % LPST_RECENT_M1_STARTUP_EVENTS);
    if (state->recent_m1_startup_count < LPST_RECENT_M1_STARTUP_EVENTS) {
        state->recent_m1_startup_count++;
    }
}

static void print_recent_m1_startup_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent M1 startup count=%u [", state->recent_m1_startup_count);
    for (i = 0; i < state->recent_m1_startup_count; i++) {
        uint16_t slot = (uint16_t)((state->recent_m1_startup_next + LPST_RECENT_M1_STARTUP_EVENTS
            - state->recent_m1_startup_count + i) % LPST_RECENT_M1_STARTUP_EVENTS);
        const lpst_recent_m1_startup_event *event = &state->recent_m1_startup[slot];

        if (i > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "{p%u pc=0x%04X op=0x%02X G0=0x%04X G1=0x%04X M1G0=0x%04X M8G59=0x%04X}",
            event->procedure_index,
            event->pc,
            event->opcode,
            event->g0,
            event->g1,
            event->m1g0,
            event->m8g59);
    }
    fprintf(stream, "]\n");
}

void record_recent_m1_proc2_entry_event(
    lpst_exec_state *state,
    uint16_t source_kind,
    uint16_t source_module_id,
    uint16_t source_procedure_index,
    uint16_t source_pc,
    uint16_t selector_or_token,
    uint16_t target_pc)
{
    lpst_recent_m1_proc2_entry_event *event;
    uint16_t slot;

    slot = state->recent_m1_proc2_entries_next;
    event = &state->recent_m1_proc2_entries[slot];
    event->source_kind = source_kind;
    event->source_module_id = source_module_id;
    event->source_procedure_index = source_procedure_index;
    event->source_pc = source_pc;
    event->selector_or_token = selector_or_token;
    event->target_pc = target_pc;

    state->recent_m1_proc2_entries_next = (uint16_t)((slot + 1u) % LPST_RECENT_M1_PROC2_ENTRY_EVENTS);
    if (state->recent_m1_proc2_entries_count < LPST_RECENT_M1_PROC2_ENTRY_EVENTS) {
        state->recent_m1_proc2_entries_count++;
    }
}

void record_recent_m2_proc166_return_event(
    lpst_exec_state *state,
    uint16_t caller_module_id,
    uint16_t caller_procedure_index,
    uint16_t caller_pc,
    uint16_t result)
{
    lpst_recent_m2_proc166_return_event *event;
    uint16_t slot;

    slot = state->recent_m2_proc166_returns_next;
    event = &state->recent_m2_proc166_returns[slot];
    event->caller_module_id = caller_module_id;
    event->caller_procedure_index = caller_procedure_index;
    event->caller_pc = caller_pc;
    event->result = result;
    event->l0 = state->current_frame.local_count > 0 ? state->local_storage[0] : LPST_FALSE_SENTINEL;
    event->l1 = state->current_frame.local_count > 1 ? state->local_storage[1] : LPST_FALSE_SENTINEL;
    event->l2 = state->current_frame.local_count > 2 ? state->local_storage[2] : LPST_FALSE_SENTINEL;
    event->l3 = state->current_frame.local_count > 3 ? state->local_storage[3] : LPST_FALSE_SENTINEL;
    event->l4 = state->current_frame.local_count > 4 ? state->local_storage[4] : LPST_FALSE_SENTINEL;

    state->recent_m2_proc166_returns_next =
        (uint16_t)((slot + 1u) % LPST_RECENT_M2_PROC166_RETURN_EVENTS);
    if (state->recent_m2_proc166_returns_count < LPST_RECENT_M2_PROC166_RETURN_EVENTS) {
        state->recent_m2_proc166_returns_count++;
    }
}

void record_recent_open_event(
    lpst_exec_state *state,
    uint16_t result,
    uint16_t error_detail,
    uint8_t mode,
    const char *name)
{
    lpst_recent_open_event *event;
    uint16_t slot;

    slot = state->recent_open_events_next;
    event = &state->recent_open_events[slot];
    event->module_id = state->current_module_id;
    event->procedure_index = (uint16_t)state->current_frame.procedure_index;
    event->pc = (uint16_t)(state->program_counter - 1);
    event->result = result;
    event->error_detail = error_detail;
    event->mode = mode;
    snprintf(event->name, sizeof(event->name), "%s", name != NULL ? name : "");

    state->recent_open_events_next = (uint16_t)((slot + 1u) % LPST_RECENT_OPEN_EVENTS);
    if (state->recent_open_events_count < LPST_RECENT_OPEN_EVENTS) {
        state->recent_open_events_count++;
    }
}

static void print_recent_m1_proc2_entry_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent M1:p2 entries count=%u [", state->recent_m1_proc2_entries_count);
    for (i = 0; i < state->recent_m1_proc2_entries_count; i++) {
        uint16_t slot = (uint16_t)((state->recent_m1_proc2_entries_next + LPST_RECENT_M1_PROC2_ENTRY_EVENTS
            - state->recent_m1_proc2_entries_count + i) % LPST_RECENT_M1_PROC2_ENTRY_EVENTS);
        const lpst_recent_m1_proc2_entry_event *event = &state->recent_m1_proc2_entries[slot];
        const char *kind = "?";

        if (i > 0) {
            fprintf(stream, ", ");
        }

        if (event->source_kind == 1) {
            kind = "CALLF";
        } else if (event->source_kind == 2) {
            kind = "LONGJMP";
        } else if (event->source_kind == 3) {
            kind = "LONGJMPR";
        }

        fprintf(stream,
            "{%s from=m%u:p%u pc=0x%04X key=0x%04X target=0x%04X}",
            kind,
            event->source_module_id,
            event->source_procedure_index,
            event->source_pc,
            event->selector_or_token,
            event->target_pc);
    }
    fprintf(stream, "]\n");
}

static void print_recent_m2_proc166_return_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent M2:p166 returns count=%u [", state->recent_m2_proc166_returns_count);
    for (i = 0; i < state->recent_m2_proc166_returns_count; i++) {
        uint16_t slot = (uint16_t)((state->recent_m2_proc166_returns_next + LPST_RECENT_M2_PROC166_RETURN_EVENTS
            - state->recent_m2_proc166_returns_count + i) % LPST_RECENT_M2_PROC166_RETURN_EVENTS);
        const lpst_recent_m2_proc166_return_event *event = &state->recent_m2_proc166_returns[slot];

        if (i > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "{to=m%u:p%u pc=0x%04X result=0x%04X L0=0x%04X L1=0x%04X L2=0x%04X L3=0x%04X L4=0x%04X}",
            event->caller_module_id,
            event->caller_procedure_index,
            event->caller_pc,
            event->result,
            event->l0,
            event->l1,
            event->l2,
            event->l3,
            event->l4);
    }
    fprintf(stream, "]\n");
}

static void print_recent_open_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent OPEN count=%u [", state->recent_open_events_count);
    for (i = 0; i < state->recent_open_events_count; i++) {
        uint16_t slot = (uint16_t)((state->recent_open_events_next + LPST_RECENT_OPEN_EVENTS
            - state->recent_open_events_count + i) % LPST_RECENT_OPEN_EVENTS);
        const lpst_recent_open_event *event = &state->recent_open_events[slot];
        const char *result_text = "ok";

        if (i > 0) {
            fprintf(stream, ", ");
        }

        switch (event->result) {
        case 1:
            result_text = "empty";
            break;
        case 2:
            result_text = "path";
            break;
        case 3:
            result_text = "channel";
            break;
        case 4:
            result_text = "fopen";
            break;
        default:
            break;
        }

        fprintf(stream,
            "{m%u:p%u pc=0x%04X mode=0x%02X %s err=0x%04X name=\"%s\"}",
            event->module_id,
            event->procedure_index,
            event->pc,
            event->mode,
            result_text,
            event->error_detail,
            event->name);
    }
    fprintf(stream, "]\n");
}

static void print_recent_m8_priv0f28_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent M8:priv_0F28 count=%u [", state->recent_m8_priv0f28_count);
    for (i = 0; i < state->recent_m8_priv0f28_count; i++) {
        uint16_t si = (uint16_t)((state->recent_m8_priv0f28_next + LPST_RECENT_M8_PRIV0F28_EVENTS
            - state->recent_m8_priv0f28_count + i) % LPST_RECENT_M8_PRIV0F28_EVENTS);
        const lpst_recent_m8_priv0f28_event *event = &state->recent_m8_priv0f28[si];

        if (i > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "{m%u:p%u pc=0x%04X args=[0x%04X 0x%04X 0x%04X 0x%04X 0x%04X 0x%04X]}",
            event->caller_module_id,
            event->caller_procedure_index,
            event->caller_pc,
            event->arg0,
            event->arg1,
            event->arg2,
            event->arg3,
            event->arg4,
            event->arg5);
    }
    fprintf(stream, "]\n");
}

static void print_recent_word_events(
    FILE *stream,
    const char *label,
    const lpst_recent_word_event *events,
    uint16_t capacity,
    uint16_t next_index,
    uint16_t count)
{
    uint16_t i;

    fprintf(stream, "%s count=%u [", label, count);
    for (i = 0; i < count; i++) {
        uint16_t slot = (uint16_t)((next_index + capacity - count + i) % capacity);

        if (i > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "m%u:0x%04X=0x%04X",
            events[slot].module_id,
            events[slot].pc,
            events[slot].value);
    }
    fprintf(stream, "]\n");
}

static void print_recent_p201_events(const lpst_exec_state *state, FILE *stream)
{
    uint16_t i;

    fprintf(stream, "[HALTDBG] recent M2:P201 count=%u [", state->recent_p201_count);
    for (i = 0; i < state->recent_p201_count; i++) {
        uint16_t slot = (uint16_t)((state->recent_p201_next + LPST_RECENT_P201_EVENTS
            - state->recent_p201_count + i) % LPST_RECENT_P201_EVENTS);
        const lpst_recent_p201_event *event = &state->recent_p201[slot];

        if (i > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "pc=0x%04X[L0=0x%04X L1=0x%04X L2=0x%04X L3=0x%04X L4=0x%04X]",
            event->pc,
            event->l0,
            event->l1,
            event->l2,
            event->l3,
            event->l4);
    }
    fprintf(stream, "]\n");
}

void lpst_trace_halt_context(const lpst_exec_state *state, FILE *stream)
{
    unsigned local_index;
    unsigned stack_index;
    unsigned call_index;

    if (state == NULL || stream == NULL || (state->halt_code != 0x0011
        && state->halt_code != 0xFFFF
        && state->halt_code != 0x0004
        && state->halt_code != 0x0005)) {
        return;
    }

    if ((state->halt_code == 0x0004 || state->halt_code == 0x0005) && state->current_module_id == 1) {
        fprintf(stream,
            "[HALTDBG] startup globals: G0=0x%04X G1=0x%04X M1G0=0x%04X M8G59=0x%04X\n",
            state->program_global_count > 0 ? state->program_globals[0] : LPST_FALSE_SENTINEL,
            state->program_global_count > 1 ? state->program_globals[1] : LPST_FALSE_SENTINEL,
            load_module_global_for_module(state, 1, 0),
            load_module_global_for_module(state, 8, 0x59));
        print_recent_m1_startup_events(state, stream);
        print_recent_m1_proc2_entry_events(state, stream);
        print_recent_m2_proc166_return_events(state, stream);
    }

    print_recent_open_events(state, stream);

    print_recent_word_events(
        stream,
        "[HALTDBG] recent KBINPUT",
        state->recent_kbinput,
        LPST_RECENT_INPUT_EVENTS,
        state->recent_kbinput_next,
        state->recent_kbinput_count);
    print_recent_word_events(
        stream,
        "[HALTDBG] recent LOOKUP",
        state->recent_ext36,
        LPST_RECENT_EXT36_EVENTS,
        state->recent_ext36_next,
        state->recent_ext36_count);
    print_recent_p201_events(state, stream);
    print_recent_m8_priv0f28_events(state, stream);

    fprintf(stream, "[HALTDBG] eval stack top=%u [", state->eval_stack_top);
    for (stack_index = 0; stack_index < state->eval_stack_top && stack_index < 8; stack_index++) {
        if (stack_index > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream, "S%u=0x%04X",
            stack_index,
            state->eval_stack[state->eval_stack_top - 1 - stack_index]);
    }
    fprintf(stream, "]\n");

    fprintf(stream, "[HALTDBG] call stack depth=%u [", state->call_stack_top);
    for (call_index = 0; call_index < state->call_stack_top && call_index < 8; call_index++) {
        const lpst_call_continuation *continuation = &state->call_stack[state->call_stack_top - 1 - call_index];

        if (call_index > 0) {
            fprintf(stream, ", ");
        }

        fprintf(stream,
            "C%u=m%u:p%u ret=0x%04X",
            call_index,
            continuation->module_id,
            continuation->procedure_index,
            continuation->return_pc);
    }
    fprintf(stream, "]\n");

    {
        const uint16_t depth = state->call_stack_top;
        unsigned ci;
        for (ci = 0; ci < 2 && ci < depth; ci++) {
            const lpst_call_continuation *cont = &state->call_stack[depth - 1 - ci];
            unsigned li;
            fprintf(stream,
                "[HALTDBG] C%u=m%u:p%u saved locals=[",
                ci,
                cont->module_id,
                cont->procedure_index);
            for (li = 0; li < cont->saved_local_count; li++) {
                if (li > 0) {
                    fprintf(stream, ", ");
                }
                fprintf(stream, "L%u=0x%04X", li, cont->saved_locals[li]);
            }
            fprintf(stream, "]\n");
        }
    }

    if (state->halt_code != 0x0011 || state->current_module_id != 2) {
        fprintf(stream, "[HALTDBG] frame: module=%u proc=%u code=0x%04X pc=0x%04X locals=[",
            state->current_module_id,
            state->current_frame.procedure_index,
            state->current_frame.code_offset,
            state->program_counter);
        for (local_index = 0; local_index < state->current_frame.local_count; local_index++) {
            if (local_index > 0) {
                fprintf(stream, ", ");
            }
            fprintf(stream, "L%u=0x%04X", local_index, state->local_storage[local_index]);
        }
        fprintf(stream, "]\n");
        return;
    }

    if (state->current_frame.local_count >= 4) {
        uint16_t handle = state->local_storage[3];

        if (is_usable_aggregate_handle(handle)) {
            unsigned word_index;

            fprintf(stream, "[HALT0011] row handle=0x%04X L3[9]=0x%04X", handle,
                read_aggregate_word(state, handle, 9));

            fprintf(stream, " [");
            for (word_index = 0; word_index < 10; word_index++) {
                if (word_index > 0) {
                    fprintf(stream, ", ");
                }

                fprintf(stream, "%u=0x%04X", word_index, read_aggregate_word(state, handle, (int)word_index));
            }

            fprintf(stream, "]\n");
        }
    }

    if (state->m5p128_trace_count > 0) {
        unsigned ti;
        const uint16_t tc = state->m5p128_trace_count;
        const uint16_t tn = state->m5p128_trace_next;
        fprintf(stream, "[M5P128] trace count=%u:\n", tc);
        for (ti = 0; ti < tc; ti++) {
            uint16_t si = (uint16_t)((tn + LPST_M5P128_TRACE_EVENTS - tc + ti) % LPST_M5P128_TRACE_EVENTS);
            const lpst_m5p128_trace_event *ev = &state->m5p128_trace[si];
            fprintf(stream,
                "  pc=0x%04X stk_depth=%u stk=[0x%04X, 0x%04X] L8=0x%04X L9=0x%04X\n",
                ev->pc, ev->stk_depth, ev->stk0, ev->stk1, ev->l8, ev->l9);
        }
    }

    fprintf(stream, "[HALTDBG] frame: module=%u proc=%u code=0x%04X pc=0x%04X locals=[",
        state->current_module_id,
        state->current_frame.procedure_index,
        state->current_frame.code_offset,
        state->program_counter);
    for (local_index = 0; local_index < state->current_frame.local_count; local_index++) {
        if (local_index > 0) {
            fprintf(stream, ", ");
        }
        fprintf(stream, "L%u=0x%04X", local_index, state->local_storage[local_index]);
    }
    fprintf(stream, "]\n");
}
