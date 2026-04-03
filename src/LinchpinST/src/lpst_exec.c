/*
 * lpst_exec.c — Execution-state initialisation and teardown.
 *
 * lpst_exec_init prepares the full runtime environment for a loaded VM image:
 *   - Allocates the four 32 KB RAM banks and seeds them from the image's
 *     initial-RAM snapshot.
 *   - Allocates the LRU code-cache, sized to consume up to one quarter of
 *     available free memory (with a safe fallback if detection fails).
 *   - Copies program-global and module-global initial values from the image.
 *   - Initialises the system-module global table (screen dimensions, date/
 *     time, channel descriptor, and the DBF extension token).
 *   - Opens the OBJ file for bytecode I/O and pre-registers it as channel 1.
 *   - Sets up the entry-point frame so lpst_run can begin immediately.
 *
 * lpst_exec_free releases all memory allocated by lpst_exec_init.
 */
#include "lpst_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__MINT__) || defined(__unix__) || defined(__APPLE__) || defined(_POSIX_VERSION)
#include <unistd.h>
#endif

#if defined(__MINT__)
#include <mint/sysbind.h>
#endif

#define LPST_CODE_CACHE_BUDGET_DIVISOR 4u
#define LPST_CODE_CACHE_FALLBACK_PAGES 128u
#define LPST_CODE_CACHE_MIN_PAGES 1u
#define LPST_CODE_CACHE_RESERVE_BYTES (64u * 1024u)

static void free_ram_banks(lpst_exec_state *state)
{
    size_t index;

    if (state == NULL) {
        return;
    }

    for (index = 0; index < LPST_RAM_BANK_COUNT; index++) {
        free(state->ram_banks[index]);
        state->ram_banks[index] = NULL;
    }
}

static bool allocate_ram_banks(lpst_exec_state *state)
{
    size_t index;

    for (index = 0; index < LPST_RAM_BANK_COUNT; index++) {
        state->ram_banks[index] = (uint8_t *)malloc(LPST_RAM_BANK_BYTES);
        if (state->ram_banks[index] == NULL) {
            free_ram_banks(state);
            return false;
        }
    }

    return true;
}

