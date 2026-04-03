/* lp128_host_c128.c – Cornerstone VM host implementation for the real C128.
 *
 * Uses the VDC (8563) chip in 80-column mode for all display output.
 * Characters are written to VDC screen RAM via register I/O at $D600/$D601.
 * The KERNAL is used only during init (videomode switch + PET_CLR to set up
 * default attributes).  All put_char, erase, scroll, and cursor callbacks
 * operate through direct VDC register access.
 *
 * Why not CHROUT?  The C128 KERNAL screen editor modifies zero-page
 * locations that overlap with llvm-mos compiler registers ($02-$35+).
 * A single CHROUT call can corrupt loop counters and pointers, causing
 * screen garbage.  Direct VDC register access avoids the KERNAL entirely.
 *
 * ASCII → screen-code mapping in upper/lowercase mode:
 *   0x20-0x3F → same       (space, digits, punctuation)
 *   0x40      → 0x00       (@)
 *   0x41-0x5A → same       (uppercase A-Z)
 *   0x5B-0x5F → 0x1B-0x1F  ([\]^_)
 *   0x61-0x7A → 0x01-0x1A  (lowercase a-z)
 */

#include "lp128_host.h"
#include "lp128_vm.h"   /* for LP128_FALSE_SENTINEL, LP128_SCREEN_WIDTH/HEIGHT */

#if defined(__mos__) || defined(__llvm_mos__)

#include <stdint.h>
#include <string.h>
#include <cbm.h>
#include <c128.h>
#include "lp128_mmu.h"

/* ── PETSCII control codes (used only during init) ──────────────────────── */
#define PET_LOWER_UPPER 0x0Eu   /* switch to upper/lowercase charset */
#define PET_UPPER_GRAPH 0x8Eu   /* switch to uppercase/graphics charset */
#define PET_CLR         0x93u   /* clear screen + HOME (CHR$(147)) */

/* ── VDC 80-column screen geometry ──────────────────────────────────────── */
#define C128_HOST_VIDEO_MODE   VIDEOMODE_80x25
#define C128_HOST_SCREEN_COLS  80u
#define C128_HOST_SCREEN_ROWS  25u

/* ── VDC (8563) register access ─────────────────────────────────────────── *
 * The VDC has its own 16 KB RAM, accessed indirectly through two I/O
 * registers at $D600 (address/status) and $D601 (data).  These are only
 * visible when the MMU maps I/O at $D000-$DFFF (config $3E).  Our normal
 * all-RAM mode ($3F) hides I/O, so every VDC access requires a brief
 * MMU switch.
 */
#define VDC_STATUS_REG   (*(volatile uint8_t *)0xD600u)
#define VDC_DATA_REG     (*(volatile uint8_t *)0xD601u)

/* VDC internal register indices */
#define VDC_R_CURSOR_MODE     10   /* cursor start scan line + mode bits */
#define VDC_R_CURSOR_HI       14
#define VDC_R_CURSOR_LO       15
#define VDC_R_UPDATE_HI       18
#define VDC_R_UPDATE_LO       19
#define VDC_R_ATTR_HI         20
#define VDC_R_ATTR_LO         21
#define VDC_R_DATA            31   /* read/write at update addr */

#define VDC_ATTR_REVERSE      0x40u

/* ── MMU I/O mode helpers ───────────────────────────────────────────────── */
/* Save the current MMU configuration register ($FF00) and switch to
 * configuration $3E (RAM + I/O visible at $D000-$DFFF), allowing access
 * to VDC and CIA registers.  Returns the saved config for use with
 * leave_io(). */
static uint8_t enter_io(void)
{
    uint8_t prev = *(volatile uint8_t *)0xFF00u;
    *(volatile uint8_t *)0xFF00u = 0x3Eu;   /* RAM + I/O visible */
    return prev;
}

/* Restore the MMU configuration register to the value saved by enter_io(). */
static void leave_io(uint8_t prev)
{
    *(volatile uint8_t *)0xFF00u = prev;
}

/* ── VDC register helpers (call only while in I/O mode) ─────────────────── */
/* Spin until the VDC ready flag (bit 7 of the status register) is set.
 * The brief delay writes a diagnostic screen code so a crash in this
 * loop is visible on screen. */
