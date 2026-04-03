/*
 * lp128_reu.c – Commodore REU DMA implementation for the llvm-mos C128 target.
 *
 * All functions are no-ops on non-MOS builds (the #if guard wraps the whole
 * translation unit so the compiler can dead-strip it when cross-compiling for
 * debug on POSIX).
 *
 * IMPORTANT: REU registers live at $DF00-$DF0A inside the I/O block at
 * $D000-$DFFF.  In "full-RAM" MMU mode ($3F), bit 0 = 1 hides the I/O
 * block behind RAM, making the REU hardware invisible to the CPU.  We must
 * temporarily switch to $3E (all RAM + I/O) for every register access.
 * REU DMA itself bypasses the CPU MMU and accesses physical RAM directly,
 * so only the CPU register writes/reads need I/O visibility.
 */

#if defined(__mos__) || defined(__llvm_mos__)

#include "lp128_reu.h"

/* MMU Configuration Register. */
#define MMU_CR (*(volatile uint8_t *)0xFF00u)

/* $3E = all RAM in bank 0 + I/O visible at $D000-$DFFF.
 * Differs from $3F only in bit 0 (I/O enable). */
#define MMU_CFG_RAM_IO 0x3Eu

static inline uint8_t io_enter(void)
{
    uint8_t prev = MMU_CR;
    MMU_CR = MMU_CFG_RAM_IO;
    return prev;
}

static inline void io_leave(uint8_t prev)
{
    MMU_CR = prev;
}

/* Two-byte staging area used by the single-byte/word accessors.
 * CRITICAL: This buffer is the C128-side DMA target during io_enter ($3E)
 * when the I/O block at $D000-$DFFF is visible.  A normal BSS variable can
 * land above $D000 and overlap with VIC-II/SID/CIA hardware registers,
 * causing DMA writes to hit I/O chips instead of RAM.  We pin the staging
 * buffers to a fixed address in the cassette-buffer region ($0340-$03FF)
 * which is always plain RAM regardless of MMU configuration. */
#define REU_STAGE_ADDR     0x0340u
#define REU_SCRATCH_ADDR   0x0B00u
#define REU_SCRATCH_SIZE   32u
#define REU_CMD_STORE_ADDR 0x0342u   /* 1 byte: active store command */
#define REU_CMD_FETCH_ADDR 0x0343u   /* 1 byte: active fetch command */

static uint8_t *const reu_stage     = (uint8_t *)REU_STAGE_ADDR;
static uint8_t *const reu_scratch   = (uint8_t *)REU_SCRATCH_ADDR;

/* Active command bytes; autodetected by reu_self_test for emulator variance.
 * CRITICAL: These MUST be pinned to addresses below $D000 so they are never
 * shadowed by I/O when the MMU is in $3E mode.  A normal BSS variable can
 * land in the $D000-$DFFF I/O shadow and return SID/CIA register values
 * instead of our stored bytes. */
static uint8_t *const reu_cmd_store_ptr = (uint8_t *)REU_CMD_STORE_ADDR;
static uint8_t *const reu_cmd_fetch_ptr = (uint8_t *)REU_CMD_FETCH_ADDR;

/* ── Internal helpers (called only while I/O is visible) ─────────────────── */

static void set_c128_addr(const void *ptr)
{
    uint16_t a = (uint16_t)(uintptr_t)ptr;
    REU_C128_LO = (uint8_t)a;
    REU_C128_HI = (uint8_t)(a >> 8);
}

static void set_reu_addr(reu_addr_t addr)
{
    REU_ADDR_LO  = (uint8_t)(addr);
    REU_ADDR_MID = (uint8_t)(addr >> 8);
    REU_ADDR_HI  = (uint8_t)(addr >> 16);
}

/* ── Public DMA ─────────────────────────────────────────────────────────── */

void reu_store(const void *c128_ptr, reu_addr_t reu_addr, uint16_t len)
{
    uint8_t cmd = *reu_cmd_store_ptr;  /* read from pinned low-RAM address */
    uint8_t prev = io_enter();
    set_c128_addr(c128_ptr);
    set_reu_addr(reu_addr);
    REU_XFER_LO  = (uint8_t)len;
    REU_XFER_HI  = (uint8_t)(len >> 8);
    REU_ADDR_CTL = 0x00u;
    REU_COMMAND  = cmd;
    io_leave(prev);
}

void reu_fetch(void *c128_ptr, reu_addr_t reu_addr, uint16_t len)
{
    uint8_t cmd = *reu_cmd_fetch_ptr;  /* read from pinned low-RAM address */
    uint8_t prev = io_enter();
    set_c128_addr(c128_ptr);
    set_reu_addr(reu_addr);
    REU_XFER_LO  = (uint8_t)len;
    REU_XFER_HI  = (uint8_t)(len >> 8);
    REU_ADDR_CTL = 0x00u;
    REU_COMMAND  = cmd;
    io_leave(prev);
}

