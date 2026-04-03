/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_image.c — VM image loader.
 *
 * Parses the MME metadata file and the OBJ bytecode file into an lpst_image
 * that can be handed to lpst_exec_init.  The MME file contains the fixed
 * 96-byte program header, global-variable initial values, the module/procedure
 * table, and the initial RAM snapshot.  The OBJ file contains raw bytecode
 * for all modules packed at 256-byte page boundaries.
 */
#include "lpst_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t *load_file(const char *path, size_t *out_size)
{
    FILE *f;
    long length;
    uint8_t *buf;
    size_t n;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    length = ftell(f);
    if (length < 0) {
        fclose(f);
        return NULL;
    }

    rewind(f);
    buf = (uint8_t *)malloc((size_t)length);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    n = fread(buf, 1, (size_t)length, f);
    fclose(f);
    if (n != (size_t)length) {
        free(buf);
        return NULL;
    }

    *out_size = (size_t)length;
    return buf;
}

static bool load_module_bytes(FILE *obj_file, uint32_t module_offset, uint32_t module_length, uint8_t **out_bytes)
{
    uint8_t *module_bytes;

    if (obj_file == NULL || out_bytes == NULL) {
        return false;
    }

    *out_bytes = NULL;

    if (module_length == 0) {
        return true;
    }

    module_bytes = (uint8_t *)malloc(module_length);
    if (module_bytes == NULL) {
        return false;
    }

    if (fseek(obj_file, (long)module_offset, SEEK_SET) != 0
        || fread(module_bytes, 1, module_length, obj_file) != module_length) {
        free(module_bytes);
        return false;
    }

    *out_bytes = module_bytes;
    return true;
}

/* Round value up to the next 256-byte boundary, matching the page alignment
 * used for module code and for the initial RAM region in the OBJ/MME files. */
static uint32_t align_to_page(uint32_t value)
{
    return (value + 0xFFu) & ~0xFFu;
}

/* The MME header stores the initial RAM size in a single 16-bit word whose
 * interpretation is ambiguous: it may be a word count or a byte count.
 * Try the word-count interpretation first (multiply by 2); fall back to
 * the byte-count interpretation if that would fit, so that both cases are
 * handled correctly. */
static size_t infer_initial_ram_size(uint16_t raw_header_value, size_t available_bytes)
{
    size_t as_bytes = raw_header_value;
    size_t as_words = (size_t)raw_header_value * 2u;

    if (as_words <= available_bytes) {
        return as_words;
    }

    if (as_bytes <= available_bytes) {
        return as_bytes;
    }

    return 0;
}

/*
 * Parse the binary procedure header at procedure_offset in module_bytes.
 *
 * Header format:
 *   byte 0: low 7 bits = local count; bit 7 set if initializers follow
 *   optional initializer sequence (when bit 7 is set):
 *     per-initializer marker byte:
 *       bits 5-0 = local index
 *       bit 6 set  = value is a sign-extended byte (1 byte follows)
 *       bit 6 clear = value is a 16-bit LE word  (2 bytes follow)
 *       bit 7 set  = this is the last initializer
 *
 * Fills out_header and returns LPST_OK on success.
 */
static lpst_result parse_procedure_header(
    const uint8_t *module_bytes,
    uint32_t module_length,
    uint16_t procedure_offset,
    lpst_proc_header *out_header)
{
    uint32_t cursor;
    uint8_t header_byte;
    uint8_t init_count;

    if (module_bytes == NULL || out_header == NULL || procedure_offset >= module_length) {
        return LPST_ERR_HEADER_PARSE;
    }

    header_byte = module_bytes[procedure_offset];
    cursor = (uint32_t)procedure_offset + 1u;
    out_header->local_count = header_byte & 0x7Fu;
    out_header->initializer_count = 0;

    if ((header_byte & 0x80u) != 0) {
        init_count = 0;
        for (;;) {
            uint8_t marker;
            uint8_t local_idx;
            bool is_byte;
            bool is_last;
            uint16_t value;

            if (cursor >= module_length) {
                return LPST_ERR_HEADER_PARSE;
            }

            marker = module_bytes[cursor];
            cursor++;
            local_idx = marker & 0x3Fu;
            is_byte = (marker & 0x40u) != 0;
            is_last = (marker & 0x80u) != 0;

            if (is_byte) {
                if (cursor >= module_length) {
                    return LPST_ERR_HEADER_PARSE;
                }

                header_byte = module_bytes[cursor];
                value = (uint16_t)(int16_t)(int8_t)header_byte;
                cursor++;
            } else {
                if (cursor + 1u >= module_length) {
                    return LPST_ERR_HEADER_PARSE;
                }

                value = read_le16(&module_bytes[cursor]);
                cursor += 2u;
            }

            if (init_count < LPST_MAX_INITIALIZERS_PER_PROC) {
                out_header->initializers[init_count].local_index = local_idx;
                out_header->initializers[init_count].value = value;
                init_count++;
            }

            if (is_last) {
                break;
            }
        }

        out_header->initializer_count = init_count;
    }

    out_header->header_size = (uint8_t)(cursor - procedure_offset);
    return LPST_OK;
}