static void vdc_wait(void)
{
    /* Diagnostic: mark position 15 with 'W' while waiting for VDC ready.
     * $0400 is plain RAM in all MMU modes — safe to write even in $3E. */
    *(volatile uint8_t *)(0x0400u + 15u) = 0x17u;  /* screen code 'W' */
    while (!(VDC_STATUS_REG & 0x80u)) {}
    *(volatile uint8_t *)(0x0400u + 15u) = 0x20u;  /* space = cleared */
}

/* Write val to internal VDC register reg.  Selects the register via the
 * address port ($D600), waits for VDC ready, then writes via the data
 * port ($D601). */
static void vdc_write(uint8_t reg, uint8_t val)
{
    VDC_STATUS_REG = reg;
    vdc_wait();
    VDC_DATA_REG = val;
}

/* Read from internal VDC register reg. */
static uint8_t vdc_read(uint8_t reg)
{
    VDC_STATUS_REG = reg;
    vdc_wait();
    return VDC_DATA_REG;
}

/* Load the VDC update-address registers (R18:R19) so subsequent R31
 * reads/writes target the given VDC RAM address. */
static void vdc_set_addr(uint16_t addr)
{
    vdc_write(VDC_R_UPDATE_HI, (uint8_t)(addr >> 8));
    vdc_write(VDC_R_UPDATE_LO, (uint8_t)(addr & 0xFFu));
}

/* ── IRQ setup ──────────────────────────────────────────────────────────── *
 * Outside KERNAL IEC calls we keep the CPU in SEI state and point the RAM
 * IRQ/NMI vectors at a single RTI, so stray interrupts during all-RAM mode
 * cannot jump into garbage.
 *
 * Important: do NOT disable the CIA interrupt sources themselves here.
 * Runtime file I/O wrappers temporarily switch to KERNAL ROM and execute
 * with CLI; KERNAL disk/IEC code expects normal CIA/timer IRQ activity for
 * serial timing and timeout handling.  Leaving the sources enabled allows
 * those IRQs to be serviced by the ROM handler while inside the wrapper.
 */

