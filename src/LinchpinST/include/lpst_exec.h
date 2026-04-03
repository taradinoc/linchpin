/*
 * lpst_exec.h — Execution state for the LinchpinST VM interpreter.
 *
 * lpst_exec_state holds everything needed to run a loaded VM image:
 * RAM banks, code cache, evaluation and call stacks, global variable
 * tables, open file channels, and diagnostic ring-buffers.
 *
 * Use lpst_exec_init to create a state from an image+host pair, run it with
 * lpst_run, then release resources with lpst_exec_free.
 */
#ifndef LPST_EXEC_H
#define LPST_EXEC_H

#include "lpst_host.h"
#include "lpst_image.h"
#include "lpst_vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LPST_EVAL_STACK_CAPACITY 1024
#define LPST_CALL_STACK_CAPACITY 256
#define LPST_MAX_LOCALS          128
/* Number of per-size free-list buckets for the vector allocator. */
#define LPST_FREE_LIST_BUCKETS   64
#define LPST_MAX_CHANNELS        25
#define LPST_CHANNEL_NAME_MAX    64
/* Maximum number of concurrently live SETJMP snapshots. */
#define LPST_JUMP_SNAPSHOT_CAPACITY 64
/* All bytecode is accessed through a paged cache; each page is 256 bytes. */
#define LPST_CODE_PAGE_SIZE      0x100u
/* Capacities of the diagnostic ring-buffers kept in lpst_exec_state.
 * These record recent events of various kinds so that a crash dump can
 * show what the VM was doing in the moments before it stopped. */
#define LPST_RECENT_INPUT_EVENTS 24
#define LPST_RECENT_EXT36_EVENTS 24
#define LPST_RECENT_P201_EVENTS  16
#define LPST_M5P128_TRACE_EVENTS 32
#define LPST_RECENT_M8_PRIV0F28_EVENTS 16
#define LPST_RECENT_M1_STARTUP_EVENTS 24
#define LPST_RECENT_M1_PROC2_ENTRY_EVENTS 16
#define LPST_RECENT_M2_PROC166_RETURN_EVENTS 16
#define LPST_RECENT_OPEN_EVENTS 8

/* One entry in the bytecode page cache (LRU eviction policy). */
typedef struct lpst_code_cache_entry {
    uint32_t page_base;       /* absolute OBJ file offset of the start of this page */
    size_t page_size;         /* bytes of valid data in bytes[] (may be < LPST_CODE_PAGE_SIZE at EOF) */
    uint32_t last_used_tick;  /* value of code_cache_tick when this entry was last accessed */
    bool valid;
    uint8_t bytes[LPST_CODE_PAGE_SIZE];
} lpst_code_cache_entry;

typedef struct lpst_channel {
    FILE *fp;
    int mode;
    long position;       /* sequential read/write position (byte offset) */
    long size_bytes;
    char name[LPST_CHANNEL_NAME_MAX];
    char path[260];
    bool in_use;
} lpst_channel;

/* A saved return address and caller frame, pushed onto the call stack when
 * entering a procedure and popped on return. */
typedef struct lpst_call_continuation {
    uint16_t module_id;
    uint16_t procedure_index;
    uint16_t procedure_start_offset;
    uint16_t return_pc;
    uint16_t frame_upper_bound;
    uint16_t *saved_locals;
    uint16_t saved_local_count;
    uint16_t saved_eval_stack_depth;
} lpst_call_continuation;

/* A complete VM state snapshot taken by the SETJMP instruction.
 * longjmp_program_counter is the target for a plain LONGJMP;
 * longjmpr_program_counter is the target for a LONGJMPR (which also pushes
 * a return value).  The token uniquely identifies this snapshot so that
 * LONGJMP/LONGJMPR can find it later. */
typedef struct lpst_jump_snapshot {
    bool in_use;
    uint16_t token;
    uint16_t module_id;
    uint16_t longjmp_program_counter;
    uint16_t longjmpr_program_counter;
    uint32_t tuple_stack_byte;
    lpst_vm_frame frame;
    uint16_t frame_upper_bound;
    uint16_t *frame_locals;
    uint16_t eval_stack_top;
    uint16_t *eval_stack;
    uint16_t call_stack_top;
    lpst_call_continuation *call_stack;
} lpst_jump_snapshot;

typedef struct lpst_recent_word_event {
    uint16_t module_id;
    uint16_t pc;
    uint16_t value;
} lpst_recent_word_event;

