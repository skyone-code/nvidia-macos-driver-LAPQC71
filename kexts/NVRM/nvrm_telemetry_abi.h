/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

// A second kernel-only platform function next to NVRMGpuUtilization, carrying what the
// utilization ring does not: every thermal sensor, the clocks, board power and the
// throttle reason, rather than the one TSENS register the accelerator reads for itself.
//
// It is a separate call on purpose. The utilization query builds its own RM client and
// runs at 1 Hz; this one is cheap enough to run beside it and would otherwise force a
// larger structure through the same ABI every time either side gained a field.
//
// What this build can and cannot fill in, and why -- so nobody has to re-derive it:
//
//   Sensors  yes. NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2 has a real implementation
//            in the open kernel modules and forwards to the PHYSICAL variant when its
//            cache misses, so it works on both GSP and monolithic builds.
//
//   Clocks   from the shared block first. The control calls that would report them
//            (PERF_GET_LEVEL_INFO_V2) are attempted as a fallback and are usually refused
//            on a monolithic build, where the non-VF body is a stub returning
//            NV_ERR_NOT_SUPPORTED; the firmware's own copy of the clocks is the reliable
//            source and costs a memory copy instead of a control call.
//
//   Power    from the shared block, when the firmware meters it. No RM control reports
//            board power; the reading lives in NV00DE_SHARED_DATA, which the firmware
//            fills. nvrm_telemetry_rusd.c reads it. A part whose firmware does not track
//            power leaves the flag clear.
//
//   Throttle from the same block, and the same "only if the firmware tracks it" rule.
//
//   PCIe     the link speed and width come from NV2080_CTRL_CMD_BUS_GET_INFO_V2, which
//            answers on a monolithic build as well; the error counters come from the
//            firmware's PCIe block, because BUS_INFO returns them in a format the RM
//            documents only as "see the PCIe spec".
//
//   Fan      from the shared block. The open SDK's ctrl2080fan.h is empty in 615 -- the
//            FAN interface is not part of the published headers any more -- so the block
//            is the only way. Notebook boards leave it at zero; the EC owns the fan.
//
//   ECC      from the shared block, and only on parts that have ECC. A consumer Turing
//            board reports nothing here, which is why the flag exists.

// 2 added the PCIe link, the fan, the memory error counters, the engine utilization the
// firmware keeps beside the clocks, and the per-profile clock table. Everything new is
// appended, so a reader built against 1 that only looks at its own prefix still works;
// the version is bumped because the structure it copies is now larger.
#define NVRM_TELEMETRY_ABI_VERSION 2u
#define NVRM_TELEMETRY_FN "NVRMGpuTelemetry"

// The clock table: NV2080_CTRL_PERF_GET_LEVEL_INFO_V2 answers for three profiles
// (standard, conservative, aggressive), each with its own current/min/max per domain.
// One entry per profile, in that order.
#define NVRM_TELEMETRY_MAX_LEVELS 3u
#define NVRM_TELEMETRY_MAX_FANS   4u

// Upper bound on the sensors one query returns. Turing boards report one to four
// (GPU edge, memory, board, power supply); 8 leaves room without growing the struct
// past what a stack copy should carry.
#define NVRM_TELEMETRY_MAX_SENSORS 8u

// The sensor's target, as NV2080_CTRL_THERMAL_SYSTEM_TARGET_* reports it. Kept as
// plain numbers so user space can decode the struct without the RM headers.
#define NVRM_TELEMETRY_TARGET_NONE         0x00000000u
#define NVRM_TELEMETRY_TARGET_GPU          0x00000001u
#define NVRM_TELEMETRY_TARGET_MEMORY       0x00000002u
#define NVRM_TELEMETRY_TARGET_POWER_SUPPLY 0x00000004u
#define NVRM_TELEMETRY_TARGET_BOARD        0x00000008u
#define NVRM_TELEMETRY_TARGET_UNKNOWN      0xffffffffu

struct NVRMTelemetrySensor {
    unsigned int target;     // NVRM_TELEMETRY_TARGET_*
    int          value;      // degrees Celsius
    int          minimum;    // the range the firmware reports for this sensor
    int          maximum;
};

// One performance profile's clocks, in MHz. current is what the profile would run at
// now, minimum and maximum the band the RM allows for it.
struct NVRMTelemetryLevel {
    unsigned int level;        // 0 standard, 1 conservative, 2 aggressive
    unsigned int graphicsMhz;
    unsigned int graphicsMinMhz;
    unsigned int graphicsMaxMhz;
    unsigned int memoryMhz;
    unsigned int memoryMinMhz;
    unsigned int memoryMaxMhz;
};

struct NVRMTelemetry {
    unsigned int version;    // NVRM_TELEMETRY_ABI_VERSION

    // What the query actually managed to read. A zero flag means the field below is
    // not a reading, and a consumer must not print it as one.
    unsigned int valid;      // NVRM_TELEMETRY_VALID_*

    // Power, milliwatts. Set only when the firmware meters it; see the note at the top.
    unsigned int powerMw;        // total GPU power, milliwatts
    unsigned int powerLimitMw;   // the enforced cap, milliwatts (0 when not reported)
    unsigned int gpuPowerMw;     // average GPU rail
    unsigned int memoryPowerMw;  // average memory rail

    // Clocks, in MHz. currentFreq is what the performance level runs at now;
    // maxFreq is the ceiling that level allows, which is how a cap shows up.
    unsigned int clockGraphicsMhz;
    unsigned int clockMemoryMhz;
    unsigned int clockVideoMhz;
    unsigned int clockSmMhz;
    unsigned int clockGraphicsMaxMhz;
    unsigned int clockMemoryMaxMhz;

