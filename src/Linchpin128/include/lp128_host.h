#ifndef LP128_HOST_H
#define LP128_HOST_H

#include <stdbool.h>
#include <stdint.h>

#define LP128_SCREEN_WIDTH  80u
#define LP128_SCREEN_HEIGHT 25u

typedef struct lp128_host lp128_host;

struct lp128_host {
    void    *user_data;
    uint8_t  screen_width;
    uint8_t  screen_height;
    uint16_t current_text_attribute;

    /* Output: place character ch at (row, col); both 0-based */
    void     (*put_char)(lp128_host *host, uint8_t ch, uint8_t row, uint8_t col);
    /* Output: write len characters starting at (row, col), no line wrap.
     * buf contains ASCII bytes.  If NULL, falls back to per-char put_char. */
    void     (*put_string)(lp128_host *host, const uint8_t *buf, uint8_t len,
                           uint8_t row, uint8_t col);
    /* Output: set hardware cursor to (row, col) */
    void     (*set_cursor)(lp128_host *host, uint8_t row, uint8_t col);
    /* Output: clear characters from (row,col) to end of row */
    void     (*erase_to_eol)(lp128_host *host, uint8_t row, uint8_t col);
    /* Output: clear all characters in rows [from_row, to_row] inclusive */
    void     (*clear_rows)(lp128_host *host, uint8_t from_row, uint8_t to_row);
    /* Output: scroll rows [from_row, to_row] up by `lines` lines */
    void     (*scroll_up)(lp128_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines);
    /* Output: flush buffered output to screen */
    void     (*flush)(lp128_host *host);

    /* Input: poll for a pending key; returns ASCII value or LP128_FALSE_SENTINEL if none */
    uint16_t (*poll_key)(lp128_host *host);
};

/* POSIX console host (stdin/stdout, ANSI escapes) */
void lp128_host_posix_init(lp128_host *host);
void lp128_host_posix_cleanup(lp128_host *host);

#endif
