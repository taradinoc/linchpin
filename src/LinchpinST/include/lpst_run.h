#ifndef LPST_RUN_H
#define LPST_RUN_H

#include "lpst_exec.h"

/**
 * Run the VM until it halts, hits the instruction limit, or encounters an error.
 * Returns LPST_OK on normal halt, or an error code on failure.
 */
lpst_result lpst_run(lpst_exec_state *state);

#endif
