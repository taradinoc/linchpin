/* lp128_diag.h – Diagnostic output to 40-column VIC screen.
 *
 * Writes screen codes directly to VIC screen RAM at $0400.
 * Works in ANY MMU mode ($3E or $3F) because $0400 is always plain RAM.
 * Only active on the MOS/C128 target; no-ops on POSIX.
 *
 * VIC screen code reference:
 *   'A'-'Z' = 0x01-0x1A,  '0'-'9' = 0x30-0x39,  space = 0x20
 */

#ifndef LP128_DIAG_H
#define LP128_DIAG_H

#if defined(__mos__) || defined(__llvm_mos__)

#include <stdint.h>

#define DIAG_VIC_IO_BEGIN() do { \
    uint8_t _diag_prev_mmu = *(volatile uint8_t *)0xFF00u; \
    *(volatile uint8_t *)0xFF00u = 0x3Eu;

#define DIAG_VIC_IO_END() \
    *(volatile uint8_t *)0xFF00u = _diag_prev_mmu; \
} while (0)

#define DIAG_VIC(pos, sc) \
    (*(volatile uint8_t *)(0x0400u + (unsigned)(pos)) = (uint8_t)(sc))

static inline uint8_t diag_hex_sc(uint8_t nibble)
{
    return nibble < 10u ? (uint8_t)(0x30u + nibble) : (uint8_t)(nibble - 9u);
}

#define DIAG_VIC_HEX8(pos, val) do { \
    uint8_t _b = (uint8_t)(val); \
    DIAG_VIC((pos),     diag_hex_sc((uint8_t)(_b >> 4))); \
    DIAG_VIC((pos) + 1, diag_hex_sc((uint8_t)(_b & 0xFu))); \
} while (0)

#define DIAG_VIC_HEX16(pos, val) do { \
    uint16_t _w = (uint16_t)(val); \
    DIAG_VIC((pos),     diag_hex_sc((uint8_t)(_w >> 12))); \
    DIAG_VIC((pos) + 1, diag_hex_sc((uint8_t)((_w >> 8) & 0xFu))); \
    DIAG_VIC((pos) + 2, diag_hex_sc((uint8_t)((_w >> 4) & 0xFu))); \
    DIAG_VIC((pos) + 3, diag_hex_sc((uint8_t)(_w & 0xFu))); \
} while (0)

static inline void diag_set_vic_border(uint8_t color)
{
    DIAG_VIC_IO_BEGIN();
    *(volatile uint8_t *)0xD020u = (uint8_t)(color & 0x0Fu);
    DIAG_VIC_IO_END();
}

static inline void diag_set_vic_background(uint8_t color)
{
    DIAG_VIC_IO_BEGIN();
    *(volatile uint8_t *)0xD021u = (uint8_t)(color & 0x0Fu);
    DIAG_VIC_IO_END();
}

/* Screen-code constants for marker letters */
#define DIAG_SC_B  0x02u  /* 'B' */
#define DIAG_SC_L  0x0Cu  /* 'L' */
#define DIAG_SC_I  0x09u  /* 'I' */
#define DIAG_SC_H  0x08u  /* 'H' */
#define DIAG_SC_V  0x16u  /* 'V' */
#define DIAG_SC_R  0x12u  /* 'R' */
#define DIAG_SC_D  0x04u  /* 'D' */
#define DIAG_SC_X  0x18u  /* 'X' */
#define DIAG_SC_K  0x0Bu  /* 'K' */
#define DIAG_SC_P  0x10u  /* 'P' */
#define DIAG_SC_W  0x17u  /* 'W' */
#define DIAG_SC_F  0x06u  /* 'F' */
#define DIAG_SC_E  0x05u  /* 'E' */
#define DIAG_SC_Q  0x11u  /* 'Q' */

#else /* POSIX */

#define DIAG_VIC(pos, sc)        ((void)0)
#define DIAG_VIC_HEX8(pos, val)  ((void)0)
#define DIAG_VIC_HEX16(pos, val) ((void)0)

#endif

#endif /* LP128_DIAG_H */
