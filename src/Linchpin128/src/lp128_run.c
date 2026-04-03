/* lp128_run.c – Cornerstone VM execution engine for the C128 port.
 *
 * Implements the full fetch-decode-execute loop and all opcodes.
 * Core opcode semantics are shared with the other Linchpin implementations.
 * Key differences for the C128 port:
 *   • Code pages reside in the REU and are fetched via DMA into pinned
 *     CPU-RAM page cache slots; there is no seek-based lazy loading.
 *   • The VM heap lives entirely in the REU (128 KB across two 64-KB
 *     segments); all read/write goes through reu_*() helpers.
 *   • Call-frame locals are spilled to REU on call and reloaded on return,
 *     because they are too large to keep in the pinned CPU-RAM area.
 *   • Most EXT_* opcodes for file I/O and floating-point raise HALT;
 *     the OPEN/CLOSE/READREC opcodes use the KERNAL IEC bus.
 */

#include "lp128_vm.h"
#include "lp128_diag.h"

#include <string.h>

/* On mos/C128 suppress all diagnostic output to avoid linking in printf. */
#if defined(__mos__) || defined(__llvm_mos__)
#include "lp128_reu.h"
#include "lp128_mmu.h"
#define VM_DIAG(...)  ((void)0)

/* Heap page cache lives in plain RAM below the software stack and below the
 * REU scratch buffer at $0B00, so REU DMA never targets the $D000-$DFFF I/O
 * shadow while MMU=$3E. */
#define CODE_PAGE_CACHE0_ADDR 0x0800u
#define CODE_PAGE_CACHE1_ADDR 0x0900u
#define HEAP_PAGE_CACHE_ADDR 0x0A00u
#define VM_REU_DMA_STAGE_ADDR 0x0B40u
#define VM_REU_DMA_STAGE_SIZE 32u
#define VM_FILE_NAME_BUF_ADDR 0x0360u
#define VM_FILE_NAME_MAX      12u
#define VM_FILE_LFN           3u
#define VM_FILE_SECONDARY     2u
#define VM_FILE_INIT_FLAG_ADDR 0x035Cu
#define VM_FILE_NEXT_ID_ADDR   0x035Eu
#define VM_FILE_TABLE_ADDR     0x0370u
#define VM_MAX_FILE_CHANNELS   6u
static uint8_t *const code_page_cache_slots[LP128_VM_CODE_PAGE_CACHE_SLOTS] = {
    (uint8_t *)CODE_PAGE_CACHE0_ADDR,
    (uint8_t *)CODE_PAGE_CACHE1_ADDR,
};
static uint8_t *const heap_page_cache = (uint8_t *)HEAP_PAGE_CACHE_ADDR;
static uint8_t *const vm_reu_dma_stage = (uint8_t *)VM_REU_DMA_STAGE_ADDR;
static char *const vm_file_name_buf = (char *)VM_FILE_NAME_BUF_ADDR;

static uint8_t diag_ascii_upper_sc(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (uint8_t)(ch - '@') : 0x20u;
}

static void diag_file_stage(char stage, uint8_t status)
{
    uint8_t color = 0x06u;

    switch (stage) {
    case 'M': color = 0x0Eu; break;
    case 'N': color = 0x03u; break;
    case 'L': color = 0x07u; break;
    case 'O': color = 0x08u; break;
    case 'K': color = 0x02u; break;
    case 'R': color = 0x05u; break;
    case 'G': color = 0x04u; break;
    default:  color = 0x06u; break;
    }

    DIAG_VIC(13, diag_ascii_upper_sc(stage));
    DIAG_VIC_HEX8(14, status);
    diag_set_vic_background(color);
    diag_set_vic_border((uint8_t)(status == 0u ? color : 0x0Au));
}

static void vm_write_byte(lp128_vm_state *vm, uint32_t offset, uint8_t value);
static uint32_t handle_to_byte_offset(uint16_t handle);
static uint16_t agg_read_word(const lp128_vm_state *vm, uint16_t handle, int index);
static uint8_t agg_read_payload_byte(const lp128_vm_state *vm, uint16_t handle, int byte_index);

typedef struct {
    bool     valid;
    uint8_t  mode;
    uint16_t id;
    uint16_t size_bytes;
    char     name[VM_FILE_NAME_MAX + 1u];
} __attribute__((packed)) lp128_file_channel;

static uint8_t *const s_file_channels_init_flag = (uint8_t *)VM_FILE_INIT_FLAG_ADDR;
static uint16_t *const s_next_file_channel_id_ptr = (uint16_t *)VM_FILE_NEXT_ID_ADDR;
static lp128_file_channel *const s_file_channels = (lp128_file_channel *)VM_FILE_TABLE_ADDR;

/* Transfer len bytes from src (anywhere in CPU RAM) to the REU, chunking
 * through a fixed low-RAM staging buffer so the DMA source address is
 * always below $D000 and never falls in the I/O shadow when MMU=$3E. */
static void reu_store_safe(const void *src, reu_addr_t dst, uint16_t len)
{
    const uint8_t *cursor = (const uint8_t *)src;

    while (len > 0u) {
        uint16_t chunk = (len > VM_REU_DMA_STAGE_SIZE) ? VM_REU_DMA_STAGE_SIZE : len;
        memcpy(vm_reu_dma_stage, cursor, chunk);
        reu_store(vm_reu_dma_stage, dst, chunk);
        cursor += chunk;
        dst += chunk;
        len -= chunk;
    }
}

/* Transfer len bytes from the REU to dst (anywhere in CPU RAM), chunking
 * through a fixed low-RAM staging buffer for the same reason as reu_store_safe. */
static void reu_fetch_safe(void *dst, reu_addr_t src, uint16_t len)
{
    uint8_t *cursor = (uint8_t *)dst;

    while (len > 0u) {
        uint16_t chunk = (len > VM_REU_DMA_STAGE_SIZE) ? VM_REU_DMA_STAGE_SIZE : len;
        reu_fetch(vm_reu_dma_stage, src, chunk);
        memcpy(cursor, vm_reu_dma_stage, chunk);
        cursor += chunk;
        src += chunk;
        len -= chunk;
    }
}

/* Return the access-family portion (bits 1-0) of a file open-mode byte.
 * 0 = read, 1 = write, 2 = append, 3 = read-write. */
static uint8_t file_mode_access_family(uint8_t mode)
{
    return (uint8_t)(mode & 0x03u);
}

/* Lazily initialise the file channel table the first time it is used.
 * The table and its state live in fixed CPU-RAM locations so they persist
 * across vm_run calls without needing dynamic allocation. */
static void ensure_file_channels_initialized(void)
{
    if (*s_file_channels_init_flag != 0xA5u) {
        memset(s_file_channels, 0, sizeof(lp128_file_channel) * VM_MAX_FILE_CHANNELS);
        *s_next_file_channel_id_ptr = 2u;
        *s_file_channels_init_flag = 0xA5u;
    }
}

/* Find a file channel by its runtime ID. Returns NULL if not found. */
static lp128_file_channel *find_file_channel(uint16_t id)
{
    uint8_t i;

    ensure_file_channels_initialized();

    for (i = 0u; i < VM_MAX_FILE_CHANNELS; i++) {
        if (s_file_channels[i].valid && s_file_channels[i].id == id) {
            return &s_file_channels[i];
        }
    }

    return NULL;
}

/* Claim a free slot in the channel table and populate it with the given
 * filename, mode, and size. Returns a pointer to the slot, or NULL if
 * the table is full. */
static lp128_file_channel *allocate_file_channel(const char *name, uint8_t mode, uint16_t size_bytes)
{
    uint8_t i;

    ensure_file_channels_initialized();

    for (i = 0u; i < VM_MAX_FILE_CHANNELS; i++) {
        if (!s_file_channels[i].valid) {
            lp128_file_channel *channel = &s_file_channels[i];
            uint8_t j = 0u;

            memset(channel, 0, sizeof(*channel));
            channel->valid = true;
            channel->mode = mode;
            channel->id = (*s_next_file_channel_id_ptr)++;
            channel->size_bytes = size_bytes;

            while (j < VM_FILE_NAME_MAX && name[j] != '\0') {
                channel->name[j] = name[j];
                j++;
            }
            channel->name[j] = '\0';
            return channel;
        }
    }

    return NULL;
}

/* Return the file channel slot with the given ID to the free pool. */
static void release_file_channel(uint16_t id)
{
    lp128_file_channel *channel = find_file_channel(id);
    if (channel != NULL) {
        memset(channel, 0, sizeof(*channel));
    }
}

/* Copy a VM Pascal-layout string (a length word followed by raw ASCII bytes)
 * into the C string buffer out. At most out_size-1 bytes are copied,
 * and the result is always NUL-terminated. */
static void read_vm_string_ascii(lp128_vm_state *vm, uint16_t handle, char *out, uint8_t out_size)
{
    uint16_t byte_length;
    uint8_t oi = 0u;
    uint16_t si;

    if (out == NULL || out_size == 0u) {
        return;
    }

    out[0] = '\0';
    if (handle == 0u || handle == LP128_FALSE_SENTINEL) {
        return;
    }

    byte_length = agg_read_word(vm, handle, 0);
    for (si = 0u; si < byte_length && oi + 1u < out_size; si++) {
        uint8_t ch = agg_read_payload_byte(vm, handle, (int)si);
        if (ch == 0u) {
            break;
        }
        out[oi++] = (char)ch;
    }
    out[oi] = '\0';
}

/* Open a file by name on the IEC bus (drive 8) and select it as the current
 * input channel.  The filename is copied to fixed low-RAM so KERNAL SETNAM
 * can see it after the MMU switches to KERNAL ROM mode. */
static bool c128_open_runtime_file(lp128_vm_state *vm, const char *path)
{
    enum { RUNTIME_NAMEBUF_ADDR = 0x033Cu, RUNTIME_NAMEBUF_MAX = 31u };
    char *const namebuf = (char *)RUNTIME_NAMEBUF_ADDR;
    uint8_t rv;
    uint8_t i;

    diag_file_stage('N', 0u);
    /* OPEN reads the filename bytes after KERNAL ROM is mapped in, so the
     * pointer must reference low RAM that remains visible in that mode. */
    for (i = 0u; i < RUNTIME_NAMEBUF_MAX && path[i] != '\0'; i++) {
        namebuf[i] = path[i];
    }
    namebuf[i] = '\0';

    lp128_k_setnam(namebuf);
    diag_file_stage('L', 0u);
    lp128_k_setlfs(VM_FILE_LFN, 8u, VM_FILE_SECONDARY);
    diag_file_stage('O', 0u);
    rv = lp128_k_open();
    diag_file_stage('O', rv);
    if (rv != 0u) {
        return false;
    }

    diag_file_stage('K', 0u);
    rv = lp128_k_chkin(VM_FILE_LFN);
    diag_file_stage('K', rv);
    if (rv != 0u) {
        lp128_k_close(VM_FILE_LFN);
        return false;
    }

    (void)lp128_k_readst();
    diag_file_stage('R', 0u);
    return true;
}

/* Close the currently open runtime file and deselect the input channel. */
static void c128_close_runtime_file(void)
{
    lp128_k_clrch();
    lp128_k_close(VM_FILE_LFN);
}

/* Read one byte from the currently open file via KERNAL CHRIN.
 * Returns 1 on success, 0 on EOF, -1 on device error.  If out_byte is
 * non-NULL the byte is stored there regardless of the return value. */
static int c128_read_runtime_file_byte(uint8_t *out_byte)
{
    uint8_t byte = lp128_k_chrin();
    uint8_t st = lp128_k_readst();

    if (out_byte != NULL) {
        *out_byte = byte;
    }
    if (st & 0x80u) {
        return -1;
    }
    if (st & 0x40u) {
        return 0;
    }
    return 1;
}

/* Determine the byte length of a named file by opening it, reading it
 * completely, and counting the bytes.  Needed before allocating a receive
 * buffer because the IEC bus does not expose file sizes. */
static bool c128_measure_runtime_file(lp128_vm_state *vm, const char *path, uint16_t *out_size)
{
    uint16_t size = 0u;
    uint8_t byte;
    int status;

    diag_file_stage('M', 0u);
    if (!c128_open_runtime_file(vm, path)) {
        return false;
    }

    do {
        status = c128_read_runtime_file_byte(&byte);
        if (status >= 0) {
            size++;
        }
    } while (status > 0);

    c128_close_runtime_file();
    diag_file_stage('M', (uint8_t)(status < 0 ? 0xEEu : 0u));
    if (status < 0) {
        return false;
    }

    if (out_size != NULL) {
        *out_size = size;
    }
    return true;
}

/* Read byte_count bytes from file path starting at start_offset into the
 * VM vector vec_handle.  Bytes not reached due to EOF are zero-filled.
 * Returns the number of bytes actually read from the file. */
static uint16_t c128_read_runtime_file_range(lp128_vm_state *vm,
                                             const char *path,
                                             uint32_t start_offset,
                                             uint16_t byte_count,
                                             uint16_t vec_handle)
{
    uint32_t skipped = 0u;
    uint16_t copied = 0u;
    uint16_t bytes_read = 0u;
    uint32_t dst_base = handle_to_byte_offset(vec_handle);
    uint8_t byte;
    int status;

    diag_file_stage('G', 0u);
    if (!c128_open_runtime_file(vm, path)) {
        goto zero_fill;
    }

    while (skipped < start_offset) {
        status = c128_read_runtime_file_byte(&byte);
        if (status <= 0) {
            c128_close_runtime_file();
            goto zero_fill;
        }
        skipped++;
    }

    while (copied < byte_count) {
        status = c128_read_runtime_file_byte(&byte);
        if (status < 0) {
            c128_close_runtime_file();
            goto zero_fill;
        }

        vm_write_byte(vm, dst_base + copied, byte);
        copied++;
        bytes_read++;

        if (status == 0) {
            break;
        }
    }

    c128_close_runtime_file();

zero_fill:
    while (copied < byte_count) {
        vm_write_byte(vm, dst_base + copied, 0u);
        copied++;
    }

    return bytes_read;
}

/* Write uppercase ASCII text to the 40-column VIC screen at (row, col)
 * using screen codes.  Readable in any MMU mode.  Used for diagnostic
 * output when the VDC 80-column display is unavailable or unreliable. */
static void diag_write_text(uint8_t row, uint8_t col, const char *text)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * 40u + col);

    while (*text != '\0' && col < 40u) {
        char ch = *text++;
        if (ch >= 'A' && ch <= 'Z') {
            *dst++ = (uint8_t)(ch - '@');
        } else if (ch >= '0' && ch <= '9') {
            *dst++ = (uint8_t)ch;
        } else if (ch == '=') {
            *dst++ = 0x3Du;
        } else {
            *dst++ = 0x20u;
        }
        col++;
    }
}

/* Write a 2-digit hex value to the 40-column VIC screen at (row, col). */
static void diag_write_hex8(uint8_t row, uint8_t col, uint8_t value)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * 40u + col);
    dst[0] = diag_hex_sc((uint8_t)(value >> 4));
    dst[1] = diag_hex_sc((uint8_t)(value & 0x0Fu));
}

/* Write a 4-digit hex value to the 40-column VIC screen at (row, col). */
static void diag_write_hex16_row(uint8_t row, uint8_t col, uint16_t value)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * 40u + col);
    dst[0] = diag_hex_sc((uint8_t)(value >> 12));
    dst[1] = diag_hex_sc((uint8_t)((value >> 8) & 0x0Fu));
    dst[2] = diag_hex_sc((uint8_t)((value >> 4) & 0x0Fu));
    dst[3] = diag_hex_sc((uint8_t)(value & 0x0Fu));
}