void lp128_install_irq_handler(void)
{
    /* Disable interrupts so nothing fires while we set up. */
    __asm__ volatile("sei" ::: "memory");

    /* Place a single RTI at $03A0 as a safety net. */
    *(volatile uint8_t *)0x03A0u = 0x40u;   /* RTI */

    uint16_t rti_addr = 0x03A0u;

    /* Point IRQ vector at $FFFE/$FFFF (RAM) to the RTI. */
    *(volatile uint8_t *)0xFFFEu = (uint8_t)(rti_addr & 0xFFu);
    *(volatile uint8_t *)0xFFFFu = (uint8_t)((rti_addr >> 8) & 0xFFu);

    /* Point NMI vector at $FFFA/$FFFB to the same RTI. */
    *(volatile uint8_t *)0xFFFAu = (uint8_t)(rti_addr & 0xFFu);
    *(volatile uint8_t *)0xFFFBu = (uint8_t)((rti_addr >> 8) & 0xFFu);

    /* ── Minimal IEC IRQ handler at $03A1 ───────────────────────────────
     *
     * When IEC-active wrappers enable interrupts (CLI) in KERNAL MMU
     * mode, the hardware vector at $FFFE/$FFFF points to KERNAL ROM,
     * which pushes A/X/Y, checks BRK, then JMP ($0314) through CINV.
     *
     * The default CINV target is the KERNAL screen editor handler,
     * which services the CIA and jiffy clock (good) but ALSO touches
     * VDC registers for the 80-column cursor blink (bad — it corrupts
     * our custom VDC display).
     *
     * This minimal handler replaces the screen editor target:
     *   • Clears CIA1 interrupt flags (read $DC0D)
     *   • Increments the jiffy clock at $A0-$A2
     *   • Pulls Y/X/A (matching the KERNAL push convention)
     *   • RTI
     *
     * The handler is only reached when in KERNAL MMU mode with IRQs
     * enabled, which only happens inside IEC-active KERNAL wrappers.
     * In normal operation (SEI, full-RAM mode), the RAM $FFFE vector
     * points to the bare RTI at $03A0 and this handler is never
     * reached.
     *
     * Layout at $03A1 (19 bytes):
     *   $03A1: LDA $DC0D       ; AD 0D DC  clear CIA1 ICR
     *   $03A4: INC $A2         ; E6 A2     jiffy clock low
     *   $03A6: BNE +6          ; D0 06     -> $03AE
     *   $03A8: INC $A1         ; E6 A1     jiffy clock mid
     *   $03AA: BNE +2          ; D0 02     -> $03AE
     *   $03AC: INC $A0         ; E6 A0     jiffy clock high
     *   $03AE: PLA             ; 68        restore Y
     *   $03AF: TAY             ; A8
     *   $03B0: PLA             ; 68        restore X
     *   $03B1: TAX             ; AA
     *   $03B2: PLA             ; 68        restore A
     *   $03B3: RTI             ; 40
     */
    static const uint8_t iec_handler[] = {
        0xAD, 0x0D, 0xDC,      /* LDA $DC0D  */
        0xE6, 0xA2,             /* INC $A2    */
        0xD0, 0x06,             /* BNE +6     */
        0xE6, 0xA1,             /* INC $A1    */
        0xD0, 0x02,             /* BNE +2     */
        0xE6, 0xA0,             /* INC $A0    */
        0x68,                   /* PLA        */
        0xA8,                   /* TAY        */
        0x68,                   /* PLA        */
        0xAA,                   /* TAX        */
        0x68,                   /* PLA        */
        0x40                    /* RTI        */
    };
    volatile uint8_t *dst = (volatile uint8_t *)0x03A1u;
    for (uint8_t i = 0; i < sizeof(iec_handler); i++) {
        dst[i] = iec_handler[i];
    }

    /* Point CINV ($0314/$0315) at our minimal handler so that when
     * IEC-active wrappers do CLI in KERNAL mode, the KERNAL ROM IRQ
     * entry dispatches to us instead of the screen editor. */
    uint16_t cinv_addr = 0x03A1u;
    *(volatile uint8_t *)0x0314u = (uint8_t)(cinv_addr & 0xFFu);
    *(volatile uint8_t *)0x0315u = (uint8_t)((cinv_addr >> 8) & 0xFFu);

    /* Leave interrupts disabled — we never need them. */
}

/* ── ASCII → screen code ────────────────────────────────────────────────── */
/* Convert an ASCII character to the VDC uppercase/lowercase screen code.
 * See the mapping table at the top of this file for the translation rules. */
static uint8_t ascii_to_screencode(uint8_t c)
{
    if (c >= 0x61u && c <= 0x7Au) return (uint8_t)(c - 0x60u);
    if (c >= 0x41u && c <= 0x5Au) return c;
    if (c >= 0x40u && c <= 0x5Fu) return (uint8_t)(c - 0x40u);
    return c;   /* 0x20-0x3F (space, digits, punctuation) pass through */
}

/* Return the linear VDC RAM offset (character position index) for the
 * given screen row and column. */
static uint16_t screen_offset(uint8_t row, uint8_t col)
{
    return (uint16_t)((uint16_t)row * C128_HOST_SCREEN_COLS + col);
}

/* ── Shadow buffer ──────────────────────────────────────────────────────── *
 * We maintain a CPU-RAM mirror of VDC screen contents.  Scrolling reads
 * from this shadow (which is always accessible in the default all-RAM
 * MMU mode $3F) instead of trying to read VDC internal RAM through R31,
 * which has tricky timing requirements that are difficult to satisfy
 * reliably from compiled C code.
 *
 * IMPORTANT: BSS lives at $D0FD+ which overlaps the I/O mirror at
 * $D000-$DFFF visible in MMU $3E mode.  The shadow buffer MUST only
 * be accessed while in $3F mode (all RAM).  When writing to the VDC
 * (in $3E mode), data is first copied to a LOCAL stack buffer.
 */
static uint8_t g_shadow[C128_HOST_SCREEN_COLS * C128_HOST_SCREEN_ROWS];
static uint16_t g_attr_base;
static uint8_t g_default_attr;

/* Return the VDC attribute byte to use for the current text attribute.
 * Applies the REVERSE bit from host->current_text_attribute (bit 0)
 * on top of the default attribute captured at init time. */
