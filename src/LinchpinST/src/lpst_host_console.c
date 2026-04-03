/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_host_console.c — Live interactive console host for LinchpinST.
 *
 * Uses a native Atari VT52 console path on Atari builds, an ANSI console plus
 * AmigaDOS raw input path on Amiga builds, and a POSIX termios/ANSI path
 * elsewhere. All backends map console keys to Cornerstone's ESC+scan-code
 * protocol.
 */
#include "lpst_platform.h"

#if LPST_PLATFORM_POSIX
#define _POSIX_C_SOURCE 199309L
#endif
#include "lpst_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if LPST_PLATFORM_ATARI
#include <mint/sysbind.h>
#elif LPST_PLATFORM_AMIGA
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/conunit.h>
#include <intuition/intuition.h>
#include <proto/dos.h>
#else
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#define FALSE_SENTINEL UINT16_C(0x8001)
#define LPST_PENDING_SEQUENCE_IDLE_LIMIT UINT8_C(3)

#if LPST_PLATFORM_ATARI
#define LPST_ATARI_CONTERM_ADDR ((volatile uint8_t *)0x484L)
#define LPST_ATARI_CONTERM_SHIFT_STATUS UINT8_C(0x08)
#define LPST_ATARI_SHIFT_MASK UINT8_C(0x03)
#elif LPST_PLATFORM_AMIGA
#define LPST_AMIGA_REPORT_BUFFER_SIZE 64
#define LPST_AMIGA_READ_TIMEOUT_TICKS 50
#endif

typedef struct console_state {
    uint8_t cells[LPST_SCREEN_HEIGHT][LPST_SCREEN_WIDTH];
    uint16_t attrs[LPST_SCREEN_HEIGHT][LPST_SCREEN_WIDTH];
    uint16_t current_style_bits;
    uint8_t visible_width;
    bool reverse_active;
    bool bright_active;
    bool blink_active;

#if LPST_PLATFORM_POSIX
    struct termios orig_termios;
#endif

#if !LPST_PLATFORM_ATARI
    bool raw_mode;
#endif

#if LPST_PLATFORM_AMIGA
    BPTR input_handle;
#endif

#if LPST_PLATFORM_ATARI
    bool atari_conterm_saved;
    uint8_t atari_conterm_original;
#endif

    /* Key queue for multi-byte sequences (ESC+scan pairs) */
    uint16_t key_queue[16];
    int key_queue_head;
    int key_queue_count;

    /* Pending console escape/CSI sequence assembled across polls. */
    uint8_t pending_sequence[16];
    int pending_sequence_len;
    uint8_t pending_sequence_kind;
    uint8_t pending_sequence_idle_polls;

    /* Pre-supplied input (from --input-text, drained before live input) */
    const uint8_t *input_keys;
    size_t input_len;
    size_t input_pos;
} console_state;

static console_state g_console_state;

static uint8_t console_visible_width(const console_state *cs)
{
    if (cs->visible_width == 0 || cs->visible_width > LPST_SCREEN_WIDTH) {
        return LPST_SCREEN_WIDTH;
    }

    return cs->visible_width;
}

#if LPST_PLATFORM_AMIGA
static void discard_pending_input(console_state *cs);
static uint8_t amiga_detect_console_width(void);
#endif

static console_state *get_cs(lpst_host *host)
{
    return (console_state *)host->user_data;
}

static bool style_uses_reverse(uint16_t style_bits)
{
    return (style_bits & 0x0001u) != 0;
}

static bool style_uses_bright(uint16_t style_bits)
{
    return (style_bits & 0x0008u) != 0;
}

static bool style_uses_blink(uint16_t style_bits)
{
    return (style_bits & 0x0020u) != 0;
}

/* ---- Key queue management ---- */

/*
 * Add a key code to the circular key queue.
 * Keys are encoded as 16-bit values: ASCII and control characters occupy a
 * single entry; extended (non-ASCII) keys are stored as two entries — 0x1B
 * followed by a scan code.  If the queue is full, the key is silently
 * discarded rather than blocking.
 */
static void queue_key(console_state *cs, uint16_t key)
{
    if (cs->key_queue_count < 16) {
        int idx = (cs->key_queue_head + cs->key_queue_count) % 16;
        cs->key_queue[idx] = key;
        cs->key_queue_count++;
    }
}

/*
 * Remove and return the oldest key code from the circular key queue.
 * Returns FALSE_SENTINEL (0x8001) when the queue is empty, signalling
 * "no key available" to the caller without blocking.
 */
static uint16_t dequeue_key(console_state *cs)
{
    uint16_t key;
    if (cs->key_queue_count == 0) return FALSE_SENTINEL;
    key = cs->key_queue[cs->key_queue_head];
    cs->key_queue_head = (cs->key_queue_head + 1) % 16;
    cs->key_queue_count--;
    return key;
}

#define LPST_SEQUENCE_KIND_NONE UINT8_C(0)
#define LPST_SEQUENCE_KIND_ESC  UINT8_C(1)
#define LPST_SEQUENCE_KIND_CSI  UINT8_C(2)

#if LPST_PLATFORM_ATARI

/* ---- Atari VT52 display helpers ---- */

static void atari_move_cursor(uint8_t row, uint8_t col)
{
    char seq[5];

    seq[0] = 0x1B;
    seq[1] = 'Y';
    seq[2] = (char)(row + 32);
    seq[3] = (char)(col + 32);
    seq[4] = '\0';
    Cconws(seq);
}

static void atari_clear_screen(void)
{
    Cconws("\033E");
}

static void atari_set_cursor_visible(bool visible)
{
    Cconws(visible ? "\033e" : "\033f");
}

static void atari_set_wrap(bool enabled)
{
    Cconws(enabled ? "\033v" : "\033w");
}

static void atari_insert_line(void)
{
    Cconws("\033L");
}

static void atari_delete_line(void)
{
    Cconws("\033M");
}

static void atari_put_display_char(uint8_t ch)
{
    Cconout((int16_t)ch);
}

static void atari_apply_style(console_state *cs, uint16_t style_bits)
{
    bool want_reverse = style_uses_reverse(style_bits);
    bool want_bright = style_uses_bright(style_bits);
    bool want_blink = style_uses_blink(style_bits);

    if (cs->reverse_active == want_reverse &&
        cs->bright_active == want_bright &&
        cs->blink_active == want_blink) {
        return;
    }

    /*
     * Atari VT52 only supports reverse video (\033p / \033q).
     * Bright and blink have no VT52 equivalents, so we track them
     * (to keep state consistent across redraws) but cannot render them.
     */
    if (want_reverse != cs->reverse_active) {
        Cconws(want_reverse ? "\033p" : "\033q");
    }
    cs->reverse_active = want_reverse;
    cs->bright_active = want_bright;
    cs->blink_active = want_blink;
}

/*
 * Redraw one display row on the Atari screen from the backing cell buffer.
 * Issues a VT52 erase-to-end-of-line before outputting characters so that
 * unset cells appear as blanks.  Style attributes are applied inline via
 * atari_apply_style() to minimise the number of escape sequences emitted.
 */
static void atari_redraw_row(console_state *cs, uint8_t row)
{
    uint8_t col;
    uint8_t width = console_visible_width(cs);
    uint16_t active_style = UINT16_C(0xFFFF);

    atari_move_cursor(row, 0);
    Cconws("\033l");

    for (col = 0; col < width; col++) {
        uint8_t ch = cs->cells[row][col];
        uint16_t style_bits = cs->attrs[row][col];
        if (style_bits != active_style) {
            atari_apply_style(cs, style_bits);
            active_style = style_bits;
        }
        if (ch >= 0x20 && ch < 0x7F) {
            atari_put_display_char(ch);
        } else {
            atari_put_display_char(' ');
        }
    }
}