/*
 * Parse all modules from the OBJ file.  For each module this function:
 *  1. Reads the module's bytecode into a temporary buffer.
 *  2. Parses each procedure header to determine local count and initializers.
 *  3. Stores the resulting lpst_procedure records in image->modules[i].
 */
static lpst_result parse_modules(
    lpst_image *image,
    FILE *obj_file,
    const uint16_t *module_offset_pages,
    const uint16_t *proc_offset_table,
    const uint16_t *proc_counts)
{
    uint16_t i;

    image->modules = (lpst_module *)calloc(image->module_count, sizeof(lpst_module));
    if (image->modules == NULL) {
        return LPST_ERR_ALLOC;
    }

    for (i = 0; i < image->module_count; i++) {
        uint32_t mod_offset = (uint32_t)module_offset_pages[i] * 0x100u;
        uint32_t next_offset;
        uint32_t mod_length;
        uint8_t *module_bytes = NULL;
        uint16_t proc_count;
        uint16_t j;
        lpst_module *mod;

        if (i == image->module_count - 1u) {
            next_offset = image->code_end_offset;
        } else {
            next_offset = (uint32_t)module_offset_pages[i + 1u] * 0x100u;
        }

        if (mod_offset > next_offset || next_offset > image->obj_size) {
            return LPST_ERR_MODULE_SPAN_INVALID;
        }

        mod_length = next_offset - mod_offset;
        proc_count = proc_counts[i];
        mod = &image->modules[i];
        mod->module_id = (uint16_t)(i + 1u);
        mod->object_offset = mod_offset;
        mod->length = mod_length;
        mod->procedure_count = proc_count;

        if (proc_count > 0) {
            mod->procedures = (lpst_procedure *)calloc(proc_count, sizeof(lpst_procedure));
            if (mod->procedures == NULL) {
                return LPST_ERR_ALLOC;
            }

            if (!load_module_bytes(obj_file, mod_offset, mod_length, &module_bytes)) {
                return LPST_ERR_IO;
            }
        } else {
            mod->procedures = NULL;
        }

        for (j = 0; j < proc_count; j++) {
            uint16_t proc_off = proc_offset_table[j];
            lpst_proc_header header;
            lpst_result res;

            if (proc_off >= mod_length) {
                return LPST_ERR_PROCEDURE_OUT_OF_RANGE;
            }

            mod->procedures[j].exported_index = j;
            mod->procedures[j].start_offset = proc_off;
            res = parse_procedure_header(
                module_bytes,
                mod_length,
                proc_off,
                &header);
            if (res != LPST_OK) {
                free(module_bytes);
                return res;
            }

            mod->procedures[j].code_offset = (uint16_t)(proc_off + header.header_size);
            mod->procedures[j].local_count = header.local_count;
            mod->procedures[j].initializer_count = header.initializer_count;
            mod->procedures[j].upper_bound = (j + 1u < proc_count)
                ? proc_offset_table[j + 1u]
                : (uint16_t)mod_length;

            if (header.initializer_count > 0) {
                size_t bytes = (size_t)header.initializer_count * sizeof(lpst_proc_initializer);

                mod->procedures[j].initializers = (lpst_proc_initializer *)malloc(bytes);
                if (mod->procedures[j].initializers == NULL) {
                    free(module_bytes);
                    return LPST_ERR_ALLOC;
                }

                memcpy(mod->procedures[j].initializers, header.initializers, bytes);
            }
        }

        free(module_bytes);

        proc_offset_table += proc_count;
    }

    return LPST_OK;
}