static uint8_t active_vdc_attr(const lp128_host *host)
{
    uint8_t attr = (uint8_t)(g_default_attr & (uint8_t)~VDC_ATTR_REVERSE);

    if (host != NULL && (host->current_text_attribute & 0x0001u) != 0) {
        attr = (uint8_t)(attr | VDC_ATTR_REVERSE);
    }

    return attr;
}

/* Write one byte to the VDC character-screen RAM at addr.
 * Must be called while in I/O mode ($3E). */
static void vdc_write_byte(uint16_t addr, uint8_t val)
{
    vdc_set_addr(addr);
    vdc_write(VDC_R_DATA, val);
}

/* Write one row from the shadow buffer to VDC screen RAM.
 * Copies through a local stack buffer to bridge $3F → $3E mode. */
static void vdc_write_shadow_row(uint8_t row)
{
    uint8_t local_buf[C128_HOST_SCREEN_COLS];
    uint16_t off = (uint16_t)row * C128_HOST_SCREEN_COLS;

    /* Copy from shadow (BSS, accessible in $3F) to a local buffer (stack). */
    for (uint8_t i = 0; i < C128_HOST_SCREEN_COLS; i++) {
        local_buf[i] = g_shadow[off + i];
    }

    /* Write local buffer to VDC (in $3E mode). */
    uint8_t prev = enter_io();
    vdc_set_addr(off);
    for (uint8_t i = 0; i < C128_HOST_SCREEN_COLS; i++) {
        vdc_write(VDC_R_DATA, local_buf[i]);
    }
    leave_io(prev);
}

/* ── Host callback implementations ─────────────────────────────────────── */

/* Forward declaration — defined below with the keyboard code. */
static void kb_scan(void);
/* Draw character ch at (row, col) on the VDC 80-column screen.
 * Updates the shadow buffer and VDC character + attribute RAM.  The
 * keyboard matrix is scanned opportunistically to keep the ring buffer fed. */
static void c128_put_char(lp128_host *host, uint8_t ch, uint8_t row, uint8_t col)
{
    kb_scan();                              /* catch keys during line redraws */
    uint16_t addr = screen_offset(row, col);
    uint8_t sc = ascii_to_screencode(ch);
    uint8_t attr = active_vdc_attr(host);
    g_shadow[addr] = sc;                    /* update shadow ($3F mode) */
    uint8_t prev = enter_io();
    vdc_set_addr(addr);
    vdc_write(VDC_R_DATA, sc);
    vdc_set_addr((uint16_t)(g_attr_base + addr));
    vdc_write(VDC_R_DATA, attr);
    leave_io(prev);
}

/* Move the VDC hardware cursor to (row, col). */
static void c128_set_cursor(lp128_host *host, uint8_t row, uint8_t col)
{
    (void)host;
    uint16_t addr = screen_offset(row, col);
    uint8_t prev = enter_io();
    vdc_write(VDC_R_CURSOR_HI, (uint8_t)(addr >> 8));
    vdc_write(VDC_R_CURSOR_LO, (uint8_t)(addr & 0xFFu));
    leave_io(prev);
}

/* Clear from (row, col) to the end of the row in both the shadow buffer
 * and VDC screen + attribute RAM. */
static void c128_erase_to_eol(lp128_host *host, uint8_t row, uint8_t col)
{
    kb_scan();
    uint16_t base = screen_offset(row, col);
    uint8_t count = (uint8_t)(C128_HOST_SCREEN_COLS - col);
    uint8_t attr = active_vdc_attr(host);
    for (uint8_t i = 0; i < count; i++) {   /* update shadow ($3F mode) */
        g_shadow[base + i] = 0x20u;
    }
    uint8_t prev = enter_io();
    vdc_set_addr(base);                     /* VDC auto-increments */
    for (uint8_t i = 0; i < count; i++) {
        vdc_write(VDC_R_DATA, 0x20u);
    }
    vdc_set_addr((uint16_t)(g_attr_base + base));
    for (uint8_t i = 0; i < count; i++) {
        vdc_write(VDC_R_DATA, attr);
    }
    leave_io(prev);
}

/* Fill rows from_row through to_row (inclusive) with spaces in the
 * shadow buffer and VDC screen + attribute RAM. */
