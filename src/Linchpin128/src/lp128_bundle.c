#include "lp128_bundle.h"

#include <string.h>

#if defined(__mos__) || defined(__llvm_mos__)
/* C128/MOS path: use REU and static arrays; no malloc, no stdio FILE*.
 * Runtime executes in full-RAM mode and switches to KERNAL mapping only around
 * ROM calls via lp128_mmu.h wrappers. */
#include "lp128_reu.h"
#include "lp128_mmu.h"
#include "lp128_diag.h"

#define BUNDLE_TRACE(ch) lp128_k_chrout((uint8_t)(ch))

/* REU DMA must target fixed RAM below $D000. Stack locals can land in
 * transient or MMU-sensitive locations, which is unsafe for direct record
 * fetches on the C128 path. Keep small decode buffers pinned in low RAM. */
#define BUNDLE_REU_RECORD_BUF_ADDR 0x0B20u

static uint8_t *const s_bundle_reu_record_buf = (uint8_t *)BUNDLE_REU_RECORD_BUF_ADDR;

static uint16_t s_last_read_count;

/* Write one ASCII character as a VIC screen code to VIC position 30,
 * and update the VIC border color, to show bundle-loading stage progress. */
static void bundle_diag_stage(char stage)
{
    uint8_t sc = 0x20u;
    uint8_t color = 0x05u;

    if (stage >= 'A' && stage <= 'Z') {
        sc = (uint8_t)(stage - '@');
        color = (uint8_t)(((stage - 'A') & 0x07u) + 1u);
    } else if (stage >= 'a' && stage <= 'z') {
        sc = (uint8_t)(stage - 0x60u);
        color = (uint8_t)(((stage - 'a') & 0x07u) + 8u);
    }

    DIAG_VIC(30, sc);
    diag_set_vic_border(color);
}

/* Write a one-byte hex value to VIC positions 31-32 for status display. */
static void bundle_diag_status(uint8_t value)
{
    DIAG_VIC_HEX8(31, value);
}

/* Write a two-byte hex value to VIC positions 34-37 as a byte count. */
static void bundle_diag_count(uint16_t value)
{
    DIAG_VIC_HEX16(34, value);
}

/* Emit a decimal uint8_t value (3 digits) as CHROUT characters.
 * Used for bundle I/O status tracing. */
