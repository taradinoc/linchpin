/* lp128_kernal.c – Non-inline KERNAL call wrappers for the C128.
 *
 * On the C128, setting MMU config to 0x0E maps KERNAL/editor ROM over
 * $C000-$FFFF, hiding any program code placed there.  These wrapper
 * functions MUST reside below $C000 so the MMU switch doesn't hide the
 * executing code.
 *
 * To guarantee low placement, this file is listed FIRST in C128_SOURCES
 * so the linker places its .text content at the lowest addresses.
 *
 * All functions are __attribute__((noinline)) to prevent the compiler
 * from inlining them into callers that may reside above $C000.
 */

#if defined(__mos__) || defined(__llvm_mos__)

#include <cbm.h>
#include <c128.h>
#include <stdint.h>

/* ── MMU Configuration Register ─────────────────────────────────────────── */

#define LP128_MMU_CR_ADDR  (*(volatile uint8_t *)0xFF00u)
#define LP128_MMU_CFG_KERNAL 0x0Eu
#define LP128_MMU_CFG_RAM0   0x3Fu

/** Save current MMU config, then remap $C000-$FFFF to KERNAL/editor ROM.
 *  Returns the previous MMU configuration byte so it can be restored. */
static uint8_t enter_kernal(void)
{
    uint8_t previous = LP128_MMU_CR_ADDR;
    LP128_MMU_CR_ADDR = LP128_MMU_CFG_KERNAL;
    return previous;
}

/** Restore the MMU configuration saved by enter_kernal(). */
static void restore_mmu(uint8_t previous)
{
    LP128_MMU_CR_ADDR = previous;
}

/* ── Light IEC helpers (CLI/SEI only, no ZP save) ───────────────────────
 *
 * Our custom CINV handler at $03A1 only touches $DC0D (clear CIA1) and
 * $A0-$A2 (jiffy clock) — it never modifies llvm-mos compiler registers
 * at $0A-$27 or ZP data/BSS at $2A-$58.  For per-byte IEC calls (CHRIN,
 * BASIN, READST) where the KERNAL serial code also stays out of $0A-$58,
 * a simple CLI/SEI bracket is enough.  This avoids the 79-byte ZP
 * save/restore overhead on every byte of a file read.
 */
/** Switch to KERNAL ROM mode and enable IRQs (CLI).  Returns the previous
 *  MMU configuration for restoration.  Use for per-byte IEC reads where
 *  interrupt overhead is acceptable but ZP save/restore is not. */
static uint8_t enter_kernal_cli(void)
{
    uint8_t previous = enter_kernal();
    __asm__ volatile("cli" ::: "memory");
    return previous;
}

/** Disable IRQs (SEI) and restore the MMU configuration saved by
 *  enter_kernal_cli(). */
static void sei_restore_mmu(uint8_t previous)
{
    __asm__ volatile("sei" ::: "memory");
    restore_mmu(previous);
}

/* ── ZP Save/Restore for IEC Calls ────────────────────────────────────── *
 * KERNAL routines that perform IEC serial bus communication (OPEN, CLOSE,
 * CHKIN, CLRCH) require CIA timer interrupts for bus timing and timeouts.
 * The default KERNAL screen editor IRQ handler modifies zero-page
 * locations ($02-$35+) that overlap with llvm-mos compiler virtual
 * registers (__rc0-__rc29 at $0A-$27) and ZP data/BSS ($2A-$58).
 * Our custom CINV handler avoids this, but these "heavy" operations add
 * the full ZP save/restore as a safety net against any edge-case KERNAL
 * code that might touch the same area.
 *
 * To resolve this conflict, IEC-active wrappers:
 *   1. Save ZP $0A-$58 (79 bytes) to VIC-II screen RAM at $0400
 *      (unused since we run in 80-column VDC mode)
 *   2. Load any KERNAL parameters from a fixed scratch byte ($044F)
 *   3. Switch to KERNAL MMU mode ($0E) and enable IRQs (CLI)
 *   4. Call the KERNAL routine
 *   5. Immediately disable IRQs (SEI) and stash the return value
 *   6. Restore full-RAM MMU mode ($3F)
 *   7. Restore ZP $0A-$58 from the save buffer
 *   8. Return the stashed value from a fixed scratch byte ($0450)
 *
 * The entire sequence is in a single inline asm block to prevent the
 * compiler from inserting ZP-dependent instructions in the critical window.
 *
 * CHRIN/BASIN and READST remain no-IRQ: the serial bus bitbanging for
 * per-byte transfers completes without timeouts, and the per-byte
 * overhead of 79-byte save/restore would be significant.
 */

