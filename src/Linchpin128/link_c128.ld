/* C128 extended linker script for Linchpin128.
 *
 * The native runtime needs the larger 0x1200-0xD1FF window for code and static
 * data. At runtime we expose RAM underneath BASIC/function ROM while keeping
 * the KERNAL visible, so code and BSS may legally occupy 0x8000-0xCFFF.
 */

__basic_zp_start = 0x000A;
__basic_zp_end   = 0x0090;

MEMORY {
    /* Extended again to accommodate the VDC shadow buffer (~2 KB),
     * the additional runtime state needed for DBF-backed file channels,
     * and temporary startup diagnostics.
     * BSS past $DC00 lands in CIA/VDC I/O territory when MMU is in $3E
     * mode, but all BSS variables are accessed only in the default $3F
     * (all-RAM) mode, so there is no conflict. */
    ram (rw) : ORIGIN = 0x1200, LENGTH = 0xDD00
}

INCLUDE commodore.ld

/* Software (imaginary) stack for llvm-mos.  Must NOT overlap with code
 * ($1200+), BSS, screen RAM ($0400-$07E7), or pinned variables ($0B00).
 * Placed at $1200 so it grows downward through $0B20-$11FF (~1.7 KB),
 * which is always plain RAM regardless of MMU config. */
__stack = 0x1200;

OUTPUT_FORMAT {
    /* Standard C128 PRG header: 2-byte load address */
    SHORT(ORIGIN(ram))
    TRIM(ram)
}