static void atari_redraw_rows_impl(console_state *cs, uint8_t from_row, uint8_t to_row)
{
    uint8_t row;

    if (from_row >= LPST_SCREEN_HEIGHT) {
        return;
    }

    if (to_row >= LPST_SCREEN_HEIGHT) {
        to_row = LPST_SCREEN_HEIGHT - 1;
    }

    for (row = from_row; row <= to_row; row++) {
        atari_redraw_row(cs, row);
    }
}

static void atari_redraw_rows(console_state *cs, uint8_t from_row, uint8_t to_row)
{
    atari_set_cursor_visible(false);
    atari_redraw_rows_impl(cs, from_row, to_row);
    atari_set_cursor_visible(true);
}

/*
 * Try to scroll a region up using hardware VT52 delete-line sequences.
 * Only works when the region extends to the bottom of the screen; returns
 * false otherwise so the caller can fall back to a full redraw.  Issues
 * `lines` delete-line commands at from_row, then redraws only the vacated
 * rows at the bottom of the region rather than the whole screen.
 */
static bool atari_scroll_up_native(console_state *cs, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    uint8_t region_height;
    uint8_t visible_lines;
    uint8_t first_redraw_row;

    if (to_row != LPST_SCREEN_HEIGHT - 1) {
        return false;
    }

    region_height = (uint8_t)(to_row - from_row + 1);
    visible_lines = lines < region_height ? lines : region_height;

    atari_set_cursor_visible(false);

    for (uint8_t step = 0; step < visible_lines; step++) {
        atari_move_cursor(from_row, 0);
        atari_delete_line();
    }

    first_redraw_row = (uint8_t)(to_row + 1 - visible_lines);
    atari_redraw_rows_impl(cs, first_redraw_row, to_row);
    atari_set_cursor_visible(true);
    return true;
}

/*
 * Try to scroll a region down using hardware VT52 insert-line sequences.
 * Only works when the region extends to the bottom of the screen; returns
 * false otherwise so the caller can fall back to a full redraw.  Issues
 * `lines` insert-line commands at from_row, then redraws only the rows
 * that were shifted in at the top of the region.
 */
static bool atari_scroll_down_native(console_state *cs, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    uint8_t region_height;
    uint8_t visible_lines;
    uint8_t last_redraw_row;

    if (to_row != LPST_SCREEN_HEIGHT - 1) {
        return false;
    }

    region_height = (uint8_t)(to_row - from_row + 1);
    visible_lines = lines < region_height ? lines : region_height;

    atari_set_cursor_visible(false);

    atari_move_cursor(from_row, 0);
    for (uint8_t step = 0; step < visible_lines; step++) {
        atari_insert_line();
    }

    last_redraw_row = (uint8_t)(from_row + visible_lines - 1);
    atari_redraw_rows_impl(cs, from_row, last_redraw_row);
    atari_set_cursor_visible(true);
    return true;
}

/* ---- Key mapping: Atari raw console input → Cornerstone scan codes ---- */

/*
 * Read the Atari BIOS CONTERM byte from supervisor-mode memory.
 * CONTERM controls which status information Crawcin() returns.  Setting
 * bit 3 causes the high byte of the Crawcin() return value to carry the
 * shift/ctrl/alt bitmask, which we need to distinguish Shift+Tab from
 * plain Tab.  The byte lives in low RAM and must be accessed in supervisor
 * mode.
 */
static uint8_t atari_get_conterm(void)
{
    void *saved_stack = (void *)Super(0);
    uint8_t value = *LPST_ATARI_CONTERM_ADDR;
    Super(saved_stack);
    return value;
}

/*
 * Write the Atari BIOS CONTERM byte in supervisor mode.
 * Called once at startup to enable shift-status reporting and once at
 * shutdown to restore the original value saved in the console_state.
 */
static void atari_set_conterm(uint8_t value)
{
    void *saved_stack = (void *)Super(0);
    *LPST_ATARI_CONTERM_ADDR = value;
    Super(saved_stack);
}

static void queue_atari_extended_key(console_state *cs, uint8_t scan_code)
{
    queue_key(cs, 0x1B);
    queue_key(cs, scan_code);
}

/*
 * Map Atari-specific scan codes to the Cornerstone navigation scan-code space.
 * The Atari Undo key (scan 0x61) is mapped to Page Down and the Help key
 * (scan 0x62) to Page Up, the closest equivalent Cornerstone functions.
 * All other scan codes pass through unchanged.
 */
static uint8_t translate_atari_scan_code(uint8_t scan_code)
{
    switch (scan_code) {
    case 0x61:
        return 0x51; /* Atari Undo -> Cornerstone Page Down */

    case 0x62:
        return 0x49; /* Atari Help -> Cornerstone Page Up */

    default:
        return scan_code;
    }
}

/*
 * Read one key event from the Atari raw console via Crawcin().
 * Crawcin() returns a 32-bit value: high byte = shift status, next byte =
 * scan code, low byte = ASCII.  This function translates that tuple:
 *   - Non-zero ASCII: enqueue it directly.  LF→CR, DEL→Backspace.
 *   - Zero ASCII + non-zero scan: extended key, queued as ESC + translated scan.
 *   - Shift+Tab: special-cased because it shares the Tab scan code but is
 *     distinguished by the shift-status byte.
 * Does nothing if no key is available (Cconis() returns 0).
 */
static void try_read_key(console_state *cs)
{
    long raw_key;
    uint8_t shift_status;
    uint8_t ascii;
    uint8_t scan_code;
    uint8_t translated_scan_code;

    if (Cconis() == 0) {
        return;
    }

    raw_key = Crawcin();
    shift_status = (uint8_t)((raw_key >> 24) & 0xFF);
    ascii = (uint8_t)(raw_key & 0xFF);
    scan_code = (uint8_t)((raw_key >> 16) & 0xFF);
    translated_scan_code = translate_atari_scan_code(scan_code);

    if (ascii == 0x09 && scan_code == 0x0F && (shift_status & LPST_ATARI_SHIFT_MASK) != 0) {
        queue_atari_extended_key(cs, 0x0F);
        return;
    }

    if (ascii != 0) {
        if (ascii == 0x0A) {
            queue_key(cs, 0x0D);
        } else if (ascii == 0x7F) {
            queue_key(cs, 0x08);
        } else {
            queue_key(cs, ascii);
        }
        return;
    }

    if (scan_code != 0) {
        queue_atari_extended_key(cs, translated_scan_code);
    }
}

#else

/* ---- Terminal raw mode ---- */

#if LPST_PLATFORM_AMIGA

/*
 * Flush all pending bytes from an Amiga I/O handle without processing them.
 * Called before entering raw mode and before sending a terminal inquiry
 * request, to prevent stale input bytes from contaminating the response
 * parser.  Stops as soon as no more bytes arrive within one polling interval.
 */
static void discard_pending_input_handle(BPTR input_handle)
{
    uint8_t discard[32];

    if (input_handle == (BPTR)0) {
        return;
    }

    while (WaitForChar(input_handle, 1) != 0) {
        LONG n = Read(input_handle, discard, (LONG)sizeof(discard));

        if (n <= 0) {
            break;
        }

        if (n < (LONG)sizeof(discard) && WaitForChar(input_handle, 0) == 0) {
            break;
        }
    }
}

static void discard_pending_input(console_state *cs)
{
    discard_pending_input_handle(cs->input_handle);
}

/*
 * Read one CSI window-bounds report from the given Amiga input handle.
 * After sending "CSI 0 SP q", the Amiga console device replies with a
 * CSI <top>;<left>;<bottom>;<right> r sequence.  Bytes are read one at a
 * time with a short timeout; reading stops when the 'r' terminator is seen
 * or the idle-tick limit is reached.
 * Returns the number of bytes placed in buffer, or -1 on I/O error.
 */