static void diag_trace_module2_packed_vread(lp128_vm_state *vm,
                                            uint16_t instr_start,
                                            uint8_t local_index,
                                            uint16_t packed_handle,
                                            uint8_t word_index)
{
    diag_write_text(15u, 0u, "VRPC=");
    diag_write_hex16_row(15u, 5u, instr_start);
    diag_write_text(15u, 10u, "LI=");
    diag_write_hex8(15u, 13u, local_index);
    diag_write_text(15u, 16u, "WI=");
    diag_write_hex8(15u, 19u, word_index);
    diag_write_text(15u, 22u, "H=");
    diag_write_hex16_row(15u, 24u, packed_handle);
    diag_write_text(15u, 29u, "L4=");
    diag_write_hex16_row(15u, 32u, vm->locals[4]);

    diag_write_text(14u, 0u, "L0=");
    diag_write_hex16_row(14u, 3u, vm->locals[0]);
    diag_write_text(14u, 8u, "L1=");
    diag_write_hex16_row(14u, 11u, vm->locals[1]);
    diag_write_text(14u, 16u, "L2=");
    diag_write_hex16_row(14u, 19u, vm->locals[2]);

    diag_write_text(13u, 0u, "MG11=");
    diag_write_hex16_row(13u, 5u, lp128_vm_read_module_global(vm, 2u, 0x11u));
    diag_write_text(13u, 10u, "MG12=");
    diag_write_hex16_row(13u, 15u, lp128_vm_read_module_global(vm, 2u, 0x12u));
}

static void diag_trace_module2_loadmg(uint16_t instr_start, uint8_t index, uint16_t value)
{
    diag_write_text(12u, 0u, "LDPC=");
    diag_write_hex16_row(12u, 5u, instr_start);
    diag_write_text(12u, 10u, "MG=");
    diag_write_hex8(12u, 13u, index);
    diag_write_text(12u, 16u, "V=");
    diag_write_hex16_row(12u, 18u, value);
}

static void diag_trace_module2_putl(lp128_vm_state *vm, uint16_t instr_start, uint8_t local_index, uint16_t value)
{
    diag_write_text(13u, 0u, "STPC=");
    diag_write_hex16_row(13u, 5u, instr_start);
    diag_write_text(13u, 10u, "LI=");
    diag_write_hex8(13u, 13u, local_index);
    diag_write_text(13u, 16u, "V=");
    diag_write_hex16_row(13u, 18u, value);
    diag_write_text(13u, 23u, "L1=");
    diag_write_hex16_row(13u, 26u, vm->locals[1]);
    diag_write_text(13u, 31u, "L2=");
    diag_write_hex16_row(13u, 34u, vm->locals[2]);
}

static void diag_trace_module2_after_store(lp128_vm_state *vm, uint16_t instr_start)
{
    diag_write_text(6u, 0u, "AFPC=");
    diag_write_hex16_row(6u, 5u, instr_start);
    diag_write_text(6u, 10u, "ST=");
    diag_write_hex16_row(6u, 13u, vm->eval_stack_top);
    diag_write_text(6u, 18u, "L1=");
    diag_write_hex16_row(6u, 21u, vm->locals[1]);
    diag_write_text(6u, 26u, "L2=");
    diag_write_hex16_row(6u, 29u, vm->locals[2]);
}

static void diag_trace_module2_pushl(lp128_vm_state *vm, uint16_t instr_start, uint8_t local_index, uint16_t value)
{
    diag_write_text(10u, 0u, "PSPC=");
    diag_write_hex16_row(10u, 5u, instr_start);
    diag_write_text(10u, 10u, "LI=");
    diag_write_hex8(10u, 13u, local_index);
    diag_write_text(10u, 16u, "V=");
    diag_write_hex16_row(10u, 18u, value);
    diag_write_text(10u, 23u, "ST=");
    diag_write_hex16_row(10u, 26u, vm->eval_stack_top);
    diag_write_text(10u, 31u, "L1=");
    diag_write_hex16_row(10u, 34u, vm->locals[1]);
}

static void diag_trace_module2_jumpz(lp128_vm_state *vm, uint16_t instr_start, uint16_t value)
{
    diag_write_text(11u, 0u, "JZPC=");
    diag_write_hex16_row(11u, 5u, instr_start);
    diag_write_text(11u, 10u, "V=");
    diag_write_hex16_row(11u, 12u, value);
    diag_write_text(11u, 17u, "ST=");
    diag_write_hex16_row(11u, 20u, vm->eval_stack_top);
    diag_write_text(11u, 25u, "L1=");
    diag_write_hex16_row(11u, 28u, vm->locals[1]);
    diag_write_text(11u, 33u, "L2=");
    diag_write_hex16_row(11u, 36u, vm->locals[2]);
}

static void diag_trace_module2_loop_entry(lp128_vm_state *vm, uint16_t pc)
{
    diag_write_text(8u, 0u, "ENPC=");
    diag_write_hex16_row(8u, 5u, pc);
    diag_write_text(8u, 10u, "ST=");
    diag_write_hex16_row(8u, 13u, vm->eval_stack_top);
    diag_write_text(8u, 18u, "L1=");
    diag_write_hex16_row(8u, 21u, vm->locals[1]);
    diag_write_text(8u, 26u, "L2=");
    diag_write_hex16_row(8u, 29u, vm->locals[2]);
}

static void diag_trace_module2_post_fetch(lp128_vm_state *vm, uint16_t pc, uint8_t opcode)
{
    diag_write_text(9u, 0u, "FQPC=");
    diag_write_hex16_row(9u, 5u, pc);
    diag_write_text(9u, 10u, "OP=");
    diag_write_hex8(9u, 13u, opcode);
    diag_write_text(9u, 16u, "ST=");
    diag_write_hex16_row(9u, 19u, vm->eval_stack_top);
    diag_write_text(9u, 24u, "L1=");
    diag_write_hex16_row(9u, 27u, vm->locals[1]);
    diag_write_text(9u, 32u, "L2=");
    diag_write_hex16_row(9u, 35u, vm->locals[2]);
}

/* Invalidate the heap page cache if a pending write overlaps the cached
 * region.  Must be called before every reu_write_* so that a subsequent
 * read does not serve stale data from the cache. */
static void invalidate_heap_cache_range(lp128_vm_state *vm, uint32_t offset, uint32_t count)
{
    uint32_t cache_end;
    uint32_t write_end;

    if (!vm->heap_page_cache_valid || count == 0u) {
        return;
    }

    cache_end = vm->heap_page_cache_base + LP128_CODE_PAGE_SIZE;
    write_end = offset + count;
    if (offset < cache_end && write_end > vm->heap_page_cache_base) {
        vm->heap_page_cache_valid = false;
    }
}

/* Read one byte from the VM heap, using a 256-byte page cache to amortize
 * REU DMA overhead.  On a cache miss the containing page is fetched into
 * heap_page_cache and the cache base is updated. */