/* VIC-II screen RAM at $0400 is unused in 80-column VDC mode. */
#define IEC_ZP_SAVE_BUF   0x0400u
#define IEC_PARAM_SCRATCH  0x044Fu
#define IEC_RESULT_SCRATCH 0x0450u

/* Save ZP $0A-$58 (79 bytes: __rc0-__rc29 + zp-data + zp-bss) */
#define IEC_ZP_SAVE_ASM           \
    "ldx #78\n"                   \
    "1: lda $0A,x\n"             \
    "sta $0400,x\n"              \
    "dex\n"                       \
    "bpl 1b\n"

/* Restore ZP $0A-$58 from the save buffer */
#define IEC_ZP_RESTORE_ASM        \
    "ldx #78\n"                   \
    "2: lda $0400,x\n"           \
    "sta $0A,x\n"                \
    "dex\n"                       \
    "bpl 2b\n"

/* Switch to KERNAL mode and enable IRQs for IEC timing */
#define IEC_ENTER_ASM             \
    "lda #$0E\n"                  \
    "sta $FF00\n"                 \
    "cli\n"

/* Disable IRQs and restore full-RAM mode */
#define IEC_LEAVE_ASM             \
    "sei\n"                       \
    "lda #$3F\n"                  \
    "sta $FF00\n"

/* ── Public KERNAL wrappers ─────────────────────────────────────────────── */

__attribute__((noinline))
void lp128_mmu_set_full_ram(void)
{
    LP128_MMU_CR_ADDR = LP128_MMU_CFG_RAM0;
}

__attribute__((noinline))
void lp128_k_chrout(uint8_t value)
{
    uint8_t prev = enter_kernal();
    cbm_k_chrout(value);
    restore_mmu(prev);
}

__attribute__((noinline))
void lp128_k_bsout(uint8_t value)
{
    uint8_t prev = enter_kernal();
    cbm_k_bsout(value);
    restore_mmu(prev);
}

__attribute__((noinline))
uint8_t lp128_k_getin(void)
{
    uint8_t prev = enter_kernal();
    uint8_t value = cbm_k_getin();
    restore_mmu(prev);
    return value;
}

__attribute__((noinline))
void lp128_k_setnam(const char *path)
{
    /* We CANNOT call the CRT's cbm_k_setnam() here because it calls strlen(),
     * which is linked above $C000 and gets hidden by KERNAL ROM when MMU=$0E.
     *
     * Instead: compute strlen in RAM mode, store all three SETNAM parameters
     * into ZP scratch, then switch to KERNAL mode for the bare JSR $FFBD. */
    uint8_t len = 0;
    while (path[len] != '\0') len++;

    /* $FC-$FE are KERNAL scratch / free ZP locations.  Store params there
     * while still in RAM mode so inline asm can load them after MMU switch. */
    *(volatile uint8_t *)0x00FCu = len;
    *(volatile uint8_t *)0x00FDu = (uint8_t)(uint16_t)path;
    *(volatile uint8_t *)0x00FEu = (uint8_t)((uint16_t)path >> 8);

    __asm__ volatile(
        "lda #$0E\n"
        "sta $FF00\n"       /* enter KERNAL mode */
        "lda $FC\n"         /* A = filename length */
        "ldx $FD\n"         /* X = lo(pointer) */
        "ldy $FE\n"         /* Y = hi(pointer) */
        "jsr $FFBD\n"       /* KERNAL SETNAM */
        "lda #$3F\n"
        "sta $FF00\n"       /* restore full-RAM mode */
        ::: "a", "x", "y", "memory"
    );
}

__attribute__((noinline))
void lp128_k_setlfs_noirq(uint8_t lfn, uint8_t device, uint8_t secondary)
{
    uint8_t prev = enter_kernal();
    cbm_k_setlfs(lfn, device, secondary);
    restore_mmu(prev);
}

__attribute__((noinline))
uint8_t lp128_k_open_noirq(void)
{
    uint8_t prev = enter_kernal();
    uint8_t value = cbm_k_open();
    restore_mmu(prev);
    return value;
}

__attribute__((noinline))
uint8_t lp128_k_chkin_noirq(uint8_t lfn)
{
    uint8_t prev = enter_kernal();
    uint8_t value = cbm_k_chkin(lfn);
    restore_mmu(prev);
    return value;
}

__attribute__((noinline))
void lp128_k_close_noirq(uint8_t lfn)
{
    uint8_t prev = enter_kernal();
    cbm_k_close(lfn);
    restore_mmu(prev);
}