static LONG amiga_read_window_bounds_report(BPTR input_handle, char *buffer, LONG buffer_size)
{
    LONG length = 0;
    LONG idle_ticks = 0;
    int state = 0;

    while (length < buffer_size - 1) {
        char ch;
        LONG ready = WaitForChar(input_handle, idle_ticks == 0 ? 10 : 1);

        if (ready == 0) {
            idle_ticks++;
            if (idle_ticks >= LPST_AMIGA_READ_TIMEOUT_TICKS) {
                break;
            }
            continue;
        }

        idle_ticks = 0;

        if (Read(input_handle, &ch, 1) != 1) {
            return -1;
        }

        if (state == 0) {
            if ((unsigned char)ch == 0x9B) {
                buffer[0] = ch;
                length = 1;
                state = 2;
            } else if ((unsigned char)ch == 0x1B) {
                state = 1;
            }
            continue;
        }

        if (state == 1) {
            if (ch == '[') {
                buffer[0] = 0x1B;
                buffer[1] = '[';
                length = 2;
                state = 2;
            } else {
                state = 0;
            }
            continue;
        }

        buffer[length++] = ch;
        if (ch == 'r') {
            buffer[length] = '\0';
            return length;
        }
    }

    return -1;
}

static bool amiga_parse_decimal(const char **cursor, LONG *value)
{
    char *end = NULL;
    long parsed = strtol(*cursor, &end, 10);

    if (end == *cursor || parsed < 0) {
        return false;
    }

    *cursor = end;
    *value = (LONG)parsed;
    return true;
}

/*
 * Parse an Amiga CSI window-bounds report into row and column dimensions.
 * Accepts both the compact 8-bit CSI introducer (0x9B) and the two-byte
 * ESC '[' form.  Sets *rows and *cols on success and returns true.
 * Returns false if the report is malformed or if the window does not start
 * at position (1, 1), which would indicate an unexpected console geometry.
 */
static bool amiga_parse_window_bounds_report(const char *report, LONG *rows, LONG *cols)
{
    const char *cursor = report;
    LONG top = 0;
    LONG left = 0;
    LONG bottom = 0;
    LONG right = 0;

    if ((unsigned char)cursor[0] == 0x9B) {
        cursor += 1;
    } else if ((unsigned char)cursor[0] == 0x1B && cursor[1] == '[') {
        cursor += 2;
    } else {
        return false;
    }

    if (!amiga_parse_decimal(&cursor, &top) || *cursor++ != ';') {
        return false;
    }

    if (!amiga_parse_decimal(&cursor, &left) || *cursor++ != ';') {
        return false;
    }

    if (!amiga_parse_decimal(&cursor, &bottom) || *cursor++ != ';') {
        return false;
    }

    if (!amiga_parse_decimal(&cursor, &right)) {
        return false;
    }

    while (*cursor == ' ') {
        cursor++;
    }

    if (*cursor != 'r' || top != 1 || left != 1) {
        return false;
    }

    *rows = bottom;
    *cols = right;
    return true;
}

/*
 * Query the Amiga console device for the actual window width.
 * Sends a CSI 0 SP q request and parses the window-bounds response to
 * extract the column count.  Raw mode is briefly enabled so the system
 * does not consume the CSI reply bytes before we can read them.
 * Returns LPST_DEFAULT_SCREEN_WIDTH if the handle is not interactive,
 * I/O fails, or the response cannot be parsed.
 */
static uint8_t amiga_detect_console_width(void)
{
    static const char request[] = "\x9b" "0 q";
    BPTR input_handle = Input();
    BPTR output_handle = Output();
    char report[LPST_AMIGA_REPORT_BUFFER_SIZE];
    LONG rows = 0;
    LONG cols = 0;

    if (input_handle == (BPTR)0 || output_handle == (BPTR)0) {
        return LPST_DEFAULT_SCREEN_WIDTH;
    }

    if (IsInteractive(input_handle) == DOSFALSE || IsInteractive(output_handle) == DOSFALSE) {
        return LPST_DEFAULT_SCREEN_WIDTH;
    }

    SetMode(input_handle, DOSTRUE);
    discard_pending_input_handle(input_handle);

    if (Write(output_handle, request, (LONG)(sizeof(request) - 1)) != (LONG)(sizeof(request) - 1)) {
        SetMode(input_handle, DOSFALSE);
        return LPST_DEFAULT_SCREEN_WIDTH;
    }

    Flush(output_handle);

    if (amiga_read_window_bounds_report(input_handle, report, (LONG)sizeof(report)) < 0) {
        SetMode(input_handle, DOSFALSE);
        return LPST_DEFAULT_SCREEN_WIDTH;
    }

    SetMode(input_handle, DOSFALSE);

    if (!amiga_parse_window_bounds_report(report, &rows, &cols) || cols <= 0) {
        return LPST_DEFAULT_SCREEN_WIDTH;
    }

    if (cols > LPST_SCREEN_WIDTH) {
        cols = LPST_SCREEN_WIDTH;
    }

    return (uint8_t)cols;
}

/*
 * Enable Amiga raw console input so key events arrive one byte at a time.
 * Uses SetMode(DOSTRUE) on the console input handle, which bypasses
 * line-editing.  Discards any pending input to avoid contaminating the
 * first read after the mode switch.
 */
static void enter_raw_mode(console_state *cs)
{
    if (cs->raw_mode) return;

    cs->input_handle = Input();
    if (cs->input_handle == (BPTR)0) {
        return;
    }

    SetMode(cs->input_handle, DOSTRUE);
    discard_pending_input(cs);
    cs->raw_mode = true;
}

static void leave_raw_mode(console_state *cs)
{
    if (!cs->raw_mode || cs->input_handle == (BPTR)0) return;

    SetMode(cs->input_handle, DOSFALSE);
    cs->raw_mode = false;
    cs->input_handle = (BPTR)0;
}

#else

/*
 * Enable POSIX terminal raw mode on stdin.
 * Saves the current termios settings and applies a configuration that
 * delivers bytes immediately (VMIN=0, VTIME=0), disables echo and signal
 * generation, and suppresses output processing.  This lets the console
 * host handle all keystroke logic, including multi-byte escape sequences.
 */
static void enter_raw_mode(console_state *cs)
{
    struct termios raw;
    if (cs->raw_mode) return;
    if (tcgetattr(STDIN_FILENO, &cs->orig_termios) == -1) return;
    raw = cs->orig_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return;
    cs->raw_mode = true;
}

static void leave_raw_mode(console_state *cs)
{
    if (!cs->raw_mode) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cs->orig_termios);
    cs->raw_mode = false;
}

#endif

/* ---- ANSI display helpers ---- */

static void ansi_move_cursor(uint8_t row, uint8_t col)
{
    printf("\033[%d;%dH", row + 1, col + 1);
}

static void ansi_set_wrap(bool enabled)
{
    printf(enabled ? "\033[?7h" : "\033[?7l");
}

static void ansi_apply_style(console_state *cs, uint16_t style_bits)
{
    bool want_reverse = style_uses_reverse(style_bits);
    bool want_bright = style_uses_bright(style_bits);
    bool want_blink = style_uses_blink(style_bits);

    if (cs->reverse_active == want_reverse &&
        cs->bright_active == want_bright &&
        cs->blink_active == want_blink) {
        return;
    }

    /*
     * Always reset first, then re-apply whichever attributes are active.
     * This correctly handles any transition (e.g. reverse+bright -> just bright)
     * since SGR codes are additive and \033[0m is the only reliable reset.
     */
    printf("\033[0m");
    if (want_reverse) printf("\033[7m");
    if (want_bright)  printf("\033[1m");
    if (want_blink)   printf("\033[5m");

    cs->reverse_active = want_reverse;
    cs->bright_active = want_bright;
    cs->blink_active = want_blink;
}

static void ansi_clear_screen(void)
{
    printf("\033[2J\033[H");
}

