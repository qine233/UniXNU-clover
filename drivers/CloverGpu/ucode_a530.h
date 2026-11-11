// SPDX-License-Identifier: BSD-2-Clause-Patent
// The a5xx microcode blobs the CP needs before ME_INIT. This is the empty default: a build that has not
// run scripts/make-clover-gpu-ucode.py links with no microcode and the driver refuses to start the CP
// instead of feeding it a truncated stream. The generator overwrites this file with the real arrays,
// their sizes and the sha256 of each input.
#ifndef CLOVER_GPU_UCODE_A530_H
#define CLOVER_GPU_UCODE_A530_H

#include <stddef.h>

static const unsigned char clover_gpu_pfp_blob[1] = {0};
static const unsigned char clover_gpu_pm4_blob[1] = {0};
static const size_t clover_gpu_pfp_size = 0;
static const size_t clover_gpu_pm4_size = 0;

#endif // CLOVER_GPU_UCODE_A530_H
