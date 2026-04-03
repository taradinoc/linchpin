#include "lp128_vm.h"
#include "lp128_host.h"
#include "lp128_diag.h"

#if !(defined(__mos__) || defined(__llvm_mos__))
#include <stdio.h>
#define MAIN_DIAG(...)  fprintf(stderr, __VA_ARGS__)
#define MAIN_TRACE(ch)  ((void)0)
#else
#include "lp128_mmu.h"
#include "lp128_reu.h"
#define MAIN_DIAG(...)  ((void)0)
#define MAIN_TRACE(ch)  lp128_k_chrout((uint8_t)(ch))

/* Convert one ASCII character to the VIC-II screen code used on the
 * 40-column display.  Upper/lowercase letters map to PETSCII screen codes;
 * digits, spaces, and common punctuation pass through unchanged. */
static uint8_t main_diag_sc(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (uint8_t)(ch - '@');
    }
    if (ch >= 'a' && ch <= 'z') {
        return (uint8_t)(ch - 0x60u);
    }
    if (ch >= '0' && ch <= '9') {
        return (uint8_t)ch;
    }
    switch (ch) {
    case ' ': return 0x20u;
    case '.': return 0x2Eu;
    case ',': return 0x2Cu;
    case ':': return 0x3Au;
    case '-': return 0x2Du;
    case '>': return 0x3Eu;
    default:  return 0x20u;
    }
}

/* Write a NUL-terminated ASCII string directly to VIC RAM on the given
 * 40-column screen row (0-based), padding the remainder of the row with
 * spaces.  Used for pre-init diagnostics before the VDC host is running. */
static void main_diag_write_text(uint8_t row, const char *text)
{
    volatile uint8_t *dst = (volatile uint8_t *)(0x0400u + (uint16_t)row * 40u);
    uint8_t col = 0u;

    while (col < 40u && text[col] != '\0') {
        dst[col] = main_diag_sc(text[col]);
        col++;
    }
    while (col < 40u) {
        dst[col++] = 0x20u;
    }
}
#endif

static const char *default_bundle_path(void)
{
    /* The filename must use uppercase ASCII bytes ($41-$5A), which are the
     * same values as standard PETSCII uppercase and match the CBM DOS
     * directory entry written by c1541.  llvm-mos does NOT convert string
     * literals to PETSCII automatically at compile time. */
    return "CORNER.CSB";
}

