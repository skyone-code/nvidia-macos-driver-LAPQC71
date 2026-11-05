/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once
#include "nvrm_telemetry_abi.h"

// The caller serialises queries and teardown, exactly like nvrm_query_utilization.
// The RM client and the instruction buffers belong to the opaque state, which is
// allocated on the first query and freed by nvrm_free_telemetry.
extern "C" bool nvrm_query_telemetry(unsigned int gpuId, void **state, struct NVRMTelemetry *out);
extern "C" void nvrm_free_telemetry(void **state);

// The clock domains NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2 reports, in the order the
// firmware lists them. The first four map onto RUSD_CLK_PUBLIC_DOMAIN_*, which is why
// they are named the same way; anything past them is a domain this driver does not
// know about and is skipped.
#define NVRM_CLK_DOMAIN_GRAPHICS 0u
#define NVRM_CLK_DOMAIN_MEMORY   1u
#define NVRM_CLK_DOMAIN_VIDEO    2u
#define NVRM_CLK_DOMAIN_SM       3u
