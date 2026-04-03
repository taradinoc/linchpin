/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_host.c — Transcript host implementation and shared host utilities.
 *
 * The transcript host is used for automated testing.  It records every
 * character written to the screen in a flat LPST_SCREEN_HEIGHT ×
 * LPST_SCREEN_WIDTH cell array and replays a pre-loaded input byte sequence
 * instead of reading from a live keyboard.  After the VM halts,
 * lpst_host_transcript_render converts the cell array to a text string for
 * comparison with expected output.
 */
#define _POSIX_C_SOURCE 200809L
#include "lpst_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define TS_MAX_CHANNELS 16

typedef struct ts_channel {
    FILE *fp;
    char *name;
} ts_channel;

typedef struct transcript_state {
    uint8_t cells[LPST_SCREEN_HEIGHT][LPST_SCREEN_WIDTH];
    uint8_t cursor_row;
    uint8_t cursor_col;
    uint16_t current_style_bits;
    const uint8_t *input_keys;
    size_t input_len;
    size_t input_pos;
    char *data_dir;
    ts_channel channels[TS_MAX_CHANNELS];
} transcript_state;

static char *ts_strdup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }

    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length);
    return copy;
}

static transcript_state *get_ts(lpst_host *host)
{
    return (transcript_state *)host->user_data;
}

static void ts_put_char(lpst_host *host, uint8_t ch, uint8_t row, uint8_t col)
{
    transcript_state *ts = get_ts(host);
    if (row < LPST_SCREEN_HEIGHT && col < LPST_SCREEN_WIDTH) {
        ts->cells[row][col] = ch;
    }
}

static void ts_set_cursor(lpst_host *host, uint8_t row, uint8_t col)
{
    transcript_state *ts = get_ts(host);
    ts->cursor_row = row;
    ts->cursor_col = col;
}

static void ts_set_style(lpst_host *host, uint16_t style_bits)
{
    transcript_state *ts = get_ts(host);
    ts->current_style_bits = style_bits;
}

static void ts_erase_to_eol(lpst_host *host, uint8_t row, uint8_t col)
{
    transcript_state *ts = get_ts(host);
    if (row < LPST_SCREEN_HEIGHT) {
        uint8_t c;
        for (c = col; c < LPST_SCREEN_WIDTH; c++) {
            ts->cells[row][c] = ' ';
        }
    }
}

static void ts_clear_rows(lpst_host *host, uint8_t from_row, uint8_t to_row)
{
    transcript_state *ts = get_ts(host);
    uint8_t r;
    for (r = from_row; r <= to_row && r < LPST_SCREEN_HEIGHT; r++) {
        memset(ts->cells[r], ' ', LPST_SCREEN_WIDTH);
    }
}

static void ts_scroll_up(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    transcript_state *ts = get_ts(host);
    uint8_t r;
    if (lines == 0 || from_row > to_row) {
        return;
    }

    for (r = from_row; r <= to_row; r++) {
        if (r + lines <= to_row) {
            memcpy(ts->cells[r], ts->cells[r + lines], LPST_SCREEN_WIDTH);
        } else {
            memset(ts->cells[r], ' ', LPST_SCREEN_WIDTH);
        }
    }
}

static void ts_scroll_down(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    transcript_state *ts = get_ts(host);
    int r;
    if (lines == 0 || from_row > to_row) {
        return;
    }

    for (r = to_row; r >= (int)from_row; r--) {
        if (r - lines >= (int)from_row) {
            memcpy(ts->cells[r], ts->cells[r - lines], LPST_SCREEN_WIDTH);
        } else {
            memset(ts->cells[r], ' ', LPST_SCREEN_WIDTH);
        }
    }
}

static void ts_flush(lpst_host *host)
{
    (void)host;
}

/* Return the next pre-supplied byte if available, or LPST_FALSE_SENTINEL
 * (0x8001) to signal that no input is ready.  The VM treats this sentinel
 * as "no key pressed" and retries on the next instruction cycle. */