static void ansi_redraw_row(console_state *cs, uint8_t row)
{
    uint8_t col;
    uint8_t width = console_visible_width(cs);
    uint16_t saved_style = cs->current_style_bits;
    uint16_t active_style = UINT16_C(0xFFFF);

    ansi_move_cursor(row, 0);
    printf("\033[2K");
    ansi_move_cursor(row, 0);

    for (col = 0; col + 1 < width; col++) {
        uint8_t ch = cs->cells[row][col];
        uint16_t style_bits = cs->attrs[row][col];
        if (style_bits != active_style) {
            ansi_apply_style(cs, style_bits);
            active_style = style_bits;
        }
        putchar(ch >= 0x20 && ch < 0x7F ? ch : ' ');
    }

    if (width > 0) {
        uint8_t last_col = (uint8_t)(width - 1);
        uint8_t ch = cs->cells[row][last_col];
        uint16_t style_bits = cs->attrs[row][last_col];
        if (style_bits != active_style) {
            ansi_apply_style(cs, style_bits);
        }
        ansi_move_cursor(row, last_col);
        putchar(ch >= 0x20 && ch < 0x7F ? ch : ' ');
    }

    ansi_apply_style(cs, saved_style);
}

static void ansi_redraw_rows(console_state *cs, uint8_t from_row, uint8_t to_row)
{
    uint8_t row;

    if (from_row >= LPST_SCREEN_HEIGHT) {
        return;
    }

    if (to_row >= LPST_SCREEN_HEIGHT) {
        to_row = LPST_SCREEN_HEIGHT - 1;
    }

    for (row = from_row; row <= to_row; row++) {
        ansi_redraw_row(cs, row);
    }
}

/* ---- Key mapping: ANSI escape sequences → Cornerstone scan codes ---- */

/*
 * Read available bytes from stdin into buf (non-blocking).
 * Returns the number of bytes read.
 */
static int read_available(uint8_t *buf, int max_len)
{
#if LPST_PLATFORM_AMIGA
    console_state *cs = &g_console_state;
    int total = 0;

    if (cs->input_handle == (BPTR)0) {
        return 0;
    }

    while (total < max_len) {
        LONG ready = WaitForChar(cs->input_handle, total == 0 ? 1 : 0);
        LONG n;

        if (ready == 0) break;

        n = Read(cs->input_handle, buf + total, 1);
        if (n <= 0) break;
        total += (int)n;
    }

    return total;
#else
    int total = 0;
    while (total < max_len) {
        ssize_t n = read(STDIN_FILENO, buf + total, (size_t)(max_len - total));
        if (n <= 0) break;
        total += (int)n;
    }
    return total;
#endif
}

/*
 * Map a VT/xterm escape sequence to a Cornerstone scan code.
 * Queues ESC + scan code into the key queue.
 * Returns true if the sequence was recognized.
 */
static bool map_escape_sequence(console_state *cs, const uint8_t *seq, int len)
{
    if (len == 0) {
        /* Bare ESC: Cornerstone sees it as ESC ESC */
        queue_key(cs, 0x1B);
        queue_key(cs, 0x1B);
        return true;
    }

    /* CSI sequences: ESC [ ... */
    if (seq[0] == '[') {
        if (len >= 2) {
            /* Arrow keys and basic nav: ESC [ A/B/C/D/H/F */
            switch (seq[1]) {
            case 'A': queue_key(cs, 0x1B); queue_key(cs, 0x48); return true; /* Up */
            case 'B': queue_key(cs, 0x1B); queue_key(cs, 0x50); return true; /* Down */
            case 'C': queue_key(cs, 0x1B); queue_key(cs, 0x4D); return true; /* Right */
            case 'D': queue_key(cs, 0x1B); queue_key(cs, 0x4B); return true; /* Left */
            case 'H': queue_key(cs, 0x1B); queue_key(cs, 0x47); return true; /* Home */
            case 'F': queue_key(cs, 0x1B); queue_key(cs, 0x4F); return true; /* End */
            default: break;
            }

            /* Tilde sequences: ESC [ <number> ~ */
            if (seq[1] >= '0' && seq[1] <= '9') {
                int num = seq[1] - '0';
                int idx = 2;
                while (idx < len && seq[idx] >= '0' && seq[idx] <= '9') {
                    num = num * 10 + (seq[idx] - '0');
                    idx++;
                }
                if (idx < len && seq[idx] == '~') {
                    switch (num) {
                    case 2:  queue_key(cs, 0x1B); queue_key(cs, 0x52); return true; /* Insert */
                    case 3:  queue_key(cs, 0x1B); queue_key(cs, 0x53); return true; /* Delete */
                    case 5:  queue_key(cs, 0x1B); queue_key(cs, 0x49); return true; /* PgUp */
                    case 6:  queue_key(cs, 0x1B); queue_key(cs, 0x51); return true; /* PgDn */
                    /* F1-F5 use ESC [ <num> ~ in some terminals */
                    case 11: queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* F1 */
                    case 12: queue_key(cs, 0x1B); queue_key(cs, 0x3C); return true; /* F2 */
                    case 13: queue_key(cs, 0x1B); queue_key(cs, 0x3D); return true; /* F3 */
                    case 14: queue_key(cs, 0x1B); queue_key(cs, 0x3E); return true; /* F4 */
                    case 15: queue_key(cs, 0x1B); queue_key(cs, 0x3F); return true; /* F5 */
                    case 17: queue_key(cs, 0x1B); queue_key(cs, 0x40); return true; /* F6 */
                    case 18: queue_key(cs, 0x1B); queue_key(cs, 0x41); return true; /* F7 */
                    case 19: queue_key(cs, 0x1B); queue_key(cs, 0x42); return true; /* F8 */
                    case 20: queue_key(cs, 0x1B); queue_key(cs, 0x43); return true; /* F9 */
                    case 21: queue_key(cs, 0x1B); queue_key(cs, 0x44); return true; /* F10 */
                    case 23: queue_key(cs, 0x1B); queue_key(cs, 0x85); return true; /* F11 */
                    case 24: queue_key(cs, 0x1B); queue_key(cs, 0x86); return true; /* F12 */
                    default: break;
                    }
                }
                /* Ctrl-modified sequences: ESC [ 1 ; 5 A etc. */
                if (idx < len && seq[idx] == ';' && idx + 2 < len) {
                    int mod = seq[idx + 1] - '0';
                    uint8_t suffix = seq[idx + 2];
                    if (mod == 5) { /* Ctrl modifier */
                        switch (suffix) {
                        case 'A': queue_key(cs, 0x1B); queue_key(cs, 0x8D); return true; /* Ctrl+Up */
                        case 'B': queue_key(cs, 0x1B); queue_key(cs, 0x91); return true; /* Ctrl+Down */
                        case 'C': queue_key(cs, 0x1B); queue_key(cs, 0x74); return true; /* Ctrl+Right */
                        case 'D': queue_key(cs, 0x1B); queue_key(cs, 0x73); return true; /* Ctrl+Left */
                        case 'H': queue_key(cs, 0x1B); queue_key(cs, 0x77); return true; /* Ctrl+Home */
                        case 'F': queue_key(cs, 0x1B); queue_key(cs, 0x75); return true; /* Ctrl+End */
                        default: break;
                        }
                    }
                }
            }
        }

        /* Shift+Tab: ESC [ Z */
        if (len >= 2 && seq[1] == 'Z') {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x0F);
            return true;
        }
    }

    /* SS3 sequences: ESC O P through ESC O S = F1..F4 */
    if (seq[0] == 'O' && len >= 2) {
        switch (seq[1]) {
        case 'P': queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* F1 */
        case 'Q': queue_key(cs, 0x1B); queue_key(cs, 0x3C); return true; /* F2 */
        case 'R': queue_key(cs, 0x1B); queue_key(cs, 0x3D); return true; /* F3 */
        case 'S': queue_key(cs, 0x1B); queue_key(cs, 0x3E); return true; /* F4 */
        case 'H': queue_key(cs, 0x1B); queue_key(cs, 0x47); return true; /* Home */
        case 'F': queue_key(cs, 0x1B); queue_key(cs, 0x4F); return true; /* End */
        default: break;
        }
    }

    /* Unrecognized escape sequence: drop it */
    return false;
}