static void free_jump_snapshot(lpst_jump_snapshot *snapshot)
{
    uint16_t i;

    if (!snapshot->in_use) {
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
    snapshot->in_use = false;
}

/* Query the operating system for the amount of free memory.  Used to size the
 * code cache proportionally on memory-constrained platforms (Atari MiNT,
 * POSIX).  Returns 0 when detection is not supported. */
static size_t detect_available_memory_bytes(void)
{
#if defined(__MINT__)
    if (_base != NULL && _base->p_hitpa != NULL && _base->p_lowtpa != NULL
        && _base->p_hitpa > _base->p_lowtpa) {
        return (size_t)(_base->p_hitpa - _base->p_lowtpa);
    }

    {
        long available = Malloc(-1L);
        if (available > 0) {
            return (size_t)available;
        }
    }
#endif

#if defined(_SC_AVPHYS_PAGES)
    {
        long page_count = sysconf(_SC_AVPHYS_PAGES);
        long page_size = -1;

#if defined(_SC_PAGESIZE)
        page_size = sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
        page_size = sysconf(_SC_PAGE_SIZE);
#endif

        if (page_count > 0 && page_size > 0) {
            return (size_t)page_count * (size_t)page_size;
        }
    }
#endif

    return 0;
}

static size_t compute_total_code_pages(const lpst_image *image)
{
    if (image == NULL || image->code_end_offset == 0) {
        return LPST_CODE_CACHE_MIN_PAGES;
    }

    return (image->code_end_offset + (LPST_CODE_PAGE_SIZE - 1u)) / LPST_CODE_PAGE_SIZE;
}

/* Choose the number of code-cache slots to allocate.
 *
 * Strategy: reserve 64 KB for non-cache use, then spend at most one quarter
 * of the remaining free memory on cache slots.  If detection fails or the
 * result is too small, fall back to LPST_CODE_CACHE_FALLBACK_PAGES (128).
 * Never allocate more slots than the total number of distinct 256-byte pages
 * in the OBJ file (there would be nothing to put in additional slots). */
static size_t choose_code_cache_entry_count(const lpst_image *image)
{
    size_t total_code_pages = compute_total_code_pages(image);
    size_t available_bytes = detect_available_memory_bytes();
    size_t budget_bytes;
    size_t desired_entries;

    if (available_bytes > LPST_CODE_CACHE_RESERVE_BYTES) {
        budget_bytes = (available_bytes - LPST_CODE_CACHE_RESERVE_BYTES) / LPST_CODE_CACHE_BUDGET_DIVISOR;
    } else if (available_bytes > 0) {
        budget_bytes = available_bytes / LPST_CODE_CACHE_BUDGET_DIVISOR;
    } else {
        budget_bytes = 0;
    }

    desired_entries = budget_bytes / sizeof(lpst_code_cache_entry);
    if (desired_entries < LPST_CODE_CACHE_MIN_PAGES) {
        desired_entries = LPST_CODE_CACHE_FALLBACK_PAGES;
    }

    if (desired_entries > total_code_pages) {
        desired_entries = total_code_pages;
    }

    if (desired_entries < LPST_CODE_CACHE_MIN_PAGES) {
        desired_entries = LPST_CODE_CACHE_MIN_PAGES;
    }

    return desired_entries;
}

static bool allocate_code_cache(lpst_exec_state *state)
{
    size_t entry_count;

    if (state == NULL || state->image == NULL) {
        return false;
    }

    entry_count = choose_code_cache_entry_count(state->image);
    while (entry_count >= LPST_CODE_CACHE_MIN_PAGES) {
        state->code_cache_entries = (lpst_code_cache_entry *)calloc(entry_count, sizeof(lpst_code_cache_entry));
        if (state->code_cache_entries != NULL) {
            state->code_cache_entry_count = entry_count;
            state->code_cache_tick = 0;
            return true;
        }

        if (entry_count == LPST_CODE_CACHE_MIN_PAGES) {
            break;
        }

        entry_count /= 2u;
        if (entry_count < LPST_CODE_CACHE_MIN_PAGES) {
            entry_count = LPST_CODE_CACHE_MIN_PAGES;
        }
    }

    return false;
}

lpst_result lpst_exec_init(lpst_exec_state *state, const lpst_image *image, lpst_host *host)
{
    const lpst_module *entry_mod;
    const lpst_procedure *entry_proc;
    uint16_t i;

    if (state == NULL || image == NULL || host == NULL) {
        return LPST_ERR_NULL_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->image = image;
    state->host = host;
    state->cursor_row = 0;
    state->cursor_col = 0;
    state->instruction_limit = 5000000u;
    state->current_code_page_bytes = NULL;
    state->current_code_page_base = 0;
    state->current_code_page_limit = 0;
    state->code_fp = fopen(image->obj_path, "rb");
    if (state->code_fp == NULL) {
        return LPST_ERR_IO;
    }

    if (!allocate_code_cache(state)) {
        lpst_exec_free(state);
        return LPST_ERR_ALLOC;
    }

    if (!allocate_ram_banks(state)) {
        lpst_exec_free(state);
        return LPST_ERR_ALLOC;
    }

    if (!lpst_seed_initial_ram(
            state->ram_banks,
            image->initial_ram_bytes,
            image->initial_ram_size)) {
        return LPST_ERR_INITIAL_RAM_TRUNCATED;
    }

    state->program_global_count = image->globals.program_global_count;
    state->program_globals = (uint16_t *)calloc(state->program_global_count, sizeof(uint16_t));
    if (state->program_globals == NULL && state->program_global_count > 0) {
        return LPST_ERR_ALLOC;
    }

    for (i = 0; i < state->program_global_count; i++) {
        state->program_globals[i] = image->globals.program_globals[i];
    }

    state->module_count = image->globals.module_count;
    state->module_global_counts = (uint16_t *)calloc(state->module_count, sizeof(uint16_t));
    state->module_globals = (uint16_t **)calloc(state->module_count, sizeof(uint16_t *));
    if ((state->module_global_counts == NULL || state->module_globals == NULL) && state->module_count > 0) {
        lpst_exec_free(state);
        return LPST_ERR_ALLOC;
    }

    for (i = 0; i < state->module_count; i++) {
        uint16_t count = image->globals.module_global_counts[i];
        uint16_t j;

        state->module_global_counts[i] = count;
        if (count > 0) {
            state->module_globals[i] = (uint16_t *)calloc(count, sizeof(uint16_t));
            if (state->module_globals[i] == NULL) {
                lpst_exec_free(state);
                return LPST_ERR_ALLOC;
            }

            for (j = 0; j < count; j++) {
                state->module_globals[i][j] = image->globals.module_globals[i][j];
            }
        }
    }

    /* Screen dimensions: stored as (width-1) and (height-1) so that the VM
     * can range-check coordinates with a single unsigned comparison. */
    memset(state->system_module_globals, 0, sizeof(state->system_module_globals));
    {
        uint16_t screen_width = host->screen_width > 0 ? host->screen_width : LPST_DEFAULT_SCREEN_WIDTH;
        uint16_t screen_height = host->screen_height > 0 ? host->screen_height : LPST_SCREEN_HEIGHT;

        if (screen_width > LPST_SCREEN_WIDTH) {
            screen_width = LPST_SCREEN_WIDTH;
        }

        if (screen_height > LPST_SCREEN_HEIGHT) {
            screen_height = LPST_SCREEN_HEIGHT;
        }

        /* 0xC7 = screen width - 1, 0xC8 = screen height - 1. */
        state->system_module_globals[0xC7] = (uint16_t)(screen_width - 1u);
        state->system_module_globals[0xC8] = (uint16_t)(screen_height - 1u);
    }
    /* 0xC9 = cursor column, 0xCA = cursor row (both zero-based). */
    state->system_module_globals[0xC9] = 0;
    state->system_module_globals[0xCA] = 0;
    /* 0xCD-0xD2 = month, day, year, hour, minute, second (from host clock). */
    {
        uint16_t month = 1;
        uint16_t day = 1;
        uint16_t year = 1987;
        uint16_t hour = 0;
        uint16_t minute = 0;
        uint16_t second = 0;

        if (host->get_date != NULL) {
            host->get_date(host, &month, &day, &year);
        }

        if (host->get_time != NULL) {
            host->get_time(host, &hour, &minute, &second);
        }

        state->system_module_globals[0xCD] = month;
        state->system_module_globals[0xCE] = day;
        state->system_module_globals[0xCF] = year;
        state->system_module_globals[0xD0] = hour;
        state->system_module_globals[0xD1] = minute;
        state->system_module_globals[0xD2] = second;
    }
    state->system_module_globals[0xD4] = LPST_FALSE_SENTINEL;

    /* 0xD7 = EncodeExtensionToken("DBF"): the Cornerstone VM uses a compact
     * three-letter code packed as (c0-'A')<<11 | (c1-'A')*0x2D | (c2-'A').
     * "DBF" → (3<<11) + (1*0x2D) + 5 = 0x185E. */
    state->system_module_globals[0xD7] = (uint16_t)((3u << 11) + (1u * 0x2Du) + 5u);

    /* Initialize channel table */
    memset(state->channels, 0, sizeof(state->channels));
    state->next_channel_id = 1;
    state->active_channel_id = LPST_FALSE_SENTINEL;

    /* Pre-open the OBJ file as channel 1 so the VM can do record-based I/O on
     * its own bytecode file.  Publish the channel descriptor in system slot
     * 0xDA as (code_page_count << 5) | channel_id, matching the encoding used
     * by the VM to locate data records embedded after the code pages. */
    if (image->obj_path[0] != '\0') {
        uint16_t channel_id = state->next_channel_id++;
        lpst_channel *ch = &state->channels[0];
        uint16_t code_page_count = image->header_words[7];

        ch->fp = fopen(image->obj_path, "rb");
        if (ch->fp == NULL) {
            fprintf(stderr, "warning: failed to open OBJ file for channel: %s\n",
                    image->obj_path);
        }

        ch->mode = 0x02;   /* read-only */
        ch->position = 0;
        ch->in_use = true;
        snprintf(ch->name, sizeof(ch->name), "CORNER.OBJ");
        snprintf(ch->path, sizeof(ch->path), "%s", image->obj_path);

        state->system_module_globals[0xDA] =
            (uint16_t)((code_page_count << 5) | (channel_id & 0x1F));
    }

    entry_mod = &image->modules[image->entry_module_id - 1];
    entry_proc = &entry_mod->procedures[image->entry_procedure_index];

    memset(state->local_storage, 0, sizeof(state->local_storage));
    lpst_bootstrap_locals(
        state->local_storage,
        entry_proc->local_count,
        entry_proc->initializers,
        entry_proc->initializer_count);

    lpst_initialize_frame(
        &state->current_frame,
        entry_mod->module_id,
        (uint16_t)entry_proc->exported_index,
        entry_proc->start_offset,
        entry_proc->code_offset,
        state->local_storage,
        entry_proc->local_count);

    state->current_module_id = entry_mod->module_id;
    state->program_counter = entry_proc->code_offset;
    state->frame_upper_bound = entry_proc->upper_bound;

    state->eval_stack_top = 0;
    state->call_stack_top = 0;
    state->is_halted = false;
    state->halt_code = 0;
    state->instruction_count = 0;
    state->last_return_word_count = 0;
    state->trace_enabled = false;
    state->next_jump_token = 0xA000u;

    /* Initialize arena allocator.
     * Low arena starts just past the initial RAM data (even-aligned).
     * High arena starts at the beginning of the high segment.
     * Tuple stack grows down from the top of the high segment. */
    state->next_low_arena_byte = (uint32_t)((image->initial_ram_size + 1u) & ~(size_t)1u);
    state->next_high_arena_byte = LPST_SEGMENT_SIZE_BYTES;
    state->tuple_stack_byte = LPST_FULL_RAM_BYTES;
    state->tuple_stack_floor_byte = (uint32_t)(LPST_FULL_RAM_BYTES - LPST_TUPLE_STACK_RESERVE_BYTES);
    memset(state->free_list, 0, sizeof(state->free_list));

    return LPST_OK;
}

void lpst_exec_free(lpst_exec_state *state)
{
    uint16_t i;

    if (state == NULL) {
        return;
    }

    free(state->code_cache_entries);
    state->code_cache_entries = NULL;
    state->code_cache_entry_count = 0;
    state->code_cache_tick = 0;

    free_ram_banks(state);

    /* Close all open channels */
    if (state->code_fp != NULL) {
        fclose(state->code_fp);
        state->code_fp = NULL;
    }

    for (i = 0; i < LPST_MAX_CHANNELS; i++) {
        if (state->channels[i].in_use && state->channels[i].fp != NULL) {
            fclose(state->channels[i].fp);
            state->channels[i].fp = NULL;
            state->channels[i].in_use = false;
        }
    }

    free(state->program_globals);
    state->program_globals = NULL;

    if (state->module_globals != NULL) {
        for (i = 0; i < state->module_count; i++) {
            free(state->module_globals[i]);
        }

        free(state->module_globals);
        state->module_globals = NULL;
    }

    free(state->module_global_counts);
    state->module_global_counts = NULL;

    for (i = 0; i < LPST_JUMP_SNAPSHOT_CAPACITY; i++) {
        free_jump_snapshot(&state->jump_snapshots[i]);
    }

    for (i = 0; i < state->call_stack_top; i++) {
        free(state->call_stack[i].saved_locals);
        state->call_stack[i].saved_locals = NULL;
    }
}

void lpst_exec_push(lpst_exec_state *state, uint16_t value)
{
    if (state->eval_stack_top >= LPST_EVAL_STACK_CAPACITY) {
        fprintf(stderr, "evaluation stack overflow\n");
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return;
    }

    state->eval_stack[state->eval_stack_top++] = value;
}

uint16_t lpst_exec_pop(lpst_exec_state *state)
{
    if (state->eval_stack_top == 0) {
        fprintf(stderr, "evaluation stack underflow\n");
        state->is_halted = true;
        state->halt_code = 0xFFFF;
        return 0;
    }

    return state->eval_stack[--state->eval_stack_top];
}

uint16_t lpst_exec_peek(const lpst_exec_state *state)
{
    if (state->eval_stack_top == 0) {
        fprintf(stderr, "peek on empty evaluation stack\n");
        return 0;
    }

    return state->eval_stack[state->eval_stack_top - 1];
}
