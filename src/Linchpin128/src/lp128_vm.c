#include "lp128_vm.h"
#include "lp128_diag.h"

#include <string.h>
#if !(defined(__mos__) || defined(__llvm_mos__))
#include <stdio.h>   /* snprintf */
#include <stdlib.h>  /* malloc/free */
#else
#include "lp128_reu.h"
#endif

#if defined(__mos__) || defined(__llvm_mos__)
#define PERF_VIC_ROW_WIDTH 40u

/* Convert an ASCII character to a VIC-II screen code for direct VIC RAM writes.
 * Upper-case letters and spaces are the primary use case; other characters
 * are passed through unchanged. */
static uint8_t perf_ascii_to_screen(uint8_t ch)
{
    if (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z') {
        return (uint8_t)(ch - (uint8_t)'@');
    }
    if (ch >= (uint8_t)'a' && ch <= (uint8_t)'z') {
        return (uint8_t)(ch - (uint8_t)'`');
    }
    if (ch == (uint8_t)' ') {
        return 0x20u;
    }
    return ch;
}

/* Write a NUL-terminated ASCII string directly to VIC RAM at the given
 * screen row and starting column.  Used for perf counter labels. */
static void perf_write_label(uint8_t row, uint8_t col, const char *text)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * PERF_VIC_ROW_WIDTH + col);

    while (*text != '\0' && col < PERF_VIC_ROW_WIDTH) {
        *dst++ = perf_ascii_to_screen((uint8_t)*text++);
        col++;
    }
}

/* Write a 32-bit hexadecimal value (8 digits) to VIC RAM at the given
 * row and column.  Used to display perf counter values. */
static void perf_write_hex32(uint8_t row, uint8_t col, uint32_t value)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * PERF_VIC_ROW_WIDTH + col);
    uint8_t shift;

    for (shift = 28u;; shift -= 4u) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xFu);
        *dst++ = diag_hex_sc(nibble);
        if (shift == 0u) {
            break;
        }
    }
}

/* Fill one VIC screen row with spaces, clearing any previous content. */
static void perf_clear_row(uint8_t row)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * PERF_VIC_ROW_WIDTH);
    uint8_t col;

    for (col = 0; col < PERF_VIC_ROW_WIDTH; col++) {
        dst[col] = 0x20u;
    }
}

/* Write two labelled 32-bit hex counter values side-by-side on one
 * VIC screen row.  Clears the row before writing. */
static void perf_write_counter_row(uint8_t row,
                                   const char *left_label,
                                   uint32_t left_value,
                                   const char *right_label,
                                   uint32_t right_value)
{
    perf_clear_row(row);
    perf_write_label(row, 0u, left_label);
    perf_write_hex32(row, 3u, left_value);
    perf_write_label(row, 12u, right_label);
    perf_write_hex32(row, 15u, right_value);
}
#endif

/* ── Helpers ────────────────────────────────────────────────────────────── */
/* Write a formatted error message into buf.  On C128, snprintf is not
 * available, so the format string is copied verbatim; detail is ignored. */
static void vm_error(char *buf, size_t buf_size, const char *fmt, const char *detail)
{
    if (buf != NULL && buf_size > 0) {
#if defined(__mos__) || defined(__llvm_mos__)
        /* No snprintf on C128: copy the format string verbatim. */
        size_t n = 0;
        while (n < buf_size - 1u && fmt[n] != '\0') { buf[n] = fmt[n]; n++; }
        buf[n] = '\0';
        (void)detail;
#else
        if (detail != NULL) {
            snprintf(buf, buf_size, fmt, detail);
        } else {
            snprintf(buf, buf_size, "%s", fmt);
        }
#endif
    }
}

#if defined(__mos__) || defined(__llvm_mos__)
/* Round value up to the next multiple of alignment (must be a power of 2). */
static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/* Return the number of REU bytes required to store one full SETJMP snapshot:
 * the header, the locals array, the eval stack, and the call stack. */
static uint32_t jump_snapshot_storage_size(void)
{
    return (uint32_t)sizeof(lp128_jump_snapshot_header)
        + (uint32_t)(LP128_MAX_LOCALS * sizeof(uint16_t))
        + (uint32_t)(LP128_EVAL_STACK_CAP * sizeof(uint16_t))
        + (uint32_t)(LP128_CALL_STACK_CAP * sizeof(lp128_continuation));
}
#endif