int main(int argc, char **argv)
{
#if defined(__mos__) || defined(__llvm_mos__)
    /* No command-line arguments on C128; argv is not valid. */
    (void)argc; (void)argv;
    const char *path = default_bundle_path();
#else
    const char *path = argc > 1 ? argv[1] : default_bundle_path();
#endif
    /* On 6502/mos the VM state is ~3 KB and the bundle can be several KB;
     * keep them in BSS (static) rather than on the tiny hardware stack. */
#if defined(__mos__) || defined(__llvm_mos__)
    static lp128_bundle  bundle;
    static lp128_host    host;
    static lp128_vm_state vm;
    static char error[256];  /* must be static: soft stack at $1C00 is inside code region */
#else
    lp128_bundle  bundle;
    lp128_host    host;
    lp128_vm_state vm;
    char error[256];
#endif

    MAIN_TRACE('0');
    MAIN_TRACE(13);  /* CR — new line for bundle traces */
    DIAG_VIC(0, DIAG_SC_B);   /* B = before bundle load */
    if (!lp128_bundle_load(path, &bundle, error, sizeof(error))) {
        MAIN_TRACE('E');
        MAIN_TRACE(13);
        DIAG_VIC(0, DIAG_SC_E);   /* E = error */
#if defined(__mos__) || defined(__llvm_mos__)
        main_diag_write_text(22u, "BUNDLE LOAD FAILED");
        main_diag_write_text(23u, error);
#endif
        return 1;
    }
    MAIN_TRACE('1');
    DIAG_VIC(0, DIAG_SC_L);   /* L = loaded */

    /* Install our custom IRQ handler AFTER the bundle has been loaded from
     * disk.  Under MMU $3F the hardware IRQ vector at $FFFE/$FFFF reads
     * from RAM, not KERNAL ROM — without a handler, the first CIA timer
     * IRQ sends the CPU to a garbage address.  We must wait until AFTER
     * all IEC/serial-bus I/O is complete because the handler disables the
     * default KERNAL IRQ path. */
#if defined(__mos__) || defined(__llvm_mos__)
    lp128_install_irq_handler();
#endif
    DIAG_VIC(0, DIAG_SC_I);   /* I = irq installed */

    lp128_host_posix_init(&host);
    DIAG_VIC(0, DIAG_SC_H);   /* H = host init done */

    if (!lp128_vm_init(&vm, &bundle, &host, error, sizeof(error))) {
        MAIN_DIAG("VM init failed: %s\n", error);
        lp128_host_posix_cleanup(&host);
        lp128_bundle_free(&bundle);
        DIAG_VIC(0, DIAG_SC_E);   /* E = error */
        return 1;
    }
    DIAG_VIC(0, DIAG_SC_V);   /* V = vm init done */

    lp128_vm_run(&vm);
    DIAG_VIC(0, DIAG_SC_X);   /* X = vm_run returned */
#if defined(__mos__) || defined(__llvm_mos__)
    /* On-screen halt diagnostic: write halt code + PC to row 24,
     * module + eval_stack_top + call_stack_top to row 23. */
    {
        volatile uint8_t *r24 = (volatile uint8_t *)(0x0400u + 960u);
        volatile uint8_t *r23 = (volatile uint8_t *)(0x0400u + 920u);
        uint16_t vals[2] = { vm.halt_code, vm.program_counter };
        uint8_t vi, ni;
        /* "H=" then halt code, " P=" then PC */
        r24[0] = 0x08; r24[1] = 0x3D;  /* H= */
        r24[6] = 0x20; r24[7] = 0x10; r24[8] = 0x3D;  /* _P= */
        for (vi = 0; vi < 2; vi++) {
            uint8_t base = vi ? 9 : 2;
            uint16_t v = vals[vi];
            for (ni = 0; ni < 4; ni++) {
                uint8_t d = (uint8_t)((v >> (12 - ni * 4)) & 0xF);
                r24[base + ni] = d < 10 ? (uint8_t)(0x30 + d) : (uint8_t)(d - 9);
            }
        }
        /* Row 23: "M=xx S=xxxx C=xx" */
        {
            uint16_t mvals[3] = { vm.current_module_id, vm.eval_stack_top, vm.call_stack_top };
            uint8_t labels[3][2] = { {0x0D, 0x3D}, {0x13, 0x3D}, {0x03, 0x3D} };
            uint8_t widths[3] = { 2, 4, 2 };
            uint8_t pos = 0;
            for (vi = 0; vi < 3; vi++) {
                r23[pos++] = labels[vi][0]; r23[pos++] = labels[vi][1];
                uint16_t v = mvals[vi];
                for (ni = 0; ni < widths[vi]; ni++) {
                    uint8_t shift = (uint8_t)((widths[vi] - 1 - ni) * 4);
                    uint8_t d = (uint8_t)((v >> shift) & 0xFu);
                    r23[pos++] = d < 10 ? (uint8_t)(0x30 + d) : (uint8_t)(d - 9);
                }
                r23[pos++] = 0x20; /* space separator */
            }
        }
    }
#endif

    if (vm.host->flush != NULL) {
        vm.host->flush(vm.host);
    }

    lp128_vm_dump_perf_counters(&vm);

    lp128_vm_free(&vm);
    lp128_host_posix_cleanup(&host);
    lp128_bundle_free(&bundle);
    return 0;
}
