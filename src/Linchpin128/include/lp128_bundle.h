#ifndef LP128_BUNDLE_H
#define LP128_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* stdio.h is NOT included here; lp128_bundle.c includes it itself on non-MOS
 * builds.  This keeps the header usable on llvm-mos without dragging in FILE. */

/* ── C128 / MOS static-allocation limits ──────────────────────────────────
 * On llvm-mos targets malloc is avoided; the bundle loader uses fixed-size
 * static arrays instead.  Adjust if a specific game needs more. */
#if defined(__mos__) || defined(__llvm_mos__)
#define LP128_MAX_BUNDLE_SECTIONS       16u
#define LP128_MAX_BUNDLE_MODULES       16u
/* Exports and initializers are stored in REU on C128; no static CPU-RAM
 * arrays are needed, but we keep a generous sanity-check limit. */
#define LP128_MAX_BUNDLE_EXPORTS       2048u
#define LP128_MAX_BUNDLE_INITIALIZERS  4096u
#define LP128_MAX_BUNDLE_GLOBAL_LAYOUT 1024u
#endif

#define LP128_BUNDLE_MAGIC UINT32_C(0x31425343)
#define LP128_BUNDLE_VERSION_MAJOR 1u
#define LP128_BUNDLE_VERSION_MINOR 0u
#define LP128_BUNDLE_HEADER_SIZE 72u
#define LP128_BUNDLE_DIRECTORY_ENTRY_SIZE 28u

typedef enum lp128_section_kind {
    LP128_SECTION_IMAGE_SUMMARY = 1,
    LP128_SECTION_MODULE_TABLE = 2,
    LP128_SECTION_EXPORT_PROCEDURE_TABLE = 3,
    LP128_SECTION_PROCEDURE_INITIALIZER_TABLE = 4,
    LP128_SECTION_GLOBAL_LAYOUT = 5,
    LP128_SECTION_INITIAL_RAM = 6,
    LP128_SECTION_CODE_PAGES = 7,
    LP128_SECTION_READ_ONLY_DATA_PAGES = 8
} lp128_section_kind;

typedef enum lp128_section_flags {
    LP128_SECTION_FLAG_NONE = 0,
    LP128_SECTION_FLAG_MANDATORY_AT_BOOT = 1 << 0,
    LP128_SECTION_FLAG_PAGEABLE = 1 << 1,
    LP128_SECTION_FLAG_READ_ONLY = 1 << 2,
    LP128_SECTION_FLAG_HOST_ONLY = 1 << 3
} lp128_section_flags;

typedef struct lp128_bundle_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t directory_entry_size;
    uint16_t section_count;
    uint16_t bundle_flags;
    uint16_t target_profile;
    uint16_t code_page_size;
    uint16_t ram_page_size;
    uint16_t entry_module_id;
    uint16_t entry_procedure_index;
    uint16_t module_count;
    uint16_t program_global_count;
    uint32_t total_module_global_count;
    uint32_t initial_ram_low_bytes;
    uint32_t initial_ram_high_bytes;
    uint32_t directory_offset;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t code_end_offset;
    uint32_t module_header_offset;
    uint32_t module_header_length_words;
    uint32_t initial_ram_offset;
} lp128_bundle_header;

typedef struct lp128_section_directory_entry {
    uint16_t kind;
    uint16_t flags;
    uint16_t codec;
    uint16_t reserved0;
    uint32_t file_offset;
    uint32_t stored_length;
    uint32_t logical_length;
    uint32_t alignment;
    uint32_t reserved1;
} lp128_section_directory_entry;

typedef struct lp128_module_record {
    uint16_t module_id;
    uint16_t export_count;
    uint16_t export_start_index;
    uint16_t flags;
    uint32_t object_offset;
    uint32_t byte_length;
    uint32_t code_page_start;
    uint32_t code_page_count;
} lp128_module_record;

typedef struct lp128_export_procedure_record {
    uint16_t module_id;
    uint16_t exported_procedure_index;
    uint16_t local_count;
    uint16_t header_size;
    uint16_t initializer_start_index;
    uint16_t initializer_count;
    uint16_t start_offset;
    uint16_t code_offset;
    uint16_t upper_bound;
    uint16_t reserved0;
    uint32_t reserved1;
} lp128_export_procedure_record;

typedef struct lp128_initializer_record {
    uint8_t  local_index;
    uint8_t  reserved;      /* is_byte_encoded flag, currently unused */
    uint16_t value;
} lp128_initializer_record;

typedef struct lp128_bundle {
    lp128_bundle_header header;
    lp128_section_directory_entry *directory;
    lp128_module_record *modules;
    size_t module_count;
    lp128_export_procedure_record *exports;
    size_t export_count;
    lp128_initializer_record *initializers;
    size_t initializer_count;
    uint8_t *initial_ram;
    uint32_t initial_ram_size;
    uint8_t *code_pages;
    uint32_t code_pages_size;
#if defined(__mos__) || defined(__llvm_mos__)
    /* On C128, code_pages lives in REU rather than C128 main RAM.
     * code_pages is NULL; access goes through reu_read_byte() at this offset. */
    uint32_t code_pages_reu_offset;
    /* Export and initializer tables also live in REU (too large for CPU RAM).
     * Raw disk-format records are stored; fetched on demand via DMA. */
    uint32_t exports_reu_offset;
    uint32_t initializers_reu_offset;
    /* Read-only data pages in REU (for READREC / OBJ data access). */
    uint32_t ro_data_reu_offset;
#endif
    /* Read-only data (the OBJ region after CodeEndOffset).
     * On C128, stored in REU; on POSIX, stored in a malloc'd buffer. */
    uint8_t *ro_data;
    uint32_t ro_data_size;
    /* Global layout section: raw uint16_t[] of all globals data.              */
    /* Layout: [module_count, program_global_count, module_global_counts...,    */
    /*          program_globals..., module_0_globals..., module_1_globals...]   */
    uint16_t *global_layout_data;
    size_t    global_layout_data_count;  /* count of uint16_t elements */
} lp128_bundle;

bool lp128_bundle_load(const char *path, lp128_bundle *bundle, char *error_buffer, size_t error_buffer_size);
void lp128_bundle_free(lp128_bundle *bundle);
const char *lp128_bundle_section_name(uint16_t kind);

/** Fetch an export-procedure record by flat index.
 *  On POSIX, reads from the malloc'd array.  On C128, DMAs from REU. */
void lp128_bundle_read_export(const lp128_bundle *bundle, size_t index,
                              lp128_export_procedure_record *out);

/** Fetch an initializer record by flat index.
 *  On POSIX, reads from the malloc'd array.  On C128, DMAs from REU. */
void lp128_bundle_read_initializer(const lp128_bundle *bundle, size_t index,
                                    lp128_initializer_record *out);

#endif