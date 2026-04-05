/*
 * lpst_run_internal.h — Internal declarations shared across the run-loop
 * implementation files (lpst_run_core.c, lpst_run_dispatch*.c,
 * lpst_run_io.c, lpst_run_runtime.c, lpst_run.c).
 *
 * None of these are part of the public API.
 */
#ifndef LPST_RUN_INTERNAL_H
#define LPST_RUN_INTERNAL_H

#include "lpst_run.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Return the lpst_module struct for state->current_module_id. */
const lpst_module *current_module(const lpst_exec_state *state);

/* Copy a procedure's cached metadata (local count, initializers, etc.) into
 * out_header, which the caller can then treat like a freshly parsed header. */
void fill_cached_proc_header(const lpst_procedure *proc, lpst_proc_header *out_header);

/* Ensure the 256-byte page covering module_offset is resident in the code cache
 * and return its entry.  Also warms the current_code_page fast-path fields. */
lpst_code_cache_entry *load_code_page(lpst_exec_state *state, const lpst_module *mod, uint16_t module_offset);

/* PINM / UNPINM helpers: preload and pin, or unpin, all code pages belonging
 * to a module.  PINM restores the current fetch page before it returns. */
bool pin_module_code_pages(lpst_exec_state *state, uint16_t module_id);
void unpin_module_code_pages(lpst_exec_state *state, uint16_t module_id);

/* Read a single byte from the module at the given offset, faulting in the
 * appropriate code-cache page if necessary. */
uint8_t read_code_byte_at(lpst_exec_state *state, const lpst_module *mod, uint16_t module_offset);

/* Read the next bytecode byte from the current module, advancing the program
 * counter.  Uses the fast-path page pointer when possible. */
uint8_t fetch_byte(lpst_exec_state *state);

/* Read the next two bytecode bytes as a little-endian 16-bit word. */
uint16_t fetch_le16(lpst_exec_state *state);

/* Decode a branch target: a non-zero 8-bit signed displacement relative to
 * instruction_start+2, or two bytes of an absolute 16-bit offset when it is zero. */
uint16_t decode_jump_target(lpst_exec_state *state, uint16_t instruction_start);

/* Convert a VM aggregate handle to an absolute byte offset in RAM.
 * Bit 15 of the handle selects the segment (0 = low 0..0xFFFF, 1 = high 0x10000..0x1FFFF).
 * The remaining 15 bits are the word address within that segment. */
uint32_t handle_to_byte_offset(uint16_t handle);

/* Low-level RAM access by absolute byte offset. */
uint8_t read_ram_byte(const lpst_exec_state *state, uint32_t byte_offset);
void write_ram_byte(lpst_exec_state *state, uint32_t byte_offset, uint8_t value);
void zero_ram_bytes(lpst_exec_state *state, uint32_t byte_offset, size_t byte_count);

/* Copy byte_count bytes within RAM, handling forward and backward overlaps. */
void move_ram_bytes(lpst_exec_state *state, uint32_t destination_offset, uint32_t source_offset, size_t byte_count);

/* Case-insensitive ASCII string comparison (strcasecmp portable replacement). */
int lpst_ascii_stricmp(const char *left, const char *right);

/* Aggregate (vector/tuple) access through a VM handle.
 * word_index 0 is the header word; payload words begin at word_index 1.
 * read_aggregate_payload_byte / write_aggregate_payload_byte index relative
 * to the first payload byte (i.e., byte 0 of word_index 1). */
uint16_t read_aggregate_word(const lpst_exec_state *state, uint16_t handle, int word_index);
void write_aggregate_word(lpst_exec_state *state, uint16_t handle, int word_index, uint16_t value);
uint8_t read_aggregate_payload_byte(const lpst_exec_state *state, uint16_t handle, int byte_index);
/* Read a byte at raw_byte_offset relative to the start of the aggregate object
 * (i.e., including the header word, which payload accessors skip). */
uint8_t read_aggregate_raw_byte(const lpst_exec_state *state, uint16_t handle, int raw_byte_offset);
void write_aggregate_payload_byte(lpst_exec_state *state, uint16_t handle, int byte_index, uint8_t value);

/* Copy payload bytes between two aggregates (both offsets payload-relative). */
void copy_aggregate_bytes(lpst_exec_state *state,
    uint16_t source_handle,
    int source_offset,
    uint16_t destination_handle,
    int destination_offset,
    uint16_t count);

/* Copy from a raw offset in source to a payload offset in destination, so
 * the caller can treat the source header bytes as part of the data stream. */