static uint16_t ts_poll_key(lpst_host *host)
{
    transcript_state *ts = get_ts(host);
    if (ts->input_keys != NULL && ts->input_pos < ts->input_len) {
        return (uint16_t)ts->input_keys[ts->input_pos++];
    }
    return UINT16_C(0x8001);
}

static int ts_find_free_channel_slot(const transcript_state *ts)
{
    int slot;

    for (slot = 0; slot < TS_MAX_CHANNELS; slot++) {
        if (ts->channels[slot].fp == NULL) {
            return slot;
        }
    }

    return -1;
}

/* Map a file-open mode byte to the corresponding fopen mode string.
 * mode 0 = read-only, mode 1 = write (create/truncate), mode 2 = read+write. */
static const char *ts_mode_to_fopen_mode(uint8_t mode)
{
    if (mode == 0) {
        return "rb";
    }

    if (mode == 1) {
        return "wb";
    }

    return "r+b";
}

/* Open a data file by joining the configured data directory with name.
 * The channel ID written to *out_channel is 1-based. */
static bool ts_file_open(lpst_host *host, const char *name, uint8_t mode, uint16_t *out_channel)
{
    transcript_state *ts = get_ts(host);
    int slot;
    char *path;
    FILE *fp;

    if (ts->data_dir == NULL || name == NULL) {
        return false;
    }

    slot = ts_find_free_channel_slot(ts);
    if (slot < 0) {
        return false;
    }

    path = (char *)malloc(strlen(ts->data_dir) + strlen(name) + 2u);
    if (path == NULL) {
        return false;
    }

    snprintf(path, strlen(ts->data_dir) + strlen(name) + 2u, "%s/%s", ts->data_dir, name);

    fp = fopen(path, ts_mode_to_fopen_mode(mode));
    free(path);
    if (fp == NULL && mode == 1) {
        /* Create succeeded but file didn't exist; already handled by "wb" */
        return false;
    }
    if (fp == NULL) {
        return false;
    }

    ts->channels[slot].fp = fp;
    ts->channels[slot].name = ts_strdup(name);
    if (ts->channels[slot].name == NULL) {
        fclose(fp);
        ts->channels[slot].fp = NULL;
        return false;
    }
    *out_channel = (uint16_t)(slot + 1); /* 1-based channel IDs */
    return true;
}

static bool ts_file_close(lpst_host *host, uint16_t channel)
{
    transcript_state *ts = get_ts(host);
    int slot = channel - 1;
    if (slot < 0 || slot >= TS_MAX_CHANNELS || ts->channels[slot].fp == NULL) {
        return false;
    }
    fclose(ts->channels[slot].fp);
    free(ts->channels[slot].name);
    ts->channels[slot].fp = NULL;
    ts->channels[slot].name = NULL;
    return true;
}

static int ts_file_read(lpst_host *host, uint16_t channel, uint8_t *buf, size_t count)
{
    transcript_state *ts = get_ts(host);
    int slot = channel - 1;
    if (slot < 0 || slot >= TS_MAX_CHANNELS || ts->channels[slot].fp == NULL) {
        return -1;
    }
    return (int)fread(buf, 1, count, ts->channels[slot].fp);
}

static int ts_file_write(lpst_host *host, uint16_t channel, const uint8_t *buf, size_t count)
{
    transcript_state *ts = get_ts(host);
    int slot = channel - 1;
    if (slot < 0 || slot >= TS_MAX_CHANNELS || ts->channels[slot].fp == NULL) {
        return -1;
    }
    return (int)fwrite(buf, 1, count, ts->channels[slot].fp);
}

