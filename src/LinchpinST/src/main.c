/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * main.c -- LinchpinST entry point.
 *
 * Handles command-line argument parsing, resolves the default image path,
 * loads the VM image, selects a host (live console or transcript), and
 * drives the run loop.  Prints a timing and code-cache summary on exit.
 *
 * Run modes:
 *   inspect  -- Load the image and print a structural summary; do not run.
 *   run      -- Execute the program until HALT or the instruction limit.
 *               Uses live console mode unless output is redirected,
 *               --transcript is given, or canned input is supplied.
 *
 * Default image path resolution (no explicit path argument):
 *   1. (Atari) CORNER or AUTO/CORNER with .mme/.MME extension.
 *   2. Strip the executable's extension and try .mme / .MME.
 *   3. (Atari) Search AUTO/ for a file matching the executable base name.
 *   4. (Atari) Find the one .MME file in the current directory, if unique.
 *
 * Data directory resolution (no --data argument or LINCHPIN_DATA_DIR env var):
 *   Searches for a Sample/ subdirectory of the image directory, its parent,
 *   and the current directory.  On Atari, also checks the parent of an AUTO
 *   folder.  Falls back to a top-level Sample/sample/SAMPLE in the cwd.
 */
#include "lpst_exec.h"
#include "lpst_host.h"
#include "lpst_image.h"
#include "lpst_platform.h"
#include "lpst_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* When defined, make LinchpinST behave like a terse 1986-style tool. */
/* #define COVERUP */

#if LPST_PLATFORM_POSIX
#include <sys/time.h>
#include <unistd.h>
#define LPST_HAS_ISATTY 1
#else
#define LPST_HAS_ISATTY 0
#endif

#if LPST_PLATFORM_ATARI
#include <mint/basepage.h>
#include <mint/ostruct.h>
#include <mint/sysbind.h>
#include <sys/time.h>
#endif

#define LPST_DEFAULT_TRANSCRIPT_LIMIT 5000000u
#define LPST_SMALL_HOST_TRANSCRIPT_LIMIT 50000u

/* Timing breakdown for a single Linchpin run, measured in microseconds. */
typedef struct lpst_runtime_timings {
    uint64_t default_image_resolution_microseconds;
    uint64_t image_load_microseconds;
    uint64_t host_setup_microseconds;
    uint64_t exec_init_microseconds;
    uint64_t startup_microseconds;
    uint64_t run_microseconds;
    uint64_t transcript_render_microseconds;
    uint64_t total_microseconds;
} lpst_runtime_timings;

/*
 * Return the current time as a microsecond count.
 * Uses gettimeofday() on POSIX and Atari for the best available resolution.
 * Falls back to clock() on platforms that provide neither, at the coarser
 * granularity of CLOCKS_PER_SEC.
 */
static uint64_t lpst_now_microseconds(void)
{
#if LPST_PLATFORM_POSIX || LPST_PLATFORM_ATARI
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == 0) {
        return (uint64_t)tv.tv_sec * UINT64_C(1000000) + (uint64_t)tv.tv_usec;
    }
#endif

    return (uint64_t)clock() * UINT64_C(1000000) / (uint64_t)CLOCKS_PER_SEC;
}

static uint64_t lpst_elapsed_microseconds(uint64_t start_microseconds)
{
    uint64_t now_microseconds = lpst_now_microseconds();

    return now_microseconds >= start_microseconds ? now_microseconds - start_microseconds : 0;
}

static bool file_exists(const char *path)
{
    FILE *file;

    if (path == NULL) {
        return false;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    fclose(file);
    return true;
}

static const char *find_extension(const char *path)
{
    const char *last_dot = NULL;
    const char *cursor = path;

    while (*cursor != '\0') {
        if (*cursor == '/' || *cursor == '\\' || *cursor == ':') {
            last_dot = NULL;
        } else if (*cursor == '.') {
            last_dot = cursor;
        }

        cursor++;
    }

    return last_dot;
}

static char *replace_extension(const char *path, const char *new_extension)
{
    const char *ext = find_extension(path);
    size_t stem_length = ext != NULL ? (size_t)(ext - path) : strlen(path);
    size_t extension_length = strlen(new_extension);
    char *result = malloc(stem_length + extension_length + 1);

    if (result == NULL) {
        return NULL;
    }

    memcpy(result, path, stem_length);
    memcpy(result + stem_length, new_extension, extension_length + 1);
    return result;
}

static const char *find_last_path_separator(const char *path)
{
    const char *last = NULL;

    while (path != NULL && *path != '\0') {
        if (*path == '/' || *path == '\\' || *path == ':') {
            last = path;
        }

        path++;
    }

    return last;
}

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length;
    size_t name_length;
    bool needs_separator;
    char *result;

    if (directory == NULL || *directory == '\0' || name == NULL || *name == '\0') {
        return NULL;
    }

    directory_length = strlen(directory);
    name_length = strlen(name);
    needs_separator = directory[directory_length - 1] != '/'
        && directory[directory_length - 1] != '\\'
        && directory[directory_length - 1] != ':';
    result = malloc(directory_length + (needs_separator ? 1u : 0u) + name_length + 1u);
    if (result == NULL) {
        return NULL;
    }

    memcpy(result, directory, directory_length);
    if (needs_separator) {
        result[directory_length++] = '/';
    }

    memcpy(result + directory_length, name, name_length + 1u);
    return result;
}

