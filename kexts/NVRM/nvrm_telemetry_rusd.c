/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Reading the GPU's own telemetry block.
 *
 * The card keeps a shared structure (NV00DE_SHARED_DATA) that its firmware fills with
 * the readings macOS has never been able to show: board power, the clocks each domain
 * is running at, why the clock is being held down, fan speed. RM maps it and hands the
 * firmware the physical address; from then on the firmware writes into it on its own
 * schedule and this driver only reads.
 *
 * Why it lives in a C file rather than beside the rest of the kext: nv.h and nv-priv.h
 * do not compile as C++ (an anonymous bit-field with a const qualifier, and a forward
 * reference to an enum). The other RM-touching files here -- nvrm_pageoff.c,
 * nvrm_kapi_fields.c -- are C for the same reason.
 *
 * Reading rules, from the block's own header (class/cl00de.h):
 *
 *   - Every field is a timestamp followed by the data. Read the timestamp, copy the
 *     data, read the timestamp again; if the two agree the copy is not torn.
 *   - A timestamp of RUSD_TIMESTAMP_INVALID (0) means "not available on this platform".
 *     That is the normal answer for a field the firmware does not track, not an error.
 *   - A timestamp at or above RUSD_SEQ_START is a sequence number for data the driver
 *     writes itself (non-polled), not a clock reading.
 *
 * Which fields have anything in them is decided by the firmware and by the poll mask,
 * so every read here can legitimately come back empty. The caller decides what to
 * publish; nothing in this file invents a zero.
 */

#include <nv.h>
#include <nv-priv.h>
#include "gpu/gpu.h"
#include "kernel/gpu/gpu_shared_data_map.h"
#include "class/cl00de.h"

#include "nvrm_telemetry_rusd.h"

#include <IOKit/IOLib.h>

#define FBILOG_RUSD(fmt, ...) IOLog("NVRM-rusd: " fmt "\n", ##__VA_ARGS__)

// The one entry point this file needs from the kext proper: the live nv_state_t. Declared
// here rather than pulled in through a header so the C compiler sees exactly one
// signature, the same one NVRM.cpp defines.
extern nv_state_t *nv_xnu_gpu_state(void);

// The map the RM allocated for this GPU, or NULL when the firmware did not get one.
// pMapBuffer is the kernel mapping; the struct beside it is the fallback the RM itself
// uses when the mapping is absent (gpushareddataWriteStart_INTERNAL does the same).
static NV00DE_SHARED_DATA *rusdMap(nv_state_t *nv)
{
    OBJGPU *pGpu = NV_GET_NV_PRIV_PGPU(nv);
    if (!pGpu) return NULL;
    if (pGpu->userSharedData.pMapBuffer) return (NV00DE_SHARED_DATA *)pGpu->userSharedData.pMapBuffer;
    return &pGpu->userSharedData.data;
}

// A polled field is stale when it was never written or when the write is older than the
// caller's tolerance. Timestamps are the kernel's own nanosecond clock, the same one the
// firmware is given, so no conversion is needed.
static int rusdFresh(NvU64 stamp, NvU64 now, NvU64 toleranceNs)
{
    if (stamp == RUSD_TIMESTAMP_INVALID) return 0;              // not tracked on this part
    if (stamp >= (NvU64)RUSD_SEQ_START) return !(stamp & 0x1ull); // driver-written, valid if closed
    if (stamp > now) return 1;                                    // clock skew; trust the write
    return (now - stamp) <= toleranceNs;
}

int nvrm_rusd_present(void)
{
    return rusdMap(nv_xnu_gpu_state()) != NULL;
}

int nvrm_rusd_polled_mask(void)
{
    nv_state_t *nv = nv_xnu_gpu_state();
    OBJGPU *pGpu = nv ? NV_GET_NV_PRIV_PGPU(nv) : NULL;
    return pGpu ? (int)(pGpu->userSharedData.lastPolledDataMask & 0xffull) : 0;
}

