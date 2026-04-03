from __future__ import annotations

import argparse
from pathlib import Path


BASIC_LOAD_ADDR = 0x1C01
TRAMPOLINE_ADDR = 7182
SAFE_LOADER_ADDR = 0x1000
NAME_BUF_ADDR = 0x033C
SCREEN_ADDR = 0x0400


def lo(value: int) -> int:
    return value & 0xFF


def hi(value: int) -> int:
    return (value >> 8) & 0xFF


def emit_chrout(ch: int) -> bytearray:
    return bytearray([0xA9, ch, 0x20, 0xD2, 0xFF])


def emit_store_absolute(value: int, address: int) -> bytearray:
    return bytearray([0xA9, value, 0x8D, lo(address), hi(address)])


def emit_store_zero_page(value: int, address: int) -> bytearray:
    return bytearray([0xA9, value, 0x85, address])


def emit_jsr(address: int) -> bytearray:
    return bytearray([0x20, lo(address), hi(address)])


def emit_jmp(address: int) -> bytearray:
    return bytearray([0x4C, lo(address), hi(address)])


def emit_copy_to_zp(src: int, zp_dest: int, size: int) -> bytearray:
    """Emit 6502 code to copy *size* bytes from *src* (absolute) to *zp_dest*."""
    if size <= 0:
        return bytearray()
    return bytearray([
        0xA2, 0x00,                         # LDX #$00
        0xBD, lo(src), hi(src),             # LDA src,X
        0x95, zp_dest,                      # STA zp_dest,X
        0xE8,                               # INX
        0xE0, size & 0xFF,                  # CPX #size
        0xD0, 0xF6,                         # BNE loop (back to LDA)
    ])


def emit_clear_range(start: int, size: int) -> bytearray:
    if size <= 0:
        return bytearray()

    size &= 0xFFFF
    return bytearray([
        0xA9, lo(start),
        0x85, 0x0C,
        0xA9, hi(start),
        0x85, 0x0D,
        0xA2, lo(size),
        0xA0, hi(size),
        0xA9, 0x00,
        0x84, 0x0E,
        0xA0, 0x00,
        0x91, 0x0C,
        0xE6, 0x0C,
        0xD0, 0x02,
        0xE6, 0x0D,
        0xCA,
        0xE0, 0xFF,
        0xD0, 0x02,
        0xC6, 0x0E,
        0xA4, 0x0E,
        0xD0, 0xEB,
        0xE0, 0x00,
        0xD0, 0xE7,
    ])


def build_loader(filename: bytes, bss_start: int, bss_size: int, zp_bss_start: int, zp_bss_size: int, zp_data_start: int, zp_data_load_start: int, zp_data_size: int, main_addr: int, exit_addr: int, stack_addr: int) -> bytearray:
    loader = bytearray()

    loader += emit_chrout(ord("S"))
    filename_patch = len(loader) + 3
    loader.extend([0xA2, 0x00, 0xBD, 0x00, 0x00, 0x9D, lo(NAME_BUF_ADDR), hi(NAME_BUF_ADDR), 0xE8, 0xE0, len(filename), 0xD0, 0xF5])

    loader += emit_chrout(ord("1"))
    loader.extend([0xA9, len(filename), 0xA2, lo(NAME_BUF_ADDR), 0xA0, hi(NAME_BUF_ADDR), 0x20, 0xBD, 0xFF])
    loader += emit_chrout(ord("2"))
    loader.extend([0xA9, 0x00, 0xA2, 0x08, 0xA0, 0x01, 0x20, 0xBA, 0xFF])
    loader += emit_chrout(ord("3"))
    loader += emit_chrout(ord("4"))
    loader.extend([0xA9, 0x00, 0x20, 0xD5, 0xFF])

    bcs_index = len(loader)
    loader.extend([0xB0, 0x00])

    loader += emit_chrout(ord("J"))
    loader.extend([0x78])  # SEI — disable IRQs before leaving KERNAL safety
    loader += emit_store_zero_page(0x0E, 0x00)
    loader += emit_store_absolute(0x3F, 0xFF00)
    loader += emit_store_absolute(0x01, SCREEN_ADDR + 0)
    loader += emit_store_zero_page(lo(stack_addr), 0x0A)
    loader += emit_store_zero_page(hi(stack_addr), 0x0B)
    loader += emit_clear_range(bss_start, bss_size)
    loader += emit_store_absolute(0x02, SCREEN_ADDR + 1)
    loader += emit_clear_range(zp_bss_start, zp_bss_size)
    loader += emit_copy_to_zp(zp_data_load_start, zp_data_start, zp_data_size)
    loader += emit_store_absolute(0x03, SCREEN_ADDR + 2)
    loader += emit_jsr(main_addr)
    loader += emit_store_absolute(0x04, SCREEN_ADDR + 3)
    loader += emit_jmp(exit_addr)

    fail_index = len(loader)
    loader.extend([0x48])
    loader += emit_chrout(ord("E"))
    loader.extend([0x68, 0x18, 0x69, 0x30])
    loader.extend([0x20, 0xD2, 0xFF, 0x60])

    loader[bcs_index + 1] = fail_index - (bcs_index + 2)

    fname_addr = SAFE_LOADER_ADDR + len(loader)
    loader[filename_patch] = lo(fname_addr)
    loader[filename_patch + 1] = hi(fname_addr)
    loader += filename
    return loader