lpst_result lpst_image_load(lpst_image *image, const char *mme_path, const char *obj_path)
{
    uint8_t *mme = NULL;
    FILE *obj_file = NULL;
    size_t cursor;
    uint16_t module_count;
    uint16_t module_offset_pages[LPST_MAX_MODULES];
    uint16_t program_global_count;
    uint16_t module_global_counts[LPST_MAX_MODULES];
    uint32_t mod_hdr_offset;
    uint32_t mod_hdr_len_bytes;
    uint32_t initial_ram_offset;
    size_t initial_ram_size;
    uint16_t *proc_offset_buf;
    uint16_t proc_counts[LPST_MAX_MODULES];
    size_t total_procs;
    uint16_t i;
    lpst_result res;

    if (image == NULL || mme_path == NULL || obj_path == NULL) {
        return LPST_ERR_NULL_ARG;
    }

    memset(image, 0, sizeof(*image));

    /* Store paths for runtime file I/O */
    {
        size_t len;
        const char *last_sep;

        len = strlen(obj_path);
        if (len >= sizeof(image->obj_path)) {
            len = sizeof(image->obj_path) - 1;
        }
        memcpy(image->obj_path, obj_path, len);
        image->obj_path[len] = '\0';

        /* Extract the base directory from mme_path so the VM can later
         * resolve data-file paths relative to the image's install location.
         * Atari paths may use '\\' and a drive-letter prefix. */
        last_sep = strrchr(mme_path, '/');
        {
            const char *bs = strrchr(mme_path, '\\');
            if (bs != NULL && (last_sep == NULL || bs > last_sep)) {
                last_sep = bs;
            }
        }
        {
            const char *colon = strrchr(mme_path, ':');
            if (colon != NULL && (last_sep == NULL || colon > last_sep)) {
                last_sep = colon;
            }
        }
        if (last_sep != NULL) {
            len = (size_t)(last_sep - mme_path);
            if (*last_sep == ':') {
                len++;
            }
            if (len >= sizeof(image->base_dir)) {
                len = sizeof(image->base_dir) - 1;
            }
            memcpy(image->base_dir, mme_path, len);
            image->base_dir[len] = '\0';
        } else {
            image->base_dir[0] = '.';
            image->base_dir[1] = '\0';
        }
    }

    mme = load_file(mme_path, &image->mme_size);
    if (mme == NULL) {
        return LPST_ERR_IO;
    }

    obj_file = fopen(obj_path, "rb");
    if (obj_file == NULL) {
        free(mme);
        return LPST_ERR_IO;
    }

    if (fseek(obj_file, 0, SEEK_END) != 0) {
        fclose(obj_file);
        free(mme);
        return LPST_ERR_IO;
    }

    {
        long obj_length = ftell(obj_file);

        if (obj_length < 0) {
            fclose(obj_file);
            free(mme);
            return LPST_ERR_IO;
        }

        image->obj_size = (size_t)obj_length;
    }

    if (fseek(obj_file, 0, SEEK_SET) != 0) {
        fclose(obj_file);
        free(mme);
        return LPST_ERR_IO;
    }

    if (image->mme_size < LPST_HEADER_BYTE_COUNT) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_MME_TOO_SMALL;
    }

    for (i = 0; i < LPST_HEADER_WORD_COUNT; i++) {
        image->header_words[i] = read_le16(&mme[i * 2u]);
    }

    /* header_words[7]: last code-page index (0-based).  The OBJ file ends
     * at page (header_words[7]+1), i.e. (header_words[7]+1)*512 bytes. */
    image->code_end_offset = ((uint32_t)image->header_words[7] + 1u) * 0x200u;
    /* header_words[8]: entry point packed as (module_id << 8) | proc_index. */
    image->entry_module_id = image->header_words[8] >> 8;
    image->entry_procedure_index = image->header_words[8] & 0xFF;
    /* header_words[13]: module-header-table start, in 256-byte pages. */
    mod_hdr_offset = (uint32_t)image->header_words[13] * 0x100u;
    image->module_header_offset = mod_hdr_offset;
    /* header_words[15]: module-header-table length in 16-bit words. */
    image->module_header_length_words = image->header_words[15];
    mod_hdr_len_bytes = (uint32_t)image->module_header_length_words * 2u;

    if (image->code_end_offset > image->obj_size) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_OBJ_TOO_SHORT;
    }

    if (mod_hdr_offset + mod_hdr_len_bytes > image->mme_size) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_MODULE_HEADER_TRUNCATED;
    }

    /* After the fixed header: module count, then one page-offset word per module,
     * then the program-global count, then one word per module (module-global count). */
    cursor = LPST_HEADER_BYTE_COUNT;
    module_count = read_le16(&mme[cursor]);
    cursor += 2;
    if (module_count > LPST_MAX_MODULES) {
        module_count = LPST_MAX_MODULES;
    }

    image->module_count = module_count;
    /* Each module_offset_pages[i] is a 256-byte page index into the OBJ file. */
    for (i = 0; i < module_count; i++) {
        module_offset_pages[i] = read_le16(&mme[cursor]);
        cursor += 2;
    }

    program_global_count = read_le16(&mme[cursor]);
    cursor += 2;
    for (i = 0; i < module_count; i++) {
        /* Number of module-specific global slots for module i. */
        module_global_counts[i] = read_le16(&mme[cursor]);
        cursor += 2;
    }

    image->globals.program_global_count = program_global_count;
    image->globals.program_globals = (uint16_t *)calloc(program_global_count, sizeof(uint16_t));
    if (image->globals.program_globals == NULL && program_global_count > 0) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_ALLOC;
    }

    for (i = 0; i < program_global_count; i++) {
        image->globals.program_globals[i] = read_le16(&mme[cursor]);
        cursor += 2;
    }

    image->globals.module_count = module_count;
    image->globals.module_global_counts = (uint16_t *)calloc(module_count, sizeof(uint16_t));
    image->globals.module_globals = (uint16_t **)calloc(module_count, sizeof(uint16_t *));
    if ((image->globals.module_global_counts == NULL || image->globals.module_globals == NULL) && module_count > 0) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_ALLOC;
    }

    for (i = 0; i < module_count; i++) {
        uint16_t count = module_global_counts[i];
        uint16_t j;

        image->globals.module_global_counts[i] = count;
        if (count > 0) {
            image->globals.module_globals[i] = (uint16_t *)calloc(count, sizeof(uint16_t));
            if (image->globals.module_globals[i] == NULL) {
                fclose(obj_file);
                free(mme);
                lpst_image_free(image);
                return LPST_ERR_ALLOC;
            }

            for (j = 0; j < count; j++) {
                image->globals.module_globals[i][j] = read_le16(&mme[cursor]);
                cursor += 2;
            }
        }
    }

    /* The module-header table interleaves proc counts and proc-offset arrays:
     *   [proc_count_0][offset_0_0][offset_0_1]...[proc_count_1][offset_1_0]...
     * First pass: tally procedure counts to allocate a flat offset buffer. */
    {
        size_t hdr_cursor = mod_hdr_offset;
        total_procs = 0;
        for (i = 0; i < module_count; i++) {
            proc_counts[i] = read_le16(&mme[hdr_cursor]);
            hdr_cursor += 2;
            total_procs += proc_counts[i];
            hdr_cursor += (size_t)proc_counts[i] * 2u;
        }
    }

    proc_offset_buf = (uint16_t *)calloc(total_procs, sizeof(uint16_t));
    if (proc_offset_buf == NULL && total_procs > 0) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_ALLOC;
    }

    {
        size_t hdr_cursor = mod_hdr_offset;
        size_t buf_idx = 0;
        for (i = 0; i < module_count; i++) {
            uint16_t pc = read_le16(&mme[hdr_cursor]);
            uint16_t j;
            hdr_cursor += 2;
            for (j = 0; j < pc; j++) {
                proc_offset_buf[buf_idx++] = read_le16(&mme[hdr_cursor]);
                hdr_cursor += 2;
            }
        }
    }

    res = parse_modules(image, obj_file, module_offset_pages, proc_offset_buf, proc_counts);
    free(proc_offset_buf);
    if (res != LPST_OK) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return res;
    }

    /* header_words[11]: initial-RAM size (word count or byte count — see
     * infer_initial_ram_size).  The RAM snapshot follows the module-header
     * table, padded to a 256-byte page boundary. */
    initial_ram_offset = align_to_page(mod_hdr_offset + mod_hdr_len_bytes);
    initial_ram_size = infer_initial_ram_size(
        image->header_words[11],
        image->mme_size - initial_ram_offset);
    if (initial_ram_size == 0 || initial_ram_offset + initial_ram_size > image->mme_size) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_INITIAL_RAM_TRUNCATED;
    }

    image->initial_ram_offset = initial_ram_offset;
    image->initial_ram_bytes = (uint8_t *)malloc(initial_ram_size);
    if (image->initial_ram_bytes == NULL) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_ALLOC;
    }

    memcpy(image->initial_ram_bytes, &mme[initial_ram_offset], initial_ram_size);
    image->initial_ram_size = initial_ram_size;

    if (image->entry_module_id < 1 || image->entry_module_id > module_count) {
        fclose(obj_file);
        free(mme);
        lpst_image_free(image);
        return LPST_ERR_ENTRY_OUT_OF_RANGE;
    }

    {
        lpst_module *entry_mod = &image->modules[image->entry_module_id - 1];
        if (image->entry_procedure_index >= entry_mod->procedure_count) {
            fclose(obj_file);
            free(mme);
            lpst_image_free(image);
            return LPST_ERR_ENTRY_OUT_OF_RANGE;
        }
    }

    fclose(obj_file);
    free(mme);

    return LPST_OK;
}