typedef struct lpst_m5p128_trace_event {
    uint16_t pc;       /* instruction start address */
    uint16_t stk0;     /* eval stack top value (0 if empty) */
    uint16_t stk1;     /* eval stack top-1 value (0 if depth<2) */
    uint16_t stk_depth;
    uint16_t l8;
    uint16_t l9;
} lpst_m5p128_trace_event;

typedef struct lpst_recent_p201_event {
    uint16_t pc;
    uint16_t l0;
    uint16_t l1;
    uint16_t l2;
    uint16_t l3;
    uint16_t l4;
} lpst_recent_p201_event;

typedef struct lpst_recent_m8_priv0f28_event {
    uint16_t caller_module_id;
    uint16_t caller_procedure_index;
    uint16_t caller_pc;
    uint16_t arg0;
    uint16_t arg1;
    uint16_t arg2;
    uint16_t arg3;
    uint16_t arg4;
    uint16_t arg5;
} lpst_recent_m8_priv0f28_event;

typedef struct lpst_recent_m1_startup_event {
    uint16_t procedure_index;
    uint16_t pc;
    uint16_t opcode;
    uint16_t g0;
    uint16_t g1;
    uint16_t m1g0;
    uint16_t m8g59;
} lpst_recent_m1_startup_event;

typedef struct lpst_recent_m1_proc2_entry_event {
    uint16_t source_kind;
    uint16_t source_module_id;
    uint16_t source_procedure_index;
    uint16_t source_pc;
    uint16_t selector_or_token;
    uint16_t target_pc;
} lpst_recent_m1_proc2_entry_event;

typedef struct lpst_recent_m2_proc166_return_event {
    uint16_t caller_module_id;
    uint16_t caller_procedure_index;
    uint16_t caller_pc;
    uint16_t result;
    uint16_t l0;
    uint16_t l1;
    uint16_t l2;
    uint16_t l3;
    uint16_t l4;
} lpst_recent_m2_proc166_return_event;

typedef struct lpst_recent_open_event {
    uint16_t module_id;
    uint16_t procedure_index;
    uint16_t pc;
    uint16_t result;
    uint16_t error_detail;
    uint8_t mode;
    char name[24];
} lpst_recent_open_event;