static bool map_amiga_csi_sequence(console_state *cs, const uint8_t *seq, int len)
{
#if LPST_PLATFORM_AMIGA
    int value;
    int index;

    switch (seq[0]) {
    case '?':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x3B);
            return true; /* Help => F1 */
        }
        break;

    case 'A':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x48);
            return true; /* Up */
        }
        break;

    case 'B':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x50);
            return true; /* Down */
        }
        break;

    case 'C':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x4D);
            return true; /* Right */
        }
        break;

    case 'D':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x4B);
            return true; /* Left */
        }
        break;

    case 'T':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x8D);
            return true; /* Shift+Up -> Ctrl+Up */
        }
        break;

    case 'S':
        if (len == 1) {
            queue_key(cs, 0x1B);
            queue_key(cs, 0x91);
            return true; /* Shift+Down -> Ctrl+Down */
        }
        break;

    case ' ':
        if (len >= 2) {
            switch (seq[1]) {
            case 'A':
                queue_key(cs, 0x1B);
                queue_key(cs, 0x73);
                return true; /* Shift+Left -> Ctrl+Left */

            case '@':
                queue_key(cs, 0x1B);
                queue_key(cs, 0x74);
                return true; /* Shift+Right -> Ctrl+Right */

            default:
                break;
            }
        }
        break;

    default:
        break;
    }

    if (len <= 1 || seq[len - 1] != '~') {
        return false;
    }

    len -= 1;

    if (seq[0] < '0' || seq[0] > '9') {
        return false;
    }

    value = seq[0] - '0';
    index = 1;

    while (index < len && seq[index] >= '0' && seq[index] <= '9') {
        value = value * 10 + (seq[index] - '0');
        index++;
    }

    if (index != len) {
        return false;
    }

    switch (value) {
    case 0:
    case 10: queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* F1 / Shift+F1 */
    case 1:
    case 11: queue_key(cs, 0x1B); queue_key(cs, 0x3C); return true; /* F2 / Shift+F2 */
    case 2:
    case 12: queue_key(cs, 0x1B); queue_key(cs, 0x3D); return true; /* F3 / Shift+F3 */
    case 3:
    case 13: queue_key(cs, 0x1B); queue_key(cs, 0x3E); return true; /* F4 / Shift+F4 */
    case 4:
    case 14: queue_key(cs, 0x1B); queue_key(cs, 0x3F); return true; /* F5 / Shift+F5 */
    case 5:
    case 15: queue_key(cs, 0x1B); queue_key(cs, 0x40); return true; /* F6 / Shift+F6 */
    case 6:
    case 16: queue_key(cs, 0x1B); queue_key(cs, 0x41); return true; /* F7 / Shift+F7 */
    case 7:
    case 17: queue_key(cs, 0x1B); queue_key(cs, 0x42); return true; /* F8 / Shift+F8 */
    case 8:
    case 18: queue_key(cs, 0x1B); queue_key(cs, 0x43); return true; /* F9 / Shift+F9 */
    case 9:
    case 19: queue_key(cs, 0x1B); queue_key(cs, 0x44); return true; /* F10 / Shift+F10 */
    case 20:
    case 30: queue_key(cs, 0x1B); queue_key(cs, 0x85); return true; /* F11 / Shift+F11 */
    case 21:
    case 31: queue_key(cs, 0x1B); queue_key(cs, 0x86); return true; /* F12 / Shift+F12 */
    case 40:
    case 50: queue_key(cs, 0x1B); queue_key(cs, 0x52); return true; /* Insert / Shift+Insert */
    case 41:
    case 51: queue_key(cs, 0x1B); queue_key(cs, 0x49); return true; /* Page Up / Shift+Page Up */
    case 42:
    case 52: queue_key(cs, 0x1B); queue_key(cs, 0x51); return true; /* Page Down / Shift+Page Down */
    case 44: queue_key(cs, 0x1B); queue_key(cs, 0x47); return true; /* Home */
    case 54: queue_key(cs, 0x1B); queue_key(cs, 0x77); return true; /* Shift+Home -> Ctrl+Home */
    case 45: queue_key(cs, 0x1B); queue_key(cs, 0x4F); return true; /* End */
    case 55: queue_key(cs, 0x1B); queue_key(cs, 0x75); return true; /* Shift+End -> Ctrl+End */
    default:
        break;
    }

    return false;
#else
    int value;
    int index;

    if (len <= 0) {
        return false;
    }

    switch (seq[0]) {
    case '?': queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* Help => F1 */
    case 'A': queue_key(cs, 0x1B); queue_key(cs, 0x48); return true; /* Up */
    case 'B': queue_key(cs, 0x1B); queue_key(cs, 0x50); return true; /* Down */
    case 'C': queue_key(cs, 0x1B); queue_key(cs, 0x4D); return true; /* Right */
    case 'D': queue_key(cs, 0x1B); queue_key(cs, 0x4B); return true; /* Left */
    case 'H': queue_key(cs, 0x1B); queue_key(cs, 0x47); return true; /* Home */
    case 'F': queue_key(cs, 0x1B); queue_key(cs, 0x4F); return true; /* End */
    case 'T': queue_key(cs, 0x1B); queue_key(cs, 0x8D); return true; /* Shift+Up */
    case 'S': queue_key(cs, 0x1B); queue_key(cs, 0x91); return true; /* Shift+Down */
    case 'Z': queue_key(cs, 0x1B); queue_key(cs, 0x0F); return true; /* Shift+Tab */
    case ' ': {
        if (len >= 2) {
            switch (seq[1]) {
            case 'A': queue_key(cs, 0x1B); queue_key(cs, 0x4B); return true; /* Shift+Left */
            case '@': queue_key(cs, 0x1B); queue_key(cs, 0x4D); return true; /* Shift+Right */
            default: break;
            }
        }
        break;
    }
    default:
        break;
    }

    if (seq[0] >= '0' && seq[0] <= '9') {
        value = seq[0] - '0';
        index = 1;

        while (index < len && seq[index] >= '0' && seq[index] <= '9') {
            value = value * 10 + (seq[index] - '0');
            index++;
        }

        if (index < len && seq[index] == '~') {
            switch (value) {
            case 2: queue_key(cs, 0x1B); queue_key(cs, 0x52); return true; /* Insert */
            case 3: queue_key(cs, 0x1B); queue_key(cs, 0x53); return true; /* Delete */
            case 5: queue_key(cs, 0x1B); queue_key(cs, 0x49); return true; /* Page Up */
            case 6: queue_key(cs, 0x1B); queue_key(cs, 0x51); return true; /* Page Down */
            case 11: queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* F1 */
            case 12: queue_key(cs, 0x1B); queue_key(cs, 0x3C); return true; /* F2 */
            case 13: queue_key(cs, 0x1B); queue_key(cs, 0x3D); return true; /* F3 */
            case 14: queue_key(cs, 0x1B); queue_key(cs, 0x3E); return true; /* F4 */
            case 15: queue_key(cs, 0x1B); queue_key(cs, 0x3F); return true; /* F5 */
            case 17: queue_key(cs, 0x1B); queue_key(cs, 0x40); return true; /* F6 */
            case 18: queue_key(cs, 0x1B); queue_key(cs, 0x41); return true; /* F7 */
            case 19: queue_key(cs, 0x1B); queue_key(cs, 0x42); return true; /* F8 */
            case 20: queue_key(cs, 0x1B); queue_key(cs, 0x43); return true; /* F9 */
            case 21: queue_key(cs, 0x1B); queue_key(cs, 0x44); return true; /* F10 */
            case 23: queue_key(cs, 0x1B); queue_key(cs, 0x85); return true; /* F11 */
            case 24: queue_key(cs, 0x1B); queue_key(cs, 0x86); return true; /* F12 */
            default: break;
            }
        }

        if (index == len) {
            switch (value) {
            case 0: queue_key(cs, 0x1B); queue_key(cs, 0x3B); return true; /* F1 */
            case 1: queue_key(cs, 0x1B); queue_key(cs, 0x3C); return true; /* F2 */
            case 2: queue_key(cs, 0x1B); queue_key(cs, 0x3D); return true; /* F3 */
            case 3: queue_key(cs, 0x1B); queue_key(cs, 0x3E); return true; /* F4 */
            case 4: queue_key(cs, 0x1B); queue_key(cs, 0x3F); return true; /* F5 */
            case 5: queue_key(cs, 0x1B); queue_key(cs, 0x40); return true; /* F6 */
            case 6: queue_key(cs, 0x1B); queue_key(cs, 0x41); return true; /* F7 */
            case 7: queue_key(cs, 0x1B); queue_key(cs, 0x42); return true; /* F8 */
            case 8: queue_key(cs, 0x1B); queue_key(cs, 0x43); return true; /* F9 */
            case 9: queue_key(cs, 0x1B); queue_key(cs, 0x44); return true; /* F10 */
            case 20: queue_key(cs, 0x1B); queue_key(cs, 0x85); return true; /* F11 */
            case 21: queue_key(cs, 0x1B); queue_key(cs, 0x86); return true; /* F12 */
            case 40: queue_key(cs, 0x1B); queue_key(cs, 0x52); return true; /* Insert */
            case 41: queue_key(cs, 0x1B); queue_key(cs, 0x49); return true; /* Page Up */
            case 42: queue_key(cs, 0x1B); queue_key(cs, 0x51); return true; /* Page Down */
            case 44: queue_key(cs, 0x1B); queue_key(cs, 0x47); return true; /* Home */
            case 45: queue_key(cs, 0x1B); queue_key(cs, 0x4F); return true; /* End */
            default: break;
            }
        }
    }

    return false;
