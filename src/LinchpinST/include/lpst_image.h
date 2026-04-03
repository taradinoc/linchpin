/*
 * lpst_image.h — VM image loader: parses the MME metadata file and the OBJ
 * bytecode file into an in-memory lpst_image that the executor can use.
 *
 * The image consists of two on-disk files:
 *   .MME  — metadata: a fixed 96-byte header, module/global tables, and the
 *            initial RAM snapshot that is copied into low memory at startup.
 *   .OBJ  — bytecode: module code segments laid out sequentially at
 *            256-byte page boundaries.
 */
#ifndef LPST_IMAGE_H
#define LPST_IMAGE_H

#include "lpst_vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LPST_HEADER_WORD_COUNT 48
#define LPST_HEADER_BYTE_COUNT (LPST_HEADER_WORD_COUNT * 2)
#define LPST_MAX_MODULES 16
#define LPST_MAX_PROCEDURES_PER_MODULE 256
#define LPST_MAX_INITIALIZERS_PER_PROC 64

/* Decoded procedure header as it appears in OBJ bytecode.
 * header_size is the number of bytes consumed by the header itself, so
 * start_offset + header_size == the first executable instruction. */
typedef struct lpst_proc_header {
    uint8_t local_count;
    uint8_t header_size;
    uint8_t initializer_count;
    lpst_proc_initializer initializers[LPST_MAX_INITIALIZERS_PER_PROC];
} lpst_proc_header;

/* A single procedure as loaded from the module/procedure table in the MME file.
 * exported_index < 0 means the procedure was discovered by parsing bytecode
 * at a given offset rather than from the module table (a "private" procedure). */
typedef struct lpst_procedure {
    int16_t exported_index;          /* index in the module's procedure table, or -1 */
    uint16_t start_offset;           /* byte offset of the procedure header within the module */
    uint16_t code_offset;            /* byte offset of the first instruction */
    uint16_t upper_bound;            /* exclusive upper bound within the module (start of next proc) */
    uint8_t local_count;
    uint8_t initializer_count;
    lpst_proc_initializer *initializers;
} lpst_procedure;

/* One module: a contiguous region of the OBJ file containing bytecode for
 * a set of procedures.  Module IDs are 1-based. */
typedef struct lpst_module {
    uint16_t module_id;
    uint32_t object_offset;          /* byte offset of this module within the OBJ file */
    uint32_t length;                 /* byte length of this module in the OBJ file */
    uint16_t procedure_count;
    lpst_procedure *procedures;
} lpst_module;

/* Global-variable tables: one shared pool for the whole program plus
 * per-module pools, all pre-populated with initial values from the MME file. */
typedef struct lpst_globals {
    uint16_t program_global_count;
    uint16_t *program_globals;
    uint16_t module_count;
    uint16_t *module_global_counts;
    uint16_t **module_globals;
} lpst_globals;

/* The fully loaded VM image, ready to be handed to lpst_exec_init.
 * Path buffers use platform-native separators and can hold up to MAX_PATH
 * characters (260 bytes including the null terminator). */
typedef struct lpst_image {
    uint16_t header_words[LPST_HEADER_WORD_COUNT];

    size_t mme_size;
    size_t obj_size;
    char obj_path[260];
    char base_dir[260];   /* directory containing the MME/OBJ files */
    char data_dir[260];   /* configured data directory override/fallback */

    uint16_t entry_module_id;
    uint16_t entry_procedure_index;
    uint32_t code_end_offset;            /* exclusive end of all bytecode in the OBJ file */
    uint32_t module_header_offset;       /* byte offset of the module header table in the MME file */
    uint16_t module_header_length_words;
    uint32_t initial_ram_offset;         /* byte offset of the RAM snapshot in the MME file */
    uint8_t *initial_ram_bytes;
    size_t initial_ram_size;

    lpst_globals globals;
    uint16_t module_count;
    lpst_module *modules;
} lpst_image;

typedef enum lpst_result {
    LPST_OK = 0,
    LPST_ERR_NULL_ARG,
    LPST_ERR_IO,
    LPST_ERR_MME_TOO_SMALL,
    LPST_ERR_OBJ_TOO_SHORT,
    LPST_ERR_MODULE_HEADER_TRUNCATED,
    LPST_ERR_INITIAL_RAM_TRUNCATED,
    LPST_ERR_ENTRY_OUT_OF_RANGE,
    LPST_ERR_MODULE_SPAN_INVALID,
    LPST_ERR_PROCEDURE_OUT_OF_RANGE,
    LPST_ERR_HEADER_PARSE,
    LPST_ERR_ALLOC,
} lpst_result;

/*
 * Parse and load the MME metadata file and OBJ bytecode file into image.
 * Both paths must be non-NULL.  On success returns LPST_OK and the caller
 * must eventually call lpst_image_free.  On failure the image is left in a
 * safe (freed) state.
 */
lpst_result lpst_image_load(lpst_image *image, const char *mme_path, const char *obj_path);

/* Release all memory owned by image.  Safe to call on a partially-loaded image. */
void lpst_image_free(lpst_image *image);

/* Return a human-readable string for an lpst_result code. */
const char *lpst_result_string(lpst_result result);

#endif