def build_trampoline(loader_length: int) -> bytearray:
    trampoline = bytearray([
        0xA2, loader_length - 1,
        0xBD, 0x00, 0x00,
        0x9D, lo(SAFE_LOADER_ADDR), hi(SAFE_LOADER_ADDR),
        0xCA,
        0xE0, 0xFF,
        0xD0, 0xF5,
        0x4C, lo(SAFE_LOADER_ADDR), hi(SAFE_LOADER_ADDR),
    ])
    src_addr = TRAMPOLINE_ADDR + len(trampoline)
    trampoline[3] = lo(src_addr)
    trampoline[4] = hi(src_addr)
    return trampoline


def build_program(output: Path, program_name: str, bss_start: int, bss_size: int, zp_bss_start: int, zp_bss_size: int, zp_data_start: int, zp_data_load_start: int, zp_data_size: int, main_addr: int, exit_addr: int, stack_addr: int) -> None:
    filename = program_name.upper().encode("ascii")
    line10 = bytes([0x9E, 0x20]) + b"7182" + bytes([0x00])
    loader = build_loader(filename, bss_start, bss_size, zp_bss_start, zp_bss_size, zp_data_start, zp_data_load_start, zp_data_size, main_addr, exit_addr, stack_addr)
    trampoline = build_trampoline(len(loader))
    code = trampoline + loader

    terminator_addr = BASIC_LOAD_ADDR + 4 + len(line10)
    program = bytearray()
    program += BASIC_LOAD_ADDR.to_bytes(2, "little")
    program += terminator_addr.to_bytes(2, "little")
    program += (10).to_bytes(2, "little")
    program += line10
    program += b"\x00\x00"
    program += code
    output.write_bytes(program)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--program-name", required=True)
    parser.add_argument("--bss-start", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--bss-size", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--zp-bss-start", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--zp-bss-size", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--zp-data-start", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--zp-data-load-start", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--zp-data-size", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--main", required=True, dest="main_addr", type=lambda value: int(value, 0))
    parser.add_argument("--exit", required=True, dest="exit_addr", type=lambda value: int(value, 0))
    parser.add_argument("--stack", required=True, dest="stack_addr", type=lambda value: int(value, 0))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    build_program(
        output=Path(args.output),
        program_name=args.program_name,
        bss_start=args.bss_start,
        bss_size=args.bss_size,
        zp_bss_start=args.zp_bss_start,
        zp_bss_size=args.zp_bss_size,
        zp_data_start=args.zp_data_start,
        zp_data_load_start=args.zp_data_load_start,
        zp_data_size=args.zp_data_size,
        main_addr=args.main_addr,
        exit_addr=args.exit_addr,
        stack_addr=args.stack_addr,
    )


if __name__ == "__main__":
    main()