#endif
}

static bool is_deliverable_console_byte(uint8_t value);

static void reset_pending_sequence(console_state *cs)
{
    cs->pending_sequence_len = 0;
    cs->pending_sequence_kind = LPST_SEQUENCE_KIND_NONE;
    cs->pending_sequence_idle_polls = 0;
}

#if !LPST_PLATFORM_AMIGA
static bool pending_sequence_all_digits(const console_state *cs)
{
    int index;

    if (cs->pending_sequence_len <= 0) {
        return false;
    }

    for (index = 0; index < cs->pending_sequence_len; index++) {
        if (cs->pending_sequence[index] < '0' || cs->pending_sequence[index] > '9') {
            return false;
        }
    }

    return true;
}
#endif

static void emit_bare_escape(console_state *cs)
{
    queue_key(cs, 0x1B);
    queue_key(cs, 0x1B);
}

/*
 * Dispatch the accumulated escape or CSI sequence and clear the pending state.
 * Translates the buffered byte sequence into one or more key-queue entries
 * using the appropriate mapping function.  If the sequence is unrecognised,
 * a bare ESC pair (0x1B 0x1B) is queued instead; for a CSI sequence, any
 * printable body bytes are also queued individually so no input is silently
 * dropped.
 */
static void finalize_pending_sequence(console_state *cs)
{
    bool handled = false;

    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_ESC) {
        handled = map_escape_sequence(cs, cs->pending_sequence, cs->pending_sequence_len);
        if (!handled) {
            emit_bare_escape(cs);
        }
    } else if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_CSI) {
        handled = map_amiga_csi_sequence(cs, cs->pending_sequence, cs->pending_sequence_len);
        if (!handled) {
            emit_bare_escape(cs);
            queue_key(cs, '[');
            for (int index = 0; index < cs->pending_sequence_len; index++) {
                if (is_deliverable_console_byte(cs->pending_sequence[index])) {
                    queue_key(cs, cs->pending_sequence[index]);
                }
            }
        }
    }

    reset_pending_sequence(cs);
}

/*
 * Return true when enough bytes have been accumulated to finalize the current
 * escape or CSI sequence.  Completion is inferred from the last byte: an SS3
 * body (ESC O x) is complete after two bytes; a standard CSI body ends when
 * the final byte falls in the range 0x40–0x7E; a bare ESC (no body bytes)
 * times out after enough idle polls.  On POSIX, all-digit CSI bodies are
 * also finalized on timeout, to handle function-key variants that lack a
 * deterministic terminator.
 */
static bool pending_sequence_is_complete(const console_state *cs)
{
    int last_index;
    uint8_t last;

    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_ESC
        && cs->pending_sequence_len == 0
        && cs->pending_sequence_idle_polls >= LPST_PENDING_SEQUENCE_IDLE_LIMIT) {
        return true;
    }

    if (cs->pending_sequence_len <= 0) {
        return false;
    }

    last_index = cs->pending_sequence_len - 1;
    last = cs->pending_sequence[last_index];

    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_ESC) {
        if (cs->pending_sequence[0] == 'O') {
            return cs->pending_sequence_len >= 2;
        }

        if (cs->pending_sequence[0] != '[') {
            return true;
        }

        return last >= 0x40 && last <= 0x7E;
    }

#if LPST_PLATFORM_AMIGA
    if (last == '~' || last == '|' || last == 'v' || last == '?') {
        return true;
    }

    switch (cs->pending_sequence[0]) {
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'S':
    case 'T':
        return true;

    case ' ':
        return cs->pending_sequence_len >= 2;

    default:
        return false;
    }
#else

    if (last == '~' || last == '?' || last == '|' || last == 'v') {
        return true;
    }

    if (cs->pending_sequence[0] == ' ' && cs->pending_sequence_len >= 2) {
        return true;
    }

    switch (cs->pending_sequence[0]) {
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'H':
    case 'F':
    case 'S':
    case 'T':
    case 'Z':
        return true;

    default:
        break;
    }

    if (pending_sequence_all_digits(cs) && cs->pending_sequence_idle_polls >= LPST_PENDING_SEQUENCE_IDLE_LIMIT) {
        return true;
    }

    return false;
#endif
}

static void append_pending_sequence_byte(console_state *cs, uint8_t value)
{
    if (cs->pending_sequence_len < (int)sizeof(cs->pending_sequence)) {
        cs->pending_sequence[cs->pending_sequence_len++] = value;
    }

    cs->pending_sequence_idle_polls = 0;
    if (pending_sequence_is_complete(cs)) {
        finalize_pending_sequence(cs);
    }
}

/*
 * Feed one raw terminal byte into the pending-sequence state machine.
 * ESC (0x1B) and 8-bit CSI (0x9B) begin a new sequence; ESC '[' within an
 * ESC sequence is promoted to CSI.  Subsequent bytes are accumulated by
 * append_pending_sequence_byte() until the sequence is complete.
 * Plain printable bytes and recognised control characters that arrive with
 * no pending sequence are translated and placed directly on the key queue:
 *   DEL (0x7F) → Backspace (0x08), LF (0x0A) → CR (0x0D).
 */
static void queue_console_byte(console_state *cs, uint8_t value)
{
    if (value == 0x1B) {
        reset_pending_sequence(cs);
        cs->pending_sequence_kind = LPST_SEQUENCE_KIND_ESC;
        return;
    }

    if (value == 0x9B) {
        reset_pending_sequence(cs);
        cs->pending_sequence_kind = LPST_SEQUENCE_KIND_CSI;
        return;
    }

    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_ESC) {
        if (cs->pending_sequence_len == 0 && value == '[') {
            cs->pending_sequence_kind = LPST_SEQUENCE_KIND_CSI;
            return;
        }

        append_pending_sequence_byte(cs, value);
        return;
    }

    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_CSI) {
        append_pending_sequence_byte(cs, value);
        return;
    }

    if (value == 0x7F) {
        queue_key(cs, 0x08);
    } else if (value == 0x0A) {
        queue_key(cs, 0x0D);
    } else if (is_deliverable_console_byte(value)) {
        queue_key(cs, (uint16_t)value);
    }
}

/*
 * Increment the idle-poll counter for the pending escape sequence.
 * Called when a key-read cycle returns no bytes.  The idle count drives
 * timeout-based completion for sequences that have no deterministic
 * terminator, such as a bare ESC or an all-digit CSI body on POSIX.
 * Calls finalize_pending_sequence() when the limit is reached.
 */