#if LPST_PLATFORM_ATARI
static char *extract_base_name(const char *path)
{
    const char *separator;
    const char *name;
    const char *extension;
    size_t stem_length;
    char *result;

    if (path == NULL || *path == '\0') {
        return NULL;
    }

    separator = find_last_path_separator(path);
    name = separator != NULL ? separator + 1 : path;
    extension = find_extension(name);
    stem_length = extension != NULL ? (size_t)(extension - name) : strlen(name);
    result = malloc(stem_length + 1u);
    if (result == NULL) {
        return NULL;
    }

    memcpy(result, name, stem_length);
    result[stem_length] = '\0';
    return result;
}
#endif

/*
 * Probe for a .mme or .MME file given a base path without any extension.
 * Returns a heap-allocated path to the first variant found on disk,
 * or NULL if neither exists.  The case is preserved so the pairing is
 * correct on both case-sensitive and case-insensitive file systems.
 */
static char *find_default_mme_path_for_base_path(const char *base_path)
{
    static const char *const candidates[] = { ".mme", ".MME" };
    size_t i;

    if (base_path == NULL || *base_path == '\0') {
        return NULL;
    }

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        size_t base_length = strlen(base_path);
        size_t extension_length = strlen(candidates[i]);
        char *candidate = malloc(base_length + extension_length + 1);

        if (candidate == NULL) {
            return NULL;
        }

        memcpy(candidate, base_path, base_length);
        memcpy(candidate + base_length, candidates[i], extension_length + 1);
        if (file_exists(candidate)) {
            return candidate;
        }

        free(candidate);
    }

    return NULL;
}

#if LPST_PLATFORM_ATARI
/*
 * Try the Atari boot-time default image paths: CORNER and AUTO/CORNER.
 * These are the conventional image locations for a Cornerstone installation
 * loaded via the Atari AUTO folder or placed directly on the boot drive.
 * Returns a heap-allocated .mme path, or NULL if none is found.
 */
static char *find_atari_boot_default_mme_path(void)
{
    static const char *const candidates[] = {
        "CORNER",
        "AUTO/CORNER",
        "AUTO\\CORNER"
    };
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *candidate = find_default_mme_path_for_base_path(candidates[i]);

        if (candidate != NULL) {
            return candidate;
        }
    }

    return NULL;
}
#endif

#if LPST_PLATFORM_ATARI
/*
 * Search a named directory for a .mme file whose base name matches
 * the program name derived from program_path.
 * Used on Atari to find the image in AUTO/ when the executable was
 * started from there.
 * Returns a heap-allocated path, or NULL if no match is found.
 */
static char *find_default_mme_path_in_directory(const char *directory, const char *program_path)
{
    char *base_name;
    char *joined_path;
    char *candidate;

    base_name = extract_base_name(program_path);
    if (base_name == NULL) {
        return NULL;
    }

    joined_path = join_path(directory, base_name);
    free(base_name);
    if (joined_path == NULL) {
        return NULL;
    }

    candidate = find_default_mme_path_for_base_path(joined_path);
    free(joined_path);
    return candidate;
}
#endif

/*
 * Derive the .OBJ bytecode file path from a .MME metadata path.
 * An uppercase .MME extension yields an uppercase .OBJ, to match the
 * original Cornerstone file naming on case-sensitive and Atari file systems.
 */
static char *infer_obj_path(const char *mme_path)
{
    const char *ext = find_extension(mme_path);
    if (ext != NULL && strcmp(ext, ".MME") == 0) {
        return replace_extension(mme_path, ".OBJ");
    }

    return replace_extension(mme_path, ".obj");
}

/*
 * Find the default .MME path by stripping the executable's extension.
 * Tries .mme and .MME on the stem, so a binary named "corner" would look
 * for "corner.mme" and then "corner.MME" in the same directory.
 * Returns a heap-allocated path, or NULL if neither variant is found.
 */
static char *find_default_mme_path(const char *argv0)
{
    char *base_path;

    if (argv0 == NULL || *argv0 == '\0') {
        return NULL;
    }

    base_path = replace_extension(argv0, "");
    if (base_path == NULL) {
        return NULL;
    }

    {
        char *candidate = find_default_mme_path_for_base_path(base_path);

        free(base_path);
        return candidate;
    }
}

#if LPST_PLATFORM_ATARI
/*
 * Adjust the Atari working directory before running.
 * When the image was launched from an AUTO folder, the working directory
 * may be AUTO\ rather than the drive root.  Change to the root so that
 * relative file paths in the Cornerstone program resolve correctly.
 */
static void normalize_atari_run_working_directory(const char *mme_path)
{
    if (mme_path == NULL) {
        return;
    }

    if (strncmp(mme_path, "AUTO/", 5) == 0 || strncmp(mme_path, "AUTO\\", 5) == 0) {
        Dsetpath("\\");
    }
}
#endif

#if LPST_PLATFORM_ATARI
/*
 * Find the single .MME file in the current directory using the Atari DTA.
 * Returns a heap-allocated filename if exactly one .MME is found.
 * Returns NULL if there are zero or more than one, to avoid ambiguity
 * when multiple programs are installed in the same directory.
 */