int nvrm_rusd_read(struct NVRMTelemetryRusd *out, unsigned long long nowNs,
                   unsigned long long toleranceNs)
{
    if (!out) return 0;
    nv_state_t *nv = nv_xnu_gpu_state();
    NV00DE_SHARED_DATA *d = rusdMap(nv);
    if (!d) return 0;

    const NvU64 now = (NvU64)nowNs;
    const NvU64 tol = (NvU64)toleranceNs;

    // Copy one field out under the timestamp protocol, and report whether it was fresh.
    // The macro needs the destination to carry its own lastModifiedTimestamp, which is
    // why each of these copies into a full RUSD_* struct rather than into the payload.
    RUSD_CLK_PUBLIC_DOMAIN_INFOS clk;
    RUSD_CLK_THROTTLE_INFO       thr;
    RUSD_POWER_LIMITS            lim;
    RUSD_AVG_POWER_USAGE         avg;
    RUSD_INST_POWER_USAGE        inst;
    RUSD_POWER_POLICY_STATUS     pol;
    RUSD_PERF_CURRENT_PSTATE     pst;
    RUSD_PERF_DEVICE_UTILIZATION utl;
    RUSD_MEM_ECC                 ecc;
    RUSD_MEM_ROW_REMAP           row;
    RUSD_PCIE_DATA               pci;
    RUSD_TEMPERATURE             tmp[RUSD_TEMPERATURE_TYPE_MAX];
    RUSD_FAN_COOLER_STATUS       fan;

    int got = 0;

    // ---- power -------------------------------------------------------------------
    // Three separate sources, and a part may have none of them. tgpmW is the total the
    // board reports; the averages are the firmware's own split.
    if (RUSD_READ_DATA(d, powerPolicyStatus, &pol) && rusdFresh(pol.lastModifiedTimestamp, now, tol)) {
        out->powerMw = pol.info.tgpmW;
        got |= NVRM_RUSD_POWER;
    }
    if (RUSD_READ_DATA(d, avgPowerUsage, &avg) && rusdFresh(avg.lastModifiedTimestamp, now, tol)) {
        out->gpuPowerMw = avg.info.averageGpuPower;
        out->memoryPowerMw = avg.info.averageMemoryPower;
        out->modulePowerMw = avg.info.averageModulePower;
        got |= NVRM_RUSD_POWER_SPLIT;
    }
    if (RUSD_READ_DATA(d, powerLimitGpu, &lim) && rusdFresh(lim.lastModifiedTimestamp, now, tol)) {
        out->powerLimitMw = lim.info.enforcedmW;
        out->powerRequestedMw = lim.info.requestedmW;
        got |= NVRM_RUSD_POWER_LIMIT;
    }
    // The instantaneous reading lives in its own block and is filled on parts whose power
    // policy block is not, so it is a separate answer rather than a fallback.
    if (RUSD_READ_DATA(d, instPowerUsage, &inst) && rusdFresh(inst.lastModifiedTimestamp, now, tol)) {
        out->instPowerMw = inst.info.instGpuPower ? inst.info.instGpuPower : inst.info.instModulePower;
        if (out->instPowerMw) got |= NVRM_RUSD_INST_POWER;
    }

    // ---- clocks ------------------------------------------------------------------
    // Domains are GRAPHICS, MEMORY, VIDEO, SM. actualClkKHz is what the part is running
    // at; targetClkMHz is what the performance level asked for, so a gap between them is
    // the part holding itself back.
    if (RUSD_READ_DATA(d, clkPublicDomainInfos, &clk) && rusdFresh(clk.lastModifiedTimestamp, now, tol)) {
        out->clockGraphicsMhz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].actualClkKHz / 1000u;
        out->clockMemoryMhz   = clk.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].actualClkKHz / 1000u;
        out->clockVideoMhz    = clk.info[RUSD_CLK_PUBLIC_DOMAIN_VIDEO].actualClkKHz / 1000u;
        out->clockSmMhz       = clk.info[RUSD_CLK_PUBLIC_DOMAIN_SM].actualClkKHz / 1000u;
        out->clockGraphicsTargetMhz = clk.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].targetClkMHz;
        out->clockMemoryTargetMhz   = clk.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].targetClkMHz;
        got |= NVRM_RUSD_CLOCK;
    }

    if (RUSD_READ_DATA(d, perfCurrentPstate, &pst) && rusdFresh(pst.lastModifiedTimestamp, now, tol)) {
        out->pstate = pst.currentPstate;
        got |= NVRM_RUSD_PSTATE;
    }

    // ---- why the clock is held down ----------------------------------------------
    if (RUSD_READ_DATA(d, clkThrottleInfo, &thr) && rusdFresh(thr.lastModifiedTimestamp, now, tol)) {
        out->throttleMask = thr.reasonMask;
        got |= NVRM_RUSD_THROTTLE;
    }

    // ---- utilization -------------------------------------------------------------
    // The firmware's own sample of the device and of the video engines. The GR/FB pair
    // next door comes from the utilization ring, which is a different measurement taken
    // on a different schedule; publishing both lets a consumer pick.
    if (RUSD_READ_DATA(d, perfDevUtil, &utl) && rusdFresh(utl.lastModifiedTimestamp, now, tol)) {
        out->utilGpu = utl.info.gpuPercentBusy;
        out->utilMemory = utl.info.memoryPercentBusy;
        for (unsigned i = 0; i < 4 && i < RUSD_ENG_UTILIZATION_COUNT; i++)
            out->utilVideo[i] = utl.info.engUtil[i].clkPercentBusy;
        got |= NVRM_RUSD_UTIL;
    }

    // ---- memory health -----------------------------------------------------------
    // Only meaningful on a part with ECC, which consumer boards do not have: the firmware
    // leaves the timestamp at zero there and nothing is reported.
    if (RUSD_READ_DATA(d, memEcc, &ecc) && rusdFresh(ecc.lastModifiedTimestamp, now, tol)) {
        /* The block keeps a row per error type; type 0 is the total the tools report. */
        const RUSD_MEM_ERROR_COUNTS *t = &ecc.count[RUSD_MEMORY_ERROR_TYPE_TOTAL];
        out->ecc[0] = (unsigned int)t->correctedVolatile;
        out->ecc[1] = (unsigned int)t->correctedAggregate;
        out->ecc[2] = (unsigned int)t->uncorrectedVolatile;
        out->ecc[3] = (unsigned int)t->uncorrectedAggregate;
        got |= NVRM_RUSD_ECC;
    }
    if (RUSD_READ_DATA(d, memRowRemap, &row) && rusdFresh(row.lastModifiedTimestamp, now, tol)) {
        out->rowRemapCorrectable = row.info.correctableRows;
        out->rowRemapUncorrectable = row.info.uncorrectableRows;
        out->rowRemapPending = row.info.isPending ? 1u : 0u;
        got |= NVRM_RUSD_ROW_REMAP;
    }

    // ---- PCIe link errors --------------------------------------------------------
    // Index 0 is the generation sketch the block keeps; the rest are counters, in the
    // order RUSD_BUS_DATA_* defines them. linecode/CRC/NAK are the ones a bad riser shows.
    if (RUSD_READ_DATA(d, pciBusData, &pci) && rusdFresh(pci.lastModifiedTimestamp, now, tol)) {
        out->pciGenInfo = pci.info.data[RUSD_BUS_DATA_PCIE_GEN_INFO];
        for (unsigned i = 0; i < 8; i++)
            out->pciErrors[i] = pci.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_LINECODE_ERRORS + i];
        got |= NVRM_RUSD_PCI;
    }

    // ---- temperatures ------------------------------------------------------------
    // The same sensors the thermal control reports, but without an RM call: the firmware
    // already sampled them.
    {
        int have = 0;
        for (unsigned i = 0; i < RUSD_TEMPERATURE_TYPE_MAX && i < NVRM_RUSD_MAX_SENSORS; i++) {
            if (!RUSD_READ_DATA(d, temperatures[i], &tmp[i])) continue;
            if (!rusdFresh(tmp[i].lastModifiedTimestamp, now, tol)) continue;
            out->sensor[i].type = i;
            // NvTemp is SFXP24_8: a signed fixed point number with 8 fractional bits, so
            // the integer part is the value shifted down by 8. Dividing here rather than
            // in user space keeps the ABI in whole degrees, which is all a temperature
            // reading is worth.
            out->sensor[i].value = (int)(tmp[i].temperature >> 8);
            have++;
        }
        if (have) {
            out->sensorCount = (unsigned)have;
            got |= NVRM_RUSD_SENSORS;
        }
    }

    // ---- fan ---------------------------------------------------------------------
    if (RUSD_READ_DATA(d, fanCoolerStatus, &fan) && rusdFresh(fan.lastModifiedTimestamp, now, tol)) {
        for (unsigned i = 0; i < NVRM_RUSD_MAX_FANS && i < RUSD_FAN_COOLER_MAX_COOLERS; i++) {
            out->fanRpm[i] = fan.info.rpmCurr[i];
        }
        got |= NVRM_RUSD_FAN;
    }

    out->polledMask = (unsigned)((nv && NV_GET_NV_PRIV_PGPU(nv)) ?
                                 NV_GET_NV_PRIV_PGPU(nv)->userSharedData.lastPolledDataMask : 0ull);
    return got;
}