static void advance_pending_sequence_idle(console_state *cs)
{
    if (cs->pending_sequence_kind == LPST_SEQUENCE_KIND_NONE) {
        return;
    }

    if (cs->pending_sequence_idle_polls < UINT8_MAX) {
        cs->pending_sequence_idle_polls++;
    }

    if (pending_sequence_is_complete(cs)) {
        finalize_pending_sequence(cs);
    }
}

/*
 * Return true if a raw byte should be passed directly to the key queue.
 * Accepts Backspace (0x08), Tab (0x09), CR (0x0D), and printable ASCII
 * (0x20–0x7E).  Other control characters and high bytes are rejected to
 * avoid delivering undisplayable or ambiguous values to the VM key handler.
 */
static bool is_deliverable_console_byte(uint8_t value)
{
    return value == 0x08
        || value == 0x09
        || value == 0x0D
        || (value >= 0x20 && value < 0x7F);
}

/*
 * Try to read and process one key event from the terminal.
 * May queue multiple keys for escape sequences.
 */
static void try_read_key(console_state *cs)
{
    uint8_t buf[16];
    int n;
    int index;

    n = read_available(buf, 1);
    if (n == 0) {
        advance_pending_sequence_idle(cs);
        return;
    }

    if (buf[0] == 0x1B || buf[0] == 0x9B) {
        /* Give the rest of the sequence a chance to arrive. */
#if LPST_PLATFORM_AMIGA
        Delay(2);
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 20000000; /* 20ms — enough for escape sequences to arrive */
        nanosleep(&ts, NULL);
#endif

        n += read_available(buf + 1, (int)sizeof(buf) - 1);
    }

    for (index = 0; index < n; index++) {
        queue_console_byte(cs, buf[index]);
    }
}

#endif

/* ---- Host vtable implementations ---- */

/*
 * Write one character to the display at the given row and column.
 * Updates the backing cell and attribute buffers unconditionally, then
 * immediately renders the character to the physical screen using the
 * current display style.  Non-printable characters are rendered as a space
 * so the backing buffer and the physical screen remain consistent.
 */
static void cs_put_char(lpst_host *host, uint8_t ch, uint8_t row, uint8_t col)
{
    console_state *cs = get_cs(host);
    if (row < LPST_SCREEN_HEIGHT && col < console_visible_width(cs)) {
        cs->cells[row][col] = ch;
        cs->attrs[row][col] = cs->current_style_bits;

#if LPST_PLATFORM_ATARI
        atari_apply_style(cs, cs->current_style_bits);
        atari_move_cursor(row, col);
        if (ch >= 0x20 && ch < 0x7F) {
            atari_put_display_char(ch);
        } else {
            atari_put_display_char(' ');
        }
#else
        ansi_apply_style(cs, cs->current_style_bits);
        ansi_move_cursor(row, col);
        if (ch >= 0x20 && ch < 0x7F)
            putchar(ch);
        else
            putchar(' ');
#endif
    }
}

static void cs_set_style(lpst_host *host, uint16_t style_bits)
{
    console_state *cs = get_cs(host);
    cs->current_style_bits = style_bits;

#if LPST_PLATFORM_ATARI
    atari_apply_style(cs, style_bits);
#else
    ansi_apply_style(cs, style_bits);
#endif
}

static void cs_set_cursor(lpst_host *host, uint8_t row, uint8_t col)
{
    console_state *cs = get_cs(host);
    uint8_t width = console_visible_width(cs);

    if (width > 0 && col >= width) {
        col = (uint8_t)(width - 1);
    }

#if LPST_PLATFORM_ATARI
    atari_move_cursor(row, col);
#else
    ansi_move_cursor(row, col);
#endif
}

static void cs_erase_to_eol(lpst_host *host, uint8_t row, uint8_t col)
{
    console_state *cs = get_cs(host);
    uint8_t c;
    uint8_t width = console_visible_width(cs);
    if (row < LPST_SCREEN_HEIGHT) {
        for (c = col; c < width; c++) {
            cs->cells[row][c] = ' ';
            cs->attrs[row][c] = cs->current_style_bits;
        }

#if LPST_PLATFORM_ATARI
        atari_move_cursor(row, col);
    Cconws("\033K");
#else
        ansi_move_cursor(row, col);
        printf("\033[K");
#endif
    }
}

static void cs_clear_rows(lpst_host *host, uint8_t from_row, uint8_t to_row)
{
    console_state *cs = get_cs(host);
    uint8_t r;

    for (r = from_row; r <= to_row && r < LPST_SCREEN_HEIGHT; r++) {
        memset(cs->cells[r], ' ', LPST_SCREEN_WIDTH);
        for (uint8_t c = 0; c < LPST_SCREEN_WIDTH; c++) {
            cs->attrs[r][c] = cs->current_style_bits;
        }

#if !LPST_PLATFORM_ATARI
        ansi_move_cursor(r, 0);
        printf("\033[2K");
#endif
    }

#if LPST_PLATFORM_ATARI
    atari_redraw_rows(cs, from_row, to_row);
#endif
}

/*
 * Scroll the display region [from_row, to_row] up by the given number of lines.
 * Updates the backing cell and attribute buffers first by copying rows toward
 * the top and clearing the vacated rows at the bottom.  Then redraws the
 * affected region on the physical screen, using hardware VT52 delete-line
 * sequences on Atari when the region touches the bottom of the screen.
 */
static void cs_scroll_up(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    console_state *cs = get_cs(host);
    uint8_t visible_lines;
    uint8_t r;
    if (lines == 0 || from_row > to_row) return;

    visible_lines = (uint8_t)((lines <= (uint8_t)(to_row - from_row + 1))
        ? lines
        : (uint8_t)(to_row - from_row + 1));

    /* Update backing buffer */
    for (r = from_row; r <= to_row; r++) {
        if (r + visible_lines <= to_row) {
            memcpy(cs->cells[r], cs->cells[r + visible_lines], LPST_SCREEN_WIDTH);
            memcpy(cs->attrs[r], cs->attrs[r + visible_lines], sizeof(cs->attrs[r]));
        } else {
            memset(cs->cells[r], ' ', LPST_SCREEN_WIDTH);
            for (uint8_t c = 0; c < LPST_SCREEN_WIDTH; c++) {
                cs->attrs[r][c] = cs->current_style_bits;
            }
        }
    }

#if LPST_PLATFORM_ATARI
    if (!atari_scroll_up_native(cs, from_row, to_row, visible_lines)) {
        atari_redraw_rows(cs, from_row, to_row);
    }
#else
    ansi_redraw_rows(cs, from_row, to_row);
#endif
}

/*
 * Scroll the display region [from_row, to_row] down by the given number of lines.
 * Updates the backing cell and attribute buffers by copying rows toward the
 * bottom and clearing the vacated rows at the top.  Then redraws the affected
 * region on the physical screen, using hardware VT52 insert-line sequences on
 * Atari when the region touches the bottom of the screen.
 */
static void cs_scroll_down(lpst_host *host, uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    console_state *cs = get_cs(host);
    uint8_t visible_lines;
    int r;
    if (lines == 0 || from_row > to_row) return;

    visible_lines = (uint8_t)((lines <= (uint8_t)(to_row - from_row + 1))
        ? lines
        : (uint8_t)(to_row - from_row + 1));

    /* Update backing buffer */
    for (r = to_row; r >= (int)from_row; r--) {
        if (r - visible_lines >= (int)from_row) {
            memcpy(cs->cells[r], cs->cells[r - visible_lines], LPST_SCREEN_WIDTH);
            memcpy(cs->attrs[r], cs->attrs[r - visible_lines], sizeof(cs->attrs[r]));
        } else {
            memset(cs->cells[r], ' ', LPST_SCREEN_WIDTH);
            for (uint8_t c = 0; c < LPST_SCREEN_WIDTH; c++) {
                cs->attrs[r][c] = cs->current_style_bits;
            }
        }
    }

#if LPST_PLATFORM_ATARI
    if (!atari_scroll_down_native(cs, from_row, to_row, visible_lines)) {
        atari_redraw_rows(cs, from_row, to_row);
    }
#else
    ansi_redraw_rows(cs, from_row, to_row);
#endif
}

