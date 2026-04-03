/*
 * lpst_host.h — Platform host interface for LinchpinST.
 *
 * The lpst_host struct is a vtable of callbacks that the VM calls to perform
 * all platform-specific operations: screen output, keyboard input, file I/O,
 * and clock queries.  Two host implementations are provided:
 *
 *   transcript host  — records all screen writes to an in-memory cell grid
 *                      and runs pre-supplied input, used for automated testing.
 *   console host     — drives a live VT52/ANSI/native terminal, used for
 *                      interactive sessions.
 */
#ifndef LPST_HOST_H
#define LPST_HOST_H

#include "lpst_platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The logical screen size that the VM assumes.  The actual visible width may
 * be narrower (e.g., 60 columns on Amiga), reported via screen_width. */
#define LPST_SCREEN_WIDTH  80
#define LPST_SCREEN_HEIGHT 25

#if LPST_PLATFORM_AMIGA
#define LPST_DEFAULT_SCREEN_WIDTH 60
#else
#define LPST_DEFAULT_SCREEN_WIDTH LPST_SCREEN_WIDTH
#endif

typedef struct lpst_host lpst_host;

struct lpst_host {
    void *user_data;       /* host-implementation private data */
    uint8_t screen_width;  /* visible column count (0 means use LPST_DEFAULT_SCREEN_WIDTH) */
    uint8_t screen_height; /* visible row count (0 means use LPST_SCREEN_HEIGHT) */

    /* Place one character at the given (row, col) position without moving the cursor. */
    void (*put_char)(lpst_host *host, uint8_t ch, uint8_t row, uint8_t col);

    /* Apply a style bitmask.  Bit 0 = reverse video, bit 3 = bold, bit 5 = blink. */
    void (*set_style)(lpst_host *host, uint16_t style_bits);

    /* Move the visible cursor to (row, col). */
    void (*set_cursor)(lpst_host *host, uint8_t row, uint8_t col);

    /* Clear from (row, col) to the end of that row. */
    void (*erase_to_eol)(lpst_host *host, uint8_t row, uint8_t col);

    /* Clear all rows in the range [from_row, to_row] inclusive. */
    void (*clear_rows)(lpst_host *host, uint8_t from_row, uint8_t to_row);

    /* Scroll the region [from_row, to_row] upward by lines rows, clearing vacated rows. */
    void (*scroll_up)(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines);

    /* Scroll the region [from_row, to_row] downward by lines rows, clearing vacated rows. */
    void (*scroll_down)(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines);

    /* Flush any buffered output to the display. */
    void (*flush)(lpst_host *host);

    /* Return the next pending input byte, or LPST_FALSE_SENTINEL (0x8001) if none. */
    uint16_t (*poll_key)(lpst_host *host);

    /* Open a named file in the data directory.  mode: 0=read, 1=write, 2=read/write.
     * On success, writes a channel ID to *out_channel and returns true. */
    bool (*file_open)(lpst_host *host, const char *name, uint8_t mode, uint16_t *out_channel);
    bool (*file_close)(lpst_host *host, uint16_t channel);
    int  (*file_read)(lpst_host *host, uint16_t channel, uint8_t *buf, size_t count);
    int  (*file_write)(lpst_host *host, uint16_t channel, const uint8_t *buf, size_t count);

    void (*get_date)(lpst_host *host, uint16_t *month, uint16_t *day, uint16_t *year);
    void (*get_time)(lpst_host *host, uint16_t *hour, uint16_t *minute, uint16_t *second);
};

/*
 * Transcript host — captures screen state in a flat cell array for automated
 * testing.  screen_buf and screen_buf_size are reserved for future use and
 * may be NULL/0.
 */
void lpst_host_transcript_init(lpst_host *host, char *screen_buf, size_t screen_buf_size);

/* Set the pre-supplied input byte sequence for the transcript host. */
void lpst_host_transcript_set_input(lpst_host *host, const uint8_t *keys, size_t count);

/* Set the data directory root used by the transcript host's file_open callback. */
void lpst_host_transcript_set_data_dir(lpst_host *host, const char *dir);

/* Render the transcript host's screen cell grid as a text string, one line
 * per row with trailing spaces stripped, rows separated by newlines. */
void lpst_host_transcript_render(lpst_host *host, char *out_buf, size_t out_buf_size);

/* Console host — drives a live interactive terminal. */
void lpst_host_console_init(lpst_host *host);

/* Set pre-supplied input that is drained before live keyboard polling begins. */
void lpst_host_console_set_input(lpst_host *host, const uint8_t *keys, size_t count);

/* Override the visible screen width (used when the terminal width is known). */
void lpst_host_console_set_screen_width(lpst_host *host, uint8_t width);

/* Restore the terminal to its original state (raw mode off, cursor visible, etc.). */
void lpst_host_console_cleanup(lpst_host *host);

#endif