/* Release all heap memory owned by image.  The image pointer itself is not
 * freed; the caller owns that allocation.  Safe to call on a partially
 * initialised image in error paths. */
void lpst_image_free(lpst_image *image)
{
    uint16_t i;

    if (image == NULL) {
        return;
    }

    if (image->modules != NULL) {
        for (i = 0; i < image->module_count; i++) {
            if (image->modules[i].procedures != NULL) {
                uint16_t j;

                for (j = 0; j < image->modules[i].procedure_count; j++) {
                    free(image->modules[i].procedures[j].initializers);
                }

                free(image->modules[i].procedures);
            }
        }

        free(image->modules);
        image->modules = NULL;
    }

    free(image->globals.program_globals);
    image->globals.program_globals = NULL;

    if (image->globals.module_globals != NULL) {
        for (i = 0; i < image->globals.module_count; i++) {
            free(image->globals.module_globals[i]);
        }

        free(image->globals.module_globals);
        image->globals.module_globals = NULL;
    }

    free(image->globals.module_global_counts);
    image->globals.module_global_counts = NULL;

    free(image->initial_ram_bytes);
    image->initial_ram_bytes = NULL;
}

const char *lpst_result_string(lpst_result result)
{
    switch (result) {
    case LPST_OK: return "OK";
    case LPST_ERR_NULL_ARG: return "null argument";
    case LPST_ERR_IO: return "I/O error";
    case LPST_ERR_MME_TOO_SMALL: return "MME file too small for header";
    case LPST_ERR_OBJ_TOO_SHORT: return "OBJ file shorter than code end";
    case LPST_ERR_MODULE_HEADER_TRUNCATED: return "module header table truncated";
    case LPST_ERR_INITIAL_RAM_TRUNCATED: return "initial RAM image truncated";
    case LPST_ERR_ENTRY_OUT_OF_RANGE: return "entry point out of range";
    case LPST_ERR_MODULE_SPAN_INVALID: return "invalid module span in OBJ";
    case LPST_ERR_PROCEDURE_OUT_OF_RANGE: return "procedure offset out of module span";
    case LPST_ERR_HEADER_PARSE: return "procedure header parse error";
    case LPST_ERR_ALLOC: return "memory allocation failed";
    }

    return "unknown error";
}