void copy_aggregate_raw_bytes_to_payload(lpst_exec_state *state,
    uint16_t source_handle,
    int source_offset,
    uint16_t destination_handle,
    int destination_offset,
    uint16_t count);

/* Return true if handle is a valid (non-null, non-false) aggregate handle. */
int is_usable_aggregate_handle(uint16_t handle);

/* Append an event to a fixed-size ring-buffer, overwriting the oldest entry. */
void record_recent_word_event(
    lpst_recent_word_event *events,
    uint16_t capacity,
    uint16_t *next_index,
    uint16_t *count,
    uint16_t module_id,
    uint16_t pc,
    uint16_t value);
void record_recent_open_event(
    lpst_exec_state *state,
    uint16_t result,
    uint16_t error_detail,
    uint8_t mode,
    const char *name);

/* Look up or allocate a channel by ID.  Passing LPST_FALSE_SENTINEL re-uses
 * the most recently activated channel without changing active_channel_id. */
lpst_channel *resolve_channel(lpst_exec_state *state, uint16_t channel_id);

/* Allocate the next available channel slot and write its ID to *out_id. */
lpst_channel *alloc_channel(lpst_exec_state *state, uint16_t *out_id);

/* Close ch->fp (if open) and mark the slot as free. */
void close_channel(lpst_channel *ch);

/* Return the record size in bytes from system slot 0xCC.
 * Zero or negative means 256 bytes per record (the default). */
int get_record_size_bytes(const lpst_exec_state *state);

/* Read a VM string (Pascal layout: length word + raw bytes) into a C string.
 * Returns the number of bytes written (excluding the null terminator). */
int read_vm_string(const lpst_exec_state *state, uint16_t handle, char *out, size_t max_len);

/* Compare two VM strings using the structured sort-key order (matching MME).
 * Returns -1 (as uint16_t), 0, or 1. */
uint16_t compare_vm_strings(const lpst_exec_state *state, uint16_t first_handle, uint16_t second_handle);

/* Case-insensitive comparison of a substring of first_handle against
 * all of second_handle.  first_offset and first_length limit the region
 * within first_handle that is compared. */
uint16_t compare_vm_strings_ignore_case(
    const lpst_exec_state *state,
    uint16_t first_handle,
    uint16_t first_offset,
    uint16_t first_length,
    uint16_t second_handle,
    uint16_t second_length);

/* Return the size of ch in bytes, trying stat(2) then fseek/ftell fallbacks. */
long query_channel_size_bytes(const lpst_channel *ch);

/* Write directory + separator + name into out.  Returns 1 on success. */
int join_path(const char *directory, const char *name, char separator, char *out, size_t out_size);

/* Resolve a bare filename against the data and base directories from image
 * and write the full path into out.  Returns 1 on success, 0 on failure. */
int resolve_file_path(const lpst_image *image, const char *name, char *out, size_t out_size);

/* Read/write a module global variable for the currently-executing module.
 * Indices that fall outside the module's own global range are read/written
 * from system_module_globals[].  Writing certain slots (e.g., 0xC9) has
 * side effects such as committing the cursor position to the host. */
uint16_t load_module_global(const lpst_exec_state *state, uint8_t index);
void store_module_global(lpst_exec_state *state, uint8_t index, uint16_t value);

/* Clamp a cursor row/column value to the valid screen range. */
uint8_t clamp_cursor_row_value(uint16_t value);
uint8_t get_visible_screen_width(const lpst_exec_state *state);
uint8_t clamp_cursor_col_value(const lpst_exec_state *state, uint16_t value);

/* Allocate a heap vector of word_count words from the arena.
 * Returns LPST_FALSE_SENTINEL on out-of-memory. */
uint16_t allocate_vector(lpst_exec_state *state, uint16_t word_count);

/* Allocate a temporary tuple of word_count words from the tuple stack.
 * Tuples grow downward and are reclaimed by do_return through the stack
 * frame's tuple_stack_byte snapshot. */
uint16_t allocate_tuple(lpst_exec_state *state, uint16_t word_count);

/* Return a vector to the per-size free list so it can be reused. */
void release_vector(lpst_exec_state *state, uint16_t handle, uint16_t word_count);

/* Push a call frame for a same-module (near) call to target_offset, passing
 * argc arguments already on the evaluation stack. */
bool enter_near_call(
    lpst_exec_state *state,
    uint16_t target_offset,
    uint8_t argc,
    uint16_t return_pc);

