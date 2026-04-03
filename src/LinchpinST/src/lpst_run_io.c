/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_io.c — VM string comparison, file I/O helpers, and channel management.
 *
 * compare_vm_strings implements the structured sort-key comparison used by the
 * Cornerstone runtime.  Sort keys are stored as aggregates with a small header:
 *   payload byte -2: flags (bit 6 = structured-key marker; bits 5-0 = secondary sort)
 *   payload byte -1: remaining sub-key count
 *   payload byte 0-3: combined header value for tie-breaking
 *   payload byte 4+:  sub-keys, each preceded by a one-byte length
 * Shorter sub-keys sort before longer ones when their content matches.
 *
 * resolve_file_path searches for a data file in this order:
 *   1. data_dir (the directory last set at runtime)
 *   2. parent of data_dir (if different)
 *   3. base_dir (the directory containing the MME file)
 *   4. the literal path (current working directory)
 * Within each directory, several filename aliases are tried for compatibility
 * with older Cornerstone installations (.DBF ↔ .NEW, .CNR ↔ .DBF, etc.).
 *
 * get_record_size_bytes converts system slot 0xCC to a byte count:
 *   0 or negative → 256 (default record size)
 *   1–127         → value * 2 (word count to bytes)
 *   128+          → raw value as unsigned byte count
 */
#include "lpst_run_internal.h"

static lpst_channel *find_channel(lpst_exec_state *state, uint16_t channel_id)
{
    int i;
    for (i = 0; i < LPST_MAX_CHANNELS; i++) {
        if (state->channels[i].in_use && (uint16_t)(i + 1) == channel_id) {
            return &state->channels[i];
        }
    }
    return NULL;
}

lpst_channel *resolve_channel(lpst_exec_state *state, uint16_t channel_id)
{
    lpst_channel *ch;
    if (channel_id == LPST_FALSE_SENTINEL) {
        if (state->active_channel_id == LPST_FALSE_SENTINEL) {
            return NULL;
        }
        ch = find_channel(state, state->active_channel_id);
        return ch;
    }
    ch = find_channel(state, channel_id);
    if (ch != NULL) {
        state->active_channel_id = channel_id;
    }
    return ch;
}

lpst_channel *alloc_channel(lpst_exec_state *state, uint16_t *out_id)
{
    uint16_t id = state->next_channel_id++;
    int slot;
    if (id == LPST_FALSE_SENTINEL) {
        id = state->next_channel_id++;
    }
    slot = (int)(id - 1);
    if (slot < 0 || slot >= LPST_MAX_CHANNELS) {
        return NULL;
    }
    if (state->channels[slot].in_use) {
        return NULL;
    }
    memset(&state->channels[slot], 0, sizeof(lpst_channel));
    state->channels[slot].in_use = true;
    *out_id = id;
    return &state->channels[slot];
}

void close_channel(lpst_channel *ch)
{
    if (ch == NULL) return;
    if (ch->fp != NULL) {
        fclose(ch->fp);
        ch->fp = NULL;
    }
    ch->in_use = false;
}

/* Returns 256 when slot 0xCC is zero or negative, treating those values as
 * "use the default record size".  Values 1-127 are interpreted as word counts
 * and doubled; values 128+ are used directly as a byte count. */
int get_record_size_bytes(const lpst_exec_state *state)
{
    int16_t record_size = (int16_t)state->system_module_globals[0xCC];
    if (record_size <= 0) {
        return 0x100;
    }
    return record_size < 0x80 ? record_size * 2 : (int)(uint16_t)record_size;
}

int read_vm_string(const lpst_exec_state *state, uint16_t handle, char *out, size_t max_len)
{
    uint16_t byte_len = read_aggregate_word(state, handle, 0);
    int i;
    int limit = (int)(byte_len < max_len - 1 ? byte_len : max_len - 1);
    for (i = 0; i < limit; i++) {
        uint8_t b = read_aggregate_payload_byte(state, handle, i);
        if (b == 0) break;
        out[i] = (char)b;
    }
    out[i] = '\0';
    return i;
}

static uint8_t fold_compare_byte(uint8_t value)
{
    if (value >= 'a' && value <= 'z') {
        return (uint8_t)(value - 32);
    }

    return value;
}

static bool try_read_ignore_case_compare_byte(
    const lpst_exec_state *state,
    uint16_t handle,
    int limit,
    int *index,
    uint8_t *value)
{
    if (*index < limit) {
        *value = fold_compare_byte(read_aggregate_payload_byte(state, handle, (int)(*index)));
        *index += 1;
        return true;
    }

    *value = 0;
    return false;
}