static void cs_flush(lpst_host *host)
{
    (void)host;
#if !LPST_PLATFORM_ATARI
    fflush(stdout);
#endif
}

/*
 * Poll for the next key event without blocking.
 * Checks three sources in priority order:
 *   1. Pre-supplied input buffer (for scripted or test operation).
 *   2. The internal key queue (keys decoded by a previous try_read_key call).
 *   3. The terminal, via a non-blocking try_read_key() call.
 * Returns FALSE_SENTINEL (0x8001) if no key is currently available.
 */
static uint16_t cs_poll_key(lpst_host *host)
{
    console_state *cs = get_cs(host);

    fflush(stdout);

    /* Drain pre-supplied input first */
    if (cs->input_keys != NULL && cs->input_pos < cs->input_len) {
        return (uint16_t)cs->input_keys[cs->input_pos++];
    }

    /* Return queued key if available */
    if (cs->key_queue_count > 0)
        return dequeue_key(cs);

    /* Try to read from terminal (non-blocking) */
    try_read_key(cs);

    if (cs->key_queue_count > 0)
        return dequeue_key(cs);

    return FALSE_SENTINEL;
}

/* File I/O stubs — same as transcript host (real file I/O is in exec layer) */

static bool cs_file_open(lpst_host *host, const char *name, uint8_t mode, uint16_t *out_channel)
{
    (void)host; (void)name; (void)mode; (void)out_channel;
    return false;
}

static bool cs_file_close(lpst_host *host, uint16_t channel)
{
    (void)host; (void)channel;
    return false;
}

static int cs_file_read(lpst_host *host, uint16_t channel, uint8_t *buf, size_t count)
{
    (void)host; (void)channel; (void)buf; (void)count;
    return -1;
}

static int cs_file_write(lpst_host *host, uint16_t channel, const uint8_t *buf, size_t count)
{
    (void)host; (void)channel; (void)buf; (void)count;
    return -1;
}

static void cs_get_date(lpst_host *host, uint16_t *month, uint16_t *day, uint16_t *year)
{
    time_t t;
    struct tm *tm;
    (void)host;
    t = time(NULL);
    tm = localtime(&t);
    if (tm != NULL) {
        *month = (uint16_t)(tm->tm_mon + 1);
        *day = (uint16_t)tm->tm_mday;
        *year = (uint16_t)(tm->tm_year + 1900);
    } else {
        *month = 1;
        *day = 1;
        *year = 1987;
    }
}

static void cs_get_time(lpst_host *host, uint16_t *hour, uint16_t *minute, uint16_t *second)
{
    time_t t;
    struct tm *tm;
    (void)host;
    t = time(NULL);
    tm = localtime(&t);
    if (tm != NULL) {
        *hour = (uint16_t)tm->tm_hour;
        *minute = (uint16_t)tm->tm_min;
        *second = (uint16_t)tm->tm_sec;
    } else {
        *hour = 0;
        *minute = 0;
        *second = 0;
    }
}

/* ---- Public API ---- */

/**
 * Initialize the console host and register its vtable in @p host.
 * Zeroes the global console_state, fills cells with spaces, and populates
 * all function pointers in @p host.  Then performs platform-specific setup:
 *   - Atari: saves CONTERM, enables shift-status reporting, clears the screen.
 *   - Amiga: detects the actual console width via a window-bounds query.
 *   - POSIX: enters terminal raw mode and clears the screen with ANSI codes.
 * Must be paired with lpst_host_console_cleanup() on exit.
 */
void lpst_host_console_init(lpst_host *host)
{
    memset(&g_console_state, 0, sizeof(g_console_state));
    memset(g_console_state.cells, ' ', sizeof(g_console_state.cells));
    g_console_state.visible_width = LPST_DEFAULT_SCREEN_WIDTH;

    host->user_data = &g_console_state;
    host->screen_width = LPST_DEFAULT_SCREEN_WIDTH;
    host->screen_height = LPST_SCREEN_HEIGHT;
    host->put_char = cs_put_char;
    host->set_style = cs_set_style;
    host->set_cursor = cs_set_cursor;
    host->erase_to_eol = cs_erase_to_eol;
    host->clear_rows = cs_clear_rows;
    host->scroll_up = cs_scroll_up;
    host->scroll_down = cs_scroll_down;
    host->flush = cs_flush;
    host->poll_key = cs_poll_key;
    host->file_open = cs_file_open;
    host->file_close = cs_file_close;
    host->file_read = cs_file_read;
    host->file_write = cs_file_write;
    host->get_date = cs_get_date;
    host->get_time = cs_get_time;

#if LPST_PLATFORM_AMIGA
    g_console_state.visible_width = amiga_detect_console_width();
    host->screen_width = g_console_state.visible_width;
#endif

#if LPST_PLATFORM_ATARI
    g_console_state.atari_conterm_original = atari_get_conterm();
    g_console_state.atari_conterm_saved = true;
    atari_set_conterm((uint8_t)(g_console_state.atari_conterm_original | LPST_ATARI_CONTERM_SHIFT_STATUS));

    atari_clear_screen();
    atari_set_wrap(false);
    atari_apply_style(&g_console_state, 0);
    atari_set_cursor_visible(true);
#else
    enter_raw_mode(&g_console_state);
    ansi_clear_screen();
    ansi_set_wrap(false);
    ansi_apply_style(&g_console_state, 0);
    fflush(stdout);
#endif
}

/**
 * Attach a pre-supplied input byte array to the console host.
 * Subsequent cs_poll_key() calls will drain from this array before
 * attempting to read the physical terminal.  Intended for automated
 * testing and replay.  The caller retains ownership of @p keys and must
 * ensure it remains valid until fully consumed or replaced by another call.
 */
void lpst_host_console_set_input(lpst_host *host, const uint8_t *keys, size_t count)
{
    console_state *cs = get_cs(host);
    cs->input_keys = keys;
    cs->input_len = count;
    cs->input_pos = 0;
}

/**
 * Override the visible screen width reported by the console host.
 * Clamps the value to [1, LPST_SCREEN_WIDTH].  Has no effect if @p width
 * is zero or exceeds the maximum.  Does not redraw the screen; the new
 * width takes effect on the next display operation.
 */
void lpst_host_console_set_screen_width(lpst_host *host, uint8_t width)
{
    console_state *cs = get_cs(host);
    if (width > 0 && width <= LPST_SCREEN_WIDTH) {
        cs->visible_width = width;
    }
}

/**
 * Restore the terminal to its normal state and release console resources.
 * Resets text attributes and re-enables line wrap, then undoes the
 * platform-specific setup performed by lpst_host_console_init():
 *   - Atari: restores the saved CONTERM value.
 *   - Amiga/POSIX: disables raw mode (SetMode / tcsetattr).
 * Positions the cursor below the display area so that the shell prompt
 * does not overwrite the last line of Cornerstone output.
 */
void lpst_host_console_cleanup(lpst_host *host)
{
    console_state *cs = get_cs(host);

#if LPST_PLATFORM_ATARI
    if (cs->atari_conterm_saved) {
        atari_set_conterm(cs->atari_conterm_original);
        cs->atari_conterm_saved = false;
    }

    atari_apply_style(cs, 0);
    atari_set_wrap(true);
    atari_set_cursor_visible(true);
    atari_move_cursor(LPST_SCREEN_HEIGHT - 1, 0);
    Cconout('\r');
    Cconout('\n');
#else
    ansi_apply_style(cs, 0);
    ansi_set_wrap(true);
    leave_raw_mode(cs);
    /* Move cursor below the screen area so the shell prompt doesn't overwrite */
    printf("\033[%d;1H\n", LPST_SCREEN_HEIGHT);
    fflush(stdout);
#endif

    (void)cs;
}
