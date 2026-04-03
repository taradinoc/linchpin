/*
 * lpst_vm.h — Core VM constants, data structures, and memory primitives.
 *
 * Defines the fundamental types and layout that describe the VM's address
 * space, procedure frames, and initial memory state.  These are shared by
 * both the image loader and the execution engine.
 */
#ifndef LPST_VM_H
#define LPST_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The canonical NULL/false sentinel value used throughout the VM.
 * Any aggregate handle equal to this value is not dereferenceable; doing
 * so is a fatal error, matching MME's behaviour. */
#define LPST_FALSE_SENTINEL ((uint16_t)0x8001u)

/* Total addressable RAM: two 64 KB segments, accessed through four 32 KB banks. */
#define LPST_FULL_RAM_BYTES ((size_t)0x20000u)
#define LPST_SEGMENT_SIZE_BYTES ((size_t)0x10000u)
#define LPST_RAM_BANK_BYTES ((size_t)0x8000u)
#define LPST_RAM_BANK_SHIFT 15u       /* log2(LPST_RAM_BANK_BYTES) */
#define LPST_RAM_BANK_MASK  0x7FFFu   /* LPST_RAM_BANK_BYTES - 1 */
#define LPST_RAM_BANK_COUNT 4u

/* Portion of the high segment reserved exclusively for the tuple (temporary)
 * stack, kept separate from the general heap. */
#define LPST_TUPLE_STACK_RESERVE_BYTES (LPST_SEGMENT_SIZE_BYTES / 64u)

/* A single local-variable initializer record from a procedure header.
 * Locals not listed here are initialised to LPST_FALSE_SENTINEL. */
typedef struct lpst_proc_initializer {
    uint8_t local_index;
    uint16_t value;
} lpst_proc_initializer;

/* The execution frame for one active procedure invocation.
 * Holds the identity of the procedure and a pointer into the shared
 * local-variable storage array. */
typedef struct lpst_vm_frame {
    uint16_t module_id;
    uint16_t procedure_index;        /* 0-based index within the module's procedure table */
    uint16_t procedure_start_offset; /* byte offset of the procedure header within the module */
    uint16_t code_offset;            /* byte offset of the first executable instruction */
    uint16_t *locals;                /* pointer into the executor's local_storage array */
    size_t local_count;
} lpst_vm_frame;

/*
 * Initialise a local-variable array: fill all slots with LPST_FALSE_SENTINEL,
 * then apply the explicit initializers from the procedure header.
 */
bool lpst_bootstrap_locals(
    uint16_t *locals,
    size_t local_count,
    const lpst_proc_initializer *initializers,
    size_t initializer_count);

/*
 * Load the initial RAM image (from the MME file) into the low segment and
 * lay down arena-allocator headers in both segments so the heap is ready
 * for use before the first instruction executes.
 */
bool lpst_seed_initial_ram(
    uint8_t *ram_banks[LPST_RAM_BANK_COUNT],
    const uint8_t *initial_ram_bytes,
    size_t initial_ram_size);

/* Populate a vm_frame struct from its constituent parts. */
void lpst_initialize_frame(
    lpst_vm_frame *frame,
    uint16_t module_id,
    uint16_t procedure_index,
    uint16_t procedure_start_offset,
    uint16_t code_offset,
    uint16_t *locals,
    size_t local_count);

#endif