static void c128_clear_rows(lp128_host *host, uint8_t from_row, uint8_t to_row)
{
    uint8_t attr = active_vdc_attr(host);
    for (uint8_t r = from_row; r <= to_row; r++) {  /* update shadow ($3F) */
        uint16_t base = screen_offset(r, 0u);
        for (uint8_t c = 0; c < C128_HOST_SCREEN_COLS; c++) {
            g_shadow[base + c] = 0x20u;
        }
    }
    uint8_t prev = enter_io();
    /* VDC auto-increments, so set each target address once and stream. */
    for (uint8_t r = from_row; r <= to_row; r++) {
        uint16_t base = screen_offset(r, 0u);
        vdc_set_addr(base);
        for (uint8_t c = 0; c < C128_HOST_SCREEN_COLS; c++) {
            vdc_write(VDC_R_DATA, 0x20u);
        }
        vdc_set_addr((uint16_t)(g_attr_base + base));
        for (uint8_t c = 0; c < C128_HOST_SCREEN_COLS; c++) {
            vdc_write(VDC_R_DATA, attr);
        }
    }
    leave_io(prev);
}

/* Scroll rows from_row..to_row up by lines rows, filling the vacated
 * bottom rows with spaces.  Both the shadow buffer and VDC RAM are updated;
 * the shadow-to-VDC writeback copies one row at a time through a local
 * stack buffer to cross the $3F → $3E MMU boundary safely. */
static void c128_scroll_up(lp128_host *host,
                            uint8_t from_row, uint8_t to_row, uint8_t lines)
{
    if (lines == 0u || from_row > to_row) return;
    uint8_t visible = to_row - from_row + 1u;
    if (lines >= visible) {
        c128_clear_rows(host, from_row, to_row);
        return;
    }

    /* Step 1: Rearrange the shadow buffer (in default $3F all-RAM mode,
     * BSS at $D0FD+ is accessible as normal RAM). */
    uint16_t dst_off = (uint16_t)from_row * C128_HOST_SCREEN_COLS;
    uint16_t src_off = (uint16_t)(from_row + lines) * C128_HOST_SCREEN_COLS;
    uint16_t move_count = (uint16_t)(visible - lines) * C128_HOST_SCREEN_COLS;
    for (uint16_t i = 0; i < move_count; i++) {
        g_shadow[dst_off + i] = g_shadow[src_off + i];
    }

    /* Clear vacated bottom rows in shadow. */
    uint16_t clear_off = (uint16_t)(to_row - lines + 1u) * C128_HOST_SCREEN_COLS;
    uint16_t clear_count = (uint16_t)lines * C128_HOST_SCREEN_COLS;
    for (uint16_t i = 0; i < clear_count; i++) {
        g_shadow[clear_off + i] = 0x20u;
    }

    /* Step 2: Write all affected rows from shadow to VDC.
     * Each row is copied through a local stack buffer to cross the
     * $3F → $3E boundary safely.  Scan keyboard between rows. */
    for (uint8_t r = from_row; r <= to_row; r++) {
        kb_scan();
        vdc_write_shadow_row(r);
    }
}

/* No-op flush.  VDC register writes take effect immediately; there is
 * no output buffer to flush. */
static void c128_flush(lp128_host *host)
{
    (void)host;
    /* VDC register writes are immediate; nothing to flush. */
}

/* ── CIA-1 keyboard matrix lookup table (unshifted) ─────────────────────── *
 * C64/C128 keyboard matrix: 8 rows selected by grounding one bit of CIA-1
 * Port A ($DC00), 8 columns read from Port B ($DC01).  Active-low.
 *
 * Table: key_matrix[pa_row][pb_col] → ASCII value, 0 = modifier/unmapped.
 */