typedef struct lpst_exec_state {
    const lpst_image *image;
    lpst_host *host;
    FILE *code_fp;                          /* open handle on the OBJ file used by the code cache */
    lpst_code_cache_entry *code_cache_entries;
    const uint8_t *current_code_page_bytes; /* fast path: pointer into the most recently loaded page */
    uint32_t current_code_page_base;        /* absolute OBJ offset of the current page */
    uint32_t current_code_page_limit;       /* exclusive upper bound of the current page */
    size_t code_cache_entry_count;
    uint32_t code_cache_tick;              /* monotonic counter used for LRU eviction */
    uint32_t code_cache_hits;
    uint32_t code_cache_misses;
    uint32_t code_cache_evictions;

    uint16_t current_module_id;   /* 1-based module currently executing */
    uint16_t program_counter;     /* byte offset within the current module */
    uint16_t frame_upper_bound;   /* exclusive upper bound of the current procedure's code */
    bool is_halted;
    uint16_t halt_code;           /* 0 = normal HALT, 0xFFFF = fatal error */
    uint16_t last_return_word_count; /* number of values left on the stack by the most recent RETURN */

    uint8_t *ram_banks[LPST_RAM_BANK_COUNT];

    uint16_t *program_globals;
    uint16_t program_global_count;

    uint16_t **module_globals;
    uint16_t *module_global_counts;
    uint16_t module_count;
    /* System-level module globals indexed by slot number.  Key slots:
     *   0xC4 = scroll-region bottom row, 0xC5 = scroll-region top row
     *   0xC7 = screen width - 1,         0xC8 = screen height - 1
     *   0xC9 = cursor column (commit triggers set_cursor)
     *   0xCA = cursor row
     *   0xCC = record size (0 or negative means 256 bytes/record)
     *   0xD5 = current display style bits
     *   0xDA = OBJ channel descriptor (code-page count << 5 | channel ID)
     */
    uint16_t system_module_globals[256];
    uint8_t cursor_row;   /* logical cursor row, mirroring 0xCA */
    uint8_t cursor_col;   /* logical cursor column, mirroring 0xC9 */

    lpst_vm_frame current_frame;
    uint16_t local_storage[LPST_MAX_LOCALS]; /* shared storage for the active frame's locals */

    uint16_t eval_stack[LPST_EVAL_STACK_CAPACITY];
    uint16_t eval_stack_top;

    lpst_call_continuation call_stack[LPST_CALL_STACK_CAPACITY];
    uint16_t call_stack_top;

    /* Arena allocator state.
     * The heap is split across two 64 KB segments.  Vectors are allocated
     * upward from next_low_arena_byte (in the low segment) or
     * next_high_arena_byte (in the high segment).  Tuples are allocated
     * downward from tuple_stack_byte and freed as a stack when procedures
     * return; tuple_stack_floor_byte is the lowest address they may reach. */
    uint32_t next_low_arena_byte;
    uint32_t next_high_arena_byte;
    uint32_t tuple_stack_byte;
    uint32_t tuple_stack_floor_byte;

    /* Simple per-size free list: free_list[i] = byte offset of freed block of size (i+1)*2, or 0 if none */
    uint32_t free_list[LPST_FREE_LIST_BUCKETS];

    /* File I/O channel table */
    lpst_channel channels[LPST_MAX_CHANNELS];
    uint16_t next_channel_id;
    uint16_t active_channel_id; /* the channel most recently made active via resolve_channel */

    uint32_t instruction_count;
    uint32_t instruction_limit; /* 0 = unlimited; the run loop stops when count reaches the limit */
    bool trace_enabled;
    uint16_t next_jump_token;   /* counter for assigning unique SETJMP tokens */
    lpst_jump_snapshot jump_snapshots[LPST_JUMP_SNAPSHOT_CAPACITY];
    /* Diagnostic ring-buffers: each records a fixed number of recent events
     * so that a halt trace can show recent activity. */
    lpst_recent_word_event recent_kbinput[LPST_RECENT_INPUT_EVENTS];
    uint16_t recent_kbinput_next;
    uint16_t recent_kbinput_count;
    lpst_recent_word_event recent_ext36[LPST_RECENT_EXT36_EVENTS];
    uint16_t recent_ext36_next;
    uint16_t recent_ext36_count;
    lpst_recent_p201_event recent_p201[LPST_RECENT_P201_EVENTS];
    uint16_t recent_p201_next;
    uint16_t recent_p201_count;
    lpst_m5p128_trace_event m5p128_trace[LPST_M5P128_TRACE_EVENTS];
    uint16_t m5p128_trace_next;
    uint16_t m5p128_trace_count;
    lpst_recent_m8_priv0f28_event recent_m8_priv0f28[LPST_RECENT_M8_PRIV0F28_EVENTS];
    uint16_t recent_m8_priv0f28_next;
    uint16_t recent_m8_priv0f28_count;
    lpst_recent_m1_startup_event recent_m1_startup[LPST_RECENT_M1_STARTUP_EVENTS];
    uint16_t recent_m1_startup_next;
    uint16_t recent_m1_startup_count;
    lpst_recent_m1_proc2_entry_event recent_m1_proc2_entries[LPST_RECENT_M1_PROC2_ENTRY_EVENTS];
    uint16_t recent_m1_proc2_entries_next;
    uint16_t recent_m1_proc2_entries_count;
    lpst_recent_m2_proc166_return_event recent_m2_proc166_returns[LPST_RECENT_M2_PROC166_RETURN_EVENTS];
    uint16_t recent_m2_proc166_returns_next;
    uint16_t recent_m2_proc166_returns_count;
    lpst_recent_open_event recent_open_events[LPST_RECENT_OPEN_EVENTS];
    uint16_t recent_open_events_next;
    uint16_t recent_open_events_count;
} lpst_exec_state;

/*
 * Initialise state from image and host: allocate RAM banks and code cache,
 * seed the heap, initialise all global tables, open the OBJ file for code
 * fetching, and position the VM at the entry-point procedure.
 */
lpst_result lpst_exec_init(lpst_exec_state *state, const lpst_image *image, lpst_host *host);

/* Release all heap memory owned by state (RAM banks, globals, code cache,
 * jump snapshots, channels).  Does not free state itself. */
void lpst_exec_free(lpst_exec_state *state);

/* Push/pop/peek on the evaluation stack.  A stack overflow or underflow
 * sets is_halted and prints a diagnostic to stderr. */
void lpst_exec_push(lpst_exec_state *state, uint16_t value);
uint16_t lpst_exec_pop(lpst_exec_state *state);
uint16_t lpst_exec_peek(const lpst_exec_state *state);

/* Print a formatted halt context (registers, stack, recent events) to stream. */
void lpst_trace_halt_context(const lpst_exec_state *state, FILE *stream);

#endif