// Diagnostic: every polled field's timestamp, in the order the block lays them out. A
// stamp of zero is the firmware saying it does not track that field on this part; a stamp
// that never moves is the firmware saying it tracks it but nothing has happened yet.
void nvrm_rusd_dump_stamps(void)
{
    nv_state_t *nv = nv_xnu_gpu_state();
    NV00DE_SHARED_DATA *d = rusdMap(nv);
    if (!d) { FBILOG_RUSD("shared block not mapped"); return; }

    FBILOG_RUSD("stamps: clk %llu thr %llu util %llu pstate %llu lim %llu "
                "temp[gpu %llu mem %llu brd %llu psu %llu hbm %llu] avg %llu inst %llu pol %llu fan %llu",
                (unsigned long long)d->clkPublicDomainInfos.lastModifiedTimestamp,
                (unsigned long long)d->clkThrottleInfo.lastModifiedTimestamp,
                (unsigned long long)d->perfDevUtil.lastModifiedTimestamp,
                (unsigned long long)d->perfCurrentPstate.lastModifiedTimestamp,
                (unsigned long long)d->powerLimitGpu.lastModifiedTimestamp,
                (unsigned long long)d->temperatures[RUSD_TEMPERATURE_SENSOR_GPU].lastModifiedTimestamp,
                (unsigned long long)d->temperatures[RUSD_TEMPERATURE_SENSOR_MEMORY].lastModifiedTimestamp,
                (unsigned long long)d->temperatures[RUSD_TEMPERATURE_SENSOR_BOARD].lastModifiedTimestamp,
                (unsigned long long)d->temperatures[RUSD_TEMPERATURE_SENSOR_POWER_SUPPLY].lastModifiedTimestamp,
                (unsigned long long)d->temperatures[RUSD_TEMPERATURE_TYPE_HBM].lastModifiedTimestamp,
                (unsigned long long)d->avgPowerUsage.lastModifiedTimestamp,
                (unsigned long long)d->instPowerUsage.lastModifiedTimestamp,
                (unsigned long long)d->powerPolicyStatus.lastModifiedTimestamp,
                (unsigned long long)d->fanCoolerStatus.lastModifiedTimestamp);
}