static const uint8_t key_matrix[8][8] = {
    /*           PB0    PB1    PB2    PB3    PB4    PB5    PB6    PB7  */
    /* PA0 */ {  0x08,  0x0D,  0,     0,     0,     0,     0,     0   },
    /* PA1 */ {  '3',   'w',   'a',   '4',   'z',   's',   'e',   0   },
    /* PA2 */ {  '5',   'r',   'd',   '6',   'c',   'f',   't',   'x' },
    /* PA3 */ {  '7',   'y',   'g',   '8',   'b',   'h',   'u',   'v' },
    /* PA4 */ {  '9',   'i',   'j',   '0',   'm',   'k',   'o',   'n' },
    /* PA5 */ {  '+',   'p',   'l',   '-',   '.',   ':',   '@',   ',' },
    /* PA6 */ {  0,     '*',   ';',   0,     0,     '=',   0,     '/' },
    /* PA7 */ {  '1',   0,     0,     '2',   ' ',   0,     'q',   0   },
};

/* Previous scan state for edge detection (0xFF = all keys released). */
static uint8_t prev_scan[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ── Keyboard ring buffer ───────────────────────────────────────────────── *
 * The VM may execute many instructions between KBINPUT calls, so a key
 * that is pressed and released between two polls would be lost by pure
 * edge detection.  We buffer up to 8 characters so fast typists don't
 * drop keystrokes.
 */
#define KEYBUF_SIZE 8u
static uint8_t keybuf[KEYBUF_SIZE];
static uint8_t keybuf_head = 0;
static uint8_t keybuf_tail = 0;
static uint8_t keybuf_count = 0;

/* Push one ASCII character into the keyboard ring buffer.  If the
 * buffer is full, the new character is silently dropped. */
static void keybuf_put(uint8_t ch)
{
    if (keybuf_count >= KEYBUF_SIZE) return;  /* full — drop oldest? no, drop new */
    keybuf[keybuf_tail] = ch;
    keybuf_tail = (uint8_t)((keybuf_tail + 1u) % KEYBUF_SIZE);
    keybuf_count++;
}

/* Remove and return the next ASCII character from the ring buffer.
 * Returns LP128_FALSE_SENTINEL if the buffer is empty. */
static uint16_t keybuf_get(void)
{
    if (keybuf_count == 0) return LP128_FALSE_SENTINEL;
    uint8_t ch = keybuf[keybuf_head];
    keybuf_head = (uint8_t)((keybuf_head + 1u) % KEYBUF_SIZE);
    keybuf_count--;
    return (uint16_t)ch;
}

/* Scan the CIA-1 keyboard matrix and push any newly pressed keys into the
 * ring buffer.  Called from poll_key; may also be called from tight loops
 * to keep the buffer fed. */
static void kb_scan(void)
{
    uint8_t row_data[8];

    /* ── Switch to I/O mode ($3E) for CIA register access ──────────── */
    uint8_t saved_mmu = *(volatile uint8_t *)0xFF00u;
    *(volatile uint8_t *)0xFF00u = 0x3Eu;

    /* Save CIA-1 data direction registers; set for keyboard scanning. */
    uint8_t saved_ddra = *(volatile uint8_t *)0xDC02u;
    uint8_t saved_ddrb = *(volatile uint8_t *)0xDC03u;
    *(volatile uint8_t *)0xDC02u = 0xFFu;  /* Port A = all outputs */
    *(volatile uint8_t *)0xDC03u = 0x00u;  /* Port B = all inputs  */

    /* Scan all 8 rows. */
    for (uint8_t r = 0; r < 8; r++) {
        *(volatile uint8_t *)0xDC00u = (uint8_t)~(1u << r);
        /* Brief settling delay for CIA output → input propagation. */
        __asm__ volatile("nop\nnop\nnop\nnop" :::);
        row_data[r] = *(volatile uint8_t *)0xDC01u;
    }

    /* Deselect all rows. */
    *(volatile uint8_t *)0xDC00u = 0xFFu;

    /* Restore CIA data direction registers. */
    *(volatile uint8_t *)0xDC02u = saved_ddra;
    *(volatile uint8_t *)0xDC03u = saved_ddrb;

    /* ── Back to previous MMU mode ─────────────────────────────────── */
    *(volatile uint8_t *)0xFF00u = saved_mmu;

    /* ── Detect shift keys ─────────────────────────────────────────── */
    uint8_t shifted = 0;
    if (!(row_data[1] & 0x80u)) shifted = 1;  /* LSHIFT: PA1, PB7 */
    if (!(row_data[6] & 0x10u)) shifted = 1;  /* RSHIFT: PA6, PB4 */

    /* ── Edge detection: buffer ALL newly pressed keys ──────────────── */
    for (uint8_t r = 0; r < 8; r++) {
        uint8_t newly_down = (uint8_t)(prev_scan[r] & ~row_data[r]);
        if (!newly_down) continue;

        for (uint8_t c = 0; c < 8; c++) {
            if (!(newly_down & (1u << c))) continue;

            /* Skip modifier keys. */
            if (r == 1 && c == 7) continue;  /* LSHIFT */
            if (r == 6 && c == 4) continue;  /* RSHIFT */
            if (r == 7 && c == 2) continue;  /* CTRL   */
            if (r == 7 && c == 5) continue;  /* C=     */

            uint8_t ch = key_matrix[r][c];
            if (ch == 0) continue;  /* unmapped key */

            if (shifted && ch >= 'a' && ch <= 'z')
                ch = (uint8_t)(ch - 0x20u);

            keybuf_put(ch);
        }
    }

    /* Update previous-scan state. */
    for (uint8_t r = 0; r < 8; r++)
        prev_scan[r] = row_data[r];
}

/* Poll for the next queued keystroke.  Scans the matrix first to
 * ensure any recently pressed keys have been captured. */
static uint16_t c128_poll_key(lp128_host *host)
{
    (void)host;
    kb_scan();
    return keybuf_get();
}

/* ── Public API ─────────────────────────────────────────────────────────── */
/* Initialise the C128 VDC host.  Switches to 80-column mode via the KERNAL,
 * issues a single PET_CLR to set up default VDC attribute RAM, then installs
 * direct VDC callbacks so all subsequent output bypasses the KERNAL screen
 * editor entirely.  Also initialises the shadow buffer and the VDC cursor. */
void lp128_host_posix_init(lp128_host *host)
{
    lp128_k_videomode(C128_HOST_VIDEO_MODE);
    lp128_k_bsout(PET_LOWER_UPPER);

    /* Use KERNAL PET_CLR once to clear the 80-column screen and set up
     * default VDC attributes.  After this, all output goes through direct
     * VDC register writes. */
    lp128_k_bsout(PET_CLR);

    {
        uint8_t prev = enter_io();
        g_attr_base = (uint16_t)(((uint16_t)vdc_read(VDC_R_ATTR_HI) << 8) |
                                 (uint16_t)vdc_read(VDC_R_ATTR_LO));
        vdc_set_addr(g_attr_base);
        g_default_attr = vdc_read(VDC_R_DATA);
        leave_io(prev);
    }

    /* Initialize shadow buffers to match the cleared screen. */
    for (uint16_t i = 0; i < C128_HOST_SCREEN_COLS * C128_HOST_SCREEN_ROWS; i++) {
        g_shadow[i] = 0x20u;
    }

    /* Enable a blinking block cursor on the VDC.
     * R10 bits 6-5 control cursor mode: 00 = solid, 01 = no cursor,
     * 10 = blink 1/16, 11 = blink 1/32.  Bits 4-0 = start scan line.
     * We use 0x60 = mode 11 (blink 1/32), start at scan line 0. */
    {
        uint8_t prev = enter_io();
        vdc_write(VDC_R_CURSOR_MODE, 0x60u);
        leave_io(prev);
    }

    host->user_data     = NULL;
    host->screen_width  = (uint8_t)C128_HOST_SCREEN_COLS;
    host->screen_height = (uint8_t)C128_HOST_SCREEN_ROWS;
    host->current_text_attribute = 0;
    host->put_char      = c128_put_char;
    host->set_cursor    = c128_set_cursor;
    host->erase_to_eol  = c128_erase_to_eol;
    host->clear_rows    = c128_clear_rows;
    host->scroll_up     = c128_scroll_up;
    host->flush         = c128_flush;
    host->poll_key      = c128_poll_key;
}

/* Restore the display to 40-column uppercase/graphics mode.  Called on
 * program exit so the C128 returns to its normal startup appearance. */
void lp128_host_posix_cleanup(lp128_host *host)
{
    (void)host;
    lp128_k_bsout(PET_UPPER_GRAPH);
    lp128_k_videomode(VIDEOMODE_40x25);
}

#endif /* __mos__ || __llvm_mos__ */
