/*
 * lp128_reu.h – Commodore RAM Expansion Unit (REU) DMA interface.
 *
 * Only used when building for the llvm-mos / mos-c128-clang target.
 * On POSIX builds this header is a no-op.
 *
 *  REU memory layout used by this runtime:
 *    0x000000 – 0x01FFFF  VM heap  (LP128_FULL_RAM_BYTES = 128 KB)
 *    0x020000 – …         bundle code_pages (loaded at startup)
 */

#ifndef LP128_REU_H
#define LP128_REU_H

#if defined(__mos__) || defined(__llvm_mos__)

#include <stdint.h>
#include <stdbool.h>

/* ── REU hardware registers ($DF00 – $DF0A) ──────────────────────────────── */

#define REU_STATUS   (*(volatile uint8_t *)0xDF00u)
#define REU_COMMAND  (*(volatile uint8_t *)0xDF01u)
#define REU_C128_LO  (*(volatile uint8_t *)0xDF02u)
#define REU_C128_HI  (*(volatile uint8_t *)0xDF03u)
#define REU_ADDR_LO  (*(volatile uint8_t *)0xDF04u)
#define REU_ADDR_MID (*(volatile uint8_t *)0xDF05u)
#define REU_ADDR_HI  (*(volatile uint8_t *)0xDF06u)
#define REU_XFER_LO  (*(volatile uint8_t *)0xDF07u)
#define REU_XFER_HI  (*(volatile uint8_t *)0xDF08u)
#define REU_ADDR_CTL (*(volatile uint8_t *)0xDF0Au)

/*
 * Command byte: execute=1, no-autoload, direction.
 *   0x90 = C128 → REU  (store)
 *   0x91 = REU → C128  (fetch)
 */
#define REU_CMD_STORE 0x90u
#define REU_CMD_FETCH 0x91u

/* ── REU address type and fixed offsets ──────────────────────────────────── */

typedef uint32_t reu_addr_t;

#define REU_VM_HEAP_OFFSET    ((reu_addr_t)0x000000u)
#define REU_CODE_PAGES_OFFSET ((reu_addr_t)0x020000u)  /* after 128 KB heap */

/* ── DMA transfer functions ──────────────────────────────────────────────── */

/** Store len bytes from C128 RAM to REU. */
void reu_store(const void *c128_ptr, reu_addr_t reu_addr, uint16_t len);

/** Fetch len bytes from REU to C128 RAM. */
void reu_fetch(void *c128_ptr, reu_addr_t reu_addr, uint16_t len);

/** Write zero to count consecutive REU bytes (C128 address is fixed). */
void reu_fill_zero(reu_addr_t reu_addr, uint16_t count);

/** Write zero to count consecutive REU bytes (supports >64 KB). */
void reu_fill_zero_32(reu_addr_t reu_addr, uint32_t count);

/** REU-to-REU copy (staged through a small C128 scratch buffer). */
void reu_copy(reu_addr_t dst, reu_addr_t src, uint16_t count);

/* ── Byte / word convenience accessors ──────────────────────────────────── */

uint8_t  reu_read_byte (reu_addr_t addr);
uint16_t reu_read_word (reu_addr_t addr);
void     reu_write_byte(reu_addr_t addr, uint8_t  val);
void     reu_write_word(reu_addr_t addr, uint16_t val);

/** Returns true if a small REU round-trip DMA test succeeds. */
bool     reu_self_test(void);

#endif /* __mos__ || __llvm_mos__ */
#endif /* LP128_REU_H */