static void bundle_trace_u8_dec(uint8_t value)
{
    BUNDLE_TRACE((uint8_t)('0' + ((value / 100u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + ((value / 10u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + (value % 10u)));
}

/* Emit a decimal uint16_t value (5 digits) as CHROUT characters.
 * Used for bundle byte-count tracing. */
static void bundle_trace_u16_dec(uint16_t value)
{
    BUNDLE_TRACE((uint8_t)('0' + ((value / 10000u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + ((value / 1000u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + ((value / 100u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + ((value / 10u) % 10u)));
    BUNDLE_TRACE((uint8_t)('0' + (value % 10u)));
}

/* Emit a uint8_t value as two uppercase hex digits via CHROUT. */
static void bundle_trace_u8_hex(uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    BUNDLE_TRACE((uint8_t)hex[(value >> 4) & 0x0Fu]);
    BUNDLE_TRACE((uint8_t)hex[value & 0x0Fu]);
}

/* Static metadata storage – replaces all calloc/malloc in the bundle loader.
 * Export and initializer tables are stored in REU (too large for CPU RAM). */
static lp128_section_directory_entry  s_dir[LP128_MAX_BUNDLE_SECTIONS];
static lp128_module_record            s_modules[LP128_MAX_BUNDLE_MODULES];
static uint16_t                       s_global_layout[LP128_MAX_BUNDLE_GLOBAL_LAYOUT];

/* Logical file number used to open the bundle file. */
#define BUNDLE_LFN 2u
#define CBM_PRG_READ_SECONDARY 2u

/* ── C128 KERNAL file I/O helpers ──────────────────────────────────────── */

/* Open the bundle file at path using the C128 KERNAL IEC routines.
 * Sets the file as the current input channel (CHKIN) so subsequent
 * CHRIN calls read from it.  The filename is written into a fixed
 * low-RAM buffer at $033C (the datasette buffer) which stays visible
 * when the KERNAL ROM is mapped. */
static bool c128_open_bundle(const char *path)
{
    /* Name buffer must be:
     * 1) Below $4000 — visible when KERNAL ROM is mapped over $4000+
     * 2) Not in any KERNAL workspace area used by CHROUT
     * $033C is the datasette buffer, safe during disk I/O.  The bootloader
     * uses the same address for its own filename. */
    enum { NAMEBUF_ADDR = 0x033Cu, NAMEBUF_MAX = 31u };
    char *const namebuf = (char *)NAMEBUF_ADDR;
    uint8_t rv;
    uint8_t i;

    BUNDLE_TRACE('O');
    bundle_diag_stage('O');
    bundle_diag_status(0u);

    /* Copy path into the fixed buffer. */
    for (i = 0u; i < NAMEBUF_MAX && path[i] != '\0'; i++) {
        namebuf[i] = path[i];
    }
    namebuf[i] = '\0';

    /* CRITICAL: No CHROUT calls between here and OPEN.  CHROUT maps
     * KERNAL ROM and the screen-editor may trash low-RAM buffers.
     * SETNAM stores the pointer; OPEN reads the bytes from that pointer.
     * Any CHROUT in between risks corrupting the filename. */
    lp128_k_setnam(namebuf);
    bundle_diag_stage('N');
    lp128_k_setlfs_noirq(BUNDLE_LFN, 8u, CBM_PRG_READ_SECONDARY);
    bundle_diag_stage('L');
    rv = lp128_k_open_noirq();
    bundle_diag_stage('P');
    bundle_diag_status(rv);

    /* Now safe to trace — file is already open (or failed). */
    BUNDLE_TRACE(rv == 0 ? '+' : '-');
    if (rv != 0) {
        return false;
    }

    rv = lp128_k_chkin_noirq(BUNDLE_LFN);
    bundle_diag_stage('K');
    bundle_diag_status(rv);
    BUNDLE_TRACE(rv == 0 ? 'K' : 'J');
    return rv == 0;
}

/* Close the bundle file and clear the active input channel. */
static void c128_close_bundle(void)
{
    lp128_k_clrch_noirq();
    lp128_k_close_noirq(BUNDLE_LFN);
}

/* Read up to |size| bytes from the currently open + CHKINed file.
 * Returns actual bytes read, -1 on I/O error. */
/* Read up to size bytes from the currently open + CHKINed file into buf.
 * Records the actual byte count in s_last_read_count.  Returns the number
 * of bytes read, or -1 if the IEC status register signals a hard error. */
static int c128_read_bytes(void *buf, uint16_t size)
{
    uint8_t  *p = (uint8_t *)buf;
    uint16_t  n = 0;

    s_last_read_count = 0u;
    (void)lp128_k_readst_noirq();  /* clear any stale status from prior IEC calls */
    bundle_diag_stage('R');
    bundle_diag_count(size);

    while (n < size) {
        uint8_t byte = lp128_k_chrin_noirq();
        uint8_t st   = lp128_k_readst_noirq();
        p[n++] = byte;
        bundle_diag_status(st);
        bundle_diag_count(n);
        if (st != 0u) {
            BUNDLE_TRACE('<');
            bundle_trace_u8_dec(st);
            BUNDLE_TRACE('>');
        }
        if (st & 0x80u) {
            s_last_read_count = n;
            return -1;   /* device not present */
        }
        if (st & 0x40u) {
            break;        /* EOF – last byte */
        }
    }

    s_last_read_count = n;
    return (int)n;
}

#else
/* POSIX path: uses stdio FILE* and malloc as before. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define BUNDLE_TRACE(ch) ((void)0)
#endif

static void set_error(char *buffer, size_t buffer_size, const char *format, ...)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

#if defined(__mos__) || defined(__llvm_mos__)
    /* No vsnprintf on C128: copy the format string verbatim. */
    size_t n = 0;
    while (n < buffer_size - 1u && format[n] != '\0') { buffer[n] = format[n]; n++; }
    buffer[n] = '\0';
#else
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, buffer_size, format, args);
    va_end(args);
#endif
}

/* ── Shared byte-order helpers ───────────────────────────────────────────── */

static uint16_t read_u16le(const uint8_t *buffer)
{
    return (uint16_t)(buffer[0] | ((uint16_t)buffer[1] << 8));
}

/* Read a little-endian 32-bit double-word from a byte buffer. */
static uint32_t read_u32le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0]
        | ((uint32_t)buffer[1] << 8)
        | ((uint32_t)buffer[2] << 16)
        | ((uint32_t)buffer[3] << 24);
}

/* ── POSIX-only file I/O helpers ─────────────────────────────────────────── */
#if !(defined(__mos__) || defined(__llvm_mos__))

/* Read exactly size bytes from file.  Returns false if fewer bytes are
 * available. */
static bool read_exact(FILE *file, void *buffer, size_t size)
{
    return fread(buffer, 1, size, file) == size;
}

/* Advance the file position by size bytes by reading and discarding
 * up to 64 bytes at a time.  Used to skip padding between sections because
 * fseek is not guaranteed on all POSIX stdio implementations for non-seekable
 * streams. */
static bool skip_exact(FILE *file, size_t size)
{
    uint8_t scratch[64];
    while (size > 0) {
        size_t chunk = size > sizeof(scratch) ? sizeof(scratch) : size;
        if (fread(scratch, 1, chunk, file) != chunk) {
            return false;
        }
        size -= chunk;
    }
    return true;
}

/* Seek (or skip) to the section described by entry and read its raw bytes
 * into a newly malloc'd buffer.  Updates *current_offset to the position
 * after the section.  Returns false and sets error_buffer on any error. */
static bool load_section(FILE *file,
                         size_t *current_offset,
                         const lp128_section_directory_entry *entry,
                         void **data,
                         size_t *size,
                         char *error_buffer,
                         size_t error_buffer_size)
{
    if (entry->file_offset < *current_offset) {
        set_error(error_buffer, error_buffer_size,
                  "Section %s is not laid out sequentially.",
                  lp128_bundle_section_name(entry->kind));
        return false;
    }

    if (entry->file_offset > *current_offset) {
        if (!skip_exact(file, entry->file_offset - *current_offset)) {
            set_error(error_buffer, error_buffer_size,
                      "Unable to skip padding before %s.",
                      lp128_bundle_section_name(entry->kind));
            return false;
        }
        *current_offset = entry->file_offset;
    }

    if (entry->stored_length == 0) {
        *data = NULL;
        *size = 0;
        return true;
    }

    *data = malloc(entry->stored_length);
    if (*data == NULL) {
        set_error(error_buffer, error_buffer_size,
                  "Out of memory while loading %s.",
                  lp128_bundle_section_name(entry->kind));
        return false;
    }

    if (!read_exact(file, *data, entry->stored_length)) {
        free(*data);
        *data = NULL;
        set_error(error_buffer, error_buffer_size,
                  "Unable to read section %s.",
                  lp128_bundle_section_name(entry->kind));
        return false;
    }

    *size = entry->stored_length;
    *current_offset += entry->stored_length;
    return true;
}

#endif /* !(defined(__mos__) || defined(__llvm_mos__)) */



/* Decode a raw bundle file header from a 66-byte byte array into the
 * structured lp128_bundle_header.  All multibyte fields are little-endian. */
static void parse_header(lp128_bundle_header *header, const uint8_t *raw)
{
    header->magic = read_u32le(raw + 0);
    header->version_major = read_u16le(raw + 4);
    header->version_minor = read_u16le(raw + 6);
    header->header_size = read_u16le(raw + 8);
    header->directory_entry_size = read_u16le(raw + 10);
    header->section_count = read_u16le(raw + 12);
    header->bundle_flags = read_u16le(raw + 14);
    header->target_profile = read_u16le(raw + 16);
    header->code_page_size = read_u16le(raw + 18);
    header->ram_page_size = read_u16le(raw + 20);
    header->entry_module_id = read_u16le(raw + 22);
    header->entry_procedure_index = read_u16le(raw + 24);
    header->module_count = read_u16le(raw + 26);
    header->program_global_count = read_u16le(raw + 28);
    header->total_module_global_count = read_u32le(raw + 30);
    header->initial_ram_low_bytes = read_u32le(raw + 34);
    header->initial_ram_high_bytes = read_u32le(raw + 38);
    header->directory_offset = read_u32le(raw + 42);
    header->reserved0 = read_u32le(raw + 46);
    header->reserved1 = read_u32le(raw + 50);
    header->code_end_offset = read_u32le(raw + 54);
    header->module_header_offset = read_u32le(raw + 58);
    header->module_header_length_words = read_u32le(raw + 62);
    header->initial_ram_offset = read_u32le(raw + 66);
}

/* Decode one raw section directory entry (28 bytes) into a
 * lp128_section_directory_entry struct. */
static void parse_directory_entry(lp128_section_directory_entry *entry, const uint8_t *raw)
{
    entry->kind = read_u16le(raw + 0);
    entry->flags = read_u16le(raw + 2);
    entry->codec = read_u16le(raw + 4);
    entry->reserved0 = read_u16le(raw + 6);
    entry->file_offset = read_u32le(raw + 8);
    entry->stored_length = read_u32le(raw + 12);
    entry->logical_length = read_u32le(raw + 16);
    entry->alignment = read_u32le(raw + 20);
    entry->reserved1 = read_u32le(raw + 24);
}

/* parse_module_table and parse_export_table are only used by the POSIX loader.
 * The MOS loader does its own inline record-by-record streaming. */
#if !(defined(__mos__) || defined(__llvm_mos__))

/* Parse the module-table section (POSIX only).  Each record is 24 bytes.
 * Allocates bundle->modules and fills it.  Returns false on error. */
static bool parse_module_table(lp128_bundle *bundle,
                               const void *data,
                               size_t size,
                               char *error_buffer,
                               size_t error_buffer_size)
{
    const uint8_t *raw = (const uint8_t *)data;
    size_t index;

    if ((size % 24u) != 0) {
        set_error(error_buffer, error_buffer_size, "Module table length %lu is not a multiple of 24.", (unsigned long)size);
        return false;
    }

    bundle->module_count = size / 24u;
    bundle->modules = (lp128_module_record *)malloc(bundle->module_count * sizeof(*bundle->modules));
    if (bundle->modules == NULL && bundle->module_count > 0) {
        set_error(error_buffer, error_buffer_size, "Out of memory while parsing module table.");
        return false;
    }

    for (index = 0; index < bundle->module_count; index++) {
        const uint8_t *record = raw + index * 24u;
        bundle->modules[index].module_id = read_u16le(record + 0);
        bundle->modules[index].export_count = read_u16le(record + 2);
        bundle->modules[index].export_start_index = read_u16le(record + 4);
        bundle->modules[index].flags = read_u16le(record + 6);
        bundle->modules[index].object_offset = read_u32le(record + 8);
        bundle->modules[index].byte_length = read_u32le(record + 12);
        bundle->modules[index].code_page_start = read_u32le(record + 16);
        bundle->modules[index].code_page_count = read_u32le(record + 20);
    }

    return true;
}

/* Parse the export-procedure table section (POSIX only).  Each record is
 * 24 bytes.  Allocates bundle->exports and fills it.  Returns false on error. */
static bool parse_export_table(lp128_bundle *bundle,
                               const void *data,
                               size_t size,
                               char *error_buffer,
                               size_t error_buffer_size)
{
    const uint8_t *raw = (const uint8_t *)data;
    size_t index;

    if ((size % 24u) != 0) {
        set_error(error_buffer, error_buffer_size,
                  "Export procedure table length %lu is not a multiple of 24.",
                  (unsigned long)size);
        return false;
    }

    bundle->export_count = size / 24u;
    bundle->exports = (lp128_export_procedure_record *)malloc(bundle->export_count * sizeof(*bundle->exports));
    if (bundle->exports == NULL && bundle->export_count > 0) {
        set_error(error_buffer, error_buffer_size, "Out of memory while parsing export procedure table.");
        return false;
    }

    for (index = 0; index < bundle->export_count; index++) {
        const uint8_t *record = raw + index * 24u;
        bundle->exports[index].module_id = read_u16le(record + 0);
        bundle->exports[index].exported_procedure_index = read_u16le(record + 2);
        bundle->exports[index].local_count = read_u16le(record + 4);
        bundle->exports[index].header_size = read_u16le(record + 6);
        bundle->exports[index].initializer_start_index = read_u16le(record + 8);
        bundle->exports[index].initializer_count = read_u16le(record + 10);
        bundle->exports[index].start_offset = read_u16le(record + 12);
        bundle->exports[index].code_offset = read_u16le(record + 14);
        bundle->exports[index].upper_bound = read_u16le(record + 16);
        bundle->exports[index].reserved0 = read_u16le(record + 18);
        bundle->exports[index].reserved1 = read_u32le(record + 20);
    }

    return true;
}

#endif /* !(defined(__mos__) || defined(__llvm_mos__)) — POSIX-only parsers */

/* ── C128/MOS bundle loader ───────────────────────────────────────────────── *
 * Uses cbm_open/cbm_read for disk I/O; streams code_pages and initial_ram     *
 * directly into the REU via DMA.  Large metadata tables (exports,              *
 * initializers) are streamed to REU rather than CPU-RAM static arrays.         */
#if defined(__mos__) || defined(__llvm_mos__)

bool lp128_bundle_load(const char *path,
                       lp128_bundle *bundle,
                       char *error_buffer,
                       size_t error_buffer_size)
{
    static uint8_t  raw_header[LP128_BUNDLE_HEADER_SIZE];  /* static: soft stack is inside code region */
    static uint8_t  raw_entry[LP128_BUNDLE_DIRECTORY_ENTRY_SIZE];
    uint32_t file_pos;
    uint16_t i;
    bool     ok = false;

    BUNDLE_TRACE('5');   /* entered lp128_bundle_load */
    bundle_diag_stage('5');
    if (bundle == NULL || path == NULL) {
        BUNDLE_TRACE(bundle == NULL ? '6' : '7');  /* 6=bundle null, 7=path null */
        set_error(error_buffer, error_buffer_size, "Bundle path and destination are required.");
        return false;
    }

    BUNDLE_TRACE('(');
    memset(bundle, 0, sizeof(*bundle));
    BUNDLE_TRACE(')');

    if (!c128_open_bundle(path)) {
        bundle_diag_stage('F');
        set_error(error_buffer, error_buffer_size, "Unable to open bundle file.");
        return false;
    }
    BUNDLE_TRACE('!');

    /* Read and validate the header. */
    if (c128_read_bytes(raw_header, LP128_BUNDLE_HEADER_SIZE)
            != LP128_BUNDLE_HEADER_SIZE) {
        bundle_diag_stage('H');
        BUNDLE_TRACE('?');
        BUNDLE_TRACE('{');
        bundle_trace_u16_dec(s_last_read_count);
        BUNDLE_TRACE('}');
        BUNDLE_TRACE('h');
        bundle_trace_u8_hex(raw_header[0]);
        set_error(error_buffer, error_buffer_size, "Unable to read bundle header.");
        goto done;
    }
    BUNDLE_TRACE('R');
    /* Trace first 8 bytes of raw header as hex. */
    {
        static const char hex[] = "0123456789ABCDEF";
        BUNDLE_TRACE('[');
        for (int k = 0; k < 8 && k < LP128_BUNDLE_HEADER_SIZE; k++) {
            BUNDLE_TRACE(hex[(raw_header[k] >> 4) & 0xF]);
            BUNDLE_TRACE(hex[raw_header[k] & 0xF]);
        }
        BUNDLE_TRACE(']');
    }
    parse_header(&bundle->header, raw_header);
    if (bundle->header.magic != LP128_BUNDLE_MAGIC) {
        bundle_diag_stage('M');
        set_error(error_buffer, error_buffer_size, "Not a CSB1 bundle file.");
        goto done;
    }
    BUNDLE_TRACE('G');
    if (bundle->header.version_major != LP128_BUNDLE_VERSION_MAJOR) {
        bundle_diag_stage('V');
        BUNDLE_TRACE('X');
        set_error(error_buffer, error_buffer_size, "Unsupported bundle version.");
        goto done;
    }
    BUNDLE_TRACE('W');
    if (bundle->header.header_size != LP128_BUNDLE_HEADER_SIZE
        || bundle->header.directory_entry_size != LP128_BUNDLE_DIRECTORY_ENTRY_SIZE) {
        bundle_diag_stage('Y');
        BUNDLE_TRACE('Y');
        set_error(error_buffer, error_buffer_size, "Bundle layout mismatch.");
        goto done;
    }
    BUNDLE_TRACE('T');
    BUNDLE_TRACE('(');
    bundle_trace_u16_dec(bundle->header.section_count);
    BUNDLE_TRACE(')');
    if (bundle->header.section_count > LP128_MAX_BUNDLE_SECTIONS) {
        BUNDLE_TRACE('s');
        set_error(error_buffer, error_buffer_size, "Too many sections for C128.");
        goto done;
    }

    /* Read the section directory. */
    BUNDLE_TRACE('D');
    bundle_diag_stage('D');
    bundle->directory = s_dir;
    for (i = 0u; i < bundle->header.section_count; i++) {
        BUNDLE_TRACE('d');
        if (c128_read_bytes(raw_entry, LP128_BUNDLE_DIRECTORY_ENTRY_SIZE)
                != LP128_BUNDLE_DIRECTORY_ENTRY_SIZE) {
            BUNDLE_TRACE('q');
            set_error(error_buffer, error_buffer_size, "Failed to read directory entry.");
            goto done;
        }
        parse_directory_entry(&s_dir[i], raw_entry);
    }
    BUNDLE_TRACE('U');

    BUNDLE_TRACE('P');
    bundle_diag_stage('P');
    if (!reu_self_test()) {
        bundle_diag_stage('Q');
        BUNDLE_TRACE('Q');
        set_error(error_buffer, error_buffer_size, "REU unavailable or DMA self-test failed.");
        goto done;
    }
    BUNDLE_TRACE('p');

    /* Pre-scan the directory to find the code_pages section size, so we can
     * allocate REU space for export/initializer tables AFTER the code pages. */
    {
        uint32_t code_pages_size = 0;
        for (i = 0u; i < bundle->header.section_count; i++) {
            if (s_dir[i].kind == LP128_SECTION_CODE_PAGES) {
                code_pages_size = s_dir[i].stored_length;
                break;
            }
        }
        /* REU layout:
         *   0x000000 — 0x001FFF : VM heap
         *   0x002000 — ...      : Code pages
         *   after code pages    : Export table (24 bytes/record, raw disk format)
         *   after exports       : Initializer table (8 bytes/record, raw disk format)
         *   after initializers  : Read-only data pages (Phase B)
         */
        bundle->exports_reu_offset = (REU_CODE_PAGES_OFFSET + code_pages_size + 0xFFu) & ~(uint32_t)0xFFu;
        /* initializers_reu_offset computed when we know export_count (during section processing). */
    }

    file_pos = (uint32_t)LP128_BUNDLE_HEADER_SIZE
               + (uint32_t)bundle->header.section_count * LP128_BUNDLE_DIRECTORY_ENTRY_SIZE;

    /* Process each section in file order (sequential, no seeking). */
    for (i = 0u; i < bundle->header.section_count; i++) {
        const lp128_section_directory_entry *e = &s_dir[i];
        uint8_t  skip_buf[16];
        uint8_t  chunk_buf[32];

        /* Skip padding before this section if needed. */
        if (e->file_offset < file_pos) {
            set_error(error_buffer, error_buffer_size, "Sections not in file order.");
            goto done;
        }
        {
            uint32_t skip = e->file_offset - file_pos;
            while (skip > 0u) {
                int n = c128_read_bytes(skip_buf,
                                 (skip > 16u) ? 16u : (uint16_t)skip);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed skipping gap."); goto done; }
                skip     -= (uint32_t)n;
                file_pos += (uint32_t)n;
            }
        }

        if (e->stored_length == 0u) continue;

        switch (e->kind) {
        case LP128_SECTION_MODULE_TABLE: {
            bundle_diag_stage('m');
            uint16_t rc = (uint16_t)(e->stored_length / 24u);
            uint8_t  rr[24];
            uint16_t j;
            if (rc > LP128_MAX_BUNDLE_MODULES) {
                set_error(error_buffer, error_buffer_size, "Module count > LP128_MAX_BUNDLE_MODULES."); goto done; }
            bundle->module_count = rc;
            bundle->modules      = s_modules;
            for (j = 0u; j < rc; j++) {
                if (c128_read_bytes(rr, 24) != 24) {
                    set_error(error_buffer, error_buffer_size, "Failed reading module table."); goto done; }
                s_modules[j].module_id         = read_u16le(rr + 0);
                s_modules[j].export_count       = read_u16le(rr + 2);
                s_modules[j].export_start_index = read_u16le(rr + 4);
                s_modules[j].flags              = read_u16le(rr + 6);
                s_modules[j].object_offset      = read_u32le(rr + 8);
                s_modules[j].byte_length        = read_u32le(rr + 12);
                s_modules[j].code_page_start    = read_u32le(rr + 16);
                s_modules[j].code_page_count    = read_u32le(rr + 20);
            }
            break;
        }
        case LP128_SECTION_EXPORT_PROCEDURE_TABLE: {
            bundle_diag_stage('e');
            /* Stream raw 24-byte records directly to REU. */
            uint16_t rc = (uint16_t)(e->stored_length / 24u);
            uint32_t remain = e->stored_length;
            reu_addr_t rpos = bundle->exports_reu_offset;
            if (rc > LP128_MAX_BUNDLE_EXPORTS) {
                set_error(error_buffer, error_buffer_size, "Export count > LP128_MAX_BUNDLE_EXPORTS."); goto done; }
            bundle->export_count = rc;
            bundle->exports      = NULL;  /* lives in REU */
            while (remain > 0u) {
                int n = c128_read_bytes(chunk_buf,
                                 (remain > 32u) ? 32u : (uint16_t)remain);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed reading export table."); goto done; }
                reu_store(chunk_buf, rpos, (uint16_t)n);
                rpos   += (uint32_t)n;
                remain -= (uint32_t)n;
            }
            /* Compute initializer table REU offset now that we know export_count. */
            bundle->initializers_reu_offset = bundle->exports_reu_offset
                + (uint32_t)rc * 24u;
            break;
        }
        case LP128_SECTION_PROCEDURE_INITIALIZER_TABLE: {
            bundle_diag_stage('i');
            /* Stream raw 8-byte records directly to REU. */
            uint16_t rc = (uint16_t)(e->stored_length / 8u);
            uint32_t remain = e->stored_length;
            reu_addr_t rpos = bundle->initializers_reu_offset;
            if (rc > LP128_MAX_BUNDLE_INITIALIZERS) {
                set_error(error_buffer, error_buffer_size, "Initializer count > LP128_MAX_BUNDLE_INITIALIZERS."); goto done; }
            bundle->initializer_count = rc;
            bundle->initializers      = NULL;  /* lives in REU */
            while (remain > 0u) {
                int n = c128_read_bytes(chunk_buf,
                                 (remain > 32u) ? 32u : (uint16_t)remain);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed reading initializer table."); goto done; }
                reu_store(chunk_buf, rpos, (uint16_t)n);
                rpos   += (uint32_t)n;
                remain -= (uint32_t)n;
            }
            break;
        }
        case LP128_SECTION_GLOBAL_LAYOUT: {
            bundle_diag_stage('g');
            BUNDLE_TRACE('g');
            uint16_t wc = (uint16_t)(e->stored_length / 2u);
            uint8_t  rr[2];
            uint16_t j;
            if (wc > LP128_MAX_BUNDLE_GLOBAL_LAYOUT) {
                set_error(error_buffer, error_buffer_size, "Global layout > LP128_MAX_BUNDLE_GLOBAL_LAYOUT."); goto done; }
            bundle->global_layout_data_count = wc;
            bundle->global_layout_data       = s_global_layout;
            for (j = 0u; j < wc; j++) {
                if (c128_read_bytes(rr, 2) != 2) {
                    set_error(error_buffer, error_buffer_size, "Failed reading global layout."); goto done; }
                s_global_layout[j] = read_u16le(rr);
            }
            BUNDLE_TRACE('G');
            break;
        }
        case LP128_SECTION_INITIAL_RAM: {
            bundle_diag_stage('r');
            BUNDLE_TRACE('r');
            /* Stream section data directly into REU VM heap. */
            uint32_t    remain  = e->stored_length;
            reu_addr_t  rpos    = REU_VM_HEAP_OFFSET;
            while (remain > 0u) {
                int n = c128_read_bytes(chunk_buf,
                                 (remain > 32u) ? 32u : (uint16_t)remain);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed reading initial RAM."); goto done; }
                reu_store(chunk_buf, rpos, (uint16_t)n);
                rpos   += (uint32_t)n;
                remain -= (uint32_t)n;
            }
            bundle->initial_ram      = (uint8_t *)0;  /* lives in REU */
            bundle->initial_ram_size = e->stored_length;
            BUNDLE_TRACE('R');
            break;
        }
        case LP128_SECTION_CODE_PAGES: {
            bundle_diag_stage('c');
            BUNDLE_TRACE('c');
            /* Stream section data directly into REU at code-pages base. */
            uint32_t    remain  = e->stored_length;
            reu_addr_t  rpos    = REU_CODE_PAGES_OFFSET;
            while (remain > 0u) {
                int n = c128_read_bytes(chunk_buf,
                                 (remain > 32u) ? 32u : (uint16_t)remain);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed reading code pages."); goto done; }
                reu_store(chunk_buf, rpos, (uint16_t)n);
                rpos   += (uint32_t)n;
                remain -= (uint32_t)n;
            }
            bundle->code_pages            = (uint8_t *)0;  /* lives in REU */
            bundle->code_pages_size       = e->stored_length;
            bundle->code_pages_reu_offset = REU_CODE_PAGES_OFFSET;
            BUNDLE_TRACE('C');
            break;
        }
        case LP128_SECTION_READ_ONLY_DATA_PAGES: {
            bundle_diag_stage('o');
            BUNDLE_TRACE('o');
            /* Stream read-only data pages into REU after the initializer table. */
            uint32_t    remain = e->stored_length;
            reu_addr_t  rpos   = bundle->initializers_reu_offset
                                 + (uint32_t)bundle->initializer_count * 8u;
            /* Align to 256 bytes. */
            rpos = (rpos + 0xFFu) & ~(uint32_t)0xFFu;
            bundle->ro_data_reu_offset = rpos;
            bundle->ro_data_size       = e->stored_length;
            while (remain > 0u) {
                int n = c128_read_bytes(chunk_buf,
                                 (remain > 32u) ? 32u : (uint16_t)remain);
                if (n <= 0) { set_error(error_buffer, error_buffer_size, "Failed reading RO data pages."); goto done; }
                reu_store(chunk_buf, rpos, (uint16_t)n);
                rpos   += (uint32_t)n;
                remain -= (uint32_t)n;
            }
            BUNDLE_TRACE('O');
            break;
        }
        default: {
            /* Unrecognised / optional section – skip it. */
            uint32_t skip = e->stored_length;
            while (skip > 0u) {
                int n = c128_read_bytes(skip_buf,
                                 (skip > 16u) ? 16u : (uint16_t)skip);
                if (n <= 0) break;
                skip -= (uint32_t)n;
            }
            break;
        }
        }
        file_pos += e->stored_length;
    }

    BUNDLE_TRACE('!');
    ok = true;

done:
    bundle_diag_stage(ok ? 'S' : 'E');
    diag_set_vic_background(ok ? 0x05u : 0x02u);
    BUNDLE_TRACE('Z');
    BUNDLE_TRACE(ok ? '+' : '-');
    c128_close_bundle();
    BUNDLE_TRACE('z');
    return ok;
}

#else  /* POSIX ─────────────────────────────────────────────────────────────── */

/* Load a bundle file on POSIX using stdio.  Reads the header, directory,
 * and sections sequentially; allocates heap storage for each section via
 * malloc.  Returns false and sets error_buffer on any error. */
bool lp128_bundle_load(const char *path, lp128_bundle *bundle, char *error_buffer, size_t error_buffer_size)
{
    FILE *file;
    uint8_t raw_header[LP128_BUNDLE_HEADER_SIZE];
    size_t current_offset;
    uint16_t index;

    if (bundle == NULL || path == NULL) {
        set_error(error_buffer, error_buffer_size, "Bundle path and destination are required.");
        return false;
    }

    memset(bundle, 0, sizeof(*bundle));
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error_buffer, error_buffer_size, "Unable to open '%s'.", path);
        return false;
    }

    if (!read_exact(file, raw_header, sizeof(raw_header))) {
        set_error(error_buffer, error_buffer_size, "Unable to read bundle header from '%s'.", path);
        fclose(file);
        return false;
    }

    parse_header(&bundle->header, raw_header);
    if (bundle->header.magic != LP128_BUNDLE_MAGIC) {
        set_error(error_buffer, error_buffer_size, "'%s' is not a CSB1 bundle.", path);
        fclose(file);
        return false;
    }

    if (bundle->header.version_major != LP128_BUNDLE_VERSION_MAJOR) {
        set_error(error_buffer, error_buffer_size,
                  "Unsupported bundle version %u.%u.",
                  bundle->header.version_major,
                  bundle->header.version_minor);
        fclose(file);
        return false;
    }

    if (bundle->header.header_size != LP128_BUNDLE_HEADER_SIZE
        || bundle->header.directory_entry_size != LP128_BUNDLE_DIRECTORY_ENTRY_SIZE) {
        set_error(error_buffer, error_buffer_size, "Bundle layout constants do not match the runtime.");
        fclose(file);
        return false;
    }

    bundle->directory = (lp128_section_directory_entry *)calloc(bundle->header.section_count, sizeof(*bundle->directory));
    if (bundle->directory == NULL && bundle->header.section_count > 0) {
        set_error(error_buffer, error_buffer_size, "Out of memory while allocating section directory.");
        fclose(file);
        return false;
    }

    for (index = 0; index < bundle->header.section_count; index++) {
        uint8_t raw_entry[LP128_BUNDLE_DIRECTORY_ENTRY_SIZE];
        if (!read_exact(file, raw_entry, sizeof(raw_entry))) {
            set_error(error_buffer, error_buffer_size, "Unable to read section directory entry %u.", index);
            fclose(file);
            lp128_bundle_free(bundle);
            return false;
        }

        parse_directory_entry(&bundle->directory[index], raw_entry);
    }

    current_offset = LP128_BUNDLE_HEADER_SIZE + (size_t)bundle->header.section_count * LP128_BUNDLE_DIRECTORY_ENTRY_SIZE;
    for (index = 0; index < bundle->header.section_count; index++) {
        const lp128_section_directory_entry *entry = &bundle->directory[index];
        void *section_data = NULL;
        size_t section_size = 0;

        if (!load_section(file, &current_offset, entry, &section_data, &section_size, error_buffer, error_buffer_size)) {
            fclose(file);
            lp128_bundle_free(bundle);
            return false;
        }

        switch (entry->kind) {
        case LP128_SECTION_MODULE_TABLE:
            if (!parse_module_table(bundle, section_data, section_size, error_buffer, error_buffer_size)) {
                free(section_data);
                fclose(file);
                lp128_bundle_free(bundle);
                return false;
            }
            free(section_data);
            break;
        case LP128_SECTION_EXPORT_PROCEDURE_TABLE:
            if (!parse_export_table(bundle, section_data, section_size, error_buffer, error_buffer_size)) {
                free(section_data);
                fclose(file);
                lp128_bundle_free(bundle);
                return false;
            }
            free(section_data);
            break;
        case LP128_SECTION_INITIAL_RAM:
            bundle->initial_ram = (uint8_t *)section_data;
            bundle->initial_ram_size = section_size;
            break;
        case LP128_SECTION_CODE_PAGES:
            bundle->code_pages = (uint8_t *)section_data;
            bundle->code_pages_size = section_size;
            break;
        case LP128_SECTION_PROCEDURE_INITIALIZER_TABLE:
            if (section_size > 0) {
                size_t rec_count = section_size / 8u;
                bundle->initializers = (lp128_initializer_record *)malloc(
                    rec_count * sizeof(*bundle->initializers));
                if (bundle->initializers == NULL) {
                    set_error(error_buffer, error_buffer_size,
                              "Out of memory while parsing initializer table.");
                    free(section_data);
                    fclose(file);
                    lp128_bundle_free(bundle);
                    return false;
                }
                bundle->initializer_count = rec_count;
                {
                    const uint8_t *raw2 = (const uint8_t *)section_data;
                    size_t ri;
                    for (ri = 0; ri < rec_count; ri++) {
                        bundle->initializers[ri].local_index = raw2[ri * 8u + 0u];
                        bundle->initializers[ri].reserved    = raw2[ri * 8u + 1u];
                        bundle->initializers[ri].value       = read_u16le(raw2 + ri * 8u + 2u);
                    }
                }
            }
            free(section_data);
            break;
        case LP128_SECTION_GLOBAL_LAYOUT:
            if (section_size > 0 && (section_size % 2u) == 0) {
                bundle->global_layout_data_count = section_size / 2u;
                bundle->global_layout_data = (uint16_t *)malloc(section_size);
                if (bundle->global_layout_data == NULL) {
                    set_error(error_buffer, error_buffer_size,
                              "Out of memory while parsing global layout.");
                    free(section_data);
                    fclose(file);
                    lp128_bundle_free(bundle);
                    return false;
                }
                {
                    const uint8_t *raw2 = (const uint8_t *)section_data;
                    size_t wi;
                    for (wi = 0; wi < bundle->global_layout_data_count; wi++) {
                        bundle->global_layout_data[wi] = read_u16le(raw2 + wi * 2u);
                    }
                }
            }
            free(section_data);
            break;
        case LP128_SECTION_READ_ONLY_DATA_PAGES:
            bundle->ro_data = (uint8_t *)section_data;
            bundle->ro_data_size = section_size;
            break;
        default:
            free(section_data);
            break;
        }
    }

    fclose(file);
    return true;
}

#endif /* defined(__mos__) || defined(__llvm_mos__) else POSIX */

/* Release all heap-allocated resources held by bundle.  On C128 all
 * arrays are static; only the memset is applied.  Safe to call after a
 * partial lp128_bundle_load failure. */
void lp128_bundle_free(lp128_bundle *bundle)
{
    if (bundle == NULL) {
        return;
    }

#if !(defined(__mos__) || defined(__llvm_mos__))
    /* On C128 all arrays are static; nothing to free. */
    free(bundle->directory);
    free(bundle->modules);
    free(bundle->exports);
    free(bundle->initializers);
    free(bundle->initial_ram);
    free(bundle->code_pages);
    free(bundle->global_layout_data);
    free(bundle->ro_data);
#endif
    memset(bundle, 0, sizeof(*bundle));
}

/* Return a human-readable name string for the given section kind constant.
 * Returns "unknown" for unrecognised values. */
const char *lp128_bundle_section_name(uint16_t kind)
{
    switch (kind) {
    case LP128_SECTION_IMAGE_SUMMARY:
        return "image-summary";
    case LP128_SECTION_MODULE_TABLE:
        return "module-table";
    case LP128_SECTION_EXPORT_PROCEDURE_TABLE:
        return "export-procedure-table";
    case LP128_SECTION_PROCEDURE_INITIALIZER_TABLE:
        return "procedure-initializer-table";
    case LP128_SECTION_GLOBAL_LAYOUT:
        return "global-layout";
    case LP128_SECTION_INITIAL_RAM:
        return "initial-ram";
    case LP128_SECTION_CODE_PAGES:
        return "code-pages";
    case LP128_SECTION_READ_ONLY_DATA_PAGES:
        return "read-only-data-pages";
    default:
        return "unknown";
    }
}

/* ── Record fetch functions ─────────────────────────────────────────────── */

/* Retrieve one export procedure record at index from the bundle.
 * On C128 the export table lives in REU; a 24-byte DMA fetch is used.
 * On POSIX the table is in a malloc'd array. */
void lp128_bundle_read_export(const lp128_bundle *bundle, size_t index,
                              lp128_export_procedure_record *out)
{
#if defined(__mos__) || defined(__llvm_mos__)
    /* DMA 24-byte raw record from REU and parse. */
    uint8_t *const raw = s_bundle_reu_record_buf;
    reu_fetch(raw, bundle->exports_reu_offset + (uint32_t)index * 24u, 24u);
    out->module_id                = read_u16le(raw + 0);
    out->exported_procedure_index = read_u16le(raw + 2);
    out->local_count              = read_u16le(raw + 4);
    out->header_size              = read_u16le(raw + 6);
    out->initializer_start_index  = read_u16le(raw + 8);
    out->initializer_count        = read_u16le(raw + 10);
    out->start_offset             = read_u16le(raw + 12);
    out->code_offset              = read_u16le(raw + 14);
    out->upper_bound              = read_u16le(raw + 16);
    out->reserved0                = read_u16le(raw + 18);
    out->reserved1                = read_u32le(raw + 20);
#else
    *out = bundle->exports[index];
#endif
}

/* Retrieve one procedure-initializer record at index from the bundle.
 * On C128 the initializer table lives in REU; an 8-byte DMA fetch is used.
 * On POSIX the table is in a malloc'd array. */
void lp128_bundle_read_initializer(const lp128_bundle *bundle, size_t index,
                                    lp128_initializer_record *out)
{
#if defined(__mos__) || defined(__llvm_mos__)
    /* DMA 8-byte raw record from REU and parse fields we use. */
    uint8_t *const raw = s_bundle_reu_record_buf;
    reu_fetch(raw, bundle->initializers_reu_offset + (uint32_t)index * 8u, 8u);
    out->local_index = raw[0];
    out->reserved    = raw[1];
    out->value       = read_u16le(raw + 2);
#else
    *out = bundle->initializers[index];
#endif
}