void reu_fill_zero(reu_addr_t reu_addr, uint16_t count)
{
    uint8_t prev;
    if (count == 0u) return;
    reu_stage[0] = 0u;            /* write BSS while still in $3F */
    prev = io_enter();
    set_c128_addr(reu_stage);
    set_reu_addr(reu_addr);
    REU_XFER_LO  = (uint8_t)count;
    REU_XFER_HI  = (uint8_t)(count >> 8);
    REU_ADDR_CTL = 0x80u;
    REU_COMMAND  = REU_CMD_STORE;
    REU_ADDR_CTL = 0x00u;
    io_leave(prev);
}

void reu_fill_zero_32(reu_addr_t reu_addr, uint32_t count)
{
    while (count > 0u) {
        uint16_t chunk = (count > 0xFFFFu) ? 0xFFFFu : (uint16_t)count;
        reu_fill_zero(reu_addr, chunk);
        reu_addr += chunk;
        count    -= chunk;
    }
}

void reu_copy(reu_addr_t dst, reu_addr_t src, uint16_t count)
{
    if (count == 0u || dst == src) return;

    if (dst < src || dst >= src + (reu_addr_t)count) {
        /* Forward copy – no harmful overlap. */
        while (count > 0u) {
            uint16_t chunk = (count > REU_SCRATCH_SIZE)
                             ? REU_SCRATCH_SIZE : count;
            reu_fetch(reu_scratch, src, chunk);
            reu_store(reu_scratch, dst, chunk);
            src   += chunk;
            dst   += chunk;
            count -= chunk;
        }
    } else {
        /* Backward copy – dst > src with overlap; copy from end to start. */
        while (count > 0u) {
            uint16_t chunk = (count > REU_SCRATCH_SIZE)
                             ? REU_SCRATCH_SIZE : count;
            count -= chunk;
            reu_fetch(reu_scratch, src + (reu_addr_t)count, chunk);
            reu_store(reu_scratch, dst + (reu_addr_t)count, chunk);
        }
    }
}

/* ── Byte / word accessors ──────────────────────────────────────────────── */

uint8_t reu_read_byte(reu_addr_t addr)
{
    reu_fetch(reu_stage, addr, 1u);
    return reu_stage[0];
}

uint16_t reu_read_word(reu_addr_t addr)
{
    reu_fetch(reu_stage, addr, 2u);
    return (uint16_t)(reu_stage[0] | ((uint16_t)reu_stage[1] << 8));
}

void reu_write_byte(reu_addr_t addr, uint8_t val)
{
    reu_stage[0] = val;
    reu_store(reu_stage, addr, 1u);
}

void reu_write_word(reu_addr_t addr, uint16_t val)
{
    reu_stage[0] = (uint8_t)(val & 0xFFu);
    reu_stage[1] = (uint8_t)(val >> 8u);
    reu_store(reu_stage, addr, 2u);
}

bool reu_self_test(void)
{
    const reu_addr_t probe = (reu_addr_t)0x000100u;
    uint8_t *const lowmem = (uint8_t *)0x0200u;

    /* Some REU implementations differ on command bit semantics. Try both the
     * plain execute commands (0x90/0x91) and the C128-style variant
     * (0xB0/0xB1), then keep the pair that actually round-trips correctly. */

    /* Candidate 1: plain execute/store/fetch. */
    *reu_cmd_store_ptr = 0x90u;
    *reu_cmd_fetch_ptr = 0x91u;
    lowmem[0] = 0x5Au;
    lowmem[1] = 0xA5u;
    reu_store(lowmem, probe, 2u);
    lowmem[0] = 0u;
    lowmem[1] = 0u;
    reu_fetch(lowmem, probe, 2u);
    if (lowmem[0] == 0x5Au && lowmem[1] == 0xA5u) {
        return true;
    }

    /* Candidate 2: execute with extra mode bits (works on some setups). */
    *reu_cmd_store_ptr = 0xB0u;
    *reu_cmd_fetch_ptr = 0xB1u;
    lowmem[0] = 0x5Au;
    lowmem[1] = 0xA5u;
    reu_store(lowmem, probe, 2u);
    lowmem[0] = 0u;
    lowmem[1] = 0u;
    reu_fetch(lowmem, probe, 2u);
    if (lowmem[0] == 0x5Au && lowmem[1] == 0xA5u) {
        return true;
    }

    /* Default back to nominal values if neither variant worked. */
    *reu_cmd_store_ptr = REU_CMD_STORE;
    *reu_cmd_fetch_ptr = REU_CMD_FETCH;
    return false;
}

#endif /* __mos__ || __llvm_mos__ */