static void ts_get_local_time(
    uint16_t *month,
    uint16_t *day,
    uint16_t *year,
    uint16_t *hour,
    uint16_t *minute,
    uint16_t *second)
{
    time_t now;
    struct tm *tm;

    now = time(NULL);
    tm = localtime(&now);
    if (tm != NULL) {
        *month = (uint16_t)(tm->tm_mon + 1);
        *day = (uint16_t)tm->tm_mday;
        *year = (uint16_t)(tm->tm_year + 1900);
        *hour = (uint16_t)tm->tm_hour;
        *minute = (uint16_t)tm->tm_min;
        *second = (uint16_t)tm->tm_sec;
    } else {
        *month = 1;
        *day = 1;
        *year = 1987;
        *hour = 0;
        *minute = 0;
        *second = 0;
    }
}

static void ts_get_date(lpst_host *host, uint16_t *month, uint16_t *day, uint16_t *year)
{
    uint16_t hour;
    uint16_t minute;
    uint16_t second;

    (void)host;
    ts_get_local_time(month, day, year, &hour, &minute, &second);
}

static void ts_get_time(lpst_host *host, uint16_t *hour, uint16_t *minute, uint16_t *second)
{
    uint16_t month;
    uint16_t day;
    uint16_t year;

    (void)host;
    ts_get_local_time(&month, &day, &year, hour, minute, second);
}

static transcript_state g_transcript_state;

void lpst_host_transcript_init(lpst_host *host, char *screen_buf, size_t screen_buf_size)
{
    (void)screen_buf;
    (void)screen_buf_size;

    memset(&g_transcript_state, 0, sizeof(g_transcript_state));
    memset(g_transcript_state.cells, ' ', sizeof(g_transcript_state.cells));

    host->user_data = &g_transcript_state;
    host->screen_width = LPST_DEFAULT_SCREEN_WIDTH;
    host->screen_height = LPST_SCREEN_HEIGHT;
    host->put_char = ts_put_char;
    host->set_style = ts_set_style;
    host->set_cursor = ts_set_cursor;
    host->erase_to_eol = ts_erase_to_eol;
    host->clear_rows = ts_clear_rows;
    host->scroll_up = ts_scroll_up;
    host->scroll_down = ts_scroll_down;
    host->flush = ts_flush;
    host->poll_key = ts_poll_key;
    host->file_open = ts_file_open;
    host->file_close = ts_file_close;
    host->file_read = ts_file_read;
    host->file_write = ts_file_write;
    host->get_date = ts_get_date;
    host->get_time = ts_get_time;
}

void lpst_host_transcript_set_data_dir(lpst_host *host, const char *dir)
{
    transcript_state *ts = get_ts(host);
    free(ts->data_dir);
    ts->data_dir = ts_strdup(dir);
}

void lpst_host_transcript_set_input(lpst_host *host, const uint8_t *keys, size_t count)
{
    transcript_state *ts = get_ts(host);
    ts->input_keys = keys;
    ts->input_len = count;
    ts->input_pos = 0;
}

/* Render the cell grid as a text string.  Each row is trimmed of trailing
 * spaces and appended with a newline.  Blank rows at the bottom of the
 * screen end the output early via lpst_host_transcript_render's outer
 * length check. */
void lpst_host_transcript_render(lpst_host *host, char *out_buf, size_t out_buf_size)
{
    transcript_state *ts = get_ts(host);
    size_t pos = 0;
    int r;

    for (r = 0; r < LPST_SCREEN_HEIGHT && pos + LPST_SCREEN_WIDTH + 2 < out_buf_size; r++) {
        int last_nonblank = -1;
        int c;

        for (c = LPST_SCREEN_WIDTH - 1; c >= 0; c--) {
            if (ts->cells[r][c] != ' ') {
                last_nonblank = c;
                break;
            }
        }

        for (c = 0; c <= last_nonblank; c++) {
            out_buf[pos++] = (char)ts->cells[r][c];
        }

        out_buf[pos++] = '\n';
    }

    if (pos < out_buf_size) {
        out_buf[pos] = '\0';
    } else if (out_buf_size > 0) {
        out_buf[out_buf_size - 1] = '\0';
    }
}
