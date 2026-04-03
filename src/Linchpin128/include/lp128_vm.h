#ifndef LP128_VM_H
#define LP128_VM_H

#include "lp128_bundle.h"
#include "lp128_host.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── VM constants ───────────────────────────────────────────────────────── */

#define LP128_FALSE_SENTINEL      UINT16_C(0x8001)

#if defined(__mos__) || defined(__llvm_mos__)
/* On 6502/mos targets size_t is 16-bit, so stack/capacity constants must fit
 * in that range.  The VM heap lives entirely in the REU, so we use uint32_t
 * addresses (not size_t) for heap bounds.  FULL_RAM_BYTES and SEGMENT_SIZE
 * therefore use UINT32_C to avoid 16-bit truncation.
 *
 * Two 64-KB segments (128 KB total) are needed for Zork-sized programs.
 * Code pages and metadata are placed after the heap in the REU. */
#define LP128_FULL_RAM_BYTES      UINT32_C(0x20000)   /* 128 KB (two segments) */
#define LP128_SEGMENT_SIZE        UINT32_C(0x10000)   /* 64 KB                 */
#define LP128_TUPLE_RESERVE_BYTES (LP128_SEGMENT_SIZE / 64u)
#define LP128_EVAL_STACK_CAP      128u
#define LP128_CALL_STACK_CAP       32u
#define LP128_MAX_LOCALS           64u
#define LP128_FREE_LIST_BUCKETS    16u
#define LP128_MAX_PROGRAM_GLOBALS 192u
#define LP128_MAX_MODULES         16u
#define LP128_MAX_MODULE_GLOBALS  128u
/* System globals array: slots 0x00..0xDF.  0xDA is the highest used slot
 * (OBJ data channel pack), but we round up to 0xE0 for headroom. */
#define LP128_SYSTEM_GLOBALS_COUNT 0xE0u              /* 224 entries */

#else
#define LP128_FULL_RAM_BYTES      UINT32_C(0x20000)   /* 128 KB */
#define LP128_SEGMENT_SIZE        UINT32_C(0x10000)   /* 64 KB  */
#define LP128_TUPLE_RESERVE_BYTES (LP128_SEGMENT_SIZE / 64u)
#define LP128_EVAL_STACK_CAP      256u
#define LP128_CALL_STACK_CAP      64u
#define LP128_MAX_LOCALS          64u
#define LP128_FREE_LIST_BUCKETS   32u
#define LP128_MAX_PROGRAM_GLOBALS 256u
#define LP128_MAX_MODULES         16u
#define LP128_MAX_MODULE_GLOBALS  128u
/* POSIX uses the full 256-entry system_globals array (idx is uint8_t so
 * the bounds check `idx >= 256` is trivially false and optimised away). */
#define LP128_SYSTEM_GLOBALS_COUNT 256u
#endif

#define LP128_CODE_PAGE_SIZE      0x100u

#define LP128_SCREEN_WIDTH        80u
#define LP128_SCREEN_HEIGHT       25u

#ifndef LP128_ENABLE_PERF_COUNTERS
#if defined(__mos__) || defined(__llvm_mos__)
#define LP128_ENABLE_PERF_COUNTERS 0
#else
#define LP128_ENABLE_PERF_COUNTERS 1
#endif
#endif

#define LP128_VM_EXPORT_CACHE_ENTRIES      16u
#define LP128_VM_INITIALIZER_CACHE_ENTRIES 16u
#define LP128_VM_CODE_PAGE_CACHE_SLOTS     2u
#define LP128_JUMP_SNAPSHOT_CAP            8u

typedef struct {
    bool valid;
    size_t index;
    lp128_export_procedure_record record;
} lp128_export_cache_entry;

typedef struct {
    bool valid;
    size_t index;
    lp128_initializer_record record;
} lp128_initializer_cache_entry;