    // P-state, as NV2080_CTRL_PERF_PSTATES_* reports it (P0 = 0x1, P12 = 0x1000).
    unsigned int pstate;

    // Why the clock is held down, as the shared block reports it. Zero with the flag clear
    // means "not tracked", not "nothing throttling".
    unsigned int throttleMask;

    // The firmware's own device utilization, percentages x100, so a consumer can get
    // utilization and sensors from one call. Duplicates the utilization ring on purpose.
    unsigned int utilGr, utilFb;

    unsigned int sensorCount;
    struct NVRMTelemetrySensor sensors[NVRM_TELEMETRY_MAX_SENSORS];

    // ---- v2 ---------------------------------------------------------------------
    //
    // Instantaneous power. The firmware's average lags by a second or more, so a monitor
    // watching a load ramp wants this one when the part meters it. Zero with the flag
    // clear means the same as everywhere else: not tracked on this board.
    unsigned int instPowerMw;

    // The PCIe link, decoded from NV2080_CTRL_CMD_BUS_GET_INFO_V2. Gen is 1..6, width is
    // the lane count. "max" is what the slot and the card agree they could do, "cur" what
    // they are doing, so a link that fell back to x8 or Gen1 shows up as a gap.
    unsigned int pcieMaxGen, pcieMaxWidth, pcieCurGen, pcieCurWidth;

    // Link error counters, from the firmware's own PCIe block. They only move when
    // something is wrong with the slot, the riser or the cable, which is exactly what
    // makes them worth publishing on a machine whose GPU sits on a flex cable.
    unsigned int pcieLinecodeErrors, pcieCrcErrors, pcieNaks;
    unsigned int pcieL0sExits, pcieCorrectableErrors;
    unsigned int pcieNonfatalErrors, pcieFatalErrors, pcieUnsupportedRequests;

    // Fan, revolutions per minute. Laptop boards usually answer zero because the fan
    // belongs to the EC, not the GPU.
    unsigned int fanRpm, fanCount;

    // Engine utilization, whole percent as the firmware samples it (the GR/FB pair above
    // is the utilization ring's x100 encoding and is a different source). A part whose
    // firmware does not sample utilization leaves these at zero with the flag clear.
    unsigned int utilMemory, utilEnc, utilDec, utilJpg, utilOfa;

    // Memory error counters. Consumer parts report no ECC at all, so these stay clear
    // unless the card actually has it.
    unsigned int eccCorrectedVolatile, eccCorrectedAggregate;
    unsigned int eccUncorrectedVolatile, eccUncorrectedAggregate;

    // Row remapping: how many rows the card has retired and whether it is about to.
    unsigned int rowRemapCorrectable, rowRemapUncorrectable, rowRemapPending;

    // The clock table, one entry per profile, in the order NVRM_TELEMETRY_MAX_LEVELS
    // documents.
    unsigned int levelCount;
    struct NVRMTelemetryLevel levels[NVRM_TELEMETRY_MAX_LEVELS];

    unsigned long long timestamp;  // nanoseconds, from the kernel's continuous clock
};

#define NVRM_TELEMETRY_VALID_POWER        0x00000001u
#define NVRM_TELEMETRY_VALID_POWER_LIMIT  0x00000002u
#define NVRM_TELEMETRY_VALID_CLOCK        0x00000004u
#define NVRM_TELEMETRY_VALID_PSTATE       0x00000008u
#define NVRM_TELEMETRY_VALID_THROTTLE     0x00000010u
#define NVRM_TELEMETRY_VALID_SENSORS      0x00000020u
#define NVRM_TELEMETRY_VALID_UTIL         0x00000040u
// v2
#define NVRM_TELEMETRY_VALID_INST_POWER   0x00000080u
#define NVRM_TELEMETRY_VALID_PCIE         0x00000100u
#define NVRM_TELEMETRY_VALID_PCIE_ERRORS  0x00000200u
#define NVRM_TELEMETRY_VALID_FAN          0x00000400u
#define NVRM_TELEMETRY_VALID_RUSD_UTIL    0x00000800u
#define NVRM_TELEMETRY_VALID_ECC          0x00001000u
#define NVRM_TELEMETRY_VALID_ROW_REMAP    0x00002000u
#define NVRM_TELEMETRY_VALID_LEVELS       0x00004000u

// RUSD_CLK_THROTTLE_REASON_*, repeated here so user space needs no RM headers.
#define NVRM_TELEMETRY_THROTTLE_GPU_IDLE                 0x00000001u
#define NVRM_TELEMETRY_THROTTLE_APPLICATION_CLOCK        0x00000002u
#define NVRM_TELEMETRY_THROTTLE_SW_POWER_CAP             0x00000004u
#define NVRM_TELEMETRY_THROTTLE_HW_SLOWDOWN              0x00000008u
#define NVRM_TELEMETRY_THROTTLE_SYNC_BOOST               0x00000010u
#define NVRM_TELEMETRY_THROTTLE_SW_THERMAL_SLOWDOWN      0x00000020u
#define NVRM_TELEMETRY_THROTTLE_HW_THERMAL_SLOWDOWN      0x00000040u
#define NVRM_TELEMETRY_THROTTLE_HW_POWER_BRAKES_SLOWDOWN 0x00000080u
#define NVRM_TELEMETRY_THROTTLE_DISPLAY_CLOCK            0x00000100u
#define NVRM_TELEMETRY_THROTTLE_BOARD_LIMIT              0x00000200u
#define NVRM_TELEMETRY_THROTTLE_RELIABILITY              0x00000400u