static char *find_single_mme_in_current_directory(void)
{
    _DTA *previous_dta;
    _DTA dta;
    long result;
    int match_count = 0;
    char *match = NULL;

    previous_dta = Fgetdta();
    Fsetdta(&dta);

    result = Fsfirst("*.MME", 0);
    while (result == 0) {
        char *candidate;
        size_t length;

        length = strlen(dta.dta_name);
        candidate = malloc(length + 1);
        if (candidate == NULL) {
            free(match);
            match = NULL;
            match_count = 0;
            break;
        }

        memcpy(candidate, dta.dta_name, length + 1);
        free(match);
        match = candidate;
        match_count++;
        if (match_count > 1) {
            break;
        }

        result = Fsnext();
    }

    Fsetdta(previous_dta);

    if (match_count == 1) {
        return match;
    }

    free(match);
    return NULL;
}
#endif

/*
 * Return the path of the running executable.
 * On Atari, falls back to the path stored in the program base page when
 * argv[0] is empty or unavailable, because TOS boot often provides a
 * truncated or zero-length argv[0].
 * Returns a heap-allocated string, or NULL on failure.
 */
static char *get_executable_path(const char *argv0)
{
    if (argv0 != NULL && *argv0 != '\0') {
        return replace_extension(argv0, find_extension(argv0) != NULL ? find_extension(argv0) : "");
    }

#if LPST_PLATFORM_ATARI
    if (_base != NULL && _base->p_env != NULL) {
        char *cursor = _base->p_env;

        while (*cursor != '\0') {
            cursor += strlen(cursor) + 1;
        }

        cursor++;
        if (*cursor != '\0') {
            return replace_extension(cursor, find_extension(cursor) != NULL ? find_extension(cursor) : "");
        }
    }
#endif

    return NULL;
}

static void set_image_data_dir(lpst_image *image, const char *path)
{
    if (image == NULL) {
        return;
    }

    image->data_dir[0] = '\0';
    if (path == NULL || *path == '\0') {
        return;
    }

    snprintf(image->data_dir, sizeof(image->data_dir), "%s", path);
}

static void set_data_dir(lpst_image *image, lpst_host *host, const char *path)
{
    set_image_data_dir(image, path);
    lpst_host_transcript_set_data_dir(host, path);
}

static bool directory_exists(const char *path)
{
    struct stat st;

    if (path == NULL || *path == '\0') {
        return false;
    }

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *extract_directory(const char *path)
{
    const char *last_sep;
    size_t length;
    char *result;

    if (path == NULL || *path == '\0') {
        return NULL;
    }

    last_sep = find_last_path_separator(path);
    if (last_sep == NULL) {
        return NULL;
    }

    length = (size_t)(last_sep - path);
    if (*last_sep == ':') {
        length++;
    }

    if (length == 0) {
        return NULL;
    }

    result = malloc(length + 1u);
    if (result == NULL) {
        return NULL;
    }

    memcpy(result, path, length);
    result[length] = '\0';
    return result;
}

#if LPST_PLATFORM_ATARI
static int ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }

    return ch;
}