/* Compare two VM string aggregates, case-insensitively.
 * first_offset and first_length allow comparing a substring of first_handle.
 * Returns 1 if first < second, -1 if first > second, 0 if equal. */
uint16_t compare_vm_strings_ignore_case(
    const lpst_exec_state *state,
    uint16_t first_handle,
    uint16_t first_offset,
    uint16_t first_length,
    uint16_t second_handle,
    uint16_t second_length)
{
    int first_index = first_offset;
    int second_index = 0;
    int first_limit = (int)first_offset + (int)first_length;
    int second_limit = (int)second_length;

    while (1) {
        uint8_t first_value = 0;
        uint8_t second_value = 0;
        bool first_has_value = try_read_ignore_case_compare_byte(
            state,
            first_handle,
            first_limit,
            &first_index,
            &first_value);
        bool second_has_value = try_read_ignore_case_compare_byte(
            state,
            second_handle,
            second_limit,
            &second_index,
            &second_value);

        if (!first_has_value || !second_has_value) {
            if (first_has_value == second_has_value) {
                return 0;
            }

            return first_has_value ? (uint16_t)(int16_t)-1 : 1;
        }

        if (first_value == second_value) {
            continue;
        }

        return first_value < second_value ? 1 : (uint16_t)(int16_t)-1;
    }
}

/* Lexicographic comparison of two VM sort-key aggregates.
 * See the file header for the sort-key format description. */
uint16_t compare_vm_strings(const lpst_exec_state *state, uint16_t first_handle, uint16_t second_handle)
{
    /* Structured sort key comparison matching the STRCMP opcode in MME.
       Reads payload bytes (with +2 header skip) using length-based field traversal. */
    int first_remaining = read_aggregate_payload_byte(state, first_handle, -1);
    int second_remaining = read_aggregate_payload_byte(state, second_handle, -1);
    int payload_index = 4;

    while (1) {
        /* Phase 1: check if either remaining count is zero */
        if (first_remaining == 0) {
            if (second_remaining == 0) {
                /* Both exhausted - check bit 6 of payload byte -2 */
                uint8_t flag_byte = read_aggregate_payload_byte(state, first_handle, -2);
                if ((flag_byte & 0x40) == 0) {
                    return 0;
                }

                /* Structured key tie-break: compare combined header values */
                int first_combined = (read_aggregate_payload_byte(state, first_handle, 3) & 0x3F) << 8
                    | read_aggregate_payload_byte(state, first_handle, 2);
                int second_combined = (read_aggregate_payload_byte(state, second_handle, 3) & 0x3F) << 8
                    | read_aggregate_payload_byte(state, second_handle, 2);

                if (first_combined < second_combined)
                    return (uint16_t)(int16_t)-1;
                if (first_combined > second_combined)
                    return 1;

                /* Still equal - compare payload byte -2 (6-bit) */
                int first_flag = read_aggregate_payload_byte(state, first_handle, -2) & 0x3F;
                int second_flag = read_aggregate_payload_byte(state, second_handle, -2) & 0x3F;

                if (first_flag < second_flag)
                    return (uint16_t)(int16_t)-1;

                return 1;
            }

            /* First exhausted, second has data */
            return (uint16_t)(int16_t)-1;
        }

        if (second_remaining == 0) {
            /* First has data, second exhausted */
            return 1;
        }

        /* Phase 2: read sub-key length from each handle */
        int first_field_len = read_aggregate_payload_byte(state, first_handle, payload_index);
        int second_field_len = read_aggregate_payload_byte(state, second_handle, payload_index);
        payload_index++;

        first_remaining -= first_field_len + 1;
        second_remaining -= second_field_len + 1;

        /* Phase 3: compare min(len1, len2) data bytes */
        int compare_count = first_field_len < second_field_len ? first_field_len : second_field_len;

        for (int i = 0; i < compare_count; i++) {
            uint8_t fb = read_aggregate_payload_byte(state, first_handle, payload_index);
            uint8_t sb = read_aggregate_payload_byte(state, second_handle, payload_index);
            payload_index++;

            if (fb > sb)
                return 1;
            if (fb < sb)
                return (uint16_t)(int16_t)-1;
        }

        /* All compared bytes equal - check length difference */
        if (first_field_len > second_field_len)
            return 1;
        if (first_field_len < second_field_len)
            return (uint16_t)(int16_t)-1;

        /* Equal - continue to next sub-key */
    }
}