typedef struct {
    uint32_t code_byte_fetches;
    uint32_t code_page_cache_hits;
    uint32_t code_page_cache_loads;
    uint32_t heap_page_cache_hits;
    uint32_t heap_page_cache_loads;
    uint32_t export_fetch_requests;
    uint32_t export_cache_hits;
    uint32_t export_cache_misses;
    uint32_t initializer_fetch_requests;
    uint32_t initializer_cache_hits;
    uint32_t initializer_cache_misses;
    uint32_t near_calls;
    uint32_t far_calls;
    uint32_t private_header_parses;
} lp128_perf_counters;

/* ── Call-stack continuation (saved caller frame) ───────────────────────── */

/* Saved caller execution state pushed by enter_procedure and popped by
 * do_return.  On C128, locals are spilled to REU (saved_locals is absent)
 * because there is no room to store LP128_MAX_LOCALS per frame in cpu RAM. */
typedef struct {
    uint16_t module_id;              /* module executing in the caller frame */
    uint16_t proc_index;             /* 0-based export index of the caller procedure */
    uint16_t proc_start_offset;      /* byte offset of the caller procedure header */
    uint16_t return_pc;              /* PC to restore when returning */
    uint16_t frame_upper_bound;      /* upper bound of the caller procedure body */
    uint16_t saved_local_count;      /* number of locals in the caller frame */
    uint16_t saved_eval_stack_depth; /* eval stack top at the call site */
#if !(defined(__mos__) || defined(__llvm_mos__))
    uint16_t saved_locals[LP128_MAX_LOCALS]; /* caller locals (POSIX only) */
#endif
} lp128_continuation;

/* Compact representation of all mutable execution state needed to
 * resume (or re-enter) a SETJMP-protected block.  Stored in a
 * lp128_jump_snapshot_slot alongside the eval stack, call stack, and
 * locals arrays. */
typedef struct {
    uint16_t module_id;             /* module at the time the snapshot was taken */
    uint16_t long_jump_pc;          /* PC of the protected entry point */
    uint16_t long_jump_return_pc;   /* PC to jump to when the snapshot is used */
    uint16_t frame_upper_bound;
    uint16_t frame_module_id;
    uint16_t frame_proc_index;
    uint16_t frame_start_offset;
    uint16_t frame_code_offset;
    uint16_t frame_local_count;
    uint16_t eval_stack_top;
    uint16_t call_stack_top;
    uint32_t tuple_stack_byte;
} lp128_jump_snapshot_header;

typedef struct {
    bool valid;
    uint16_t token;
#if defined(__mos__) || defined(__llvm_mos__)
    uint32_t reu_offset;
#else
    lp128_jump_snapshot_header header;
    uint16_t locals[LP128_MAX_LOCALS];
    uint16_t eval_stack[LP128_EVAL_STACK_CAP];
    lp128_continuation call_stack[LP128_CALL_STACK_CAP];
#endif
} lp128_jump_snapshot_slot;

/* ── VM execution state ─────────────────────────────────────────────────── */

