#include "lp128_host.h"
#include "lp128_vm.h"    /* for LP128_FALSE_SENTINEL */

#if !defined(__mos__) && !defined(__llvm_mos__)

/* ── POSIX console host ──────────────────────────────────────────────────────
 * Uses VT100/ANSI escape sequences for screen management.
 * poll_key reads from stdin using non-blocking I/O where available.
 * -------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    struct termios saved;
    bool           raw_active;
} posix_host_data;

/* Restore the terminal to the settings saved when raw mode was entered. */
static void restore_terminal(posix_host_data *d)
{
    if (d != NULL && d->raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &d->saved);
        d->raw_active = false;
    }
}

/* Switch the terminal to raw, non-blocking mode and save the
 * original settings so they can be restored by restore_terminal(). */
static void enter_raw(posix_host_data *d)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &d->saved) != 0) {
        return;
    }

    raw = d->saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;   /* non-blocking */
    raw.c_cc[VTIME] = 1;   /* 0.1 s timeout */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return;
    }

    d->raw_active = true;
}

/* Read one character from stdin without blocking.  Returns '\r' for
 * newline (to match C128 keyboard convention), or LP128_FALSE_SENTINEL
 * if no key is available. */
static uint16_t posix_poll_key(lp128_host *host)
{
    posix_host_data *d = (posix_host_data *)host->user_data;
    uint8_t ch;
    int n;

    if (d == NULL || !d->raw_active) {
        /* Fallback: blocking getchar */
        int c = getchar();
        if (c == EOF) return LP128_FALSE_SENTINEL;
        if (c == '\n') c = '\r';
        return (uint16_t)(uint8_t)c;
    }

    n = (int)read(STDIN_FILENO, &ch, 1);
    if (n <= 0) {
        return LP128_FALSE_SENTINEL;
    }
    if (ch == '\n') ch = '\r';
    return ch;
}

#else /* non-POSIX fallback */

typedef struct { int dummy; } posix_host_data;

static uint16_t posix_poll_key(lp128_host *host)
{
    int c;
    (void)host;
    c = getchar();
    if (c == EOF) return LP128_FALSE_SENTINEL;
    if (c == '\n') c = '\r';
    return (uint16_t)(uint8_t)c;
}

#endif /* __unix__ */

/* ── Screen helpers using ANSI escapes ─────────────────────────────────── */

/* Emit an ANSI cursor-position sequence to move to (row, col),
 * both 0-based. */
static void ansi_cursor(uint8_t row, uint8_t col)
{
    printf("\033[%u;%uH", (unsigned)(row + 1u), (unsigned)(col + 1u));
}

/* Emit an ANSI SGR sequence to apply the current text attribute:
 * reverse video when bit 0 of current_text_attribute is set, normal otherwise. */
static void posix_apply_text_attribute(const lp128_host *host)
{
    if ((host->current_text_attribute & 0x0001u) != 0) {
        printf("\033[7m");
    } else {
        printf("\033[27m");
    }
}

/* Write character ch at screen position (row, col). */
static void posix_put_char(lp128_host *host, uint8_t ch, uint8_t row, uint8_t col)
{
    ansi_cursor(row, col);
    posix_apply_text_attribute(host);
    putchar(ch);
    fflush(stdout);
}

/* Move the terminal cursor to (row, col). */
static void posix_set_cursor(lp128_host *host, uint8_t row, uint8_t col)
{
    (void)host;
    ansi_cursor(row, col);
    fflush(stdout);
}

/* Erase from (row, col) to the end of the line using ANSI EL. */
static void posix_erase_to_eol(lp128_host *host, uint8_t row, uint8_t col)
{
    ansi_cursor(row, col);
    posix_apply_text_attribute(host);
    printf("\033[K");
    fflush(stdout);
}

/* Erase every row from from_row to to_row using ANSI EL2 (erase whole line). */
static void posix_clear_rows(lp128_host *host, uint8_t from_row, uint8_t to_row)
{
    uint8_t r;
    for (r = from_row; r <= to_row; r++) {
        ansi_cursor(r, 0);
        posix_apply_text_attribute(host);
        printf("\033[2K");
    }
    fflush(stdout);
}

/* Scroll the rows from_row..to_row up by lines, using the ANSI
 * DECSTBM scroll-region sequence to let the terminal do the work. */
static void posix_scroll_up(lp128_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    /* Use terminal scroll region */
    posix_apply_text_attribute(host);
    printf("\033[%u;%ur", (unsigned)(from_row + 1u), (unsigned)(to_row + 1u));
    while (lines-- > 0) {
        ansi_cursor(to_row, 0);
        printf("\n");
    }
    printf("\033[r");    /* reset scroll region */
    fflush(stdout);
}

/* Flush the stdio output buffer.  Needed after any sequence of puts/printf
 * calls that do not end in a newline. */
static void posix_flush(lp128_host *host)
{
    (void)host;
    fflush(stdout);
}

/* ── Initialise / cleanup ───────────────────────────────────────────────── */
/* Initialise host for POSIX terminal use.  Enters raw non-blocking mode
 * (on Unix/macOS), clears the screen, and installs ANSI-terminal callbacks. */
void lp128_host_posix_init(lp128_host *host)
{
    static posix_host_data data;
    memset(&data, 0, sizeof(data));

    memset(host, 0, sizeof(*host));
    host->user_data     = &data;
    host->screen_width  = (uint8_t)LP128_SCREEN_WIDTH;
    host->screen_height = (uint8_t)LP128_SCREEN_HEIGHT;
    host->current_text_attribute = 0;

    host->put_char    = posix_put_char;
    host->set_cursor  = posix_set_cursor;
    host->erase_to_eol = posix_erase_to_eol;
    host->clear_rows  = posix_clear_rows;
    host->scroll_up   = posix_scroll_up;
    host->flush       = posix_flush;
    host->poll_key    = posix_poll_key;

#if defined(__unix__) || defined(__APPLE__)
    enter_raw(&data);
    /* Clear screen on startup */
    printf("\033[2J");
    ansi_cursor(0, 0);
    fflush(stdout);
#endif
}

/* Restore the terminal to its original settings and emit a final newline. */
void lp128_host_posix_cleanup(lp128_host *host)
{
#if defined(__unix__) || defined(__APPLE__)
    posix_host_data *d = (posix_host_data *)host->user_data;
    restore_terminal(d);
    printf("\n");
    fflush(stdout);
#else
    (void)host;
#endif
}

#endif /* !__mos__ && !__llvm_mos__ */