/* Push a call frame for a cross-module (far) call.  selector encodes the
 * target as (module_id << 8) | procedure_index. */
bool enter_far_call(
    lpst_exec_state *state,
    uint16_t selector,
    uint8_t argc,
    uint16_t return_pc);

/* Pop the top call frame, restore the caller's locals, and push result_count
 * values from the results array onto the evaluation stack. */
void do_return(lpst_exec_state *state, uint16_t *results, uint16_t result_count);

/* Take a SETJMP snapshot of the current VM state.  protected_offset is the
 * target for LONGJMP and landing_offset is the target for LONGJMPR.
 * Pushes the snapshot token onto the evaluation stack. */
bool save_jump_snapshot(lpst_exec_state *state, uint16_t protected_offset, uint16_t landing_offset);

/* Restore the VM to the snapshot identified by token.  If use_return_target
 * is true, jump to longjmpr_program_counter and push return_value; otherwise
 * jump to longjmp_program_counter. */
bool restore_jump_snapshot(
    lpst_exec_state *state,
    uint16_t token,
    bool use_return_target,
    uint16_t return_value);

/* Send a single character to the host after applying display style state. */
void host_print_char(lpst_exec_state *state, uint8_t ch);

/* Print length bytes of a VM string vector starting at start_offset. */
void host_print_vector(lpst_exec_state *state, uint16_t handle, uint16_t length, uint16_t start_offset);

/* Execute the SETWIN opcode: configure a window descriptor and return the
 * previous scroll region as a packed value. */
uint16_t execute_setwin(lpst_exec_state *state, uint16_t descriptor_handle);

/* Execute the WPRINTV opcode: write char_count bytes from source_handle
 * (at source_offset) into window descriptor_handle. */
uint16_t execute_wprintv(lpst_exec_state *state,
    uint16_t descriptor_handle,
    uint16_t source_handle,
    uint16_t char_count,
    uint16_t source_offset);

/* Execute a DISP suboperation (clears, scrolls, cursor moves, etc.). */
void host_apply_disp(lpst_exec_state *state, uint8_t subop);

/* Bit-field extraction and replacement.
 * control_word format: bits 15-12 = shift, bits 11-8 = width.
 * For aggregate operations (non-_from_local variants), bits 7-0 = word index.
 * For local operations (_from_local variants), bits 3-0 = word index. */
uint16_t extract_bit_field(const lpst_exec_state *state, uint16_t handle, uint16_t control_word);
uint16_t extract_bit_field_from_local(const lpst_exec_state *state, uint16_t handle, uint16_t control_word);
void replace_bit_field(lpst_exec_state *state, uint16_t handle, uint16_t control_word, uint16_t value);
void replace_bit_field_in_local(lpst_exec_state *state, uint16_t handle, uint16_t control_word, uint16_t value);

/* Set or clear a single bit in a word of an aggregate or local variable.
 * control_word format: bits 15-12 = bit number, bit 8 = desired value,
 * and the same word-index encoding as the bit-field operations above. */
void replace_single_bit(lpst_exec_state *state, uint16_t handle, uint16_t control_word);
void replace_single_bit_in_local(lpst_exec_state *state, uint16_t handle, uint16_t control_word);

/* Floating-point arithmetic on values stored as 8-word (16-byte) doubles
 * in VM aggregates.  Pop two operand handles, operate, push result handle. */
uint16_t execute_real_add(lpst_exec_state *state);
uint16_t execute_real_subtract(lpst_exec_state *state);
uint16_t execute_real_multiply(lpst_exec_state *state);
uint16_t execute_real_divide(lpst_exec_state *state);
uint16_t execute_real_log(lpst_exec_state *state);
uint16_t execute_real_exp(lpst_exec_state *state);

/* Extended-opcode dispatch helpers. */
void handle_ext34(lpst_exec_state *state);
void handle_ext35(lpst_exec_state *state);
void handle_ext37(lpst_exec_state *state);
void handle_ext_open(lpst_exec_state *state);
void handle_ext_close(lpst_exec_state *state);
void handle_ext_seqread(lpst_exec_state *state);
void handle_ext_seqwrite(lpst_exec_state *state);
void handle_ext_readrec(lpst_exec_state *state);
void handle_ext_writerec(lpst_exec_state *state);
void handle_ext_fsize(lpst_exec_state *state);
void handle_ext_unlink(lpst_exec_state *state);

/* Main extended-opcode dispatcher: reads the sub-opcode byte and calls the
 * appropriate handler above. */
void dispatch_extended(lpst_exec_state *state);

#endif