static int file_exists(const char *path)
{
    FILE *f;

    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }

    return 0;
}

static long query_file_size_bytes(const char *path, FILE *fp)
{
    struct stat st;

    if (path != NULL && path[0] != '\0' && stat(path, &st) == 0) {
        return (long)st.st_size;
    }

    if (fp != NULL) {
        long saved = ftell(fp);
        long size;

        if (saved >= 0 && fseek(fp, 0, SEEK_END) == 0) {
            size = ftell(fp);
            if (size >= 0) {
                fseek(fp, saved, SEEK_SET);
                return size;
            }
        }

        if (saved >= 0) {
            fseek(fp, saved, SEEK_SET);
        }
    }

    return 0;
}

long query_channel_size_bytes(const lpst_channel *ch)
{
    long size;

    if (ch == NULL) {
        return 0;
    }

    size = query_file_size_bytes(ch->path, ch->fp);
    if (size > 0) {
        return size;
    }

    if (ch->size_bytes > 0) {
        return ch->size_bytes;
    }

    return 0;
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

int join_path(const char *directory, const char *name, char separator, char *out, size_t out_size)
{
    size_t dir_len;
    int written;

    if (directory == NULL || name == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    dir_len = strlen(directory);
    if (dir_len == 0) {
        return 0;
    }

    if (dir_len == 1 && directory[0] == '.') {
        written = snprintf(out, out_size, "%s", name);
        return written > 0 && (size_t)written < out_size;
    }

    if (directory[dir_len - 1] == '/' || directory[dir_len - 1] == '\\' || directory[dir_len - 1] == ':') {
        written = snprintf(out, out_size, "%s%s", directory, name);
    } else {
        written = snprintf(out, out_size, "%s%c%s", directory, separator, name);
    }

    return written > 0 && (size_t)written < out_size;
}

static int try_resolve_candidate(const char *directory, const char *name, char *out, size_t out_size)
{
    char candidate[260];

    if (directory == NULL || directory[0] == '\0') {
        return 0;
    }

    if (join_path(directory, name, '/', candidate, sizeof(candidate)) && file_exists(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 1;
    }

    if (join_path(directory, name, '\\', candidate, sizeof(candidate)) && file_exists(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 1;
    }

    return 0;
}

static int has_extension_ci(const char *name, const char *extension)
{
    size_t name_len = strlen(name);
    size_t extension_len = strlen(extension);

    if (name_len < extension_len) {
        return 0;
    }

    return lpst_ascii_stricmp(name + name_len - extension_len, extension) == 0;
}

static int build_extension_alias(const char *name, const char *source_extension, const char *target_extension,
                                 char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    size_t source_len = strlen(source_extension);
    size_t target_len = strlen(target_extension);

    if (!has_extension_ci(name, source_extension) || name_len - source_len + target_len + 1 > out_size) {
        return 0;
    }

    memcpy(out, name, name_len - source_len);
    memcpy(out + name_len - source_len, target_extension, target_len);
    out[name_len - source_len + target_len] = '\0';
    return 1;
}

static int path_equals_ci(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return 0;
    }

    return lpst_ascii_stricmp(left, right) == 0;
}

static int try_get_parent_directory(const char *path, char *out, size_t out_size)
{
    const char *last_separator;
    size_t length;

    if (path == NULL || path[0] == '\0' || out == NULL || out_size == 0) {
        return 0;
    }

    last_separator = find_last_path_separator(path);
    if (last_separator == NULL) {
        return 0;
    }

    length = (size_t)(last_separator - path);
    if (*last_separator == ':') {
        length++;
    }

    if (length == 0 || length >= out_size) {
        return 0;
    }

    memcpy(out, path, length);
    out[length] = '\0';
    return 1;
}

static int try_resolve_in_directory_with_aliases(const char *directory, const char *name, char *out, size_t out_size)
{
    char alias[LPST_CHANNEL_NAME_MAX];

    if (try_resolve_candidate(directory, name, out, out_size)) {
        return 1;
    }

    if (lpst_ascii_stricmp(name, "SY000000.DBF") == 0) {
        if (try_resolve_candidate(directory, "SYCO.NEW", out, out_size) ||
            try_resolve_candidate(directory, "SYBW.NEW", out, out_size)) {
            return 1;
        }
    }

    if (lpst_ascii_stricmp(name, "SY000000.SLS") == 0) {
        if (try_resolve_candidate(directory, "SYCO.NEW", out, out_size) ||
            try_resolve_candidate(directory, "SYBW.NEW", out, out_size)) {
            return 1;
        }
    }

    if (build_extension_alias(name, ".DBF", ".NEW", alias, sizeof(alias)) &&
        try_resolve_candidate(directory, alias, out, out_size)) {
        return 1;
    }

    if (build_extension_alias(name, ".CNR", ".NEW", alias, sizeof(alias)) &&
        try_resolve_candidate(directory, alias, out, out_size)) {
        return 1;
    }

    if (build_extension_alias(name, ".CNR", ".DBF", alias, sizeof(alias)) &&
        try_resolve_candidate(directory, alias, out, out_size)) {
        return 1;
    }

    if (build_extension_alias(name, ".SLS", ".NEW", alias, sizeof(alias)) &&
        try_resolve_candidate(directory, alias, out, out_size)) {
        return 1;
    }

    return 0;
}

static int try_resolve_in_search_order(const lpst_image *image, const char *name, char *out, size_t out_size)
{
    char parent_dir[260];

    if (try_resolve_in_directory_with_aliases(image->data_dir, name, out, out_size)) {
        return 1;
    }

    if (try_get_parent_directory(image->data_dir, parent_dir, sizeof(parent_dir)) &&
        !path_equals_ci(parent_dir, image->data_dir) &&
        try_resolve_in_directory_with_aliases(parent_dir, name, out, out_size)) {
        return 1;
    }

    if (!path_equals_ci(image->base_dir, image->data_dir) &&
        (!try_get_parent_directory(image->data_dir, parent_dir, sizeof(parent_dir)) ||
            !path_equals_ci(parent_dir, image->base_dir)) &&
        try_resolve_in_directory_with_aliases(image->base_dir, name, out, out_size)) {
        return 1;
    }

    if (file_exists(name)) {
        snprintf(out, out_size, "%s", name);
        return 1;
    }

    return 0;
}

int resolve_file_path(const lpst_image *image, const char *name, char *out, size_t out_size)
{
    return try_resolve_in_search_order(image, name, out, out_size);
}

void handle_ext_open(lpst_exec_state *state)
{
    uint8_t mode = fetch_byte(state);
    uint16_t extra = lpst_exec_pop(state);
    uint16_t filename_handle = lpst_exec_pop(state);
    char name[LPST_CHANNEL_NAME_MAX];
    char path[260];
    int access_family = mode & 0x03;
    const char *fmode;
    uint16_t new_id;
    lpst_channel *ch;

    (void)extra;
    read_vm_string(state, filename_handle, name, sizeof(name));

    if (name[0] == '\0') {
        record_recent_open_event(state, 1, 0, mode, name);
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    if (state->trace_enabled) {
        fprintf(stderr, "[OPEN] name=\"%s\" mode=0x%02X\n", name, mode);
    }

    if (access_family == 0x01 || access_family == 0x03) {
        if (!join_path(state->image->base_dir, name, '/', path, sizeof(path))) {
            record_recent_open_event(state, 2, 1, mode, name);
            lpst_exec_push(state, LPST_FALSE_SENTINEL);
            return;
        }

        fmode = "w+b";
    } else if (!resolve_file_path(state->image, name, path, sizeof(path))) {
        record_recent_open_event(state, 2, 2, mode, name);
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    } else {
        switch (access_family) {
        case 0x00: fmode = "rb"; break;
        case 0x02: fmode = "r+b"; break;
        default:   fmode = "rb"; break;
        }
    }

    ch = alloc_channel(state, &new_id);
    if (ch == NULL) {
        record_recent_open_event(state, 3, 0, mode, name);
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    errno = 0;
    ch->fp = fopen(path, fmode);
    if (ch->fp == NULL) {
        ch->in_use = false;
        record_recent_open_event(state, 4, (uint16_t)errno, mode, name);
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    ch->mode = mode;
    ch->position = 0;
    ch->size_bytes = 0;
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    snprintf(ch->path, sizeof(ch->path), "%s", path);

    if (state->trace_enabled) {
        fprintf(stderr, "[OPEN] -> channel %u path=\"%s\"\n", new_id, path);
    }

    record_recent_open_event(state, 0, new_id, mode, name);
    lpst_exec_push(state, new_id);
}

void handle_ext_close(lpst_exec_state *state)
{
    uint16_t channel_id = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);

    if (state->trace_enabled) {
        fprintf(stderr, "[CLOSE] channel=%u %s\n", channel_id,
                ch ? "ok" : "missing");
    }

    if (ch != NULL) {
        if (state->active_channel_id == channel_id) {
            state->active_channel_id = LPST_FALSE_SENTINEL;
        }
        close_channel(ch);
    }

    lpst_exec_push(state, 0);
}

void handle_ext_seqread(lpst_exec_state *state)
{
    uint16_t word_count = lpst_exec_pop(state);
    uint16_t channel_id = lpst_exec_pop(state);
    uint16_t vec_handle = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);
    int transfer_bytes = (int)word_count * 2;
    int i;

    if (ch == NULL || ch->fp == NULL) {
        for (i = 0; i < transfer_bytes; i++) {
            write_aggregate_payload_byte(state, vec_handle, i, 0);
        }
        if (state->trace_enabled) {
            fprintf(stderr, "[READ] channel=%u words=%u -> missing\n",
                    channel_id, word_count);
        }
        lpst_exec_push(state, 0xFFFF);
        return;
    }

    fseek(ch->fp, ch->position, SEEK_SET);
    {
        uint8_t buf[512];
        int total_read = 0;
        while (total_read < transfer_bytes) {
            int chunk = transfer_bytes - total_read;
            size_t n;
            if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
            n = fread(buf, 1, (size_t)chunk, ch->fp);
            for (i = 0; i < (int)n; i++) {
                write_aggregate_payload_byte(state, vec_handle, total_read + i, buf[i]);
            }
            total_read += (int)n;
            if ((int)n < chunk) break;
        }
        for (i = total_read; i < transfer_bytes; i++) {
            write_aggregate_payload_byte(state, vec_handle, i, 0);
        }
        ch->position += total_read;
        if (state->trace_enabled) {
            fprintf(stderr, "[READ] channel=%u words=%u copied=%d\n",
                    channel_id, word_count, total_read);
        }
        lpst_exec_push(state, total_read == transfer_bytes ? (uint16_t)0 : (uint16_t)0xFFFF);
    }
}

void handle_ext_seqwrite(lpst_exec_state *state)
{
    uint16_t word_count = lpst_exec_pop(state);
    uint16_t channel_id = lpst_exec_pop(state);
    uint16_t vec_handle = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);
    int transfer_bytes = (int)word_count;
    int i;

    if (ch == NULL || ch->fp == NULL) {
        if (state->trace_enabled) {
            fprintf(stderr, "[WRITE] channel=%u words=%u -> missing\n",
                    channel_id, word_count);
        }
        lpst_exec_push(state, 0xFFFF);
        return;
    }

    fseek(ch->fp, ch->position, SEEK_SET);
    {
        uint8_t buf[512];
        int total_written = 0;
        while (total_written < transfer_bytes) {
            int chunk = transfer_bytes - total_written;
            size_t n;
            if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
            for (i = 0; i < chunk; i++) {
                buf[i] = (uint8_t)read_aggregate_payload_byte(state, vec_handle, (total_written + i) * 2);
            }
            n = fwrite(buf, 1, (size_t)chunk, ch->fp);
            total_written += (int)n;
            if ((int)n < chunk) break;
        }
        ch->position += total_written;
        if (ch->position > ch->size_bytes) {
            ch->size_bytes = ch->position;
        }
        if (state->trace_enabled) {
            fprintf(stderr, "[WRITE] channel=%u words=%u written=%d\n",
                    channel_id, word_count, total_written);
        }
        lpst_exec_push(state, 0);
    }
}

void handle_ext_readrec(lpst_exec_state *state)
{
    uint16_t word_count = lpst_exec_pop(state);
    uint16_t record = lpst_exec_pop(state);
    uint16_t channel_id = lpst_exec_pop(state);
    uint16_t vec_handle = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);
    int record_size = get_record_size_bytes(state);
    int byte_count = (int)word_count * record_size;
    int word_transfer = (byte_count + 1) / 2;
    int copied = 0;
    int wi;

    if (ch == NULL || ch->fp == NULL) {
        for (wi = 0; wi < word_transfer; wi++) {
            write_aggregate_word(state, vec_handle, wi, 0);
        }
    } else {
        uint8_t buf[512];
        int total_read = 0;
        fseek(ch->fp, (long)record * record_size, SEEK_SET);
        while (total_read < byte_count) {
            int chunk = byte_count - total_read;
            size_t n;
            if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
            n = fread(buf, 1, (size_t)chunk, ch->fp);
            if (n > 0) {
                int j;
                for (j = 0; j < (int)n; j++) {
                    int abs_pos = total_read + j;
                    int word_idx = abs_pos / 2;
                    if ((abs_pos % 2) == 0) {
                        write_aggregate_word(state, vec_handle, word_idx, (uint16_t)buf[j]);
                    } else {
                        uint16_t low = read_aggregate_word(state, vec_handle, word_idx);
                        write_aggregate_word(state, vec_handle, word_idx,
                                             (uint16_t)((low & 0x00FFu) | ((uint16_t)buf[j] << 8)));
                    }
                }
                total_read += (int)n;
            }
            if ((int)n < chunk) break;
        }
        copied = total_read;
        for (wi = (copied + 1) / 2; wi < word_transfer; wi++) {
            write_aggregate_word(state, vec_handle, wi, 0);
        }
    }

    if (state->trace_enabled) {
        fprintf(stderr, "[READREC] vec=0x%04X channel=%u record=%u words=%u recsize=%d bytes=%d/%d\n",
                vec_handle, channel_id, record, word_count, record_size, copied, byte_count);
    }

    lpst_exec_push(state, (uint16_t)copied);
}

void handle_ext_writerec(lpst_exec_state *state)
{
    uint16_t word_count = lpst_exec_pop(state);
    uint16_t record = lpst_exec_pop(state);
    uint16_t channel_id = lpst_exec_pop(state);
    uint16_t vec_handle = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);
    int record_size = get_record_size_bytes(state);
    int byte_count = (int)word_count * record_size;
    long target_offset = (long)record * record_size;
    int word_transfer = byte_count / 2;
    int wi;

    if (ch == NULL || ch->fp == NULL) {
        if (state->trace_enabled) {
            fprintf(stderr, "[WRITEREC] channel=%u record=%u words=%u -> missing\n",
                    channel_id, record, word_count);
        }
        lpst_exec_push(state, 0);
        return;
    }

    fseek(ch->fp, target_offset, SEEK_SET);
    {
        uint8_t buf[512];
        int buf_pos = 0;
        for (wi = 0; wi < word_transfer; wi++) {
            uint16_t w = read_aggregate_word(state, vec_handle, wi);
            buf[buf_pos++] = (uint8_t)(w & 0xFF);
            buf[buf_pos++] = (uint8_t)(w >> 8);
            if (buf_pos >= (int)sizeof(buf) || wi == word_transfer - 1) {
                fwrite(buf, 1, (size_t)buf_pos, ch->fp);
                buf_pos = 0;
            }
        }
        fflush(ch->fp);
        if (target_offset + byte_count > ch->size_bytes) {
            ch->size_bytes = target_offset + byte_count;
        }
    }

    if (state->trace_enabled) {
        fprintf(stderr, "[WRITEREC] channel=%u record=%u words=%u bytes=%d\n",
                channel_id, record, word_count, byte_count);
    }

    lpst_exec_push(state, (uint16_t)byte_count);
}

void handle_ext_fsize(lpst_exec_state *state)
{
    uint16_t channel_id = lpst_exec_pop(state);
    lpst_channel *ch = resolve_channel(state, channel_id);
    long size;

    if (ch == NULL || ch->fp == NULL) {
        if (state->trace_enabled) {
            fprintf(stderr, "[FSIZE] channel=%u -> missing\n", channel_id);
        }
        lpst_exec_push(state, 0);
        return;
    }

    size = query_channel_size_bytes(ch);

    if (state->trace_enabled) {
        fprintf(stderr, "[FSIZE] channel=%u -> %ld bytes (%ld blocks)\n",
                channel_id, size, (size + 0xFF) / 0x100);
    }

    lpst_exec_push(state, (uint16_t)((size + 0xFF) / 0x100));
}

void handle_ext_unlink(lpst_exec_state *state)
{
    uint16_t filename_handle = lpst_exec_pop(state);
    char name[LPST_CHANNEL_NAME_MAX];
    char path[260];

    read_vm_string(state, filename_handle, name, sizeof(name));

    if (state->trace_enabled) {
        fprintf(stderr, "[UNLINK] name=\"%s\"\n", name);
    }

    if (resolve_file_path(state->image, name, path, sizeof(path))) {
        remove(path);
        lpst_exec_push(state, 0);
    } else {
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
    }
}