__attribute__((noinline))
void lp128_k_clrch_noirq(void)
{
    uint8_t prev = enter_kernal();
    cbm_k_clrch();
    restore_mmu(prev);
}

__attribute__((noinline))
uint8_t lp128_k_readst_noirq(void)
{
    uint8_t prev = enter_kernal();
    uint8_t value = cbm_k_readst();
    restore_mmu(prev);
    return value;
}

__attribute__((noinline))
uint8_t lp128_k_chrin_noirq(void)
{
    uint8_t prev = enter_kernal();
    uint8_t value = cbm_k_chrin();
    restore_mmu(prev);
    return value;
}

__attribute__((noinline))
void lp128_k_setlfs(uint8_t lfn, uint8_t device, uint8_t secondary)
{
    /* SETLFS only updates KERNAL file parameters; it does not perform IEC
     * bus activity and does not need IRQs enabled. Enabling IRQs here lets
     * the ROM handler interrupt compiled C code and corrupt live state. */
    uint8_t prev = enter_kernal();
    cbm_k_setlfs(lfn, device, secondary);
    restore_mmu(prev);
}

__attribute__((noinline))
uint8_t lp128_k_open(void)
{
    __asm__ volatile(
        IEC_ZP_SAVE_ASM
        IEC_ENTER_ASM
        "jsr $FFC0\n"        /* KERNAL OPEN */
        "sei\n"              /* immediately disable IRQs */
        "sta $0450\n"        /* stash return value while A is live */
        "lda #$3F\n"
        "sta $FF00\n"        /* restore full-RAM mode */
        IEC_ZP_RESTORE_ASM
        ::: "a", "x", "y", "memory"
    );
    return *(volatile uint8_t *)IEC_RESULT_SCRATCH;
}

__attribute__((noinline))
uint8_t lp128_k_chkin(uint8_t lfn)
{
    *(volatile uint8_t *)IEC_PARAM_SCRATCH = lfn;
    __asm__ volatile(
        IEC_ZP_SAVE_ASM
        "ldx $044F\n"        /* load LFN before IRQs (X safe across ISR) */
        IEC_ENTER_ASM
        "jsr $FFC6\n"        /* KERNAL CHKIN */
        "sei\n"              /* immediately disable IRQs */
        "sta $0450\n"        /* stash return value while A is live */
        "lda #$3F\n"
        "sta $FF00\n"        /* restore full-RAM mode */
        IEC_ZP_RESTORE_ASM
        ::: "a", "x", "y", "memory"
    );
    return *(volatile uint8_t *)IEC_RESULT_SCRATCH;
}

__attribute__((noinline))
void lp128_k_close(uint8_t lfn)
{
    *(volatile uint8_t *)IEC_PARAM_SCRATCH = lfn;
    __asm__ volatile(
        IEC_ZP_SAVE_ASM
        "lda #$0E\n"
        "sta $FF00\n"        /* switch to KERNAL mode */
        "lda $044F\n"        /* load LFN (visible in all MMU modes) */
        "cli\n"              /* enable IRQs; A safe across ISR */
        "jsr $FFC3\n"        /* KERNAL CLOSE */
        IEC_LEAVE_ASM
        IEC_ZP_RESTORE_ASM
        ::: "a", "x", "y", "memory"
    );
}

__attribute__((noinline))
void lp128_k_clrch(void)
{
    __asm__ volatile(
        IEC_ZP_SAVE_ASM
        IEC_ENTER_ASM
        "jsr $FFCC\n"        /* KERNAL CLRCH */
        IEC_LEAVE_ASM
        IEC_ZP_RESTORE_ASM
        ::: "a", "x", "y", "memory"
    );
}

__attribute__((noinline))
uint8_t lp128_k_readst(void)
{
    uint8_t prev = enter_kernal_cli();
    uint8_t value = cbm_k_readst();
    sei_restore_mmu(prev);
    return value;
}

__attribute__((noinline))
uint8_t lp128_k_basin(void)
{
    uint8_t prev = enter_kernal_cli();
    uint8_t value = cbm_k_basin();
    sei_restore_mmu(prev);
    return value;
}

__attribute__((noinline))
uint8_t lp128_k_chrin(void)
{
    uint8_t prev = enter_kernal_cli();
    uint8_t value = cbm_k_chrin();
    sei_restore_mmu(prev);
    return value;
}

__attribute__((noinline))
void lp128_k_videomode(uint8_t mode)
{
    uint8_t prev = enter_kernal();
    videomode(mode);
    restore_mmu(prev);
}

#endif /* __mos__ || __llvm_mos__ */
