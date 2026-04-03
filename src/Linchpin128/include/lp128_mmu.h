#ifndef LP128_MMU_H
#define LP128_MMU_H

#include <stdint.h>

#if defined(__mos__) || defined(__llvm_mos__)

/* ── MMU configuration constants ────────────────────────────────────────── */

#define LP128_MMU_CR_ADDR    (*(volatile uint8_t *)0xFF00u)
#define LP128_MMU_CFG_KERNAL 0x0Eu
#define LP128_MMU_CFG_RAM0   0x3Fu

/* ── KERNAL call wrappers ───────────────────────────────────────────────── *
 * Implemented in lp128_kernal.c, which MUST be linked first so its code
 * resides below $C000.  When the MMU maps KERNAL ROM over $C000-$FFFF,
 * only code below $C000 can safely execute.  These wrappers are noinline
 * to prevent the compiler from inlining the MMU switch into callers that
 * may reside above $C000.                                                  */

/** Switch the MMU to all-RAM mode ($3F), hiding ROM and I/O. */
void    lp128_mmu_set_full_ram(void);

/** Install the custom IRQ handler at $03A0 and enable interrupts.
 *  Must be called early (before any IEC serial I/O) so the KERNAL's
 *  jiffy clock ticks and serial bus timing works correctly. */
void    lp128_install_irq_handler(void);

/** Output one character to the current output device via KERNAL CHROUT.
 *  Briefly maps KERNAL ROM; safe to call in full-RAM mode. */
void    lp128_k_chrout(uint8_t value);

/** Output one character via KERNAL BSOUT (same as CHROUT on C128). */
void    lp128_k_bsout(uint8_t value);

/** Read one character from the current input device via KERNAL GETIN.
 *  Returns 0 if no key is waiting. */
uint8_t lp128_k_getin(void);

/** Set the filename for subsequent OPEN.  path must be in low RAM. */
void    lp128_k_setnam(const char *path);

/* The _noirq variants briefly enable IRQs while in KERNAL ROM mode so that
 * the CIA/serial bus timing works.  The non-noirq variants keep SEI throughout
 * and are safe only for non-IEC devices. */

/** SETLFS + OPEN with IRQs enabled (needed for IEC disk access). */
void    lp128_k_setlfs_noirq(uint8_t lfn, uint8_t device, uint8_t secondary);
uint8_t lp128_k_open_noirq(void);
uint8_t lp128_k_chkin_noirq(uint8_t lfn);
void    lp128_k_close_noirq(uint8_t lfn);
void    lp128_k_clrch_noirq(void);
uint8_t lp128_k_readst_noirq(void);
uint8_t lp128_k_chrin_noirq(void);

/** SETLFS, OPEN, CHKIN, etc. without enabling IRQs during the KERNAL call. */
void    lp128_k_setlfs(uint8_t lfn, uint8_t device, uint8_t secondary);
uint8_t lp128_k_open(void);
uint8_t lp128_k_chkin(uint8_t lfn);
void    lp128_k_close(uint8_t lfn);
void    lp128_k_clrch(void);
uint8_t lp128_k_readst(void);
uint8_t lp128_k_basin(void);
uint8_t lp128_k_chrin(void);

/** Switch to the given VIDEOMODE_* constant via the KERNAL video-mode call. */
void    lp128_k_videomode(uint8_t mode);

#endif /* __mos__ || __llvm_mos__ */

#endif /* LP128_MMU_H */