static uint8_t read_heap_byte_cached(lp128_vm_state *vm, uint32_t offset)
{
    uint32_t page_base = offset & ~(uint32_t)(LP128_CODE_PAGE_SIZE - 1u);
    uint16_t page_offset = (uint16_t)(offset - page_base);

    if (vm->heap_page_cache_valid && vm->heap_page_cache_base == page_base) {
#if LP128_ENABLE_PERF_COUNTERS
        vm->perf.heap_page_cache_hits++;
#endif
        return heap_page_cache[page_offset];
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.heap_page_cache_loads++;
#endif
    reu_fetch(heap_page_cache, REU_VM_HEAP_OFFSET + page_base, LP128_CODE_PAGE_SIZE);
    vm->heap_page_cache_valid = true;
    vm->heap_page_cache_base = page_base;
    return heap_page_cache[page_offset];
}
#else
#include <stdio.h>
#define VM_DIAG(...)  fprintf(stderr, __VA_ARGS__)

static uint8_t code_page_cache_storage[LP128_VM_CODE_PAGE_CACHE_SLOTS][LP128_CODE_PAGE_SIZE];
static uint8_t *const code_page_cache_slots[LP128_VM_CODE_PAGE_CACHE_SLOTS] = {
    code_page_cache_storage[0],
    code_page_cache_storage[1],
};
#endif

/* ── Inline stack helpers ────────────────────────────────────────────────── */

/* Push a value onto the evaluation stack.  Sets is_halted on overflow. */
static void vm_push(lp128_vm_state *vm, uint16_t v)
{
    if (vm->eval_stack_top < LP128_EVAL_STACK_CAP) {
        vm->eval_stack[vm->eval_stack_top++] = v;
    } else {
        VM_DIAG("eval stack overflow\n");
        vm->is_halted  = true;
        vm->halt_code  = 0xFFFF;
    }
}

/* Pop and return the top value from the evaluation stack.  Sets is_halted
 * on underflow and returns 0. */
static uint16_t vm_pop(lp128_vm_state *vm)
{
    if (vm->eval_stack_top > 0) {
        return vm->eval_stack[--vm->eval_stack_top];
    }
    VM_DIAG("eval stack underflow\n");
    vm->is_halted = true;
    vm->halt_code = 0xFFFF;
    return 0;
}

/* Return the top value without popping it.  Returns 0 if the stack is empty. */
static uint16_t vm_peek(const lp128_vm_state *vm)
{
    if (vm->eval_stack_top > 0) {
        return vm->eval_stack[vm->eval_stack_top - 1];
    }
    return 0;
}

/* ── RAM helpers ─────────────────────────────────────────────────────────── *
 * On C128 the VM heap lives in the REU; all access goes through DMA.          *
 * On POSIX the heap is a malloc'd flat buffer pointed to by vm->ram.          */

/* Read one byte from the VM heap at the given byte offset. */
static uint8_t vm_read_byte(const lp128_vm_state *vm, uint32_t offset)
{
    if (offset >= LP128_FULL_RAM_BYTES) return 0;
#if defined(__mos__) || defined(__llvm_mos__)
    return read_heap_byte_cached((lp128_vm_state *)vm, offset);
#else
    return vm->ram[offset];
#endif
}

/* Write one byte to the VM heap at the given byte offset. */
static void vm_write_byte(lp128_vm_state *vm, uint32_t offset, uint8_t value)
{
    if (offset >= LP128_FULL_RAM_BYTES) return;
#if defined(__mos__) || defined(__llvm_mos__)
    invalidate_heap_cache_range(vm, offset, 1u);
    reu_write_byte(REU_VM_HEAP_OFFSET + offset, value);
#else
    vm->ram[offset] = value;
#endif
}

/* Read a little-endian 16-bit word from the VM heap at the given byte offset. */
static uint16_t vm_read_word(const lp128_vm_state *vm, uint32_t offset)
{
    if (offset + 1u >= LP128_FULL_RAM_BYTES) return 0;
#if defined(__mos__) || defined(__llvm_mos__)
    if ((offset & (LP128_CODE_PAGE_SIZE - 1u)) != (LP128_CODE_PAGE_SIZE - 1u)) {
        lp128_vm_state *mutable_vm = (lp128_vm_state *)vm;
        uint8_t lo = read_heap_byte_cached(mutable_vm, offset);
        uint8_t hi = read_heap_byte_cached(mutable_vm, offset + 1u);
        return (uint16_t)(lo | ((uint16_t)hi << 8));
    }
    return (uint16_t)(read_heap_byte_cached((lp128_vm_state *)vm, offset)
        | ((uint16_t)read_heap_byte_cached((lp128_vm_state *)vm, offset + 1u) << 8));
#else
    return (uint16_t)(vm->ram[offset] | ((uint16_t)vm->ram[offset + 1] << 8));
#endif
}

/* Write a little-endian 16-bit word to the VM heap at the given byte offset. */
static void vm_write_word(lp128_vm_state *vm, uint32_t offset, uint16_t value)
{
    if (offset + 1u >= LP128_FULL_RAM_BYTES) return;
#if defined(__mos__) || defined(__llvm_mos__)
    invalidate_heap_cache_range(vm, offset, 2u);
    reu_write_word(REU_VM_HEAP_OFFSET + offset, value);
#else
    vm->ram[offset]     = (uint8_t)(value & 0xFFu);
    vm->ram[offset + 1] = (uint8_t)(value >> 8);
#endif
}

/* Zero count bytes in the VM heap starting at byte offset. */
static void vm_zero_bytes(lp128_vm_state *vm, uint32_t offset, uint32_t count)
{
    if (offset + count > LP128_FULL_RAM_BYTES) return;
#if defined(__mos__) || defined(__llvm_mos__)
    invalidate_heap_cache_range(vm, offset, count);
    if (count > 0xFFFFu) return;  /* single-op limit */
    reu_fill_zero(REU_VM_HEAP_OFFSET + offset, (uint16_t)count);
#else
    memset(vm->ram + offset, 0, (size_t)count);
#endif
}

/* Copy count bytes within the VM heap, handling overlapping regions correctly. */
static void vm_move_bytes(lp128_vm_state *vm, uint32_t dst, uint32_t src, uint32_t count)
{
    if (count == 0 || dst == src) return;
    if (dst + count > LP128_FULL_RAM_BYTES || src + count > LP128_FULL_RAM_BYTES) return;
#if defined(__mos__) || defined(__llvm_mos__)
    invalidate_heap_cache_range(vm, dst, count);
    invalidate_heap_cache_range(vm, src, count);
    if (count > 0xFFFFu) return;  /* single-op limit */
    reu_copy(REU_VM_HEAP_OFFSET + dst,
             REU_VM_HEAP_OFFSET + src,
             (uint16_t)count);
#else
    memmove(vm->ram + dst, vm->ram + src, (size_t)count);
#endif
}

/* ── Handle / aggregate helpers ─────────────────────────────────────────── */

/* Convert a VM object handle to a byte offset into the 128-KB heap.
 * The handle format is: bit 15 = segment selector (0 = low 64 KB,
 * 1 = high 64 KB); bits 14-0 = word address within that segment.
 * Multiplying the word address by 2 gives the byte offset within the
 * segment; the segment selector adds LP128_SEGMENT_SIZE if set. */
static uint32_t handle_to_byte_offset(uint16_t handle)
{
    uint32_t seg  = (handle & 0x8000u) ? LP128_SEGMENT_SIZE : 0u;
    uint32_t word = (uint32_t)(handle & 0x7FFFu);
    return seg + word * 2u;
}

/* Convert a byte offset back to a VM object handle (inverse of
 * handle_to_byte_offset).  Used after allocating heap space to
 * produce a handle that the bytecode can store in a local or global. */
static uint16_t byte_offset_to_handle(uint32_t offset)
{
    uint16_t seg_bit = (offset >= LP128_SEGMENT_SIZE) ? 0x8000u : 0u;
    uint16_t word_addr = (uint16_t)((offset & 0xFFFFu) / 2u);
    return seg_bit | word_addr;
}

/* Read the 16-bit word at word index word_idx within an aggregate.
 * Word indices are 0-based; word 0 is typically the length or header word. */
static uint16_t agg_read_word(const lp128_vm_state *vm, uint16_t handle, int word_idx)
{
    uint32_t base = handle_to_byte_offset(handle);
    return vm_read_word(vm, base + (uint32_t)word_idx * 2u);
}

/* Write val to word index word_idx within an aggregate. */
static void agg_write_word(lp128_vm_state *vm, uint16_t handle, int word_idx, uint16_t val)
{
    uint32_t base = handle_to_byte_offset(handle);
    vm_write_word(vm, base + (uint32_t)word_idx * 2u, val);
}

/* Read a single byte from the payload region of an aggregate.
 * The payload begins at byte offset 2 (immediately after the header word).
 * byte_idx is 0-based within the payload. */
static uint8_t agg_read_payload_byte(const lp128_vm_state *vm, uint16_t handle, int byte_idx)
{
    uint32_t base = handle_to_byte_offset(handle);
    return vm_read_byte(vm, base + 2u + (uint32_t)byte_idx);
}

/* Write a single byte into the payload region of an aggregate. */
static void agg_write_payload_byte(lp128_vm_state *vm, uint16_t handle, int byte_idx, uint8_t val)
{
    uint32_t base = handle_to_byte_offset(handle);
    vm_write_byte(vm, base + 2u + (uint32_t)byte_idx, val);
}

/* Read a single byte at a raw byte offset from the start of the aggregate
 * (not offset from the payload; use this when accessing the header word
 * itself as bytes). */
static uint8_t agg_read_raw_byte(const lp128_vm_state *vm, uint16_t handle, int raw_offset)
{
    uint32_t base = handle_to_byte_offset(handle);
    return vm_read_byte(vm, base + (uint32_t)raw_offset);
}

/* Return non-zero if h is a valid non-null, non-FALSE handle. */
static int is_usable_handle(uint16_t h)
{
    return h != 0u && h != LP128_FALSE_SENTINEL;
}

/* ── Arena allocator ─────────────────────────────────────────────────────── */

/* Allocate word_count 16-bit words from the low-segment or high-segment
 * bump allocator, or reclaim from the free list.
 * Returns a VM object handle, or LP128_FALSE_SENTINEL on exhaustion. */
static uint16_t alloc_vector(lp128_vm_state *vm, uint16_t word_count)
{
    uint32_t byte_size = (uint32_t)(word_count > 0 ? word_count : 1u) * 2u;
    uint32_t offset;

    /* Check free-list first */
    if (word_count > 0 && word_count <= LP128_FREE_LIST_BUCKETS
        && vm->free_list[word_count - 1] != 0) {
        offset = vm->free_list[word_count - 1];
        vm->free_list[word_count - 1] = 0;
        vm_zero_bytes(vm, offset, byte_size);
        return byte_offset_to_handle(offset);
    }

    if (vm->next_low_arena_byte + byte_size <= LP128_SEGMENT_SIZE) {
        offset = vm->next_low_arena_byte;
        vm->next_low_arena_byte += byte_size;
        vm_zero_bytes(vm, offset, byte_size);
        return byte_offset_to_handle(offset);
    }

    if (vm->next_high_arena_byte + byte_size <= vm->tuple_stack_floor_byte) {
        offset = vm->next_high_arena_byte;
        vm->next_high_arena_byte += byte_size;
        vm_zero_bytes(vm, offset, byte_size);
        return byte_offset_to_handle(offset);
    }

    VM_DIAG("out of memory allocating %u words\n", word_count);
    return LP128_FALSE_SENTINEL;
}

/* Allocate word_count words from the tuple stack (a downward-growing
 * region at the top of the high segment reserved for short-lived tuples).
 * Returns a handle, or LP128_FALSE_SENTINEL on overflow. */
static uint16_t alloc_tuple(lp128_vm_state *vm, uint16_t word_count)
{
    uint32_t byte_size = (uint32_t)(word_count > 0 ? word_count : 1u) * 2u;
    uint32_t new_top;

    if (vm->tuple_stack_byte < byte_size) {
        VM_DIAG("tuple stack underflow\n");
        return LP128_FALSE_SENTINEL;
    }

    new_top = vm->tuple_stack_byte - byte_size;
    if (new_top < vm->tuple_stack_floor_byte) {
        VM_DIAG("tuple stack overflow\n");
        return LP128_FALSE_SENTINEL;
    }

    vm->tuple_stack_byte = new_top;
    vm_zero_bytes(vm, new_top, byte_size);
    return byte_offset_to_handle(new_top);
}

/* Return a vector allocation to the free list so it can be reused.
 * Only the first LP128_FREE_LIST_BUCKETS sizes can be reclaimed; larger
 * allocations are simply leaked (arena bump pointers never retreat). */
static void release_vector(lp128_vm_state *vm, uint16_t handle, uint16_t word_count)
{
    uint32_t offset;

    if (handle == LP128_FALSE_SENTINEL || word_count == 0) return;

    offset = handle_to_byte_offset(handle);
    if (word_count <= LP128_FREE_LIST_BUCKETS && vm->free_list[word_count - 1] == 0) {
        vm->free_list[word_count - 1] = offset;
    }
}

/* ── Globals helpers ─────────────────────────────────────────────────────── */

/* Read the module global at idx.  If idx falls within the current module's
 * own global range, that module's per-module storage is used; otherwise
 * the shared system_globals array is used. */
static uint16_t load_module_global(const lp128_vm_state *vm, uint8_t idx)
{
    if (vm->current_module_id >= 1 && vm->current_module_id <= vm->module_count) {
        uint16_t module_index = (uint16_t)(vm->current_module_id - 1u);
        if (idx < vm->module_global_counts[module_index]) {
            return lp128_vm_read_module_global(vm, vm->current_module_id, idx);
        }
    }
    return vm->system_globals[idx < LP128_SYSTEM_GLOBALS_COUNT ? idx : 0u];
}

/* Flush the logical cursor position from system globals (0xCA = row,
 * 0xC9 = column) to the host screen driver. */
static void commit_cursor(lp128_vm_state *vm)
{
    uint8_t sw = vm->host->screen_width;
    uint8_t sh = vm->host->screen_height;
    uint8_t row, col;

    if (sw == 0) sw = (uint8_t)LP128_SCREEN_WIDTH;
    if (sh == 0) sh = (uint8_t)LP128_SCREEN_HEIGHT;

    row = (vm->system_globals[0xCA] < sh) ? (uint8_t)vm->system_globals[0xCA] : (uint8_t)(sh - 1u);
    col = (vm->system_globals[0xC9] < sw) ? (uint8_t)vm->system_globals[0xC9] : (uint8_t)(sw - 1u);
    vm->cursor_row = row;
    vm->cursor_col = col;

    if (vm->host->set_cursor != NULL) {
        vm->host->set_cursor(vm->host, row, col);
    }
}

/* Write value to module global idx.  Mirrors the routing logic of
 * load_module_global.  Writing slot 0xD5 also updates the host's
 * current_text_attribute; writing slot 0xC9 (column) commits the
 * cursor position to the host screen. */
static void store_module_global(lp128_vm_state *vm, uint8_t idx, uint16_t value)
{
    if (vm->current_module_id >= 1 && vm->current_module_id <= vm->module_count) {
        uint16_t module_index = (uint16_t)(vm->current_module_id - 1u);
        if (idx < vm->module_global_counts[module_index]) {
            lp128_vm_write_module_global(vm, vm->current_module_id, idx, value);
#if !(defined(__mos__) || defined(__llvm_mos__))
            if (vm->current_module_id == 2u && (idx == 0x11u || idx == 0x12u)) {
                VM_DIAG("[lp128] PUTMG M2[%02X]=%04X pc=%04X sp=%u cs=%u\n",
                        idx, value, vm->program_counter, vm->eval_stack_top, vm->call_stack_top);
            }
#endif
            return;
        }
    }

    if (idx < LP128_SYSTEM_GLOBALS_COUNT) {
        vm->system_globals[idx] = value;
    }

    if (idx == 0xD5 && vm->host != NULL) {
        vm->host->current_text_attribute = value;
    }

    /* Writing slot 0xC9 (column) commits the cursor */
    if (idx == 0xC9) {
        commit_cursor(vm);
    }
}

/* ── Code fetch ──────────────────────────────────────────────────────────── */

/* Return the module record for the module that is currently executing,
 * or NULL if the current_module_id is out of range. */
static const lp128_module_record *current_module_rec(const lp128_vm_state *vm)
{
    if (vm->current_module_id >= 1 && (size_t)(vm->current_module_id - 1) < vm->bundle->module_count) {
        return &vm->bundle->modules[vm->current_module_id - 1];
    }
    return NULL;
}

/* Read one byte of code from mod at mod_offset, using a two-slot page
 * cache stored in cpu RAM to avoid one REU DMA per byte fetch.  Sets
 * is_halted on out-of-bounds access. */
static uint8_t read_code_byte_at(lp128_vm_state *vm, const lp128_module_record *mod, uint16_t mod_offset)
{
    uint32_t abs = mod->object_offset + mod_offset;
    uint32_t page_base;
    uint16_t page_offset;
    uint8_t current_slot;
    uint8_t other_slot;
    uint8_t load_slot;
#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.code_byte_fetches++;
#endif
    if (abs >= vm->bundle->code_pages_size) {
        VM_DIAG("code read out of bounds at module %u offset 0x%04X\n",
                mod->module_id, mod_offset);
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return 0;
    }

    page_base = abs & ~(uint32_t)(LP128_CODE_PAGE_SIZE - 1u);
    page_offset = (uint16_t)(abs - page_base);
    current_slot = vm->current_code_page_cache_slot;
    other_slot = (uint8_t)(current_slot ^ 1u);

    if (vm->code_page_cache_valid[current_slot]
        && vm->code_page_cache_base[current_slot] == page_base) {
#if LP128_ENABLE_PERF_COUNTERS
        vm->perf.code_page_cache_hits++;
#endif
        return code_page_cache_slots[current_slot][page_offset];
    }

    if (vm->code_page_cache_valid[other_slot]
        && vm->code_page_cache_base[other_slot] == page_base) {
#if LP128_ENABLE_PERF_COUNTERS
        vm->perf.code_page_cache_hits++;
#endif
        vm->current_code_page_cache_slot = other_slot;
        return code_page_cache_slots[other_slot][page_offset];
    }

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.code_page_cache_loads++;
#endif
    if (!vm->code_page_cache_valid[current_slot]) {
        load_slot = current_slot;
    } else if (!vm->code_page_cache_valid[other_slot]) {
        load_slot = other_slot;
    } else {
        load_slot = other_slot;
    }
#if defined(__mos__) || defined(__llvm_mos__)
    reu_fetch(code_page_cache_slots[load_slot],
              vm->bundle->code_pages_reu_offset + page_base,
              LP128_CODE_PAGE_SIZE);
#else
    {
        uint32_t available = vm->bundle->code_pages_size - page_base;
        if (available >= LP128_CODE_PAGE_SIZE) {
            memcpy(code_page_cache_slots[load_slot],
                   vm->bundle->code_pages + page_base,
                   LP128_CODE_PAGE_SIZE);
        } else {
            memcpy(code_page_cache_slots[load_slot],
                   vm->bundle->code_pages + page_base,
                   (size_t)available);
            memset(code_page_cache_slots[load_slot] + available,
                   0,
                   LP128_CODE_PAGE_SIZE - (size_t)available);
        }
    }
#endif
    vm->code_page_cache_valid[load_slot] = true;
    vm->code_page_cache_base[load_slot] = page_base;
    vm->current_code_page_cache_slot = load_slot;
    return code_page_cache_slots[load_slot][page_offset];
}

/* Fetch the next byte from the instruction stream, advancing program_counter. */
static uint8_t fetch_byte(lp128_vm_state *vm)
{
    const lp128_module_record *mod = current_module_rec(vm);
    uint8_t value;

    if (mod == NULL) {
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return 0;
    }

    value = read_code_byte_at(vm, mod, vm->program_counter);
    vm->program_counter++;
    return value;
}

/* Fetch the next two bytes from the instruction stream as a
 * little-endian 16-bit word, advancing program_counter by 2. */
static uint16_t fetch_le16(lp128_vm_state *vm)
{
    uint8_t lo = fetch_byte(vm);
    uint8_t hi = fetch_byte(vm);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

/* Read a compact jump target.  A non-zero selector byte is a signed
 * 8-bit displacement from instr_start + 2; a zero selector means the
 * full 16-bit absolute target address follows as a little-endian word. */
static uint16_t decode_jump_target(lp128_vm_state *vm, uint16_t instr_start)
{
    uint8_t sel = fetch_byte(vm);
    if (sel != 0) {
        int16_t disp = (int16_t)(int8_t)sel;
        return (uint16_t)(instr_start + 2u + disp);
    }
    return fetch_le16(vm);
}

/* Handle the high opcode ranges (0x60–0xFF), which encode compact
 * local-variable operations with embedded operands:
 *   0xE0–0xFF  STOREKEEP local[operand] = stack top (do not pop)
 *   0xC0–0xDF  STORE      pop and write to local[operand]
 *   0xA0–0xBF  LOAD       push local[operand]
 *   0x60–0x9F  VLOADW     push word at index [(opcode>>4)&3] of
 *                          the aggregate handle in local[opcode&0xF]
 * Returns true if the opcode was handled. */
static bool try_execute_fast_opcode(lp128_vm_state *vm, uint16_t instr_start, uint8_t opcode)
{
    uint8_t operand_u8;
    uint16_t val;

    if (opcode >= 0xE0u) {
        operand_u8 = opcode & 0x1Fu;
        val = vm_peek(vm);
        if (operand_u8 < vm->frame_local_count) {
            vm->locals[operand_u8] = val;
        }
        return true;
    }

    if (opcode >= 0xC0u) {
        operand_u8 = opcode & 0x1Fu;
        val = vm_pop(vm);
        if (operand_u8 < vm->frame_local_count) {
            vm->locals[operand_u8] = val;
        }
        return true;
    }

    if (opcode >= 0xA0u) {
        operand_u8 = opcode & 0x1Fu;
        val = (operand_u8 < vm->frame_local_count)
            ? vm->locals[operand_u8]
            : LP128_FALSE_SENTINEL;
        vm_push(vm, val);
        return true;
    }

    if (opcode >= 0x60u) {
        uint8_t word_index = (opcode >> 4) & 0x03u;
        uint8_t local_index = opcode & 0x0Fu;
        uint16_t packed_handle = (local_index < vm->frame_local_count)
            ? vm->locals[local_index]
            : LP128_FALSE_SENTINEL;

#if !(defined(__mos__) || defined(__llvm_mos__))
        if (vm->current_module_id == 2u
            && (instr_start == 0x189Fu || instr_start == 0x18A1u
                || instr_start == 0x18D1u || instr_start == 0x18D3u
                || instr_start == 0x1904u || instr_start == 0x1918u)) {
            VM_DIAG("[lp128] VREAD packed pc=%04X local=%u handle=%04X word=%u L0=%04X L1=%04X L2=%04X L3=%04X L4=%04X MG11=%04X MG12=%04X\n",
                    instr_start,
                    local_index,
                    packed_handle,
                    word_index,
                    vm->locals[0],
                    vm->locals[1],
                    vm->locals[2],
                    vm->locals[3],
                    vm->locals[4],
                    lp128_vm_read_module_global(vm, 2u, 0x11u),
                    lp128_vm_read_module_global(vm, 2u, 0x12u));
        }
#endif

        if (packed_handle == LP128_FALSE_SENTINEL) {
#if defined(__mos__) || defined(__llvm_mos__)
            if (vm->current_module_id == 2u && instr_start == 0x1904u) {
                diag_trace_module2_packed_vread(vm, instr_start, local_index, packed_handle, word_index);
            }
#endif
            VM_DIAG("VLOADW from FALSE at PC 0x%04X\n", instr_start);
            vm->is_halted = true;
            vm->halt_code = 0xFFFF;
            return true;
        }

        vm_push(vm, agg_read_word(vm, packed_handle, word_index));
        return true;
    }

    return false;
}

/* ── Host I/O ────────────────────────────────────────────────────────────── */

/* Return the effective screen width, defaulting to LP128_SCREEN_WIDTH
 * if the host has not set a width. */
static uint8_t screen_width(const lp128_vm_state *vm)
{
    uint8_t sw = vm->host->screen_width;
    return (sw == 0) ? (uint8_t)LP128_SCREEN_WIDTH : sw;
}

/* Return the effective screen height, defaulting to LP128_SCREEN_HEIGHT. */
static uint8_t screen_height(const lp128_vm_state *vm)
{
    uint8_t sh = vm->host->screen_height;
    return (sh == 0) ? (uint8_t)LP128_SCREEN_HEIGHT : sh;
}

/* Output one character at the current cursor position.  Newline/carriage
 * return advances to the next row and scrolls if at the bottom edge. */
static void print_char(lp128_vm_state *vm, uint8_t ch)
{
    DIAG_VIC(14, DIAG_SC_P);
    uint8_t sh = screen_height(vm);
    uint8_t sw = screen_width(vm);

    if (ch == '\r' || ch == '\n') {
        vm->cursor_row++;
        vm->cursor_col = 0;
        if (vm->cursor_row >= sh) {
            if (vm->host->scroll_up != NULL) {
                vm->host->scroll_up(vm->host, 0, (uint8_t)(sh - 1u), 1);
            }
            vm->cursor_row = (uint8_t)(sh - 1u);
        }
        if (vm->host->set_cursor != NULL) {
            vm->host->set_cursor(vm->host, vm->cursor_row, vm->cursor_col);
        }
        return;
    }

    if (vm->host->put_char != NULL) {
        vm->host->put_char(vm->host, ch, vm->cursor_row, vm->cursor_col);
    }
    if (vm->cursor_col + 1u < sw) {
        vm->cursor_col++;
    }
    if (vm->host->set_cursor != NULL) {
        vm->host->set_cursor(vm->host, vm->cursor_row, vm->cursor_col);
    }
}

/* Print length bytes from the payload of handle starting at start_offset,
 * one character at a time via print_char. */
static void print_vector(lp128_vm_state *vm, uint16_t handle, uint16_t length, uint16_t start_offset)
{
    uint16_t i;

    if (handle == LP128_FALSE_SENTINEL) return;

    for (i = 0; i < length; i++) {
        uint8_t ch = agg_read_payload_byte(vm, handle, (int)(start_offset + i));
        print_char(vm, ch);
    }
}

/* ── Bitfield extraction (used by SETWIN) ───────────────────────────────── */

/* Extract a bit field from word word_idx of handle.  The control_word
 * encodes: bits 15-12 = right-shift amount, bits 11-8 = field width,
 * bits 7-0 = 0-based word index within the aggregate. */
static uint16_t extract_bit_field(const lp128_vm_state *vm,
                                  uint16_t handle, uint16_t control_word)
{
    uint16_t word_idx  = (uint16_t)(control_word & 0xFFu);
    uint16_t shift     = (uint16_t)((control_word >> 12) & 0x0Fu);
    uint16_t width     = (uint16_t)((control_word >> 8) & 0x0Fu);
    uint16_t value_mask;
    uint16_t word;
    if (width == 0 || shift + width > 16) return 0;
    value_mask = (uint16_t)((1u << width) - 1u);
    word = agg_read_word(vm, handle, word_idx);
    return (uint16_t)((word >> shift) & value_mask);
}

/* ── Display descriptor helpers (SETWIN / WPRINTV) ─────────────────────── */

/* Resolve a logical column/row within a display window descriptor to
 * physical screen coordinates.  Returns false if the logical position
 * is outside the window bounds or off the physical screen. */
static bool try_resolve_display_window(
    const lp128_vm_state *vm,
    uint16_t descriptor_handle,
    uint16_t logical_column,
    uint16_t logical_row,
    uint8_t *out_row, uint8_t *out_col)
{
    uint16_t geo;
    int col, row, g_col, g_row, g_pcol, g_prow, g_cspan, g_rspan;
    int dcol, drow, pcol, prow;

    geo = agg_read_word(vm, descriptor_handle, 3);
    if (geo == LP128_FALSE_SENTINEL || geo == 0) return false;

    col     = (int16_t)logical_column;
    row     = (int16_t)logical_row;
    g_col   = (int16_t)agg_read_word(vm, geo, 0);
    g_row   = (int16_t)agg_read_word(vm, geo, 1);
    g_pcol  = (int16_t)agg_read_word(vm, geo, 2);
    g_prow  = (int16_t)agg_read_word(vm, geo, 3);
    g_cspan = agg_read_word(vm, geo, 4);
    g_rspan = agg_read_word(vm, geo, 5);

    dcol = col - g_col;
    drow = row - g_row;
    if (dcol < 0 || drow < 0 || dcol >= g_cspan || drow >= g_rspan)
        return false;

    pcol = g_pcol + dcol;
    prow = g_prow + drow;
    if (prow < 0 || prow >= (int)screen_height(vm) ||
        pcol < 0 || pcol >= (int)screen_width(vm))
        return false;

    *out_row = (uint8_t)prow;
    *out_col = (uint8_t)pcol;
    return true;
}

/* Activate a display window descriptor: move the cursor to the descriptor's
 * current logical position and update the text attribute.  Stores the active
 * descriptor in system global 0xD3.  Returns the physical column, or
 * LP128_FALSE_SENTINEL if the position is off-screen. */
static uint16_t execute_setwin(lp128_vm_state *vm, uint16_t desc)
{
    uint16_t lcol, lrow, attr;
    uint8_t hr, hc;

    if (desc == LP128_FALSE_SENTINEL || desc == 0) {
        store_module_global(vm, 0xD3, LP128_FALSE_SENTINEL);
        return LP128_FALSE_SENTINEL;
    }

    lcol = agg_read_word(vm, desc, 0);
    lrow = agg_read_word(vm, desc, 1);
    if (!try_resolve_display_window(vm, desc, lcol, lrow, &hr, &hc)) {
        store_module_global(vm, 0xD3, LP128_FALSE_SENTINEL);
        return LP128_FALSE_SENTINEL;
    }

    store_module_global(vm, 0xD3, desc);
    attr = extract_bit_field(vm, desc, UINT16_C(0x0802));
    store_module_global(vm, 0xD5, attr);
    vm->cursor_row = hr;
    vm->cursor_col = hc;
    if (vm->host->set_cursor != NULL)
        vm->host->set_cursor(vm->host, hr, hc);
    return lcol;
}

/* Print char_count characters from src_handle (starting at src_offset)
 * into the display window described by desc, clipping to the window's
 * column span.  Advances the descriptor's current column and repositions
 * the cursor after printing.  Returns the post-print cursor column, or
 * LP128_FALSE_SENTINEL on error. */
static uint16_t execute_wprintv(lp128_vm_state *vm,
    uint16_t desc, uint16_t src_handle,
    uint16_t char_count, uint16_t src_offset)
{
    uint16_t start_col, geo, src_limit, setwin_result;
    int col_origin, col_span;
    int start, end_req, end_src, end;
    int win_min, win_max;
    int start_clipped, src_start, end_clipped, count;

    if (desc == LP128_FALSE_SENTINEL || desc == 0 ||
        src_handle == LP128_FALSE_SENTINEL || src_handle == 0)
        return LP128_FALSE_SENTINEL;

    start_col = agg_read_word(vm, desc, 0);
    geo       = agg_read_word(vm, desc, 3);
    if (geo == LP128_FALSE_SENTINEL || geo == 0) return LP128_FALSE_SENTINEL;

    src_limit  = agg_read_word(vm, src_handle, 0);
    col_origin = (int16_t)agg_read_word(vm, geo, 0);
    col_span   = agg_read_word(vm, geo, 4);

    start   = (int16_t)start_col;
    end_req = start + (int)char_count;

    if ((uint32_t)src_offset + (uint32_t)char_count > (uint32_t)src_limit) {
        int avail = (int)src_limit - (int)src_offset;
        if (avail < 0) avail = 0;
        end_src = start + avail;
    } else {
        end_src = end_req;
    }
    end = (end_req < end_src) ? end_req : end_src;

    win_min = col_origin;
    win_max = col_origin + col_span;

    start_clipped = start;
    src_start     = src_offset;
    if (start_clipped < win_min) {
        int adv = win_min - start_clipped;
        src_start     += adv;
        start_clipped  = win_min;
    }
    end_clipped = (end < win_max) ? end : win_max;
    count       = end_clipped - start_clipped;
    if (count < 0)   count = 0;
    if (count > 100)  count = 100;

    /* SETWIN at clipped start column */
    agg_write_word(vm, desc, 0, (uint16_t)start_clipped);
    setwin_result = execute_setwin(vm, desc);
    if (setwin_result == LP128_FALSE_SENTINEL) {
        agg_write_word(vm, desc, 0, (uint16_t)end_req);
        return LP128_FALSE_SENTINEL;
    }

    /* Emit characters */
    if (count > 0)
        print_vector(vm, src_handle, (uint16_t)count, (uint16_t)src_start);

    /* Advance D[0] to logical post-print position + SETWIN */
    agg_write_word(vm, desc, 0, (uint16_t)end_req);
    execute_setwin(vm, desc);
    return setwin_result;
}

/* ── Call/return ─────────────────────────────────────────────────────────── */

/* Locate a procedure by offset within the given module (searches exports table).
 * Returns true if found (fills *out), false if not found. */
static bool find_export_by_offset(
    lp128_vm_state *vm,
    const lp128_module_record *mod,
    uint16_t target_offset,
    lp128_export_procedure_record *out)
{
    uint16_t i;
    const lp128_bundle *bundle = vm->bundle;
    uint16_t start = mod->export_start_index;
    uint16_t end   = start + mod->export_count;

    if (end > (uint16_t)bundle->export_count) end = (uint16_t)bundle->export_count;

    for (i = start; i < end; i++) {
        lp128_export_procedure_record export_record;
        lp128_vm_read_export(vm, i, &export_record);
        uint16_t export_start = export_record.start_offset;
        uint16_t export_code = export_record.code_offset;
        if (export_start == target_offset || export_code == target_offset) {
            *out = export_record;
            return true;
        }
    }
    return false;
}

/* Invalidate both code page cache slots, forcing the next access to
 * reload from the bundle.  Called on module switches and snapshot restores. */
static void invalidate_code_page_cache(lp128_vm_state *vm)
{
    vm->current_code_page_cache_slot = 0u;
    memset(vm->code_page_cache_valid, 0, sizeof(vm->code_page_cache_valid));
    memset(vm->code_page_cache_base, 0, sizeof(vm->code_page_cache_base));
}

#if defined(__mos__) || defined(__llvm_mos__)
/* Return the REU address where the saved locals for call-stack frame at
 * depth are stored.  Locals are spilled to REU on every call so that the
 * small cpu-RAM locals[] array can be reused for each new frame. */
static reu_addr_t call_stack_locals_reu_addr(const lp128_vm_state *vm, uint16_t depth)
{
    return vm->call_stack_locals_reu_offset + (reu_addr_t)depth * sizeof(vm->locals);
}
#endif

/* Find the snapshot slot whose token matches token, or NULL. */
static lp128_jump_snapshot_slot *find_jump_snapshot_slot(lp128_vm_state *vm, uint16_t token)
{
    uint16_t index;

    for (index = 0; index < LP128_JUMP_SNAPSHOT_CAP; index++) {
        lp128_jump_snapshot_slot *slot = &vm->jump_snapshot_slots[index];
        if (slot->valid && slot->token == token) {
            return slot;
        }
    }

    return NULL;
}

/* Find an unused snapshot slot, or NULL if all LP128_JUMP_SNAPSHOT_CAP
 * slots are occupied.  Called when creating a new SETJMP snapshot. */
static lp128_jump_snapshot_slot *find_free_jump_snapshot_slot(lp128_vm_state *vm)
{
    uint16_t index;

    for (index = 0; index < LP128_JUMP_SNAPSHOT_CAP; index++) {
        lp128_jump_snapshot_slot *slot = &vm->jump_snapshot_slots[index];
        if (!slot->valid) {
            return slot;
        }
    }

    return NULL;
}

/* Save the current full execution state as a non-local jump snapshot.
 * protected_pc is the entry point of the body under protection;
 * landing_pc is where execution resumes if the snapshot is used as a
 * LONGJMP target.  The allocated token is returned in *out_token and
 * must be stored somewhere accessible to the LONGJMP opcode. */
static bool create_jump_snapshot(lp128_vm_state *vm,
                                 uint16_t protected_pc,
                                 uint16_t landing_pc,
                                 uint16_t *out_token)
{
    lp128_jump_snapshot_slot *slot;
    lp128_jump_snapshot_header header;

    if (out_token == NULL) {
        return false;
    }

    slot = find_free_jump_snapshot_slot(vm);
    if (slot == NULL) {
        VM_DIAG("jump snapshot overflow\n");
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    do {
        if (vm->next_jump_token == LP128_FALSE_SENTINEL) {
            vm->next_jump_token++;
        }
        if (find_jump_snapshot_slot(vm, vm->next_jump_token) == NULL) {
            break;
        }
        vm->next_jump_token++;
    } while (true);

    memset(&header, 0, sizeof(header));
    header.module_id            = vm->current_module_id;
    header.long_jump_pc         = protected_pc;
    header.long_jump_return_pc  = landing_pc;
    header.frame_upper_bound    = vm->frame_upper_bound;
    header.frame_module_id      = vm->frame_module_id;
    header.frame_proc_index     = vm->frame_proc_index;
    header.frame_start_offset   = vm->frame_start_offset;
    header.frame_code_offset    = vm->frame_code_offset;
    header.frame_local_count    = vm->frame_local_count;
    header.eval_stack_top       = vm->eval_stack_top;
    header.call_stack_top       = vm->call_stack_top;
    header.tuple_stack_byte     = vm->tuple_stack_byte;

#if defined(__mos__) || defined(__llvm_mos__)
    {
        reu_addr_t cursor = slot->reu_offset;

        reu_store_safe(&header, cursor, (uint16_t)sizeof(header));
        cursor += (reu_addr_t)sizeof(header);
        reu_store_safe(vm->locals, cursor, (uint16_t)sizeof(vm->locals));
        cursor += (reu_addr_t)sizeof(vm->locals);
        reu_store_safe(vm->eval_stack, cursor, (uint16_t)sizeof(vm->eval_stack));
        cursor += (reu_addr_t)sizeof(vm->eval_stack);
        reu_store_safe(vm->call_stack, cursor, (uint16_t)sizeof(vm->call_stack));
    }
#else
    slot->header = header;
    memcpy(slot->locals, vm->locals, sizeof(slot->locals));
    memcpy(slot->eval_stack, vm->eval_stack, sizeof(slot->eval_stack));
    memcpy(slot->call_stack, vm->call_stack, sizeof(slot->call_stack));
#endif

    slot->valid = true;
    slot->token = vm->next_jump_token;
    vm->next_jump_token++;
    *out_token = slot->token;
    return true;
}

/* Restore execution state from the snapshot identified by token, then
 * free the slot.  If use_return_target is true, execution resumes at
 * the snapshot's landing_pc and return_value is pushed onto the eval
 * stack; otherwise execution resumes at the original protected_pc. */
static bool restore_jump_snapshot(lp128_vm_state *vm, uint16_t token, bool use_return_target, uint16_t return_value)
{
    lp128_jump_snapshot_slot *slot = find_jump_snapshot_slot(vm, token);
    lp128_jump_snapshot_header header;

    if (slot == NULL) {
        VM_DIAG("unknown jump token %04x\n", token);
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

#if defined(__mos__) || defined(__llvm_mos__)
    {
        reu_addr_t cursor = slot->reu_offset;

        reu_fetch_safe(&header, cursor, (uint16_t)sizeof(header));
        cursor += (reu_addr_t)sizeof(header);
        reu_fetch_safe(vm->locals, cursor, (uint16_t)sizeof(vm->locals));
        cursor += (reu_addr_t)sizeof(vm->locals);
        reu_fetch_safe(vm->eval_stack, cursor, (uint16_t)sizeof(vm->eval_stack));
        cursor += (reu_addr_t)sizeof(vm->eval_stack);
        reu_fetch_safe(vm->call_stack, cursor, (uint16_t)sizeof(vm->call_stack));
    }
#else
    header = slot->header;
    memcpy(vm->locals, slot->locals, sizeof(vm->locals));
    memcpy(vm->eval_stack, slot->eval_stack, sizeof(vm->eval_stack));
    memcpy(vm->call_stack, slot->call_stack, sizeof(vm->call_stack));
#endif

    if (header.frame_local_count > LP128_MAX_LOCALS
        || header.eval_stack_top > LP128_EVAL_STACK_CAP
        || header.call_stack_top > LP128_CALL_STACK_CAP) {
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    vm->current_module_id   = header.module_id;
    vm->program_counter     = use_return_target ? header.long_jump_return_pc : header.long_jump_pc;
    vm->frame_upper_bound   = header.frame_upper_bound;
    vm->frame_module_id     = header.frame_module_id;
    vm->frame_proc_index    = header.frame_proc_index;
    vm->frame_start_offset  = header.frame_start_offset;
    vm->frame_code_offset   = header.frame_code_offset;
    vm->frame_local_count   = header.frame_local_count;
    vm->eval_stack_top      = header.eval_stack_top;
    vm->call_stack_top      = header.call_stack_top;
    vm->tuple_stack_byte    = header.tuple_stack_byte;

    invalidate_code_page_cache(vm);
    slot->valid = false;

    if (use_return_target) {
        vm_push(vm, return_value);
    }

    return true;
}

/* Parse a 1-byte private procedure header from code_pages. */
typedef struct {
    uint16_t local_count;
    uint16_t header_size;          /* bytes between start_offset and code_offset */
    uint16_t initializer_count;
    uint16_t start_offset;
    uint16_t code_offset;
    uint16_t upper_bound;
    /* Initializers for private procs are stored inline; read them here. */
    uint8_t  init_local[4];
    uint16_t init_value[4];
} private_proc_info;

/* Find the smallest export start_offset in mod that is strictly greater
 * than start_offset, to establish an upper bound for a private (non-exported)
 * procedure.  Returns mod->byte_length if no higher export exists. */
static uint16_t compute_upper_bound_for_offset(
    lp128_vm_state *vm,
    const lp128_module_record *mod,
    uint16_t start_offset)
{
    uint16_t best = (uint16_t)mod->byte_length;
    uint16_t i;
    const lp128_bundle *bundle = vm->bundle;
    uint16_t bstart = mod->export_start_index;
    uint16_t bend   = bstart + mod->export_count;

    if (bend > (uint16_t)bundle->export_count) bend = (uint16_t)bundle->export_count;

    for (i = bstart; i < bend; i++) {
        lp128_export_procedure_record export_record;
        lp128_vm_read_export(vm, i, &export_record);
        uint16_t export_start = export_record.start_offset;
        if (export_start > start_offset && export_start < best) best = export_start;
    }
    return best;
}

/* Parse the compact procedure header that precedes private (non-exported)
 * procedure code.  The first byte encodes local_count (bits 6-0) and an
 * extended-header flag (bit 7).  If the flag is set, an additional byte
 * gives the initializer count followed by (local_index, value_lo, value_hi)
 * triples.  Fills *out and returns true. */
static bool parse_private_header(
    lp128_vm_state *vm,
    const lp128_module_record *mod,
    uint16_t start_offset,
    private_proc_info *out)
{
    uint8_t  header_byte = read_code_byte_at(vm, mod, start_offset);
    uint32_t cursor      = (uint32_t)start_offset + 1u;

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.private_header_parses++;
#endif

    out->local_count       = header_byte & 0x7Fu;
    out->start_offset      = start_offset;
    out->initializer_count = 0;

    if (header_byte & 0x80u) {
        /* Extended: next byte gives initializer count, then pairs (index, value). */
        uint8_t ic = read_code_byte_at(vm, mod, (uint16_t)cursor++);
        uint8_t ii;

        out->initializer_count = ic > 4u ? 4u : ic;   /* cap at our static buffer */
        for (ii = 0; ii < ic; ii++) {
            uint8_t  li  = read_code_byte_at(vm, mod, (uint16_t)cursor++);
            uint8_t  vlo = read_code_byte_at(vm, mod, (uint16_t)cursor++);
            uint8_t  vhi = read_code_byte_at(vm, mod, (uint16_t)cursor++);
            if (ii < 4u) {
                out->init_local[ii] = li;
                out->init_value[ii] = (uint16_t)(vlo | ((uint16_t)vhi << 8));
            }
        }
    }

    out->header_size  = (uint16_t)(cursor - start_offset);
    out->code_offset  = (uint16_t)cursor;
    out->upper_bound  = compute_upper_bound_for_offset(vm, mod, start_offset);
    return true;
}

/* Push a new call frame and begin executing a procedure.
 * Arguments (argc of them) are popped from the eval stack and placed
 * into the first locals of the new frame.  Locals are initialised from
 * the bundle's initialiser table (if init_start != 0xFFFF) or from the
 * inline private-procedure initialisers.
 *
 * On C128: detects entry into Cornerstone's top-level exit handler
 * (module 4, offset 0x4620) and treats it as a clean program halt,
 * storing the return address as the halt code. */
static bool enter_procedure(
    lp128_vm_state *vm,
    uint16_t target_module_id,
    uint16_t proc_start_offset,
    uint16_t proc_code_offset,
    uint16_t proc_upper_bound,
    uint16_t proc_index,
    uint16_t local_count,
    uint16_t init_start,  /* index into bundle->initializers, or 0xFFFF for private */
    uint16_t init_count,
    const uint8_t *priv_init_local,    /* used when init_start==0xFFFF */
    const uint16_t *priv_init_value,
    uint16_t return_pc,
    uint8_t argc)
{
    lp128_continuation *cont;
    uint16_t args[LP128_MAX_LOCALS];
    uint16_t copy_count;
    uint8_t  ai;

    if (vm->call_stack_top >= LP128_CALL_STACK_CAP) {
        VM_DIAG("call stack overflow\n");
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    /* Pop arguments from eval stack (first arg = oldest, i.e. deepest) */
    if (argc > LP128_MAX_LOCALS) argc = LP128_MAX_LOCALS;
    for (ai = argc; ai > 0; ai--) {
        args[ai - 1] = vm_pop(vm);
    }

    /* Save current frame */
    cont = &vm->call_stack[vm->call_stack_top];
    cont->module_id             = vm->current_module_id;
    cont->proc_index            = vm->frame_proc_index;
    cont->proc_start_offset     = vm->frame_start_offset;
    cont->return_pc             = return_pc;
    cont->frame_upper_bound     = vm->frame_upper_bound;
    cont->saved_local_count     = vm->frame_local_count;
    cont->saved_eval_stack_depth = vm->eval_stack_top;
    if (vm->frame_local_count > 0) {
#if defined(__mos__) || defined(__llvm_mos__)
        reu_store_safe(vm->locals,
            call_stack_locals_reu_addr(vm, vm->call_stack_top),
            (uint16_t)sizeof(vm->locals));
#else
        memcpy(cont->saved_locals, vm->locals,
               (size_t)vm->frame_local_count * sizeof(uint16_t));
#endif
    }

#if defined(__mos__) || defined(__llvm_mos__)
    if (target_module_id == 4u && proc_start_offset == 0x4620u) {
        vm->is_halted = true;
        vm->halt_code = return_pc;
        return false;
    }
#endif

    vm->call_stack_top++;

    /* Initialise new locals */
    if (local_count > LP128_MAX_LOCALS) local_count = LP128_MAX_LOCALS;
    {
        uint16_t li;
        for (li = 0; li < local_count; li++) {
            vm->locals[li] = LP128_FALSE_SENTINEL;
        }
    }

    /* Apply initialisers */
    if (init_start != 0xFFFFu && init_count > 0) {
        uint16_t ii;
        for (ii = 0; ii < init_count; ii++) {
            lp128_initializer_record init_rec;
            lp128_vm_read_initializer(vm, (size_t)(init_start + ii), &init_rec);
            if (init_rec.local_index < local_count) {
                vm->locals[init_rec.local_index] = init_rec.value;
            }
        }
    } else if (priv_init_local != NULL && priv_init_value != NULL) {
        uint16_t ii;
        for (ii = 0; ii < init_count; ii++) {
            if (priv_init_local[ii] < local_count) {
                vm->locals[priv_init_local[ii]] = priv_init_value[ii];
            }
        }
    }

    /* Copy arguments into first locals */
    copy_count = argc;
    if (copy_count > local_count) copy_count = local_count;
    for (ai = 0; ai < copy_count; ai++) {
        vm->locals[ai] = args[ai];
    }

    /* Switch to new frame */
    vm->current_module_id    = target_module_id;
    vm->program_counter      = proc_code_offset;
    vm->frame_module_id      = target_module_id;
    vm->frame_proc_index     = proc_index;
    vm->frame_start_offset   = proc_start_offset;
    vm->frame_code_offset    = proc_code_offset;
    vm->frame_local_count    = local_count;
    vm->frame_upper_bound    = proc_upper_bound;

    return true;
}

/* Locate a procedure by offset within the current module and call
 * enter_procedure.  The target may be an exported or private procedure;
 * the function searches the export table first and falls back to parsing
 * a private procedure header at target_offset. */
static bool enter_near_call(lp128_vm_state *vm, uint16_t target_offset, uint8_t argc, uint16_t return_pc)
{
    const lp128_module_record           *mod = current_module_rec(vm);
    lp128_export_procedure_record        ep;

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.near_calls++;
#endif

    if (mod == NULL) {
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    if (find_export_by_offset(vm, mod, target_offset, &ep)) {
        return enter_procedure(vm,
            vm->current_module_id,
            ep.start_offset,
            ep.code_offset,
            ep.upper_bound,
            ep.exported_procedure_index,
            ep.local_count,
            ep.initializer_start_index,
            ep.initializer_count,
            NULL, NULL,
            return_pc, argc);
    } else {
        private_proc_info ppi;
        if (!parse_private_header(vm, mod, target_offset, &ppi)) return false;
        return enter_procedure(vm,
            vm->current_module_id,
            ppi.start_offset,
            ppi.code_offset,
            ppi.upper_bound,
            0xFFFFu,
            ppi.local_count,
            0xFFFFu,
            ppi.initializer_count,
            ppi.init_local,
            ppi.init_value,
            return_pc, argc);
    }
}

/* Enter an exported procedure in another module using a 16-bit selector.
 * The high byte of selector is the 1-based module ID; the low byte is the
 * 0-based index into that module's export table. */
static bool enter_far_call(lp128_vm_state *vm, uint16_t selector, uint8_t argc, uint16_t return_pc)
{
    DIAG_VIC(14, DIAG_SC_F);
    uint16_t target_module_id = selector >> 8;
    uint16_t proc_index       = selector & 0xFFu;
    const lp128_module_record           *tmod;
    lp128_export_procedure_record        ep;
    uint16_t mi;

#if LP128_ENABLE_PERF_COUNTERS
    vm->perf.far_calls++;
#endif

    if (target_module_id < 1 || (size_t)(target_module_id - 1) >= vm->bundle->module_count) {
        VM_DIAG("far call to invalid module %u\n", target_module_id);
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    tmod = &vm->bundle->modules[target_module_id - 1];
    mi   = tmod->export_start_index + proc_index;

    if (mi >= (uint16_t)vm->bundle->export_count) {
        VM_DIAG("far call to invalid proc %u in module %u\n", proc_index, target_module_id);
        vm->is_halted = true;
        vm->halt_code = 0xFFFF;
        return false;
    }

    lp128_vm_read_export(vm, mi, &ep);

    return enter_procedure(vm,
        target_module_id,
        ep.start_offset,
        ep.code_offset,
        ep.upper_bound,
        ep.exported_procedure_index,
        ep.local_count,
        ep.initializer_start_index,
        ep.initializer_count,
        NULL, NULL,
        return_pc, argc);
}

/* Return from the current procedure, restoring the caller's frame.
 * result_count values from results[] are pushed onto the eval stack for
 * the caller.  If the call stack is empty, sets is_halted and uses the
 * first result (or 0) as the halt code.
 *
 * On C128: detects a return into Cornerstone's exit-dispatch point
 * (module 4, PC 0x45EB) and treats it as a top-level halt, using the
 * value in local[4] as the halt code. */
static void do_return(lp128_vm_state *vm, uint16_t *results, uint16_t result_count)
{
    lp128_continuation *cont;
    uint16_t i;

    vm->last_return_word_count = result_count;

    if (vm->call_stack_top == 0) {
        vm->is_halted = true;
        vm->halt_code = (result_count > 0) ? results[0] : 0;
        return;
    }

    vm->call_stack_top--;
    cont = &vm->call_stack[vm->call_stack_top];

    vm->current_module_id    = cont->module_id;
    vm->program_counter      = cont->return_pc;
    vm->frame_module_id      = cont->module_id;
    vm->frame_proc_index     = cont->proc_index;
    vm->frame_start_offset   = cont->proc_start_offset;
    vm->frame_upper_bound    = cont->frame_upper_bound;
    vm->frame_local_count    = cont->saved_local_count;
    vm->eval_stack_top       = cont->saved_eval_stack_depth;

    if (cont->saved_local_count > 0) {
#if defined(__mos__) || defined(__llvm_mos__)
        reu_fetch_safe(vm->locals,
            call_stack_locals_reu_addr(vm, vm->call_stack_top),
            (uint16_t)sizeof(vm->locals));
#else
        memcpy(vm->locals, cont->saved_locals,
               (size_t)cont->saved_local_count * sizeof(uint16_t));
#endif
    }

#if defined(__mos__) || defined(__llvm_mos__)
    if (vm->current_module_id == 4u && vm->program_counter == 0x45EBu) {
        vm->is_halted = true;
        vm->halt_code = vm->locals[4];
        return;
    }
#endif

    /* Restore frame_code_offset from the exports table if possible */
    {
        const lp128_module_record *mod = current_module_rec(vm);
        if (mod != NULL) {
            lp128_export_procedure_record ep;
            if (find_export_by_offset(vm, mod, cont->proc_start_offset, &ep)) {
                vm->frame_code_offset = ep.code_offset;
            } else {
                vm->frame_code_offset = cont->proc_start_offset;
            }
        }
    }

    for (i = 0; i < result_count; i++) {
        vm_push(vm, results[i]);
    }
}

/* ── DISP (cursor movement / erasure, EXT 0x12) ─────────────────────────── */

/* Execute a DISP sub-operation: move the cursor one step in the given
 * direction, or erase to end of line/screen. */
static void handle_disp(lp128_vm_state *vm, uint8_t subop)
{
    DIAG_VIC(14, DIAG_SC_D);
    uint8_t sw = screen_width(vm);
    uint8_t sh = screen_height(vm);
    uint8_t row = vm->cursor_row;
    uint8_t col = vm->cursor_col;

    switch (subop) {
    case 0x00: if (col + 1u < sw) col++; break;              /* cursor right */
    case 0x01: if (col > 0) col--; break;                    /* cursor left  */
    case 0x02: if (row + 1u < sh) row++; break;              /* cursor down  */
    case 0x03: if (row > 0) row--; break;                    /* cursor up    */
    case 0x04: /* erase to end of line */
        if (vm->host->erase_to_eol != NULL) vm->host->erase_to_eol(vm->host, row, col);
        break;
    case 0x05: /* erase to end of screen */
        if (vm->host->erase_to_eol != NULL) vm->host->erase_to_eol(vm->host, row, col);
        if (row + 1u < sh && vm->host->clear_rows != NULL)
            vm->host->clear_rows(vm->host, (uint8_t)(row + 1u), (uint8_t)(sh - 1u));
        break;
    default:
        break;
    }

    vm->cursor_row = row;
    vm->cursor_col = col;
    if (vm->host->set_cursor != NULL) vm->host->set_cursor(vm->host, row, col);
}

/* ── Main dispatch loop ──────────────────────────────────────────────────── */

void lp128_vm_run(lp128_vm_state *vm)
{
    DIAG_VIC(0, DIAG_SC_R);   /* R = entered vm_run */

    while (!vm->is_halted) {
        uint16_t instr_start = vm->program_counter;
        uint8_t  opcode      = fetch_byte(vm);
        uint16_t val, val2;
        int16_t  sval, sval2;
        uint8_t  operand_u8;
        uint16_t operand_u16;
        uint16_t target;

        /* Write last opcode + PC to VIC positions 5-12 for crash diagnosis */
        DIAG_VIC_HEX8(5, opcode);
        DIAG_VIC_HEX16(8, instr_start);

        /* ── Fast opcode ranges ───────────────────────────────────────── */
        if (try_execute_fast_opcode(vm, instr_start, opcode)) {
            continue;
        }

        /* ── Switched opcodes ─────────────────────────────────────────── */
        switch (opcode) {

        /* ── Arithmetic ────────────────────── */
        case 0x01: case 0x53:
            val2 = vm_pop(vm); val = vm_pop(vm);
            vm_push(vm, (uint16_t)(val + val2));
            break;
        case 0x02:
            val2 = vm_pop(vm); val = vm_pop(vm);
            vm_push(vm, (uint16_t)(val - val2));
            break;
        case 0x03:
            sval2 = (int16_t)vm_pop(vm); sval = (int16_t)vm_pop(vm);
            vm_push(vm, (uint16_t)(sval * sval2));
            break;
        case 0x04:
            sval2 = (int16_t)vm_pop(vm); sval = (int16_t)vm_pop(vm);
            vm_push(vm, sval2 == 0 ? 0u : (uint16_t)(sval / sval2));
            break;
        case 0x05:
            sval2 = (int16_t)vm_pop(vm); sval = (int16_t)vm_pop(vm);
            vm_push(vm, sval2 == 0 ? 0u : (uint16_t)(sval % sval2));
            break;
        case 0x06:
            sval = (int16_t)vm_pop(vm);
            vm_push(vm, (uint16_t)(-sval));
            break;
        case 0x07:
            sval2 = (int16_t)vm_pop(vm); sval = (int16_t)vm_pop(vm);
            vm_push(vm, sval2 >= 0
                ? (uint16_t)((uint16_t)sval << sval2)
                : (uint16_t)((uint16_t)sval >> (-sval2)));
            break;

        /* ── Inc/Dec local ─────────────────── */
        case 0x08:
            operand_u8 = fetch_byte(vm);
            if (operand_u8 < vm->frame_local_count) {
                val = (uint16_t)(vm->locals[operand_u8] + 1u);
                vm->locals[operand_u8] = val;
                vm_push(vm, val);
            }
            break;
        case 0x0B:
            operand_u8 = fetch_byte(vm);
            if (operand_u8 < vm->frame_local_count) {
                val = (uint16_t)(vm->locals[operand_u8] - 1u);
                vm->locals[operand_u8] = val;
                vm_push(vm, val);
            }
            break;

        /* ── Small constants ───────────────── */
        case 0x09: vm_push(vm, 8);           break;
        case 0x0A: vm_push(vm, 4);           break;
        case 0x0C: vm_push(vm, 0xFFFFu);     break;
        case 0x0D: vm_push(vm, 3);           break;
        case 0x24: vm_push(vm, 2);           break;
        case 0x27: vm_push(vm, LP128_FALSE_SENTINEL); break;
        case 0x28: vm_push(vm, 0);           break;
        case 0x2A: vm_push(vm, (uint16_t)(int16_t)-8); break;
        case 0x2B: vm_push(vm, 5);           break;
        case 0x2C: vm_push(vm, 1);           break;
        case 0x2E: vm_push(vm, 0x00FFu);     break;
        case 0x4C: vm_push(vm, 6);           break;
        case 0x4F: vm_push(vm, 7);           break;

        /* ── Bitwise ───────────────────────── */
        case 0x0E:
            val2 = vm_pop(vm); val = vm_pop(vm);
            vm_push(vm, val & val2);
            break;
        case 0x0F:
            val2 = vm_pop(vm); val = vm_pop(vm);
            vm_push(vm, val | val2);
            break;
        case 0x10:
            sval2 = (int16_t)vm_pop(vm); val = vm_pop(vm);
            vm_push(vm, sval2 >= 0
                ? (uint16_t)(val << sval2)
                : (uint16_t)(val >> (-sval2)));
            break;

        /* ── Vector allocation ─────────────── */
        case 0x11: { /* VALLOC */
            uint16_t wc = vm_pop(vm);
            uint16_t h  = alloc_vector(vm, wc);
            if (h == LP128_FALSE_SENTINEL) { vm->is_halted = true; vm->halt_code = 0xFFFF; }
            vm_push(vm, h);
            break;
        }
        case 0x12: { /* VALLOC+INIT (words from stack) */
            uint16_t wc = vm_pop(vm);
            uint16_t h  = alloc_vector(vm, wc);
            int      vi;
            if (h == LP128_FALSE_SENTINEL) { vm->is_halted = true; vm->halt_code = 0xFFFF; vm_push(vm, h); break; }
            for (vi = (int)wc - 1; vi >= 0; vi--) {
                agg_write_word(vm, h, vi, vm_pop(vm));
            }
            vm_push(vm, h);
            break;
        }
        case 0x13: { /* VFREE */
            uint16_t wc = vm_pop(vm);
            uint16_t h  = vm_pop(vm);
            release_vector(vm, h, wc);
            break;
        }
        case 0x14: { /* PVALLOC */
            uint16_t wc = vm_pop(vm);
            uint16_t h  = alloc_tuple(vm, wc);
            if (h == LP128_FALSE_SENTINEL) { vm->is_halted = true; vm->halt_code = 0xFFFF; }
            vm_push(vm, h);
            break;
        }
        case 0x15: { /* PVALLOC+INIT */
            uint16_t wc = vm_pop(vm);
            uint16_t h  = alloc_tuple(vm, wc);
            int      vi;
            if (h == LP128_FALSE_SENTINEL) { vm->is_halted = true; vm->halt_code = 0xFFFF; vm_push(vm, h); break; }
            for (vi = (int)wc - 1; vi >= 0; vi--) {
                agg_write_word(vm, h, vi, vm_pop(vm));
            }
            vm_push(vm, h);
            break;
        }

        /* ── Dynamic vector element access ─── */
        case 0x16: { /* VLOADW dynamic */
            uint16_t idx = vm_pop(vm);
            uint16_t h   = vm_pop(vm);
            int resolved = (int)idx;
            if (is_usable_handle(h) && idx > 0) resolved = (int)(idx - 1);
            if (h == LP128_FALSE_SENTINEL) {
                VM_DIAG("VLOADW from FALSE at PC 0x%04X\n", instr_start);
                vm->is_halted = true; vm->halt_code = 0xFFFF;
            } else {
                vm_push(vm, agg_read_word(vm, h, resolved));
            }
            break;
        }
        case 0x17: { /* VLOADB dynamic */
            int16_t idx  = (int16_t)vm_pop(vm);
            uint16_t h   = vm_pop(vm);
            if (h == LP128_FALSE_SENTINEL) {
                VM_DIAG("VLOADB from FALSE at PC 0x%04X\n", instr_start);
                vm->is_halted = true; vm->halt_code = 0xFFFF;
            } else {
                vm_push(vm, agg_read_payload_byte(vm, h, idx - 1));
            }
            break;
        }
        case 0x18: /* VLOADW_ (immediate word index) */
            operand_u8 = fetch_byte(vm);
            {
                uint16_t h = vm_pop(vm);
                if (h == LP128_FALSE_SENTINEL) {
                    VM_DIAG("VLOADW_ from FALSE at PC 0x%04X\n", instr_start);
                    vm->is_halted = true; vm->halt_code = 0xFFFF;
                } else {
                    vm_push(vm, agg_read_word(vm, h, operand_u8));
                }
            }
            break;
        case 0x19: /* VLOADB_ (immediate byte index) */
            operand_u8 = fetch_byte(vm);
            {
                uint16_t h = vm_pop(vm);
                if (h == LP128_FALSE_SENTINEL) {
                    VM_DIAG("VLOADB_ from FALSE at PC 0x%04X\n", instr_start);
                    vm->is_halted = true; vm->halt_code = 0xFFFF;
                } else {
                    vm_push(vm, agg_read_payload_byte(vm, h, operand_u8));
                }
            }
            break;
        case 0x1A: { /* VPUTW dynamic */
            uint16_t wval = vm_pop(vm);
            uint16_t idx  = vm_pop(vm);
            uint16_t h    = vm_pop(vm);
            if (h != LP128_FALSE_SENTINEL) {
                int resolved = (int)idx;
                if (is_usable_handle(h) && idx > 0) resolved = (int)(idx - 1);
                agg_write_word(vm, h, resolved, wval);
            }
            break;
        }
        case 0x1B: { /* VPUTB dynamic */
            uint16_t bval = vm_pop(vm);
            int16_t  idx  = (int16_t)vm_pop(vm);
            uint16_t h    = vm_pop(vm);
            if (h != LP128_FALSE_SENTINEL) {
                agg_write_payload_byte(vm, h, idx - 1, (uint8_t)(bval & 0xFFu));
            }
            break;
        }
        case 0x1C: /* VPUTW_ (immediate word index) */
            operand_u8 = fetch_byte(vm);
            {
                uint16_t wval = vm_pop(vm);
                uint16_t h    = vm_pop(vm);
                if (h != LP128_FALSE_SENTINEL) {
                    agg_write_word(vm, h, operand_u8, wval);
                }
            }
            break;
        case 0x1D: /* VPUTB_ (immediate byte index, raw) */
            operand_u8 = fetch_byte(vm);
            {
                uint16_t bval = vm_pop(vm);
                uint16_t h    = vm_pop(vm);
                if (h != LP128_FALSE_SENTINEL) {
                    uint32_t base = handle_to_byte_offset(h);
                    vm_write_byte(vm, base + 2u + operand_u8, (uint8_t)(bval & 0xFFu));
                }
            }
            break;
        case 0x1E: { /* VFILL words */
            uint16_t fill = vm_pop(vm);
            uint16_t count = vm_pop(vm);
            uint16_t h = vm_pop(vm);
            uint16_t vi;
            for (vi = 0; vi < count; vi++) agg_write_word(vm, h, vi, fill);
            vm_push(vm, h);
            break;
        }
        case 0x1F: { /* VFILL bytes */
            uint16_t fill = vm_pop(vm);
            uint16_t count = vm_pop(vm);
            uint16_t h = vm_pop(vm);
            uint16_t vi;
            for (vi = 0; vi < count; vi++) agg_write_payload_byte(vm, h, vi, (uint8_t)(fill & 0xFFu));
            vm_push(vm, h);
            break;
        }

        /* ── Memory copy (words) ───────────── */
        case 0x20: { /* VECCPYW */
            uint16_t dst = vm_pop(vm);
            uint16_t count = vm_pop(vm);
            uint16_t src = vm_pop(vm);
            vm_move_bytes(vm, handle_to_byte_offset(dst), handle_to_byte_offset(src), (uint32_t)count * 2u);
            vm_push(vm, dst);
            break;
        }
        /* ── Memory copy (bytes with offset) ─ */
        case 0x21: { /* VECCPYB */
            int16_t  dst_off = (int16_t)vm_pop(vm);
            int16_t  src_off = (int16_t)vm_pop(vm);
            uint16_t dst = vm_pop(vm);
            uint16_t count = vm_pop(vm);
            uint16_t src = vm_pop(vm);
            vm_move_bytes(vm,
                handle_to_byte_offset(dst) + 2u + (uint32_t)dst_off,
                handle_to_byte_offset(src) + 2u + (uint32_t)src_off,
                count);
            vm_push(vm, dst);
            break;
        }

        /* ── Global loads ──────────────────── */
        case 0x22: /* LOADG program global */
            operand_u8 = fetch_byte(vm);
            vm_push(vm, (operand_u8 < vm->program_global_count)
                ? vm->program_globals[operand_u8]
                : LP128_FALSE_SENTINEL);
            break;
        case 0x23: /* LOADG module global */
            operand_u8 = fetch_byte(vm);
            val = load_module_global(vm, operand_u8);
            vm_push(vm, val);
            break;

        /* ── Constant pushes ───────────────── */
        case 0x25:
            operand_u16 = fetch_le16(vm);
            vm_push(vm, operand_u16);
            break;
        case 0x26:
            operand_u8 = fetch_byte(vm);
            vm_push(vm, (uint16_t)operand_u8);
            break;

        /* ── Stack ops ─────────────────────── */
        case 0x29:
            vm_push(vm, vm_peek(vm));
            break;
        case 0x2F:
            vm_pop(vm);
            break;

        /* ── Store module global ───────────── */
        case 0x2D:
            operand_u8 = fetch_byte(vm);
            val = vm_pop(vm);
            store_module_global(vm, operand_u8, val);
            break;


        /* ── Jump ──────────────────────────── */
        case 0x30: /* JUMP unconditional */
            target = decode_jump_target(vm, instr_start);
            vm->program_counter = target;
            break;
        case 0x31: /* JUMPZ */
            target = decode_jump_target(vm, instr_start);
            val = vm_pop(vm);
            if (val == 0) vm->program_counter = target;
            break;
        case 0x32: /* JUMPNZ */
            target = decode_jump_target(vm, instr_start);
            val = vm_pop(vm);
            if (val != 0) vm->program_counter = target;
            break;
        case 0x33: /* JUMPF */
            target = decode_jump_target(vm, instr_start);
            val = vm_pop(vm);
            if (val == LP128_FALSE_SENTINEL) vm->program_counter = target;
            break;
        case 0x34: /* JUMPNF */
            target = decode_jump_target(vm, instr_start);
            val = vm_pop(vm);
            if (val != LP128_FALSE_SENTINEL) vm->program_counter = target;
            break;
        case 0x35: /* JUMPP */
            target = decode_jump_target(vm, instr_start);
            sval = (int16_t)vm_pop(vm);
            if (sval > 0) vm->program_counter = target;
            break;
        case 0x36: /* JUMPNP */
            target = decode_jump_target(vm, instr_start);
            sval = (int16_t)vm_pop(vm);
            if (sval <= 0) vm->program_counter = target;
            break;
        case 0x37: /* JUMPN */
            target = decode_jump_target(vm, instr_start);
            sval = (int16_t)vm_pop(vm);
            if (sval < 0) vm->program_counter = target;
            break;
        case 0x38: /* JUMPNN */
            target = decode_jump_target(vm, instr_start);
            sval = (int16_t)vm_pop(vm);
            if (sval >= 0) vm->program_counter = target;
            break;
        case 0x39: /* JUMPLT (top < second) */
            target = decode_jump_target(vm, instr_start);
            sval  = (int16_t)vm_pop(vm);
            sval2 = (int16_t)vm_pop(vm);
            if (sval < sval2) vm->program_counter = target;
            break;
        case 0x3A: /* JUMPLE */
            target = decode_jump_target(vm, instr_start);
            sval  = (int16_t)vm_pop(vm);
            sval2 = (int16_t)vm_pop(vm);
            if (sval <= sval2) vm->program_counter = target;
            break;
        case 0x3B: /* JUMPGE */
            target = decode_jump_target(vm, instr_start);
            sval  = (int16_t)vm_pop(vm);
            sval2 = (int16_t)vm_pop(vm);
            if (sval >= sval2) vm->program_counter = target;
            break;
        case 0x3C: /* JUMPGT */
            target = decode_jump_target(vm, instr_start);
            sval  = (int16_t)vm_pop(vm);
            sval2 = (int16_t)vm_pop(vm);
            if (sval > sval2) vm->program_counter = target;
            break;
        case 0x3D: /* JUMPEQ */
            target = decode_jump_target(vm, instr_start);
            val2 = vm_pop(vm); val = vm_pop(vm);
            if (val == val2) vm->program_counter = target;
            break;
        case 0x3E: /* JUMPNE */
            target = decode_jump_target(vm, instr_start);
            val2 = vm_pop(vm); val = vm_pop(vm);
            if (val != val2) vm->program_counter = target;
            break;

        /* ── Near calls ────────────────────── */
        case 0x3F:
            operand_u16 = fetch_le16(vm);
            enter_near_call(vm, operand_u16, 0, vm->program_counter);
            break;
        case 0x40:
            operand_u16 = fetch_le16(vm);
            enter_near_call(vm, operand_u16, 1, vm->program_counter);
            break;
        case 0x41:
            operand_u16 = fetch_le16(vm);
            enter_near_call(vm, operand_u16, 2, vm->program_counter);
            break;
        case 0x42:
            operand_u16 = fetch_le16(vm);
            enter_near_call(vm, operand_u16, 3, vm->program_counter);
            break;
        case 0x43:
            operand_u8  = fetch_byte(vm);
            operand_u16 = vm_pop(vm);
            enter_near_call(vm, operand_u16, operand_u8, vm->program_counter);
            break;

        /* ── Far calls ─────────────────────── */
        case 0x44:
            operand_u16 = fetch_le16(vm);
            enter_far_call(vm, operand_u16, 0, vm->program_counter);
            break;
        case 0x45:
            operand_u16 = fetch_le16(vm);
            enter_far_call(vm, operand_u16, 1, vm->program_counter);
            break;
        case 0x46:
            operand_u16 = fetch_le16(vm);
            enter_far_call(vm, operand_u16, 2, vm->program_counter);
            break;
        case 0x47:
            operand_u16 = fetch_le16(vm);
            enter_far_call(vm, operand_u16, 3, vm->program_counter);
            break;
        case 0x48:
            operand_u8  = fetch_byte(vm);
            operand_u16 = vm_pop(vm);
            enter_far_call(vm, operand_u16, operand_u8, vm->program_counter);
            break;

        /* ── Return ────────────────────────── */
        case 0x49: { uint16_t r = vm_pop(vm); do_return(vm, &r, 1); break; }
        case 0x4A: { uint16_t r = LP128_FALSE_SENTINEL; do_return(vm, &r, 1); break; }
        case 0x4B: { uint16_t r = 0; do_return(vm, &r, 1); break; }
        case 0x55: { do_return(vm, NULL, 0); break; }   /* RETURN0 (no value) */

        /* ── HALT ──────────────────────────── */
        case 0x4D:
            operand_u16 = fetch_le16(vm);
            vm->is_halted = true;
            vm->halt_code = operand_u16;
            break;

        /* ── NEXTB (advance to next 256-byte block) ── */
        case 0x4E:
            vm->program_counter = (uint16_t)((instr_start & ~0xFFu) + 0x100u);
            break;

        /* ── I/O ───────────────────────────── */
        case 0x50: { /* PRINTV */
            uint16_t start_off  = vm_pop(vm);
            uint16_t length     = vm_pop(vm);
            uint16_t print_hdl  = vm_pop(vm);
            print_vector(vm, print_hdl, length, start_off);
            break;
        }

        /* 0x54: INCL_IF_NOT_FALSE – if local[idx] != FALSE, increment it */
        case 0x54:
            operand_u8 = fetch_byte(vm);
            if (operand_u8 < vm->frame_local_count) {
                val = vm->locals[operand_u8];
                if (val != LP128_FALSE_SENTINEL) {
                    vm->locals[operand_u8] = (uint16_t)(val + 1u);
                }
            }
            break;

        /* 0x56: STOREL – reads local index, pop, store */
        case 0x56:
            operand_u8 = fetch_byte(vm);
            val = vm_pop(vm);
            if (operand_u8 < vm->frame_local_count) {
                vm->locals[operand_u8] = val;
            }
            break;

        /* 0x57: LOADL – reads local index, push */
        case 0x57:
            operand_u8 = fetch_byte(vm);
            vm_push(vm, (operand_u8 < vm->frame_local_count)
                ? vm->locals[operand_u8] : LP128_FALSE_SENTINEL);
            break;

        /* 0x58: STOREL_PEEK – reads local index, peek top, store without pop */
        case 0x58:
            operand_u8 = fetch_byte(vm);
            if (operand_u8 < vm->frame_local_count) {
                vm->locals[operand_u8] = vm_peek(vm);
            }
            break;

        /* ── Misc raw mem ops ──────────────── */
        case 0x51: { /* LOADVB2 (raw handle + 1-based byte index) */
            int16_t  idx = (int16_t)vm_pop(vm);
            uint16_t h   = vm_pop(vm);
            if (h == LP128_FALSE_SENTINEL) {
                vm->is_halted = true; vm->halt_code = 0xFFFF;
            } else {
                uint32_t base = handle_to_byte_offset(h);
                vm_push(vm, vm_read_byte(vm, base + (uint32_t)(idx - 1)));
            }
            break;
        }
        case 0x52: { /* STOREVB2 */
            uint16_t bval = vm_pop(vm);
            int16_t  idx  = (int16_t)vm_pop(vm);
            uint16_t h    = vm_pop(vm);
            if (h != LP128_FALSE_SENTINEL) {
                uint32_t base = handle_to_byte_offset(h);
                vm_write_byte(vm, base + (uint32_t)(idx - 1), (uint8_t)(bval & 0xFFu));
            }
            break;
        }

        /* 0x59-0x5E: Bitfield operations – read 16-bit operand, perform field op */
        case 0x59: { /* BITSV load from local */
            uint16_t bfspec = fetch_le16(vm);
            operand_u8 = (uint8_t)((bfspec >> 4) & 0x0Fu);
            val = (operand_u8 < vm->frame_local_count) ? vm->locals[operand_u8] : LP128_FALSE_SENTINEL;
            if (val != LP128_FALSE_SENTINEL) {
                uint8_t first_bit = (uint8_t)(bfspec & 0xFu);
                uint8_t bit_count = (uint8_t)((bfspec >> 8) & 0xFu);
                uint16_t raw = agg_read_word(vm, val, 0);
                uint16_t mask = (bit_count >= 16) ? 0xFFFFu : (uint16_t)((1u << bit_count) - 1u);
                vm_push(vm, (uint16_t)((raw >> first_bit) & mask));
            } else { vm_push(vm, 0); }
            break;
        }
        case 0x5A: { /* BITSV load from handle */
            uint16_t bfspec = fetch_le16(vm);
            val = vm_pop(vm);
            if (val != LP128_FALSE_SENTINEL) {
                uint8_t first_bit = (uint8_t)(bfspec & 0xFu);
                uint8_t bit_count = (uint8_t)((bfspec >> 8) & 0xFu);
                uint16_t raw = agg_read_word(vm, val, 0);
                uint16_t mask = (bit_count >= 16) ? 0xFFFFu : (uint16_t)((1u << bit_count) - 1u);
                vm_push(vm, (uint16_t)((raw >> first_bit) & mask));
            } else { vm_pop(vm); vm_push(vm, 0); }
            break;
        }
        case 0x5B: { /* BBSETVL – replace multi-bit field via local's handle */
            uint16_t bfspec  = fetch_le16(vm);
            uint16_t new_val = vm_pop(vm);
            uint8_t  shift    = (uint8_t)((bfspec >> 12) & 0x0Fu);
            uint8_t  width    = (uint8_t)((bfspec >> 8) & 0x0Fu);
            uint8_t  loc_idx  = (uint8_t)((bfspec >> 4) & 0x0Fu);
            uint8_t  word_idx = (uint8_t)(bfspec & 0x0Fu);
            uint16_t bh = (loc_idx < vm->frame_local_count)
                        ? vm->locals[loc_idx] : LP128_FALSE_SENTINEL;
            if (bh != 0 && bh != LP128_FALSE_SENTINEL) {
                uint16_t vmask = (width >= 16) ? 0xFFFFu
                               : (uint16_t)((1u << width) - 1u);
                uint16_t wmask = (uint16_t)(vmask << shift);
                uint16_t raw   = agg_read_word(vm, bh, word_idx);
                agg_write_word(vm, bh, word_idx,
                    (uint16_t)((raw & ~wmask) | ((new_val & vmask) << shift)));
            }
            break;
        }
        case 0x5C: { /* BBSETV – replace multi-bit field via stack handle */
            uint16_t bfspec  = fetch_le16(vm);
            uint16_t bh      = vm_pop(vm);
            uint16_t new_val = vm_pop(vm);
            uint8_t  shift    = (uint8_t)((bfspec >> 12) & 0x0Fu);
            uint8_t  width    = (uint8_t)((bfspec >> 8) & 0x0Fu);
            uint8_t  word_idx = (uint8_t)(bfspec & 0xFFu);
            if (bh != 0 && bh != LP128_FALSE_SENTINEL) {
                uint16_t vmask = (width >= 16) ? 0xFFFFu
                               : (uint16_t)((1u << width) - 1u);
                uint16_t wmask = (uint16_t)(vmask << shift);
                uint16_t raw   = agg_read_word(vm, bh, word_idx);
                agg_write_word(vm, bh, word_idx,
                    (uint16_t)((raw & ~wmask) | ((new_val & vmask) << shift)));
            }
            break;
        }
        case 0x5D: { /* BSETVL – set/clear single bit via local's handle */
            uint16_t bfspec  = fetch_le16(vm);
            uint8_t  bit_num  = (uint8_t)((bfspec >> 12) & 0x0Fu);
            uint8_t  loc_idx  = (uint8_t)((bfspec >> 4) & 0x0Fu);
            uint8_t  word_idx = (uint8_t)(bfspec & 0x0Fu);
            uint16_t bh = (loc_idx < vm->frame_local_count)
                        ? vm->locals[loc_idx] : LP128_FALSE_SENTINEL;
            if (bh != 0 && bh != LP128_FALSE_SENTINEL) {
                uint16_t mask = (uint16_t)(1u << bit_num);
                uint16_t raw  = agg_read_word(vm, bh, word_idx);
                agg_write_word(vm, bh, word_idx,
                    ((bfspec >> 8) & 0x01u)
                        ? (uint16_t)(raw | mask)
                        : (uint16_t)(raw & ~mask));
            }
            break;
        }
        case 0x5E: { /* BSETV – set/clear single bit via stack handle */
            uint16_t bfspec  = fetch_le16(vm);
            uint16_t bh      = vm_pop(vm);
            uint8_t  bit_num  = (uint8_t)((bfspec >> 12) & 0x0Fu);
            uint8_t  word_idx = (uint8_t)(bfspec & 0xFFu);
            if (bh != 0 && bh != LP128_FALSE_SENTINEL) {
                uint16_t mask = (uint16_t)(1u << bit_num);
                uint16_t raw  = agg_read_word(vm, bh, word_idx);
                agg_write_word(vm, bh, word_idx,
                    ((bfspec >> 8) & 0x01u)
                        ? (uint16_t)(raw | mask)
                        : (uint16_t)(raw & ~mask));
            }
            break;
        }

        /* ── Extended dispatch ─────────────── */
        case 0x5F: {
            uint8_t ext_op = fetch_byte(vm);

            switch (ext_op) {

            case 0x01: /* XOR */
                val2 = vm_pop(vm); val = vm_pop(vm);
                vm_push(vm, (uint16_t)(val ^ val2));
                break;

            case 0x02: /* NOT (bitwise complement) */
                val = vm_pop(vm);
                vm_push(vm, (uint16_t)(~val));
                break;

            case 0x05: { /* Find first char in string (1-based index, or FALSE) */
                uint16_t fh      = vm_pop(vm);
                uint16_t sch     = vm_pop(vm);
                if (fh == LP128_FALSE_SENTINEL) {
                    vm_push(vm, LP128_FALSE_SENTINEL);
                } else {
                    uint16_t str_len = agg_read_word(vm, fh, 0);
                    uint16_t found   = LP128_FALSE_SENTINEL;
                    uint8_t  target  = (uint8_t)(sch & 0xFFu);
                    uint16_t si;
                    for (si = 0; si < str_len; si++) {
                        uint8_t ch2 = agg_read_payload_byte(vm, fh, si);
                        if (ch2 == 0) break;
                        if (ch2 == target) { found = (uint16_t)(si + 1u); break; }
                    }
                    vm_push(vm, found);
                }
                break;
            }

            case 0x09: { /* LONGJMPR */
                uint16_t return_value = vm_pop(vm);
                uint16_t token = vm_pop(vm);
                restore_jump_snapshot(vm, token, true, return_value);
                break;
            }

            case 0x0A: { /* LONGJMP */
                uint16_t token = vm_pop(vm);
                restore_jump_snapshot(vm, token, false, 0u);
                break;
            }

            case 0x0B: { /* SETJMP */
                uint16_t protected_pc = fetch_le16(vm);
                uint16_t landing_pc = fetch_le16(vm);
                uint16_t token;
                if (create_jump_snapshot(vm, protected_pc, landing_pc, &token)) {
                    vm_push(vm, token);
                }
                break;
            }

            case 0x0C: { /* OPEN */
#if defined(__mos__) || defined(__llvm_mos__)
                uint16_t ignored = vm_pop(vm);
                uint16_t file_name_handle = vm_pop(vm);
                uint16_t size_bytes = 0u;
                lp128_file_channel *channel;
                uint8_t open_mode = fetch_byte(vm);

                (void)ignored;
                read_vm_string_ascii(vm, file_name_handle, vm_file_name_buf, VM_FILE_NAME_MAX + 1u);
                if (vm_file_name_buf[0] == '\0') {
                    vm_push(vm, LP128_FALSE_SENTINEL);
                    break;
                }

                if (!c128_measure_runtime_file(vm, vm_file_name_buf, &size_bytes)) {
                    uint8_t access_family = file_mode_access_family(open_mode);
                    if (access_family == 0x01u || access_family == 0x03u) {
                        size_bytes = 0u;
                    } else {
                        vm_push(vm, LP128_FALSE_SENTINEL);
                        break;
                    }
                }

                channel = allocate_file_channel(vm_file_name_buf, open_mode, size_bytes);
                vm_push(vm, channel != NULL ? channel->id : LP128_FALSE_SENTINEL);
#else
                operand_u8 = fetch_byte(vm);
                (void)operand_u8;
                vm_pop(vm);
                vm_pop(vm);
                vm_push(vm, LP128_FALSE_SENTINEL);
#endif
                break;
            }

            case 0x0D: { /* CLOSE */
#if defined(__mos__) || defined(__llvm_mos__)
                uint16_t channel_id = vm_pop(vm);
                if (channel_id != 1u) {
                    release_file_channel(channel_id);
                }
                vm_push(vm, 0u);
#else
                vm_pop(vm);
                vm_push(vm, 0u);
#endif
                break;
            }

            case 0x06: /* Store program global */
                operand_u8 = fetch_byte(vm);
                val = vm_pop(vm);
                if (operand_u8 < vm->program_global_count) {
                    vm->program_globals[operand_u8] = val;
                }
                break;

            case 0x12: /* DISP cursor move/erase – reads extra sub-opcode byte */
                operand_u8 = fetch_byte(vm);
                handle_disp(vm, operand_u8);
                break;

            case 0x13: { /* XDISP scroll – reads extra sub-opcode byte + pops value */
                DIAG_VIC(14, DIAG_SC_X);
                uint8_t sh2 = screen_height(vm);
                uint8_t scroll_top = vm->system_globals[0xC5] < sh2
                    ? (uint8_t)vm->system_globals[0xC5] : (uint8_t)(sh2 - 1u);
                uint8_t scroll_bot = vm->system_globals[0xC4] < sh2
                    ? (uint8_t)vm->system_globals[0xC4] : (uint8_t)(sh2 - 1u);
                uint8_t cur_row = vm->cursor_row;
                uint8_t lines;
                operand_u8 = fetch_byte(vm);
                val        = vm_pop(vm);
                lines      = (uint8_t)(val & 0xFFu);
                switch (operand_u8) {
                case 0x00:
                    if (lines > 0 && cur_row <= scroll_bot && vm->host->scroll_up != NULL)
                        vm->host->scroll_up(vm->host, cur_row, scroll_bot, lines);
                    break;
                case 0x04:
                    if (lines > 0 && scroll_top <= scroll_bot && vm->host->scroll_up != NULL)
                        vm->host->scroll_up(vm->host, scroll_top, scroll_bot, lines);
                    break;
                default:
                    break;
                }
                if (vm->host->set_cursor != NULL)
                    vm->host->set_cursor(vm->host, cur_row, vm->cursor_col);
                vm_push(vm, val);
                break;
            }

            case 0x14: { /* FSIZE */
#if defined(__mos__) || defined(__llvm_mos__)
                uint16_t channel_id = vm_pop(vm);
                if (channel_id == 1u) {
                    vm_push(vm, (uint16_t)(vm->bundle->ro_data_size / 0x100u));
                } else {
                    lp128_file_channel *channel = find_file_channel(channel_id);
                    vm_push(vm, channel != NULL
                        ? (uint16_t)(channel->size_bytes / 0x100u)
                        : 0u);
                }
#else
                vm_pop(vm);
                vm_push(vm, 0u);
#endif
                break;
            }

            case 0x16: { /* Drop previous return values */
                uint16_t dcount = vm->last_return_word_count;
                uint16_t di;
                for (di = 0; di < dcount; di++) vm_pop(vm);
                break;
            }

            case 0x17: { /* KBINPUT – poll keyboard */
                DIAG_VIC(14, DIAG_SC_K);
                uint16_t key;
                if (vm->system_globals[0xC0] != 0) {
                    key = LP128_FALSE_SENTINEL;
                } else {
                    key = (vm->host->poll_key != NULL)
                        ? vm->host->poll_key(vm->host)
                        : LP128_FALSE_SENTINEL;
                }
                vm_push(vm, key);
                break;
            }

            case 0x1C: { /* PVFREE: release tuple */
                uint16_t wc = vm_pop(vm);
                uint32_t byte_size = (uint32_t)(wc > 0 ? wc : 1u) * 2u;
                vm->tuple_stack_byte += byte_size;
                if (vm->tuple_stack_byte > LP128_FULL_RAM_BYTES)
                    vm->tuple_stack_byte = LP128_FULL_RAM_BYTES;
                break;
            }

            case 0x1F: { /* STRCMPI with offset (first_len,first_off,second_len,h1,h2) */
                uint16_t fl = vm_pop(vm), fo = vm_pop(vm);
                uint16_t sl = vm_pop(vm);
                uint16_t h1 = vm_pop(vm), h2 = vm_pop(vm);
                uint16_t cl = fl < sl ? fl : sl;
                int      cr = 0;
                uint16_t cx;
                for (cx = 0; cx < cl; cx++) {
                    uint8_t a = agg_read_payload_byte(vm, h1, (int)(fo + cx));
                    uint8_t b = agg_read_payload_byte(vm, h2, (int)cx);
                    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);
                    if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
                    if (a < b) { cr = 1; break; }
                    if (a > b) { cr = -1; break; }
                }
                if (cr == 0 && fl != sl) cr = (fl < sl) ? 1 : -1;
                vm_push(vm, (uint16_t)(int16_t)cr);
                break;
            }

            case 0x20: { /* STRCMPI variant (first handle is 1-based) */
                uint16_t fl = vm_pop(vm), fo = vm_pop(vm);
                uint16_t sl = vm_pop(vm);
                uint16_t h1 = (uint16_t)(vm_pop(vm) - 1u), h2 = vm_pop(vm);
                uint16_t cl = fl < sl ? fl : sl;
                int      cr = 0;
                uint16_t cx;
                for (cx = 0; cx < cl; cx++) {
                    uint8_t a = agg_read_payload_byte(vm, h1, (int)(fo + cx));
                    uint8_t b = agg_read_payload_byte(vm, h2, (int)cx);
                    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);
                    if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
                    if (a < b) { cr = 1; break; }
                    if (a > b) { cr = -1; break; }
                }
                if (cr == 0 && fl != sl) cr = (fl < sl) ? 1 : -1;
                vm_push(vm, (uint16_t)(int16_t)cr);
                break;
            }

            case 0x21: { /* WPRINTV – window print from vector */
                uint16_t wp_off   = vm_pop(vm);
                uint16_t wp_count = vm_pop(vm);
                uint16_t wp_src   = vm_pop(vm);
                uint16_t wp_desc  = vm_pop(vm);
                vm_push(vm, execute_wprintv(vm, wp_desc, wp_src, wp_count, wp_off));
                break;
            }

            case 0x22: { /* SETWIN – activate display window */
                uint16_t sw_desc = vm_pop(vm);
                vm_push(vm, execute_setwin(vm, sw_desc));
                break;
            }

            case 0x24: { /* MEMCMP raw bytes */
                uint16_t bc = vm_pop(vm);
                uint16_t h1 = vm_pop(vm), h2 = vm_pop(vm);
                int cr = 0; uint16_t ci;
                for (ci = 0; ci < bc; ci++) {
                    uint8_t a = agg_read_payload_byte(vm, h1, ci);
                    uint8_t b = agg_read_payload_byte(vm, h2, ci);
                    if (a < b) { cr = 1; break; }
                    if (a > b) { cr = -1; break; }
                }
                vm_push(vm, (uint16_t)(int16_t)cr);
                break;
            }

            case 0x25: { /* MEMCMP raw bytes with first offset */
                uint16_t fo = vm_pop(vm), bc = vm_pop(vm);
                uint16_t h1 = vm_pop(vm), h2 = vm_pop(vm);
                uint32_t b1 = handle_to_byte_offset(h1);
                int cr = 0; uint16_t ci;
                for (ci = 0; ci < bc; ci++) {
                    uint8_t a = vm_read_byte(vm, b1 + fo + ci);
                    uint8_t b = agg_read_payload_byte(vm, h2, ci);
                    if (a < b) { cr = 1; break; }
                    if (a > b) { cr = -1; break; }
                }
                vm_push(vm, (uint16_t)(int16_t)cr);
                break;
            }

            case 0x27: /* DECG – decrement module global, push */
                operand_u8 = fetch_byte(vm);
                val = (uint16_t)(load_module_global(vm, operand_u8) - 1u);
                store_module_global(vm, operand_u8, val);
                vm_push(vm, val);
                break;

            case 0x28: /* DECPG – decrement program global, push */
                operand_u8 = fetch_byte(vm);
                if (operand_u8 < vm->program_global_count) {
                    val = (uint16_t)(vm->program_globals[operand_u8] - 1u);
                    vm->program_globals[operand_u8] = val;
                    vm_push(vm, val);
                }
                break;

            case 0x29: { /* DROPN – pop N items */
                uint8_t dn;
                operand_u8 = fetch_byte(vm);
                for (dn = 0; dn < operand_u8; dn++) vm_pop(vm);
                break;
            }

            case 0x2A: /* INCPG – increment program global, push */
                operand_u8 = fetch_byte(vm);
                if (operand_u8 < vm->program_global_count) {
                    val = (uint16_t)(vm->program_globals[operand_u8] + 1u);
                    vm->program_globals[operand_u8] = val;
                    vm_push(vm, val);
                }
                break;

            case 0x2B: /* INCG – increment module global, push */
                operand_u8 = fetch_byte(vm);
                val = (uint16_t)(load_module_global(vm, operand_u8) + 1u);
                store_module_global(vm, operand_u8, val);
                vm_push(vm, val);
                break;

            case 0x2C: /* STOREY_PEEK – peek top to module global */
                operand_u8 = fetch_byte(vm);
                store_module_global(vm, operand_u8, vm_peek(vm));
                break;

            case 0x2D: /* STOREG_PEEK – peek top to program global */
                operand_u8 = fetch_byte(vm);
                if (operand_u8 < vm->program_global_count)
                    vm->program_globals[operand_u8] = vm_peek(vm);
                break;

            case 0x2E: { /* RETURNN – return N values */
                uint16_t ret[LP128_MAX_LOCALS];
                int ri;
                operand_u8 = fetch_byte(vm);
                if (operand_u8 > LP128_MAX_LOCALS) operand_u8 = LP128_MAX_LOCALS;
                for (ri = (int)operand_u8 - 1; ri >= 0; ri--)
                    ret[ri] = vm_pop(vm);
                do_return(vm, ret, operand_u8);
                break;
            }

            case 0x2F: /* PRCHAR via EXT */
                val = vm_pop(vm);
                print_char(vm, (uint8_t)(val & 0xFFu));
                break;

            case 0x33: { /* ASCII filter: print char if printable, else space */
                uint16_t in_v = vm_pop(vm);
                uint8_t  ch2  = (uint8_t)(in_v & 0xFFu);
                vm_push(vm, (ch2 >= 0x20u && ch2 < 0x7Fu) ? (uint16_t)ch2 : (uint16_t)' ');
                break;
            }

            case 0x10: { /* READREC – record-oriented channel read.
                          * Stack (top→bottom): wordCount, record, channelId, vecHandle.
                          * Writes raw bytes starting at aggregate base (word 0).
                          * Pushes bytes-copied count. */
                DIAG_VIC(14, DIAG_SC_E);
                uint16_t word_count  = vm_pop(vm);
                uint16_t record      = vm_pop(vm);
                uint16_t channel_id  = vm_pop(vm);
                uint16_t vec_handle  = vm_pop(vm);

                /* Record size from system slot 0xCC (same logic as Linchpin). */
                uint16_t rec_raw = load_module_global(vm, 0xCC);
                int rec_size;
                if (rec_raw == 0)
                    rec_size = 0x100;              /* default 256 bytes */
                else if (rec_raw < 0x80u)
                    rec_size = (int)rec_raw * 2;   /* word count → bytes */
                else
                    rec_size = (int)rec_raw;

                int  byte_count   = (int)word_count * rec_size;
                uint32_t src_off  = (uint32_t)record * (uint32_t)rec_size;
                uint32_t code_end = vm->bundle->header.code_end_offset;
                uint16_t copied   = 0;

                if (vec_handle == 0 || vec_handle == LP128_FALSE_SENTINEL) {
                    /* Missing vector handle → zero-fill, return 0. */
                } else if (channel_id == 1u) {
                    /* Determine source region and bytes available. */
                    uint32_t avail;
                    uint32_t dst_base = handle_to_byte_offset(vec_handle);

                    if (src_off >= code_end) {
                        /* ── Read-only data region ── */
                        uint32_t data_off = src_off - code_end;
                        avail = (data_off < (uint32_t)vm->bundle->ro_data_size)
                              ? (uint32_t)vm->bundle->ro_data_size - data_off
                              : 0u;
                        uint16_t to_copy = (uint16_t)((uint32_t)byte_count < avail
                                                      ? (uint32_t)byte_count : avail);
#if defined(__mos__) || defined(__llvm_mos__)
                        if (to_copy > 0)
                            reu_copy(REU_VM_HEAP_OFFSET + dst_base,
                                     vm->bundle->ro_data_reu_offset + data_off,
                                     to_copy);
#else
                        if (to_copy > 0 && vm->bundle->ro_data != NULL)
                            memcpy(vm->ram + dst_base,
                                   vm->bundle->ro_data + data_off,
                                   to_copy);
#endif
                        copied = to_copy;
                    } else {
                        /* ── Code-pages region (unlikely for string reads) ── */
                        avail = code_end - src_off;
                        uint16_t to_copy = (uint16_t)((uint32_t)byte_count < avail
                                                      ? (uint32_t)byte_count : avail);
#if defined(__mos__) || defined(__llvm_mos__)
                        if (to_copy > 0)
                            reu_copy(REU_VM_HEAP_OFFSET + dst_base,
                                     vm->bundle->code_pages_reu_offset + src_off,
                                     to_copy);
#else
                        if (to_copy > 0 && vm->bundle->code_pages != NULL)
                            memcpy(vm->ram + dst_base,
                                   vm->bundle->code_pages + src_off,
                                   to_copy);
#endif
                        copied = to_copy;
                    }

                    /* Zero-fill any remaining transfer bytes. */
                    if (copied < (uint16_t)byte_count) {
                        uint16_t pad = (uint16_t)((uint16_t)byte_count - copied);
#if defined(__mos__) || defined(__llvm_mos__)
                        reu_fill_zero(REU_VM_HEAP_OFFSET + dst_base + (uint32_t)copied,
                                      pad);
#else
                        memset(vm->ram + dst_base + copied, 0, pad);
#endif
                    }
                } else {
#if defined(__mos__) || defined(__llvm_mos__)
                    lp128_file_channel *channel = find_file_channel(channel_id);
                    if (channel != NULL) {
                        copied = c128_read_runtime_file_range(vm,
                            channel->name,
                            (uint32_t)record * (uint32_t)rec_size,
                            (uint16_t)byte_count,
                            vec_handle);
                    }
#endif
                }

                vm_push(vm, copied);
                break;
            }

            case 0x30: { /* UNPACK – unpack 5-bit packed text into byte vector.
                          * Stack (top→bottom): dstOffset, wordCount, dstHandle, srcHandle.
                          * Each source word encodes three 5-bit symbols (bits 14..10, 9..5, 4..0).
                          * Bit 15 is the stop bit: if set, last symbol gets OR'd with 0x80
                          * and the function pushes FALSE. Otherwise pushes final offset. */
                uint16_t dst_off, wc, dst_h, src_h;
                int wi;
                if (vm->eval_stack_top < 4) {
                    vm_push(vm, LP128_FALSE_SENTINEL);
                    break;
                }
                dst_off = vm_pop(vm);
                wc      = vm_pop(vm);
                dst_h   = vm_pop(vm);
                src_h   = vm_pop(vm);

                for (wi = 0; wi < (int)wc; wi++) {
                    uint16_t pw = agg_read_word(vm, src_h, wi);
                    uint8_t c1 = (uint8_t)((pw >> 10) & 0x1Fu);
                    uint8_t c2 = (uint8_t)((pw >> 5)  & 0x1Fu);
                    uint8_t c3 = (uint8_t)(pw & 0x1Fu);

                    agg_write_payload_byte(vm, dst_h, dst_off++, c1);
                    agg_write_payload_byte(vm, dst_h, dst_off++, c2);

                    if (pw & 0x8000u) {
                        agg_write_payload_byte(vm, dst_h, dst_off++,
                                               (uint8_t)(c3 | 0x80u));
                        vm_push(vm, LP128_FALSE_SENTINEL);
                        goto unpack_done;
                    }

                    agg_write_payload_byte(vm, dst_h, dst_off++, c3);
                }
                vm_push(vm, dst_off);
            unpack_done:
                break;
            }

            default:
                VM_DIAG("unimplemented EXT opcode 0x%02X at PC 0x%04X\n",
                        ext_op, instr_start);
                vm->is_halted = true;
                vm->halt_code = 0xFFFF;
                break;
            }
            break;
        }

        /* ── BREAK / unrecognised ──────────── */
        default:
            VM_DIAG("unimplemented opcode 0x%02X at PC 0x%04X\n",
                    opcode, instr_start);
            vm->is_halted = true;
            vm->halt_code = 0xFFFF;
            break;
        } /* switch (opcode) */
    } /* while (!is_halted) */
}