/* Read export record at index from the bundle, using a small direct-mapped
 * cache to avoid repeated bundle reads for hot entries (e.g. frequently
 * called procedures).  Fills *out with zeros on any error. */
void lp128_vm_read_export(lp128_vm_state *vm,
                          size_t index,
                          lp128_export_procedure_record *out)
{
    lp128_export_cache_entry *entry;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (vm == NULL || vm->bundle == NULL || index >= vm->bundle->export_count) {
        return;
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.export_fetch_requests++;
#endif

    entry = &vm->export_cache[index % LP128_VM_EXPORT_CACHE_ENTRIES];
    if (entry->valid && entry->index == index) {
#if LP128_ENABLE_PERF_COUNTERS
        vm->perf.export_cache_hits++;
#endif
        *out = entry->record;
        return;
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.export_cache_misses++;
#endif
    lp128_bundle_read_export(vm->bundle, index, out);
    entry->valid = true;
    entry->index = index;
    entry->record = *out;
}

/* Read the module global at index within module_id.  On C128 the globals
 * live in REU; on POSIX they are in a heap-allocated array.  Returns
 * LP128_FALSE_SENTINEL on any out-of-bounds access. */
uint16_t lp128_vm_read_module_global(const lp128_vm_state *vm,
                                     uint16_t module_id,
                                     uint8_t index)
{
    if (vm == NULL || module_id < 1 || module_id > vm->module_count) {
        return LP128_FALSE_SENTINEL;
    }

    {
        uint16_t module_index = (uint16_t)(module_id - 1u);
        uint16_t count = vm->module_global_counts[module_index];
        if (index >= count) {
            return LP128_FALSE_SENTINEL;
        }

#if defined(__mos__) || defined(__llvm_mos__)
        return reu_read_word(vm->module_globals_reu_offset
            + (uint32_t)(vm->module_global_base_words[module_index] + index) * 2u);
#else
        return vm->module_globals[module_index][index];
#endif
    }
}

/* Write value to the module global at index within module_id.
 * Silently ignores out-of-range accesses. */
void lp128_vm_write_module_global(lp128_vm_state *vm,
                                  uint16_t module_id,
                                  uint8_t index,
                                  uint16_t value)
{
    if (vm == NULL || module_id < 1 || module_id > vm->module_count) {
        return;
    }

    {
        uint16_t module_index = (uint16_t)(module_id - 1u);
        uint16_t count = vm->module_global_counts[module_index];
        if (index >= count) {
            return;
        }

#if defined(__mos__) || defined(__llvm_mos__)
        reu_write_word(vm->module_globals_reu_offset
            + (uint32_t)(vm->module_global_base_words[module_index] + index) * 2u,
            value);
#else
        vm->module_globals[module_index][index] = value;
#endif
    }
}

/* Read initializer record at index from the bundle via a small cache.
 * Fills *out with zeros on any error. */
void lp128_vm_read_initializer(lp128_vm_state *vm,
                               size_t index,
                               lp128_initializer_record *out)
{
    lp128_initializer_cache_entry *entry;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (vm == NULL || vm->bundle == NULL || index >= vm->bundle->initializer_count) {
        return;
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.initializer_fetch_requests++;
#endif

    entry = &vm->initializer_cache[index % LP128_VM_INITIALIZER_CACHE_ENTRIES];
    if (entry->valid && entry->index == index) {
#if LP128_ENABLE_PERF_COUNTERS
        vm->perf.initializer_cache_hits++;
#endif
        *out = entry->record;
        return;
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.initializer_cache_misses++;
#endif
    lp128_bundle_read_initializer(vm->bundle, index, out);
    entry->valid = true;
    entry->index = index;
    entry->record = *out;
}

/* Dump performance counters.  On POSIX, prints to stderr.
 * On C128 (when LP128_ENABLE_PERF_COUNTERS is set), writes counter
 * pairs to VIC screen rows 16-22 via the perf_write_counter_row helpers. */
void lp128_vm_dump_perf_counters(const lp128_vm_state *vm)
{
#if LP128_ENABLE_PERF_COUNTERS && !(defined(__mos__) || defined(__llvm_mos__))
    if (vm == NULL) {
        return;
    }

    fprintf(stderr,
            "[lp128-perf] code_fetches=%lu code_hit=%lu code_load=%lu heap_hit=%lu heap_load=%lu "
            "export_req=%lu export_hit=%lu export_miss=%lu init_req=%lu init_hit=%lu init_miss=%lu "
            "near_calls=%lu far_calls=%lu private_headers=%lu\n",
            (unsigned long)vm->perf.code_byte_fetches,
            (unsigned long)vm->perf.code_page_cache_hits,
            (unsigned long)vm->perf.code_page_cache_loads,
            (unsigned long)vm->perf.heap_page_cache_hits,
            (unsigned long)vm->perf.heap_page_cache_loads,
            (unsigned long)vm->perf.export_fetch_requests,
            (unsigned long)vm->perf.export_cache_hits,
            (unsigned long)vm->perf.export_cache_misses,
            (unsigned long)vm->perf.initializer_fetch_requests,
            (unsigned long)vm->perf.initializer_cache_hits,
            (unsigned long)vm->perf.initializer_cache_misses,
            (unsigned long)vm->perf.near_calls,
            (unsigned long)vm->perf.far_calls,
            (unsigned long)vm->perf.private_header_parses);
#elif LP128_ENABLE_PERF_COUNTERS
    if (vm == NULL) {
        return;
    }

    perf_write_counter_row(16u, "HH=", vm->perf.heap_page_cache_hits,
                           "HL=", vm->perf.heap_page_cache_loads);
    perf_write_counter_row(17u, "CF=", vm->perf.code_byte_fetches,
                           "CH=", vm->perf.code_page_cache_hits);
    perf_write_counter_row(18u, "CL=", vm->perf.code_page_cache_loads,
                           "ER=", vm->perf.export_fetch_requests);
    perf_write_counter_row(19u, "EH=", vm->perf.export_cache_hits,
                           "EM=", vm->perf.export_cache_misses);
    perf_write_counter_row(20u, "IR=", vm->perf.initializer_fetch_requests,
                           "IH=", vm->perf.initializer_cache_hits);
    perf_write_counter_row(21u, "IM=", vm->perf.initializer_cache_misses,
                           "NC=", vm->perf.near_calls);
    perf_write_counter_row(22u, "FC=", vm->perf.far_calls,
                           "PH=", vm->perf.private_header_parses);
#else
    (void)vm;
#endif
}

/* ── RAM seeding ────────────────────────────────────────────────────────── */
/* Copy initial_ram_bytes into the VM heap and write the arena sentinel
 * records that the free-list allocator expects.
 *
 * The low-segment arena sentinel is placed immediately after the initial
 * data (word-aligned).  The high-segment sentinel sits at the start of
 * the high 64-KB segment.  Each sentinel is a 5-word record:
 *   word 0: always 0 (arena chain terminator)
 *   word 1: handle of the first free block
 *   words 2-3: unused padding
 *   word 4: handle of the last free block (same as word 1 for empty arena)
 *   word 5: count of free words available
 */
static bool seed_initial_ram(lp128_vm_state *vm,
                              const uint8_t *initial_ram_bytes,
                              size_t initial_ram_size)
{
    size_t low_arena_offset;
#if !(defined(__mos__) || defined(__llvm_mos__))
    size_t high_arena_offset;
    size_t i;
#else
    (void)vm;              /* vm->ram not used on C128 – heap is in REU */
    (void)initial_ram_bytes; /* already in REU heap region */
#endif

    if ((uint32_t)initial_ram_size > LP128_SEGMENT_SIZE) {
        return false;
    }

#if defined(__mos__) || defined(__llvm_mos__)
    /* initial_ram bytes are already in the REU VM heap region (loaded by the
     * bundle loader via DMA).  Zero only the portion of the heap that follows
     * the pre-loaded data. */
    if ((uint32_t)initial_ram_size < LP128_FULL_RAM_BYTES) {
        reu_fill_zero_32(REU_VM_HEAP_OFFSET + (uint32_t)initial_ram_size,
                         LP128_FULL_RAM_BYTES - (uint32_t)initial_ram_size);
    }

    /* Seed low arena sentinels via REU single-word writes. */
    low_arena_offset = (initial_ram_size + 1u) & ~(size_t)1u;
    if (low_arena_offset + 10u <= (size_t)LP128_SEGMENT_SIZE) {
        uint16_t seg_handle = (uint16_t)(low_arena_offset / 2u);
        uint16_t arena_next = seg_handle + 3u;
        uint16_t free_words = (uint16_t)((LP128_SEGMENT_SIZE - (uint32_t)(low_arena_offset + 8u)) & 0xFFFFu);
        reu_addr_t base = REU_VM_HEAP_OFFSET + (uint32_t)low_arena_offset;

        reu_write_word(base + 0u, 0u);
        reu_write_word(base + 2u, arena_next);
        /* words 2-3 of the sentinel are unused (skip) */
        reu_write_word(base + 6u, arena_next);
        reu_write_word(base + 8u, free_words);
    }

    /* Seed high segment arena sentinels at the start of the high segment. */
    {
        uint32_t    high_off   = LP128_SEGMENT_SIZE;
        uint16_t    seg_handle = (uint16_t)(0x8000u | 0u);       /* high seg, word 0 */
        uint16_t    arena_next = seg_handle + 3u;
        uint16_t    free_words = (uint16_t)((LP128_SEGMENT_SIZE - LP128_TUPLE_RESERVE_BYTES - 8u) & 0xFFFFu);
        reu_addr_t  base       = REU_VM_HEAP_OFFSET + high_off;

        reu_write_word(base + 0u, 0u);
        reu_write_word(base + 2u, arena_next);
        reu_write_word(base + 6u, arena_next);
        reu_write_word(base + 8u, free_words);
    }
#else
    /* Zero all RAM. */
    memset(vm->ram, 0, LP128_FULL_RAM_BYTES);

    /* Copy initial RAM data into the start of the low segment. */
    for (i = 0; i < initial_ram_size; i++) {
        vm->ram[i] = initial_ram_bytes[i];
    }

    /* Seed the low arena sentinels just past the initial data */
    low_arena_offset = (initial_ram_size + 1u) & ~(size_t)1u;   /* even-align */
    if (low_arena_offset + 10u <= LP128_SEGMENT_SIZE) {
        uint16_t seg_handle = (uint16_t)(low_arena_offset / 2u);  /* word addr, low seg */
        uint16_t arena_next = seg_handle + 3u;
        size_t   free_words = LP128_SEGMENT_SIZE - (low_arena_offset + 8u);

        vm->ram[low_arena_offset + 0] = 0;
        vm->ram[low_arena_offset + 1] = 0;
        vm->ram[low_arena_offset + 2] = (uint8_t)(arena_next & 0xFF);
        vm->ram[low_arena_offset + 3] = (uint8_t)(arena_next >> 8);
        /* words 2-3 of the sentinel are unused (skip) */
        vm->ram[low_arena_offset + 6] = (uint8_t)(arena_next & 0xFF);
        vm->ram[low_arena_offset + 7] = (uint8_t)(arena_next >> 8);
        vm->ram[low_arena_offset + 8] = (uint8_t)(free_words & 0xFF);
        vm->ram[low_arena_offset + 9] = (uint8_t)(free_words >> 8);
    }

#if !(defined(__mos__) || defined(__llvm_mos__))
    /* Seed the high segment arena sentinels at the start of the high segment.
     * Skipped on mos/C128 because there is only one (low) segment. */
    high_arena_offset = LP128_SEGMENT_SIZE;
    {
        uint16_t seg_handle = (uint16_t)(0x8000u | 0u);           /* high seg, word 0 */
        uint16_t arena_next = (uint16_t)(seg_handle + 3u);
        size_t   free_words = LP128_SEGMENT_SIZE - LP128_TUPLE_RESERVE_BYTES - 8u;

        vm->ram[high_arena_offset + 0] = 0;
        vm->ram[high_arena_offset + 1] = 0;
        vm->ram[high_arena_offset + 2] = (uint8_t)(arena_next & 0xFF);
        vm->ram[high_arena_offset + 3] = (uint8_t)(arena_next >> 8);
        vm->ram[high_arena_offset + 6] = (uint8_t)(arena_next & 0xFF);
        vm->ram[high_arena_offset + 7] = (uint8_t)(arena_next >> 8);
        vm->ram[high_arena_offset + 8] = (uint8_t)(free_words & 0xFF);
        vm->ram[high_arena_offset + 9] = (uint8_t)(free_words >> 8);
    }
#endif /* !(defined(__mos__)) inner guard – high segment */
#endif /* defined(__mos__) outer guard */

    return true;
}

/* ── Global layout parsing ──────────────────────────────────────────────── */
/* Parse the bundle's global layout section.  This establishes how many
 * modules exist, how many globals each module has, and what their initial
 * values are.  On C128, per-module globals are allocated in REU beyond the
 * code/export/initializer regions; call-stack local spill storage and
 * SETJMP snapshot slots are placed after them. */
static bool load_global_layout(lp128_vm_state *vm, char *error_buf, size_t error_buf_size)
{
    const uint16_t *data  = vm->bundle->global_layout_data;
    size_t          count = vm->bundle->global_layout_data_count;
    size_t          pos   = 0;
    uint16_t        mc, pgc;
    uint16_t        mi, gi;
    size_t          total_module_globals;
    size_t          data_start;

    if (data == NULL || count < 2) {
        vm_error(error_buf, error_buf_size, "Global layout section is missing or too short.", NULL);
        return false;
    }

    mc  = data[pos++];   /* module_count */
    pgc = data[pos++];   /* program_global_count */

    if (mc > LP128_MAX_MODULES) {
        vm_error(error_buf, error_buf_size, "Global layout module count exceeds LP128_MAX_MODULES.", NULL);
        return false;
    }
    if (pgc > LP128_MAX_PROGRAM_GLOBALS) {
        vm_error(error_buf, error_buf_size, "Global layout program global count exceeds LP128_MAX_PROGRAM_GLOBALS.", NULL);
        return false;
    }

    vm->module_count          = mc;
    vm->program_global_count  = pgc;

    /* Read per-module global counts */
    total_module_globals = 0;
    for (mi = 0; mi < mc; mi++) {
        if (pos >= count) {
            vm_error(error_buf, error_buf_size, "Global layout truncated in module count array.", NULL);
            return false;
        }
        uint16_t mgc = data[pos++];
        if (mgc > LP128_MAX_MODULE_GLOBALS) {
            vm_error(error_buf, error_buf_size, "Global layout module global count exceeds LP128_MAX_MODULE_GLOBALS.", NULL);
            return false;
        }
        vm->module_global_counts[mi] = mgc;
        total_module_globals += mgc;
    }

    /* Verify length: header(2) + module_counts(mc) + program_globals(pgc) + all module globals */
    data_start = 2u + mc;
    if (count < data_start + pgc + total_module_globals) {
        vm_error(error_buf, error_buf_size, "Global layout section is truncated.", NULL);
        return false;
    }

#if defined(__mos__) || defined(__llvm_mos__)
    {
        uint32_t reu_cursor = vm->bundle->initializers_reu_offset
            + (uint32_t)vm->bundle->initializer_count * 8u;
        uint16_t base_word = 0;
        uint32_t call_stack_locals_bytes;
        uint32_t module_globals_bytes;

        if (vm->bundle->ro_data_size > 0u) {
            reu_cursor = vm->bundle->ro_data_reu_offset + (uint32_t)vm->bundle->ro_data_size;
        }

        vm->module_globals_reu_offset = align_up_u32(reu_cursor, 0x100u);
        for (mi = 0; mi < mc; mi++) {
            vm->module_global_base_words[mi] = base_word;
            base_word = (uint16_t)(base_word + vm->module_global_counts[mi]);
        }

        module_globals_bytes = (uint32_t)total_module_globals * 2u;
        vm->call_stack_locals_reu_offset = align_up_u32(
            vm->module_globals_reu_offset + module_globals_bytes,
            0x100u);
        call_stack_locals_bytes = (uint32_t)LP128_CALL_STACK_CAP
            * (uint32_t)LP128_MAX_LOCALS
            * 2u;
        vm->jump_snapshot_reu_offset = align_up_u32(
            vm->call_stack_locals_reu_offset + call_stack_locals_bytes,
            0x100u);
        for (mi = 0; mi < LP128_JUMP_SNAPSHOT_CAP; mi++) {
            vm->jump_snapshot_slots[mi].reu_offset = vm->jump_snapshot_reu_offset
                + (uint32_t)mi * jump_snapshot_storage_size();
        }
    }
#endif

    /* Copy program globals */
    for (gi = 0; gi < pgc; gi++) {
        vm->program_globals[gi] = data[pos++];
    }

    /* Copy per-module globals */
    for (mi = 0; mi < mc; mi++) {
        uint16_t mgc = vm->module_global_counts[mi];
        for (gi = 0; gi < mgc; gi++) {
#if defined(__mos__) || defined(__llvm_mos__)
            reu_write_word(vm->module_globals_reu_offset
                + (uint32_t)(vm->module_global_base_words[mi] + gi) * 2u,
                data[pos++]);
#else
            vm->module_globals[mi][gi] = data[pos++];
#endif
        }
    }

    return true;
}

/* ── VM initialization ──────────────────────────────────────────────────── */
/* Initialise vm from bundle and host.  Allocates (or locates) VM heap
 * storage, loads the global layout, seeds initial RAM, and sets up the
 * entry procedure frame so that lp128_vm_run can be called immediately.
 * Returns false and writes a message to error_buf on failure. */
bool lp128_vm_init(lp128_vm_state *vm,
                   const lp128_bundle *bundle,
                   lp128_host *host,
                   char *error_buf,
                   size_t error_buf_size)
{
    const lp128_module_record        *entry_mod;
    lp128_export_procedure_record    entry_proc_buf;
    uint16_t entry_module_id;
    uint16_t entry_proc_index;
    uint16_t mi;

    if (vm == NULL || bundle == NULL || host == NULL) {
        vm_error(error_buf, error_buf_size, "vm, bundle and host must not be NULL.", NULL);
        return false;
    }

    memset(vm, 0, sizeof(*vm));
    vm->bundle = bundle;
    vm->host   = host;

    /* Allocate (or locate) VM RAM */
#if defined(__mos__) || defined(__llvm_mos__)
    /* On C128 the VM heap lives entirely in the REU.  No pointer is needed;
     * all access goes through reu_read_byte / reu_write_word etc. */
    vm->ram = NULL;
#else
    vm->ram = (uint8_t *)malloc(LP128_FULL_RAM_BYTES);
    if (vm->ram == NULL) {
        vm_error(error_buf, error_buf_size, "Out of memory allocating VM RAM.", NULL);
        return false;
    }
#endif

    /* Seed RAM from bundle */
    if (!seed_initial_ram(vm, bundle->initial_ram, bundle->initial_ram_size)) {
        vm_error(error_buf, error_buf_size, "Failed to seed initial RAM.", NULL);
        lp128_vm_free(vm);
        return false;
    }

    /* Load global layout */
    if (!load_global_layout(vm, error_buf, error_buf_size)) {
        lp128_vm_free(vm);
        return false;
    }

    /* Set up system globals:
     *   0xC7  screen width  - 1  (maximum column index)
     *   0xC8  screen height - 1  (maximum row index)
     *   0xC9  cursor column (writing this commits the cursor to the host)
     *   0xCA  cursor row
     *   0xD4  FALSE sentinel constant
     *   0xD5  current text attribute (also mirrored in host->current_text_attribute)
     *   0xD7  "DBF" extension token: 3-letter code packed as
     *         (D-A)*2048 + (B-A)*45 + (F-A)
     *   0xDA  OBJ data-channel pack: upper bits = HeaderWords[7]-1,
     *         lower 5 bits = channel ID (1)
     */
    memset(vm->system_globals, 0, sizeof(vm->system_globals));
    {
        uint8_t sw = host->screen_width;
        uint8_t sh = host->screen_height;
        if (sw == 0) sw = (uint8_t)LP128_SCREEN_WIDTH;
        if (sh == 0) sh = (uint8_t)LP128_SCREEN_HEIGHT;
        vm->system_globals[0xC7] = (uint16_t)(sw - 1u);
        vm->system_globals[0xC8] = (uint16_t)(sh - 1u);
    }
    vm->system_globals[0xC9] = 0;
    vm->system_globals[0xCA] = 0;
    vm->system_globals[0xD4] = LP128_FALSE_SENTINEL;
    host->current_text_attribute = 0;
    /* Extension token for "DBF": (D-A)*2048 + (B-A)*45 + (F-A) */
    vm->system_globals[0xD7] = (uint16_t)((3u << 11) + (1u * 45u) + 5u);

    /* Pre-open OBJ data channel (channel 1) for READREC.
     * Slot 0xDA packs HeaderWords[7] (= code_end_offset/512 - 1) in the
     * upper bits and the channel id in the low 5 bits.  The compiled
     * bytecode extracts these fields to compute record numbers. */
    if (bundle->ro_data_size > 0 && bundle->header.code_end_offset >= 0x200u) {
        uint16_t hw7 = (uint16_t)(bundle->header.code_end_offset / 0x200u - 1u);
        vm->system_globals[0xDA] = (uint16_t)((hw7 << 5) | 1u);
    }

    /* Locate entry procedure */
    entry_module_id  = bundle->header.entry_module_id;
    entry_proc_index = bundle->header.entry_procedure_index;

    if (entry_module_id < 1 || (size_t)(entry_module_id - 1) >= bundle->module_count) {
        vm_error(error_buf, error_buf_size, "Entry module ID is out of range.", NULL);
        lp128_vm_free(vm);
        return false;
    }

    entry_mod = &bundle->modules[entry_module_id - 1];
    mi = entry_mod->export_start_index;

    if ((size_t)(mi + entry_proc_index) >= bundle->export_count) {
        vm_error(error_buf, error_buf_size, "Entry procedure index is out of range.", NULL);
        lp128_vm_free(vm);
        return false;
    }

    lp128_vm_read_export(vm, (size_t)(mi + entry_proc_index), &entry_proc_buf);

    /* Bootstrap entry frame locals */
    {
        uint16_t lc = entry_proc_buf.local_count;
        uint16_t li;
        uint16_t init_start = entry_proc_buf.initializer_start_index;
        uint16_t init_count = entry_proc_buf.initializer_count;

        if (lc > LP128_MAX_LOCALS) lc = LP128_MAX_LOCALS;

        for (li = 0; li < lc; li++) {
            vm->locals[li] = LP128_FALSE_SENTINEL;
        }

        for (li = 0; li < init_count; li++) {
            lp128_initializer_record init_rec;
            lp128_vm_read_initializer(vm, (size_t)(init_start + li), &init_rec);
            if (init_rec.local_index < lc) {
                vm->locals[init_rec.local_index] = init_rec.value;
            }
        }

        vm->frame_local_count = lc;
    }

    /* Set up current frame */
    vm->frame_module_id       = entry_module_id;
    vm->frame_proc_index      = entry_proc_index;
    vm->frame_start_offset    = entry_proc_buf.start_offset;
    vm->frame_code_offset     = entry_proc_buf.code_offset;
    vm->current_module_id     = entry_module_id;
    vm->program_counter       = entry_proc_buf.code_offset;
    vm->frame_upper_bound     = entry_proc_buf.upper_bound;

    /* Arena allocator setup */
    vm->next_low_arena_byte   = (uint32_t)((bundle->initial_ram_size + 1u) & ~(size_t)1u);
    vm->next_high_arena_byte  = LP128_SEGMENT_SIZE;
    vm->tuple_stack_byte      = LP128_FULL_RAM_BYTES;
    vm->tuple_stack_floor_byte = (uint32_t)(LP128_FULL_RAM_BYTES - LP128_TUPLE_RESERVE_BYTES);
    memset(vm->free_list, 0, sizeof(vm->free_list));
    memset(vm->export_cache, 0, sizeof(vm->export_cache));
    memset(vm->initializer_cache, 0, sizeof(vm->initializer_cache));
    vm->current_code_page_cache_slot = 0u;
    memset(vm->code_page_cache_valid, 0, sizeof(vm->code_page_cache_valid));
    memset(vm->code_page_cache_base, 0, sizeof(vm->code_page_cache_base));
    vm->heap_page_cache_valid = false;
    vm->heap_page_cache_base = 0;
    memset(&vm->perf, 0, sizeof(vm->perf));

    vm->eval_stack_top  = 0;
    vm->call_stack_top  = 0;
    vm->next_jump_token = 0xA000u;
    vm->is_halted       = false;
    vm->halt_code       = 0;
    vm->cursor_row      = 0;
    vm->cursor_col      = 0;
    vm->last_return_word_count = 0;

    return true;
}

/* Release any heap-allocated resources held by vm.  Safe to call even
 * if lp128_vm_init failed partway through. */
void lp128_vm_free(lp128_vm_state *vm)
{
    if (vm == NULL) {
        return;
    }

#if !(defined(__mos__) || defined(__llvm_mos__))
    free(vm->ram);
    vm->ram = NULL;
#endif
}