typedef struct lp128_vm_state {
    /* Read-only bundle reference */
    const lp128_bundle *bundle;
    lp128_host         *host;

    /* VM RAM – 128 KB, malloc'd on host, REU-backed on C128 */
    uint8_t *ram;

    /* Program globals (slot 0 … program_global_count-1) */
    uint16_t program_global_count;
    uint16_t program_globals[LP128_MAX_PROGRAM_GLOBALS];

    /* Per-module globals */
    uint16_t module_count;
    uint16_t module_global_counts[LP128_MAX_MODULES];
#if defined(__mos__) || defined(__llvm_mos__)
    uint16_t module_global_base_words[LP128_MAX_MODULES];
    uint32_t module_globals_reu_offset;
    uint32_t call_stack_locals_reu_offset;
    uint32_t jump_snapshot_reu_offset;
#else
    uint16_t module_globals[LP128_MAX_MODULES][LP128_MAX_MODULE_GLOBALS];
#endif

    /* System module globals (indices 0x00..0xFF) */
    uint16_t system_globals[LP128_SYSTEM_GLOBALS_COUNT];

    /* Display cursor */
    uint8_t cursor_row;
    uint8_t cursor_col;

    /* ── current execution ─────────────────────────────── */
    uint16_t current_module_id;
    uint16_t program_counter;
    uint16_t frame_upper_bound;
    bool     is_halted;
    uint16_t halt_code;
    uint16_t last_return_word_count;

    /* Current frame bookkeeping */
    uint16_t frame_module_id;
    uint16_t frame_proc_index;
    uint16_t frame_start_offset;
    uint16_t frame_code_offset;
    uint16_t frame_local_count;

    /* Local variable storage */
    uint16_t locals[LP128_MAX_LOCALS];

    /* Evaluation stack */
    uint16_t eval_stack[LP128_EVAL_STACK_CAP];
    uint16_t eval_stack_top;

    /* Call stack */
    lp128_continuation call_stack[LP128_CALL_STACK_CAP];
    uint16_t call_stack_top;

    /* Non-local control transfer state */
    uint16_t next_jump_token;
    lp128_jump_snapshot_slot jump_snapshot_slots[LP128_JUMP_SNAPSHOT_CAP];

    /* Arena allocator */
    uint32_t next_low_arena_byte;
    uint32_t next_high_arena_byte;
    uint32_t tuple_stack_byte;
    uint32_t tuple_stack_floor_byte;
    uint32_t free_list[LP128_FREE_LIST_BUCKETS];   /* 0 = empty bucket */

    /* Small CPU-RAM caches for metadata that otherwise lives in REU. */
    lp128_export_cache_entry export_cache[LP128_VM_EXPORT_CACHE_ENTRIES];
    lp128_initializer_cache_entry initializer_cache[LP128_VM_INITIALIZER_CACHE_ENTRIES];

    /* Two hot code pages cover the common caller/callee return pattern. */
    uint8_t  current_code_page_cache_slot;
    bool     code_page_cache_valid[LP128_VM_CODE_PAGE_CACHE_SLOTS];
    uint32_t code_page_cache_base[LP128_VM_CODE_PAGE_CACHE_SLOTS];

    /* One pinned heap page cache for aggregate/string reads. */
    bool     heap_page_cache_valid;
    uint32_t heap_page_cache_base;

    lp128_perf_counters perf;
} lp128_vm_state;

/* ── Public API ─────────────────────────────────────────────────────────── */

/** Initialise vm from bundle and host.
 *  Allocates VM heap storage (on POSIX), loads the global layout, seeds
 *  initial RAM, and sets up the entry procedure frame so that
 *  lp128_vm_run can be called immediately.
 *  @return true on success; false on failure (message written to error_buf). */
bool lp128_vm_init(lp128_vm_state *vm,
                   const lp128_bundle *bundle,
                   lp128_host *host,
                   char *error_buf,
                   size_t error_buf_size);

/** Release any heap-allocated resources.  Safe to call after a partial
 *  lp128_vm_init failure. */
void lp128_vm_free(lp128_vm_state *vm);

/** Read export record at index via the direct-mapped cache.
 *  Fills *out with zeros on any out-of-bounds access. */
void lp128_vm_read_export(lp128_vm_state *vm,
                          size_t index,
                          lp128_export_procedure_record *out);

/** Read initializer record at index via the direct-mapped cache.
 *  Fills *out with zeros on any out-of-bounds access. */
void lp128_vm_read_initializer(lp128_vm_state *vm,
                               size_t index,
                               lp128_initializer_record *out);

/** Read a module global by module ID (1-based) and 0-based index.
 *  Returns LP128_FALSE_SENTINEL on any out-of-bounds access. */
uint16_t lp128_vm_read_module_global(const lp128_vm_state *vm,
                                     uint16_t module_id,
                                     uint8_t index);

/** Write value to a module global.  Silently ignores out-of-range accesses. */
void lp128_vm_write_module_global(lp128_vm_state *vm,
                                  uint16_t module_id,
                                  uint8_t index,
                                  uint16_t value);

/** Dump performance counters to stderr (POSIX) or VIC screen rows 16-22
 *  (C128), if LP128_ENABLE_PERF_COUNTERS is set. */
void lp128_vm_dump_perf_counters(const lp128_vm_state *vm);

/** Run the fetch-decode-execute loop until vm->is_halted is set. */
void lp128_vm_run(lp128_vm_state *vm);

#endif