static bool ascii_equals_ci(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (ascii_tolower((unsigned char)*left) != ascii_tolower((unsigned char)*right)) {
            return false;
        }

        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static bool path_last_component_equals_ci(const char *path, const char *component)
{
    const char *last_sep;

    if (path == NULL || component == NULL) {
        return false;
    }

    last_sep = find_last_path_separator(path);
    return ascii_equals_ci(last_sep != NULL ? last_sep + 1 : path, component);
}
#endif

/*
 * Search a directory for a Sample subdirectory and configure it as the data dir.
 * Tries the names "sample", "Sample", and "SAMPLE" to handle case-sensitive
 * and case-insensitive file systems.  Sets both the image data_dir field and
 * the transcript host data directory.  Returns true if a match is found.
 */
static bool try_set_data_dir_in_directory(lpst_image *image, lpst_host *host, const char *directory)
{
    static const char *const sample_names[] = { "sample", "Sample", "SAMPLE" };
    size_t i;

    for (i = 0; i < sizeof(sample_names) / sizeof(sample_names[0]); i++) {
        char *candidate = join_path(directory, sample_names[i]);

        if (candidate != NULL && directory_exists(candidate)) {
            set_data_dir(image, host, candidate);
            free(candidate);
            return true;
        }

        free(candidate);
    }

    return false;
}

#if LPST_PLATFORM_ATARI
static bool try_get_atari_current_directory(char *out, size_t out_size)
{
    char current[260];

    if (out == NULL || out_size == 0) {
        return false;
    }

    if (Dgetpath(current, Dgetdrv() + 1) != 0) {
        return false;
    }

    if (current[0] == '\0') {
        snprintf(out, out_size, "\\");
    } else {
        snprintf(out, out_size, "%s", current);
    }

    return true;
}

/*
 * On Atari, try to resolve the data directory when the image is in AUTO/.
 * If @p path ends with the component "AUTO", walks up to the parent
 * directory and searches there for a Sample subdirectory, per the
 * expected layout where Sample/ and AUTO/ are sibling directories.
 * Returns false for paths that do not end with "AUTO".
 */
static bool try_set_atari_auto_data_dir(lpst_image *image, lpst_host *host, const char *path)
{
    char *parent_dir;
    bool result;

    if (!path_last_component_equals_ci(path, "AUTO")) {
        return false;
    }

    parent_dir = extract_directory(path);
    if (parent_dir != NULL) {
        result = try_set_data_dir_in_directory(image, host, parent_dir);
        free(parent_dir);
        return result;
    }

    return try_set_data_dir_in_directory(image, host, "..");
}
#endif

/*
 * Automatically locate the Cornerstone data directory relative to the image.
 * Searches in priority order:
 *   1. (Atari) If the image is in AUTO/, check its parent directory.
 *   2. (Atari) If the cwd is AUTO/, check its parent directory.
 *   3. A Sample/ subdirectory of the image's own directory.
 *   4. A Sample/ subdirectory of the image's parent directory.
 *   5. A Sample/ subdirectory of the current directory.
 *   6. A top-level Sample/, sample/, or SAMPLE/ in the current directory.
 * Does nothing and returns false if none of the above are found.
 */
static bool try_set_default_data_dir(lpst_image *image, lpst_host *host, const char *mme_path)
{
    char *image_dir;
    char *parent_dir;

#if LPST_PLATFORM_ATARI
    {
        char current_dir[260];

        image_dir = extract_directory(mme_path);
        if (try_set_atari_auto_data_dir(image, host, image_dir)) {
            free(image_dir);
            return true;
        }

        free(image_dir);

        if (try_get_atari_current_directory(current_dir, sizeof(current_dir)) &&
            try_set_atari_auto_data_dir(image, host, current_dir)) {
            return true;
        }
    }
#endif

    image_dir = extract_directory(mme_path);
    if (image_dir == NULL) {
        if (try_set_data_dir_in_directory(image, host, ".")) {
            return true;
        }

        return directory_exists("Sample")
            ? (set_data_dir(image, host, "Sample"), true)
            : directory_exists("sample")
                ? (set_data_dir(image, host, "sample"), true)
                : directory_exists("SAMPLE")
                    ? (set_data_dir(image, host, "SAMPLE"), true)
                    : false;
    }

    if (try_set_data_dir_in_directory(image, host, image_dir)) {
        free(image_dir);
        return true;
    }

    parent_dir = extract_directory(image_dir);
    if (parent_dir != NULL) {
        if (try_set_data_dir_in_directory(image, host, parent_dir)) {
            free(parent_dir);
            free(image_dir);
            return true;
        }

        free(parent_dir);
    }

    free(image_dir);

    if (try_set_data_dir_in_directory(image, host, ".")) {
        return true;
    }

    if (directory_exists("Sample")) {
        set_data_dir(image, host, "Sample");
        return true;
    }

    if (directory_exists("sample")) {
        set_data_dir(image, host, "sample");
        return true;
    }

    if (directory_exists("SAMPLE")) {
        set_data_dir(image, host, "SAMPLE");
        return true;
    }

    return false;
}

static void print_summary(const lpst_image *image, const lpst_exec_state *state)
{
    uint16_t i;
    uint16_t total_procs = 0;

    printf("Entry selector: module %u, proc %u\n",
        image->entry_module_id, image->entry_procedure_index);
    printf("Code end: 0x%05X, module headers: 0x%04X (%u words)\n",
        image->code_end_offset,
        image->module_header_offset,
        image->module_header_length_words);
    printf("Initial RAM: 0x%04X, %u bytes\n",
        image->initial_ram_offset,
        (unsigned)image->initial_ram_size);
    printf("Program globals: %u, module globals:",
        image->globals.program_global_count);
    for (i = 0; i < image->globals.module_count; i++) {
        printf(" %u", image->globals.module_global_counts[i]);
        if (i + 1 < image->globals.module_count) {
            printf(",");
        }
    }

    printf("\n");

    printf("Bootstrap state: module %u, pc 0x%04X, locals %u, stack depth %u\n",
        state->current_module_id,
        state->program_counter,
        (unsigned)state->current_frame.local_count,
        state->eval_stack_top);

    for (i = 0; i < image->module_count; i++) {
        total_procs += image->modules[i].procedure_count;
    }

    printf("Modules: %u, procedures: %u\n\n", image->module_count, total_procs);

    for (i = 0; i < image->module_count; i++) {
        const lpst_module *mod = &image->modules[i];
        printf("Module %2u: OBJ 0x%05X-0x%05X, procedures %3u, globals %3u\n",
            mod->module_id,
            mod->object_offset,
            mod->object_offset + mod->length - 1,
            mod->procedure_count,
            image->globals.module_global_counts[i]);
    }
}

static void print_cache_stats(FILE *stream, const lpst_exec_state *state)
{
    size_t valid_entries = 0;
    size_t index;

    if (stream == NULL || state == NULL) {
        return;
    }

    if (state->code_cache_entries != NULL) {
        for (index = 0; index < state->code_cache_entry_count; index++) {
            if (state->code_cache_entries[index].valid) {
                valid_entries++;
            }
        }
    }

    fprintf(stream,
        "Code cache: %u hits, %u misses, %u evictions, %u/%u pages resident (%u bytes capacity).\n",
        state->code_cache_hits,
        state->code_cache_misses,
        state->code_cache_evictions,
        (unsigned)valid_entries,
        (unsigned)state->code_cache_entry_count,
        (unsigned)(state->code_cache_entry_count * LPST_CODE_PAGE_SIZE));
}

static double microseconds_to_milliseconds(uint64_t microseconds)
{
    return (double)microseconds / 1000.0;
}

static void print_runtime_timings(FILE *stream, const lpst_runtime_timings *timings)
{
    if (stream == NULL || timings == NULL) {
        return;
    }

    fprintf(stream,
        "Timing: startup %.3f ms (default image %.3f, image load %.3f, host setup %.3f, exec init %.3f), "
        "run %.3f ms, render %.3f ms, total %.3f ms.\n",
        microseconds_to_milliseconds(timings->startup_microseconds),
        microseconds_to_milliseconds(timings->default_image_resolution_microseconds),
        microseconds_to_milliseconds(timings->image_load_microseconds),
        microseconds_to_milliseconds(timings->host_setup_microseconds),
        microseconds_to_milliseconds(timings->exec_init_microseconds),
        microseconds_to_milliseconds(timings->run_microseconds),
        microseconds_to_milliseconds(timings->transcript_render_microseconds),
        microseconds_to_milliseconds(timings->total_microseconds));
}

static void print_runtime_timings_compact(FILE *stream, const lpst_runtime_timings *timings)
{
    if (stream == NULL || timings == NULL) {
        return;
    }

    fprintf(stream,
        "T ms: start %.0f load %.0f host %.0f init %.0f run %.0f total %.0f\n",
        microseconds_to_milliseconds(timings->startup_microseconds),
        microseconds_to_milliseconds(timings->image_load_microseconds),
        microseconds_to_milliseconds(timings->host_setup_microseconds),
        microseconds_to_milliseconds(timings->exec_init_microseconds),
        microseconds_to_milliseconds(timings->run_microseconds),
        microseconds_to_milliseconds(timings->total_microseconds));
    fflush(stream);
}

/*
 * Decode backslash escape sequences in a text string into raw bytes.
 * Converts \r to CR (0x0D), \n to LF (0x0A), \t to TAB (0x09), and \\
 * to a single backslash.  Used to translate --input-text values such as
 * "quit\r" into the actual key bytes expected by the VM's keyboard handler.
 * Returns the number of bytes written to @p out.
 */
static size_t decode_input_text(const char *text, uint8_t *out, size_t max_len)
{
    size_t pos = 0;
    const char *p = text;

    while (*p != '\0' && pos < max_len) {
        if (*p == '\\' && p[1] != '\0') {
            switch (p[1]) {
            case 'r': out[pos++] = 0x0D; p += 2; break;
            case 'n': out[pos++] = 0x0A; p += 2; break;
            case 't': out[pos++] = 0x09; p += 2; break;
            case '\\': out[pos++] = '\\'; p += 2; break;
            default: out[pos++] = (uint8_t)*p; p++; break;
            }
        } else {
            out[pos++] = (uint8_t)*p;
            p++;
        }
    }

    return pos;
}

/*
 * Read the entire contents of a binary file into a heap-allocated buffer.
 * Stores the byte count in *out_len on success.  Returns NULL on any
 * I/O or allocation error.  Used to load raw key-sequence files for
 * scripted test input (--input-file).
 */
static uint8_t *read_input_file(const char *path, size_t *out_len)
{
    FILE *file;
    long length;
    uint8_t *buffer;
    size_t read_count;

    if (out_len != NULL) {
        *out_len = 0;
    }

    if (path == NULL) {
        return NULL;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = malloc(length > 0 ? (size_t)length : 1u);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_count = length > 0 ? fread(buffer, 1, (size_t)length, file) : 0u;
    fclose(file);
    if (read_count != (size_t)length) {
        free(buffer);
        return NULL;
    }

    if (out_len != NULL) {
        *out_len = (size_t)length;
    }

    return buffer;
}

static void trim_trailing_blank_lines(char *text)
{
    size_t length;

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    while (length > 0 && text[length - 1] == '\n') {
        text[--length] = '\0';
    }
}

static void write_json_escaped(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    while (cursor != NULL && *cursor != '\0') {
        switch (*cursor) {
        case '\\':
            fputs("\\\\", stream);
            break;
        case '"':
            fputs("\\\"", stream);
            break;
        case '\n':
            fputs("\\n", stream);
            break;
        case '\r':
            fputs("\\r", stream);
            break;
        case '\t':
            fputs("\\t", stream);
            break;
        default:
            if (*cursor < 0x20) {
                fprintf(stream, "\\u%04X", (unsigned)*cursor);
            } else {
                fputc((int)*cursor, stream);
            }
            break;
        }

        cursor++;
    }
}

/*
 * Write a JSON test report to @p path.
 * The report contains the halt code, executed instruction count, the final
 * screen text (with special characters JSON-escaped), and all timing fields
 * from @p timings.  Used by automated test harnesses to validate VM output
 * and measure performance.
 * Returns true on success, false if the file cannot be opened or written.
 */
static bool write_test_report(
    const char *path,
    const lpst_exec_state *state,
    const char *screen_text,
    const lpst_runtime_timings *timings)
{
    FILE *file;

    if (path == NULL || state == NULL || screen_text == NULL || timings == NULL) {
        return false;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }

    fputs("{\n  \"haltCode\": ", file);
    fprintf(file, "%u", state->halt_code);
    fputs(",\n  \"executedInstructionCount\": ", file);
    fprintf(file, "%u", state->instruction_count);
    fputs(",\n  \"screenText\": \"", file);
    write_json_escaped(file, screen_text);
    fputs("\",\n  \"defaultImageResolutionMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->default_image_resolution_microseconds);
    fputs(",\n  \"imageLoadMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->image_load_microseconds);
    fputs(",\n  \"hostSetupMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->host_setup_microseconds);
    fputs(",\n  \"execInitMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->exec_init_microseconds);
    fputs(",\n  \"startupMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->startup_microseconds);
    fputs(",\n  \"runMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->run_microseconds);
    fputs(",\n  \"transcriptRenderMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->transcript_render_microseconds);
    fputs(",\n  \"totalMicroseconds\": ", file);
    fprintf(file, "%llu", (unsigned long long)timings->total_microseconds);
    fputs("\n}\n", file);

    fclose(file);
    return true;
}

static void print_usage(void)
{
#ifdef COVERUP
    fprintf(stderr, "ERROR: Incorrect usage\n");
#else
    fprintf(stderr,
        "Usage: linchpin_st <command> <mme-path> [options]\n"
        "Commands:\n"
        "  inspect   Show image summary\n"
        "  run       Execute the program\n"
        "Options:\n"
        "  --input-text <text>  Provide canned keyboard input (\\r=CR \\n=LF)\n"
        "  --input-file <path>  Provide canned keyboard input from a raw byte file\n"
        "  --data <path>        Use an alternate data directory instead of Sample\n"
        "  --limit <n>          Set instruction limit (default: none in interactive, 5M in transcript)\n"
        "  --test-report <path> Write a JSON runtime report\n"
        "  --transcript         Force transcript mode (no live console)\n"
        "  --screen-width <n>   Override screen width (default: auto-detect or 80)\n");
#endif
}

int main(int argc, char *argv[])
{
    const char *command = NULL;
    const char *mme_path = NULL;
    char *executable_path = NULL;
    char *default_mme_path = NULL;
    char *obj_path = NULL;
    const char *input_text = NULL;
    const char *input_file_path = NULL;
    const char *data_dir_path = NULL;
    const char *test_report_path = NULL;
    uint32_t limit = 0;
    bool limit_set = false;
    bool trace = false;
    bool force_transcript = false;
    int screen_width_override = 0;
    int positional = 0;
    int i;
    lpst_image *image;
    lpst_result res;
    lpst_host *host;
    lpst_exec_state *state;
    lpst_runtime_timings timings;
    uint64_t process_start_microseconds;
    int exit_code = 0;

    memset(&timings, 0, sizeof(timings));
    process_start_microseconds = lpst_now_microseconds();

    image = calloc(1, sizeof(*image));
    host = calloc(1, sizeof(*host));
    state = calloc(1, sizeof(*state));
    if (image == NULL || host == NULL || state == NULL) {
        fprintf(stderr, "Out of memory.\n");
        free(state);
        free(host);
        free(image);
        return 1;
    }

    for (i = 1; i < argc; i++) {
#ifdef COVERUP
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir_path = argv[++i];
        } else if (strcmp(argv[i], "--screen-width") == 0 && i + 1 < argc) {
            screen_width_override = (int)strtol(argv[++i], NULL, 10);
            if (screen_width_override < 1 || screen_width_override > LPST_SCREEN_WIDTH) {
                /* Under COVERUP, be terse and treat as incorrect usage */
                print_usage();
                exit_code = 1;
                goto cleanup;
            }
        } else if (argv[i][0] != '-') {
            switch (positional) {
            case 0: command = argv[i]; break;
            case 1: mme_path = argv[i]; break;
            default: break;
            }
            positional++;
        } else {
            /* Ignore any other options when COVERUP is enabled */
        }
#else
        if (strcmp(argv[i], "--input-text") == 0 && i + 1 < argc) {
            if (input_file_path != NULL) {
                fprintf(stderr, "Use either --input-text or --input-file, not both.\n");
                exit_code = 1;
                goto cleanup;
            }

            input_text = argv[++i];
        } else if (strcmp(argv[i], "--input-file") == 0 && i + 1 < argc) {
            if (input_text != NULL) {
                fprintf(stderr, "Use either --input-text or --input-file, not both.\n");
                exit_code = 1;
                goto cleanup;
            }

            input_file_path = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir_path = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = (uint32_t)strtoul(argv[++i], NULL, 10);
            limit_set = true;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--transcript") == 0) {
            force_transcript = true;
        } else if (strcmp(argv[i], "--test-report") == 0 && i + 1 < argc) {
            test_report_path = argv[++i];
        } else if (strcmp(argv[i], "--screen-width") == 0 && i + 1 < argc) {
            screen_width_override = (int)strtol(argv[++i], NULL, 10);
            if (screen_width_override < 1 || screen_width_override > LPST_SCREEN_WIDTH) {
                fprintf(stderr, "--screen-width must be between 1 and %d.\n", LPST_SCREEN_WIDTH);
                exit_code = 1;
                goto cleanup;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit_code = 0;
            goto cleanup;
        } else if (argv[i][0] != '-') {
            switch (positional) {
            case 0: command = argv[i]; break;
            case 1: mme_path = argv[i]; break;
            default: break;
            }
            positional++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage();
            exit_code = 1;
            goto cleanup;
        }
#endif
    }

    executable_path = get_executable_path(argv[0]);

    if (command == NULL && positional == 0) {
        uint64_t default_resolution_start_microseconds = lpst_now_microseconds();

        default_mme_path = NULL;
#if LPST_PLATFORM_ATARI
        if (default_mme_path == NULL) {
            default_mme_path = find_atari_boot_default_mme_path();
        }
        if (default_mme_path == NULL) {
            default_mme_path = find_default_mme_path(executable_path);
        }
        if (default_mme_path == NULL) {
            default_mme_path = find_default_mme_path_in_directory("AUTO", executable_path);
        }
        if (default_mme_path == NULL) {
            default_mme_path = find_single_mme_in_current_directory();
        }
#else
        default_mme_path = find_default_mme_path(executable_path);
#endif
        if (default_mme_path == NULL) {
#ifndef COVERUP
            fprintf(stderr,
                "No default .mme file found for executable: %s\n",
                executable_path != NULL ? executable_path : "<unknown>");
#endif
            print_usage();
            exit_code = 1;
            goto cleanup;
        }

        command = "run";
        mme_path = default_mme_path;
        timings.default_image_resolution_microseconds = lpst_elapsed_microseconds(default_resolution_start_microseconds);
    }

    if (command == NULL || mme_path == NULL) {
        print_usage();
        exit_code = 1;
        goto cleanup;
    }

    obj_path = infer_obj_path(mme_path);
    if (obj_path == NULL) {
        fprintf(stderr, "Out of memory.\n");
        exit_code = 1;
        goto cleanup;
    }

#if LPST_PLATFORM_ATARI
    if (strcmp(command, "run") == 0) {
        normalize_atari_run_working_directory(mme_path);
    }
#endif

#if LPST_PLATFORM_ATARI
    if (strcmp(command, "run") == 0) {
        printf("Loading, please wait...\n");
        fflush(stdout);
    }
#endif

    {
        uint64_t image_load_start_microseconds = lpst_now_microseconds();
        res = lpst_image_load(image, mme_path, obj_path);
        timings.image_load_microseconds = lpst_elapsed_microseconds(image_load_start_microseconds);
    }

    if (res != LPST_OK) {
        fprintf(stderr, "Failed to load image: %s\n", lpst_result_string(res));
        exit_code = 1;
        goto cleanup;
    }

    {
        uint64_t host_setup_start_microseconds = lpst_now_microseconds();

        lpst_host_transcript_init(host, NULL, 0);

        /* Set up data directory for file I/O: use --data, then LINCHPIN_DATA_DIR,
           or fall back to a default Sample directory only when neither is supplied. */
        if (data_dir_path != NULL && data_dir_path[0] != '\0') {
            set_data_dir(image, host, data_dir_path);
        } else {
            const char *env_dir = getenv("LINCHPIN_DATA_DIR");
            if (env_dir != NULL && env_dir[0] != '\0') {
                set_data_dir(image, host, env_dir);
            } else if (mme_path != NULL) {
                try_set_default_data_dir(image, host, mme_path);
            }
        }

        timings.host_setup_microseconds = lpst_elapsed_microseconds(host_setup_start_microseconds);
    }

    {
        uint64_t exec_init_start_microseconds = lpst_now_microseconds();
        res = lpst_exec_init(state, image, host);
        timings.exec_init_microseconds = lpst_elapsed_microseconds(exec_init_start_microseconds);
    }

    if (res != LPST_OK) {
        fprintf(stderr, "Failed to initialize execution state: %s\n", lpst_result_string(res));
        exit_code = 1;
        goto cleanup;
    }

    if (trace) {
        state->trace_enabled = true;
    }

    if (strcmp(command, "inspect") == 0) {
        print_summary(image, state);
    } else if (strcmp(command, "run") == 0) {
        /*
         * Mode selection:
         *   --transcript or --input-text or stdout redirected → transcript mode
         *   otherwise → live console mode
         */
        bool use_console = false;
        lpst_result run_res;
        uint8_t *input_keys = NULL;
        char *screen_buf = NULL;
        char *normalized_screen = NULL;
        size_t input_len = 0;

        use_console = !force_transcript && input_text == NULL && input_file_path == NULL && test_report_path == NULL;
    #if LPST_HAS_ISATTY
        use_console = use_console && isatty(STDOUT_FILENO);
#endif

        if (limit_set && limit > 0) {
            state->instruction_limit = limit;
        } else if (use_console) {
            state->instruction_limit = 0;
        } else {
#if LPST_PLATFORM_ATARI || LPST_PLATFORM_AMIGA
            state->instruction_limit = LPST_SMALL_HOST_TRANSCRIPT_LIMIT;
#else
            state->instruction_limit = LPST_DEFAULT_TRANSCRIPT_LIMIT;
#endif
        }

        if (input_text != NULL) {
            input_keys = malloc(4096);
            if (input_keys == NULL) {
                fprintf(stderr, "Out of memory.\n");
                exit_code = 1;
                goto cleanup;
            }
        } else if (input_file_path != NULL) {
            input_keys = read_input_file(input_file_path, &input_len);
            if (input_keys == NULL) {
                fprintf(stderr, "Failed to read input file: %s\n", input_file_path);
                exit_code = 1;
                goto cleanup;
            }
        }

        if (use_console) {
            /* Reinitialize host as live console */
            uint64_t console_setup_start_microseconds = lpst_now_microseconds();

            lpst_host_console_init(host);
            timings.host_setup_microseconds += lpst_elapsed_microseconds(console_setup_start_microseconds);

            if (screen_width_override > 0) {
                host->screen_width = (uint8_t)screen_width_override;
                lpst_host_console_set_screen_width(host, (uint8_t)screen_width_override);
            }

            /* Sync system variable 0xC7 with the host's screen width, which
               may have changed due to auto-detection or --screen-width. */
            state->system_module_globals[0xC7] = (uint16_t)(host->screen_width - 1u);

            if (input_text != NULL) {
                input_len = decode_input_text(input_text, input_keys, 4096);
                lpst_host_console_set_input(host, input_keys, input_len);
            } else if (input_file_path != NULL) {
                lpst_host_console_set_input(host, input_keys, input_len);
            }
        } else {
            if (screen_width_override > 0) {
                host->screen_width = (uint8_t)screen_width_override;
                state->system_module_globals[0xC7] = (uint16_t)(screen_width_override - 1);
            }

            if (input_text != NULL) {
                input_len = decode_input_text(input_text, input_keys, 4096);
                lpst_host_transcript_set_input(host, input_keys, input_len);
            } else if (input_file_path != NULL) {
                lpst_host_transcript_set_input(host, input_keys, input_len);
            }
        }

        timings.startup_microseconds = lpst_elapsed_microseconds(process_start_microseconds);

        {
            uint64_t run_start_microseconds = lpst_now_microseconds();
            run_res = lpst_run(state);
            timings.run_microseconds = lpst_elapsed_microseconds(run_start_microseconds);
        }

        if (use_console) {
            timings.total_microseconds = lpst_elapsed_microseconds(process_start_microseconds);
            lpst_host_console_cleanup(host);
            if (run_res != LPST_OK) {
                fprintf(stderr, "Run error: %s\n", lpst_result_string(run_res));
            }
            lpst_trace_halt_context(state, stderr);
#ifdef COVERUP
            if (state->halt_code != 1) {
                fprintf(stderr, "ERROR: halt code %u\n", state->halt_code);
            }
#else
            fprintf(stderr, "Halted with code %u after %u instructions.\n",
                    state->halt_code, state->instruction_count);
            print_cache_stats(stderr, state);
            print_runtime_timings_compact(stderr, &timings);
#endif
        } else {
            screen_buf = malloc(LPST_SCREEN_HEIGHT * (LPST_SCREEN_WIDTH + 2) + 1);
            if (screen_buf == NULL) {
                free(input_keys);
                fprintf(stderr, "Out of memory.\n");
                exit_code = 1;
                goto cleanup;
            }

            if (run_res != LPST_OK) {
                fprintf(stderr, "Run error: %s\n", lpst_result_string(run_res));
            }

            {
                uint64_t transcript_render_start_microseconds = lpst_now_microseconds();

                lpst_host_transcript_render(host, screen_buf,
                    LPST_SCREEN_HEIGHT * (LPST_SCREEN_WIDTH + 2) + 1);
                timings.transcript_render_microseconds = lpst_elapsed_microseconds(transcript_render_start_microseconds);
            }

            timings.total_microseconds = lpst_elapsed_microseconds(process_start_microseconds);

        #ifdef COVERUP
                 if (state->halt_code != 1) {
                  printf("ERROR: halt code %u\n", state->halt_code);
                 }
        #else
                 printf("Halted with code %u after %u instructions.\n",
                     state->halt_code, state->instruction_count);
                 print_cache_stats(stdout, state);
                 print_runtime_timings(stdout, &timings);
        #endif

            printf("--- Screen ---\n%s--- End ---\n", screen_buf);

            if (test_report_path != NULL) {
                normalized_screen = malloc(strlen(screen_buf) + 1u);
                if (normalized_screen == NULL) {
                    free(screen_buf);
                    free(input_keys);
                    fprintf(stderr, "Out of memory.\n");
                    exit_code = 1;
                    goto cleanup;
                }

                memcpy(normalized_screen, screen_buf, strlen(screen_buf) + 1u);
                trim_trailing_blank_lines(normalized_screen);
                if (!write_test_report(test_report_path, state, normalized_screen, &timings)) {
                    fprintf(stderr, "Failed to write test report: %s\n", test_report_path);
                    free(normalized_screen);
                    free(screen_buf);
                    free(input_keys);
                    exit_code = 1;
                    goto cleanup;
                }

                free(normalized_screen);
            }

            free(screen_buf);
        }

        free(input_keys);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        print_usage();
        exit_code = 1;
        goto cleanup;
    }

cleanup:
    lpst_exec_free(state);
    lpst_image_free(image);
    free(executable_path);
    free(obj_path);
    free(default_mme_path);
    free(state);
    free(host);
    free(image);
    return exit_code;
}
