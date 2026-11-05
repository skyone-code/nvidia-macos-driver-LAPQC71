/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

// The readings the firmware writes into the GPU's shared telemetry block, read without
// an RM call. See nvrm_telemetry_rusd.c for the protocol and for why this is a C file.
//
// Kept separate from nvrm_telemetry_abi.h because the two reach the same information
// different ways: that one asks the RM through control calls, this one reads memory the
// firmware already filled. A build may have either, both, or neither.

// Sensor types, matching RUSD_TEMPERATURE_SENSOR_* so the caller can map a slot to the
// same name the thermal control uses.
#define NVRM_RUSD_SENSOR_GPU          0
#define NVRM_RUSD_SENSOR_MEMORY       1
#define NVRM_RUSD_SENSOR_BOARD        2
#define NVRM_RUSD_SENSOR_POWER_SUPPLY 3
#define NVRM_RUSD_SENSOR_HBM          4
#define NVRM_RUSD_MAX_SENSORS         8u
#define NVRM_RUSD_MAX_FANS            4u

// Which groups came back with fresh data.
#define NVRM_RUSD_POWER        0x0001
#define NVRM_RUSD_POWER_SPLIT  0x0002
#define NVRM_RUSD_POWER_LIMIT  0x0004
#define NVRM_RUSD_CLOCK        0x0008
#define NVRM_RUSD_PSTATE       0x0010
#define NVRM_RUSD_THROTTLE     0x0020
#define NVRM_RUSD_SENSORS      0x0040
#define NVRM_RUSD_FAN          0x0080
// Added with ABI 2.
#define NVRM_RUSD_INST_POWER   0x0100
#define NVRM_RUSD_UTIL         0x0200
#define NVRM_RUSD_ECC          0x0400
#define NVRM_RUSD_ROW_REMAP    0x0800
#define NVRM_RUSD_PCI          0x1000

struct NVRMTelemetryRusdSensor {
    unsigned int type;   // NVRM_RUSD_SENSOR_*
    int          value;  // degrees Celsius
};

struct NVRMTelemetryRusd {
    // Power, milliwatts. Any of these can be zero because the part does not meter it;
    // the NVRM_RUSD_* bits in the return value say which ones are real readings.
    unsigned int powerMw;
    unsigned int gpuPowerMw;
    unsigned int memoryPowerMw;
    unsigned int modulePowerMw;
    unsigned int powerLimitMw;
    unsigned int powerRequestedMw;

    // Clocks. *Target is what the performance level asked for; the plain ones are what
    // the part is actually running at. A target above the actual means the part is
    // holding itself back, and throttleMask says why.
    unsigned int clockGraphicsMhz;
    unsigned int clockMemoryMhz;
    unsigned int clockVideoMhz;
    unsigned int clockSmMhz;
    unsigned int clockGraphicsTargetMhz;
    unsigned int clockMemoryTargetMhz;

    unsigned int pstate;
    unsigned int throttleMask;

    // Instantaneous power, milliwatts: what the board is drawing right now, as opposed to
    // the averages above. Unlike tgpmW this one is filled on parts whose policy block is
    // not, so it is read separately and reported with its own bit.
    unsigned int instPowerMw;

    // Device utilization the firmware sampled: percentages x100, plus one entry per video
    // engine in the order NVENC, NVDEC, NVJPG, NVOFA.
    unsigned int utilGpu, utilMemory;
    unsigned int utilVideo[4];

    // Memory error counters, volatile and aggregate, in the order corrected-volatile,
    // corrected-aggregate, uncorrected-volatile, uncorrected-aggregate.
    unsigned int ecc[4];

    unsigned int rowRemapCorrectable, rowRemapUncorrectable;
    unsigned int rowRemapPending;

    // PCIe link error counters, in the order the block defines them: line code, CRC,
    // NAKs received, failed L0s exits, correctable, non-fatal, fatal, unsupported.
    unsigned int pciErrors[8];
    unsigned int pciGenInfo;

    unsigned int sensorCount;
    struct NVRMTelemetryRusdSensor sensor[NVRM_RUSD_MAX_SENSORS];

    unsigned int fanRpm[NVRM_RUSD_MAX_FANS];

    // The poll mask the RM is running with, so a caller can tell "the firmware does not
    // track power" from "we did not ask for power".
    unsigned int polledMask;
};

#ifdef __cplusplus
extern "C" {
#endif

// Whether the shared block is mapped at all. False means RM did not create it, which is
// the case on a virtual GPU or when CPU confidential computing is on.
int nvrm_rusd_present(void);

// The mask currently polled, 0 when nothing is.
int nvrm_rusd_polled_mask(void);

// Log each field's timestamp once, so a missing key can be traced to "the firmware does
// not track this" rather than guessed at. Diagnostic only.
void nvrm_rusd_dump_stamps(void);

// Fill *out from the shared block. nowNs is the kernel's continuous clock (the firmware
// stamps with the same one); toleranceNs is how old a sample may be and still count.
// Returns the NVRM_RUSD_* bits for the groups that were fresh.
int nvrm_rusd_read(struct NVRMTelemetryRusd *out, unsigned long long nowNs,
                   unsigned long long toleranceNs);

#ifdef __cplusplus
}
#endif
