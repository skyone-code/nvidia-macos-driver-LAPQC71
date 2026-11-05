/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nv-xnu.h"
#include "nvrm_utilization.h"
#include "nvrm_telemetry.h"
#include "nvrm_telemetry_rusd.h"
#include <IOKit/IOLib.h>

extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nvmisc.h"
#include "nv-kernel-rmapi-ops.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080thermal.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "class/cl00de.h"

void NV_API_CALL rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd);
}

#define FBILOG(fmt, ...) kprintf("NVRM-fbinfo: " fmt "\n", ##__VA_ARGS__)

// How old a sample from the shared block may be and still be published. The RM polls at
// 500 ms by default and the firmware's own timer runs on that order, so five seconds
// means "the firmware stopped updating" rather than "we caught it between samples".
static const unsigned long long kNvRusdToleranceNs = 5000000000ULL;

// The groups worth polling. Clocks, throttle, utilization and pstate come from PERF,
// the power fields from POWER, the sensors from THERMAL, the error counters from
// MEMORY, the link counters from PCI and the fans from FAN. Per-process utilization
// is the one group left out: it costs the firmware a walk of every client in the
// system and nothing here consumes it.
static const unsigned long long kNvRusdWantMask =
    NV00DE_RUSD_POLL_CLOCK | NV00DE_RUSD_POLL_PERF | NV00DE_RUSD_POLL_POWER |
    NV00DE_RUSD_POLL_THERMAL | NV00DE_RUSD_POLL_MEMORY | NV00DE_RUSD_POLL_PCI |
    NV00DE_RUSD_POLL_FAN;

// The shared block numbers its sensors the same way the thermal control does, but with
// its own enum. Translate so both paths land on the same published key.
static unsigned int rusdSensorTarget(unsigned int type)
{
    switch (type) {
        case NVRM_RUSD_SENSOR_GPU:          return NVRM_TELEMETRY_TARGET_GPU;
        case NVRM_RUSD_SENSOR_MEMORY:       return NVRM_TELEMETRY_TARGET_MEMORY;
        case NVRM_RUSD_SENSOR_BOARD:        return NVRM_TELEMETRY_TARGET_BOARD;
        case NVRM_RUSD_SENSOR_POWER_SUPPLY: return NVRM_TELEMETRY_TARGET_POWER_SUPPLY;
        default:                            return NVRM_TELEMETRY_TARGET_UNKNOWN;
    }
}

enum {
    kClsRootClient   = 0x00000041u,
    kClsDevice0      = 0x00000080u,
    kClsSubdevice0   = 0x00002080u,
    kCmdGpuAttachIds = 0x00000215u,
    kCmdFbGetInfoV2  = 0x20801303u,
    kCmdMcGetArch    = 0x20801701u,   /* NV2080_CTRL_CMD_MC_GET_ARCH_INFO */
    kCmdClassListV2  = 0x00800292u,   /* NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2 */
    kClassListMax    = 200u,
    kFbIdxRamSize    = 0x00000007u,
    kFbIdxTotalRam   = 0x00000008u,
    kGpuInvalidId    = 0xffffffffu,
    kMaxAttachedGpus = 32u
};

static inline void fbiZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

struct FbiAttachIds { NvU32 gpuIds[kMaxAttachedGpus]; NvU32 failedId; };

struct FbiInfo     { NvU32 index; NvU32 data; };
struct FbiArch     { NvU32 architecture; NvU32 implementation; NvU32 revision; NvU8 subRevision; };
struct FbiClasses  { NvU32 numClasses; NvU32 classList[kClassListMax]; };
struct FbiGetInfo  { NvU32 fbInfoListSize; struct FbiInfo fbInfoList[0x80]; };

static NvV32 fbiAlloc(NvU32 hRoot, NvU32 hParent, NvU32 hClass, void *pParams, NvU32 size, NvU32 *hOut)
{
    nvidia_kernel_rmapi_ops_t ops;
    fbiZero(&ops, sizeof ops);
    ops.op = NV04_ALLOC;
    ops.params.alloc.hRoot            = hRoot;
    ops.params.alloc.hObjectParent    = hParent;
    ops.params.alloc.hObjectNew       = 0;
    ops.params.alloc.hClass           = (NvV32)hClass;
    ops.params.alloc.pAllocParms      = (NvP64)(NvUPtr)pParams;
    ops.params.alloc.pRightsRequested = (NvP64)0;
    ops.params.alloc.paramsSize       = size;
    ops.params.alloc.flags            = NVOS64_FLAGS_NONE;
    rm_kernel_rmapi_op(NULL, &ops);
    if (ops.params.alloc.status == 0 && hOut) *hOut = ops.params.alloc.hObjectNew;
    return ops.params.alloc.status;
}

static NvV32 fbiControl(NvU32 hClient, NvU32 hObject, NvU32 cmd, void *pParams, NvU32 size)
{
    nvidia_kernel_rmapi_ops_t ops;
    fbiZero(&ops, sizeof ops);
    ops.op = NV04_CONTROL;
    ops.params.control.hClient    = hClient;
    ops.params.control.hObject    = hObject;
    ops.params.control.cmd        = cmd;
    ops.params.control.params     = (NvP64)(NvUPtr)pParams;
    ops.params.control.paramsSize = size;
    rm_kernel_rmapi_op(NULL, &ops);
    return ops.params.control.status;
}

static NvV32 fbiFree(NvU32 hRoot, NvU32 hParent, NvU32 hObject)
{
    nvidia_kernel_rmapi_ops_t ops;
    fbiZero(&ops, sizeof ops);
    ops.op = NV01_FREE;
    ops.params.free.hRoot         = hRoot;
    ops.params.free.hObjectParent = hParent;
    ops.params.free.hObjectOld    = hObject;
    rm_kernel_rmapi_op(NULL, &ops);
    return ops.params.free.status;
}

struct NvrmUtilizationState {
    NvU32 hClient, hDevice, hSub;
    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS ring;
};

static void utilizationClose(NvrmUtilizationState *state)
{
    // Freeing the root recursively releases the device and subdevice as well.
    if (state->hClient) fbiFree(state->hClient, state->hClient, state->hClient);
    state->hClient = state->hDevice = state->hSub = 0;
}

extern "C" void nvrm_free_utilization(void **opaque)
{
    if (!opaque || !*opaque) return;
    NvrmUtilizationState *state = (NvrmUtilizationState *)*opaque;
    utilizationClose(state);
    IOFree(state, sizeof(*state));
    *opaque = nullptr;
}

extern "C" bool nvrm_query_utilization(unsigned int gpuId, void **opaque, struct NVRMUtilization *sample)
{
    if (!opaque || !sample) return false;
    NvrmUtilizationState *state = (NvrmUtilizationState *)*opaque;
    if (!state) {
        // The PERFMON ring is about 56 KB; it must never be put on a kernel stack.
        state = (NvrmUtilizationState *)IOMalloc(sizeof(*state));
        if (!state) return false;
        fbiZero(state, sizeof(*state));
        *opaque = state;
    }

    if (!state->hSub) {
        FbiAttachIds at;
        NV0080_ALLOC_PARAMETERS dp;
        NV2080_ALLOC_PARAMETERS sp;
        NvV32 st = fbiAlloc(0, 0, kClsRootClient, nullptr, 0, &state->hClient);
        if (st != 0 || !state->hClient) goto fail;
        fbiZero(&at, sizeof at);
        at.gpuIds[0] = gpuId;
        at.gpuIds[1] = kGpuInvalidId;
        st = fbiControl(state->hClient, state->hClient, kCmdGpuAttachIds, &at, sizeof at);
        if (st != 0) goto fail;
        fbiZero(&dp, sizeof dp);
        dp.deviceId = 0;
        dp.hClientShare = state->hClient;
        st = fbiAlloc(state->hClient, state->hClient, kClsDevice0, &dp, sizeof dp, &state->hDevice);
        if (st != 0 || !state->hDevice) goto fail;
        fbiZero(&sp, sizeof sp);
        st = fbiAlloc(state->hClient, state->hDevice, kClsSubdevice0, &sp, sizeof sp, &state->hSub);
        if (st != 0 || !state->hSub) goto fail;
    }

    fbiZero(&state->ring, sizeof(state->ring));
    state->ring.type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
    state->ring.bufSize = sizeof(state->ring.samples);
    if (fbiControl(state->hClient, state->hSub,
                   NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2,
                   &state->ring, sizeof(state->ring)) != 0) return false;
    return nvrmUtilizationLatestSample(state->ring, sample);

fail:
    // Do not leave a partially allocated client behind after setup fails.
    utilizationClose(state);
    return false;
}

extern "C" bool nvrm_query_vram_kb(NvU32 gpuId, NvU32 *pRamKb, NvU32 *pTotalKb, NvU32 *pStatus)
{
    NvU32 hClient = 0, hDevice = 0, hSub = 0;
    NvV32 st = 0;
    bool  ok = false;
    struct FbiAttachIds   at;
    NV0080_ALLOC_PARAMETERS dp;
    NV2080_ALLOC_PARAMETERS sp;
    struct FbiGetInfo     fp;

    if (pRamKb)   *pRamKb   = 0;
    if (pTotalKb) *pTotalKb = 0;
    if (pStatus)  *pStatus  = 0xffffffffu;

    do {
        st = fbiAlloc(0, 0, kClsRootClient, NULL, 0, &hClient);
        if (st != 0 || !hClient) { FBILOG("NV01_ROOT_CLIENT -> status 0x%x", (unsigned)st); break; }

        fbiZero(&at, sizeof at);
        at.gpuIds[0] = gpuId;
        at.gpuIds[1] = kGpuInvalidId;
        st = fbiControl(hClient, hClient, kCmdGpuAttachIds, &at, sizeof at);
        if (st != 0) { FBILOG("GPU_ATTACH_IDS(0x%08x) -> status 0x%x (failedId 0x%x)",
                              (unsigned)gpuId, (unsigned)st, (unsigned)at.failedId); break; }

        fbiZero(&dp, sizeof dp);
        dp.deviceId = 0; dp.hClientShare = hClient;
        st = fbiAlloc(hClient, hClient, kClsDevice0, &dp, sizeof dp, &hDevice);
        if (st != 0 || !hDevice) { FBILOG("NV01_DEVICE_0 -> status 0x%x", (unsigned)st); break; }

        fbiZero(&sp, sizeof sp);
        sp.subDeviceId = 0;
        st = fbiAlloc(hClient, hDevice, kClsSubdevice0, &sp, sizeof sp, &hSub);
        if (st != 0 || !hSub) { FBILOG("NV20_SUBDEVICE_0 -> status 0x%x", (unsigned)st); break; }

        fbiZero(&fp, sizeof fp);
        fp.fbInfoListSize = 2;
        fp.fbInfoList[0].index = kFbIdxRamSize;
        fp.fbInfoList[1].index = kFbIdxTotalRam;
        st = fbiControl(hClient, hSub, kCmdFbGetInfoV2, &fp, sizeof fp);
        if (st != 0) { FBILOG("FB_GET_INFO_V2 -> status 0x%x", (unsigned)st); break; }

        if (pRamKb)   *pRamKb   = fp.fbInfoList[0].data;
        if (pTotalKb) *pTotalKb = fp.fbInfoList[1].data;
        FBILOG("gpu 0x%08x: RAM_SIZE %u KB, TOTAL_RAM_SIZE %u KB",
               (unsigned)gpuId, (unsigned)fp.fbInfoList[0].data, (unsigned)fp.fbInfoList[1].data);
        ok = (fp.fbInfoList[0].data != 0);
    } while (0);

    if (pStatus) *pStatus = (NvU32)st;

    if (hClient) {
        NvV32 fst = fbiFree(hClient, hClient, hClient);
        if (fst != 0) FBILOG("freeing the root client -> status 0x%x", (unsigned)fst);
    }
    return ok;
}

/*
 * The chip's identity and engine classes, read from the RM the way NVIDIA's own clients choose their code path: the
 * architecture from MC_GET_ARCH_INFO, and for each engine type the newest class the RM lists for this GPU (3D xx97,
 * compute xxC0, copy xxB5, video encode xxB7, decode xxB0). Everything chip-specific in the macOS layer (copy engine,
 * scan-out model, video libraries, native shaders) is chosen from this one answer instead of a device-id range.
 */
extern "C" bool nvrm_query_gpu_profile(NvU32 gpuId, NvU32 *pArch, NvU32 *pImpl, NvU32 *pCls3d, NvU32 *pClsCompute,
                                       NvU32 *pClsCopy, NvU32 *pClsEnc, NvU32 *pClsDec, NvU32 *pStatus)
{
    NvU32 hClient = 0, hDevice = 0, hSub = 0;
    NvV32 st = 0;
    bool  ok = false;
    struct FbiAttachIds   at;
    NV0080_ALLOC_PARAMETERS dp;
    NV2080_ALLOC_PARAMETERS sp;
    struct FbiArch        ap;
    static struct FbiClasses cl;   /* 804 bytes: off the kernel stack; callers run one at a time (attach) */

    *pArch = *pImpl = *pCls3d = *pClsCompute = *pClsCopy = *pClsEnc = *pClsDec = 0;
    if (pStatus) *pStatus = 0xffffffffu;
    do {
        st = fbiAlloc(0, 0, kClsRootClient, NULL, 0, &hClient);
        if (st != 0 || !hClient) { FBILOG("profile: NV01_ROOT_CLIENT -> status 0x%x", (unsigned)st); break; }
        fbiZero(&at, sizeof at);
        at.gpuIds[0] = gpuId;
        at.gpuIds[1] = kGpuInvalidId;
        st = fbiControl(hClient, hClient, kCmdGpuAttachIds, &at, sizeof at);
        if (st != 0) { FBILOG("profile: GPU_ATTACH_IDS -> status 0x%x", (unsigned)st); break; }
        fbiZero(&dp, sizeof dp);
        dp.deviceId = 0; dp.hClientShare = hClient;
        st = fbiAlloc(hClient, hClient, kClsDevice0, &dp, sizeof dp, &hDevice);
        if (st != 0 || !hDevice) { FBILOG("profile: NV01_DEVICE_0 -> status 0x%x", (unsigned)st); break; }
        fbiZero(&sp, sizeof sp);
        st = fbiAlloc(hClient, hDevice, kClsSubdevice0, &sp, sizeof sp, &hSub);
        if (st != 0 || !hSub) { FBILOG("profile: NV20_SUBDEVICE_0 -> status 0x%x", (unsigned)st); break; }

        fbiZero(&ap, sizeof ap);
        st = fbiControl(hClient, hSub, kCmdMcGetArch, &ap, sizeof ap);
        if (st != 0) { FBILOG("profile: MC_GET_ARCH_INFO -> status 0x%x", (unsigned)st); break; }
        *pArch = ap.architecture; *pImpl = ap.implementation;

        fbiZero(&cl, sizeof cl);
        st = fbiControl(hClient, hDevice, kCmdClassListV2, &cl, sizeof cl);
        if (st != 0) { FBILOG("profile: GET_CLASSLIST_V2 -> status 0x%x", (unsigned)st); break; }
        for (NvU32 i = 0; i < cl.numClasses && i < kClassListMax; i++) {
            const NvU32 c = cl.classList[i];
            if (c < 0xC000 || c > 0xFFFF) continue;   /* Turing (0xC5xx) and later; older compat classes are not ours */
            NvU32 *slot = nullptr;
            switch (c & 0xFF) {
                case 0x97: slot = pCls3d; break;
                case 0xC0: slot = pClsCompute; break;
                case 0xB5: slot = pClsCopy; break;
                case 0xB7: slot = pClsEnc; break;
                case 0xB0: slot = pClsDec; break;
            }
            if (slot && c > *slot) *slot = c;
        }
        FBILOG("profile: gpu 0x%08x arch 0x%x impl 0x%x 3d 0x%x compute 0x%x copy 0x%x enc 0x%x dec 0x%x (%u classes)",
               (unsigned)gpuId, (unsigned)ap.architecture, (unsigned)ap.implementation, (unsigned)*pCls3d,
               (unsigned)*pClsCompute, (unsigned)*pClsCopy, (unsigned)*pClsEnc, (unsigned)*pClsDec, (unsigned)cl.numClasses);
        ok = ap.architecture != 0;
    } while (0);
    if (pStatus) *pStatus = (NvU32)st;
    if (hClient) {
        NvV32 fst = fbiFree(hClient, hClient, hClient);
        if (fst != 0) FBILOG("profile: freeing the root client -> status 0x%x", (unsigned)fst);
    }
    return ok;
}

/*
 * Thermal sensors and clocks.
 *
 * The sensors go through NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2: the RM exposes one
 * ioctl that carries up to 32 instructions, so a whole sweep costs one control call
 * instead of one per sensor. Each sensor needs three instructions to be useful (which
 * target it watches, the range the firmware claims, and the reading), so the sweep is
 * issued three sensors at a time and stays inside the 32-instruction limit whatever the
 * board reports.
 *
 * Clocks are attempted and, on this build, refused -- see nvrm_telemetry_abi.h for the
 * whole story. Asking anyway is what lets one code path serve a GSP build too.
 */
struct NvrmTelemetryState {
    NvU32 hClient, hDevice, hSub;
    // 32 instructions at 44 bytes each plus the header: too much to put on a kernel
    // stack next to the RM call frames, so it lives with the client handles.
    NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_V2_PARAMS thermal;
    // The bus query carries its answer inline (52 entries), for the same reason.
    NV2080_CTRL_BUS_GET_INFO_V2_PARAMS bus;
    bool rusdAsked;
    bool busLogged;
};

static void telemetryClose(NvrmTelemetryState *state)
{
    // Freeing the root recursively releases the device and subdevice as well.
    if (state->hClient) fbiFree(state->hClient, state->hClient, state->hClient);
    state->hClient = state->hDevice = state->hSub = 0;
}

extern "C" void nvrm_free_telemetry(void **opaque)
{
    if (!opaque || !*opaque) return;
    NvrmTelemetryState *state = (NvrmTelemetryState *)*opaque;
    telemetryClose(state);
    IOFree(state, sizeof(*state));
    *opaque = nullptr;
}

// Start a batch of `count` instructions. The instruction array itself is cleared by the
// caller filling it in; only the header fields are reset here.
static void telThermalInit(NvrmTelemetryState *state, NvU32 count)
{
    state->thermal.clientAPIVersion        = THERMAL_SYSTEM_API_VER;
    state->thermal.clientAPIRevision       = THERMAL_SYSTEM_API_REV;
    state->thermal.clientInstructionSizeOf = sizeof(NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION);
    state->thermal.executeFlags            = NV2080_CTRL_THERMAL_SYSTEM_EXECUTE_FLAGS_DEFAULT;
    state->thermal.successfulInstructions  = 0;
    state->thermal.instructionListSize     = count;
}

static void telThermalSet(NvrmTelemetryState *state, NvU32 idx, NvU32 opcode, NvU32 sensorIndex)
{
    NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *ins = &state->thermal.instructionList[idx];
    fbiZero(ins, sizeof(*ins));
    ins->opcode = opcode;
    // The operand layouts differ per opcode but all of them start with the sensor index,
    // so writing it through the union's first word is correct for every opcode used here.
    ins->operands.space[0] = sensorIndex;
}

// The thermal sweep. One batch: ask how many sensors exist, then three instructions per
// sensor for a window of them.
static void telReadSensors(NvrmTelemetryState *state, struct NVRMTelemetry *out)
{
    const NvU32 perSensor = 3u;
    const NvU32 maxWindow = (NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION_MAX_COUNT - 1u) / perSensor;

    // Probe: how many sensors does this board report?
    fbiZero(&state->thermal, sizeof(state->thermal));
    telThermalInit(state, 1);
    telThermalSet(state, 0, NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSORS_AVAILABLE_OPCODE, 0);
    if (fbiControl(state->hClient, state->hSub, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2,
                   &state->thermal, sizeof(state->thermal)) != 0) return;
    if (!state->thermal.instructionList[0].executed ||
        state->thermal.instructionList[0].result != 0) return;

    const NvU32 available =
        state->thermal.instructionList[0].operands.getInfoSensorsAvailable.availableSensors;
    static NvU32 loggedAvailable = 0xffffffffu;
    if (available != loggedAvailable) {
        loggedAvailable = available;
        FBILOG("sensors: the RM reports %u", (unsigned)available);
    }
    if (!available) return;

    for (NvU32 first = 0; first < available && out->sensorCount < NVRM_TELEMETRY_MAX_SENSORS;
         first += maxWindow) {
        NvU32 window = available - first;
        if (window > maxWindow) window = maxWindow;

        fbiZero(&state->thermal, sizeof(state->thermal));
        telThermalInit(state, 1u + window * perSensor);
        telThermalSet(state, 0, NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSORS_AVAILABLE_OPCODE, 0);
        for (NvU32 i = 0; i < window; i++) {
            const NvU32 base = 1u + i * perSensor;
            const NvU32 sensor = first + i;
            telThermalSet(state, base + 0,
                          NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSOR_TARGET_OPCODE, sensor);
            telThermalSet(state, base + 1,
                          NV2080_CTRL_THERMAL_SYSTEM_GET_INFO_SENSOR_READING_RANGE_OPCODE, sensor);
            telThermalSet(state, base + 2,
                          NV2080_CTRL_THERMAL_SYSTEM_GET_STATUS_SENSOR_READING_OPCODE, sensor);
        }
        if (fbiControl(state->hClient, state->hSub, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2,
                       &state->thermal, sizeof(state->thermal)) != 0) return;

        for (NvU32 i = 0; i < window; i++) {
            const NvU32 base = 1u + i * perSensor;
            const NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *reading = &state->thermal.instructionList[base + 2];
            if (!reading->executed || reading->result != 0) continue;
            if (out->sensorCount >= NVRM_TELEMETRY_MAX_SENSORS) break;

            struct NVRMTelemetrySensor *s = &out->sensors[out->sensorCount];
            s->target  = NVRM_TELEMETRY_TARGET_UNKNOWN;
            s->minimum = 0;
            s->maximum = 0;
            s->value   = (int)reading->operands.getStatusSensorReading.value;

            // The target arrives as an index into the board's target table. The RM can
            // translate it (GET_INFO_TARGET_TYPE), but that is one more instruction per
            // sensor for a field the consumer only uses to label a row. The index order
            // the firmware reports is stable and is what every board seen so far uses,
            // so it is mapped directly and anything past it is left unknown.
            const NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *tgt = &state->thermal.instructionList[base + 0];
            if (tgt->executed && tgt->result == 0) {
                switch (tgt->operands.getInfoSensorTarget.targetIndex) {
                    case 0:  s->target = NVRM_TELEMETRY_TARGET_GPU; break;
                    case 1:  s->target = NVRM_TELEMETRY_TARGET_MEMORY; break;
                    case 2:  s->target = NVRM_TELEMETRY_TARGET_BOARD; break;
                    case 3:  s->target = NVRM_TELEMETRY_TARGET_POWER_SUPPLY; break;
                    default: break;
                }
            }

            const NV2080_CTRL_THERMAL_SYSTEM_INSTRUCTION *rng = &state->thermal.instructionList[base + 1];
            if (rng->executed && rng->result == 0) {
                s->minimum = (int)rng->operands.getInfoSensorReadingRange.minimum;
                s->maximum = (int)rng->operands.getInfoSensorReadingRange.maximum;
            }
            out->sensorCount++;
        }
    }
}

// Clocks. Both commands answer NV_ERR_NOT_SUPPORTED on a monolithic build; asking anyway
// keeps a single code path for both build flavours.
static void telReadClocks(NvrmTelemetryState *state, struct NVRMTelemetry *out)
{
    // 32 clock domains at 24 bytes each: static, for the same stack reason as the
    // thermal instruction array.
    static NV2080_CTRL_PERF_GET_LEVEL_INFO_V2_PARAMS level;

    static bool logged = false;

    fbiZero(&level, sizeof(level));
    level.level = NV2080_CTRL_PERF_GET_LEVEL_INFO_LEVEL_STANDARD;
    level.perfGetClkInfoListSize = NV2080_CTRL_PERF_CLK_MAX_DOMAINS;
    const NvV32 levelStatus = fbiControl(state->hClient, state->hSub,
                                         NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2,
                                         &level, sizeof(level));
    if (!logged) {
        logged = true;
        FBILOG("clocks: GET_LEVEL_INFO_V2 -> status 0x%x, %u entries", (unsigned)levelStatus,
               (unsigned)level.perfGetClkInfoListSize);
        for (NvU32 i = 0; i < level.perfGetClkInfoListSize && i < NV2080_CTRL_PERF_CLK_MAX_DOMAINS; i++) {
            FBILOG("clocks:   [%u] domain %u flags 0x%x cur %u def %u min %u max %u", (unsigned)i,
                   (unsigned)level.perfGetClkInfoList[i].domain,
                   (unsigned)level.perfGetClkInfoList[i].flags,
                   (unsigned)level.perfGetClkInfoList[i].currentFreq,
                   (unsigned)level.perfGetClkInfoList[i].defaultFreq,
                   (unsigned)level.perfGetClkInfoList[i].minFreq,
                   (unsigned)level.perfGetClkInfoList[i].maxFreq);
        }
    }
    if (levelStatus == 0) {
        NvU32 found = 0;
        NvU32 n = level.perfGetClkInfoListSize;
        if (n > NV2080_CTRL_PERF_CLK_MAX_DOMAINS) n = NV2080_CTRL_PERF_CLK_MAX_DOMAINS;
        for (NvU32 i = 0; i < n; i++) {
            const NV2080_CTRL_PERF_GET_CLK_INFO &c = level.perfGetClkInfoList[i];
            switch (c.domain) {
                case NVRM_CLK_DOMAIN_GRAPHICS:
                    out->clockGraphicsMhz    = c.currentFreq;
                    out->clockGraphicsMaxMhz = c.maxFreq;
                    found++;
                    break;
                case NVRM_CLK_DOMAIN_MEMORY:
                    out->clockMemoryMhz    = c.currentFreq;
                    out->clockMemoryMaxMhz = c.maxFreq;
                    found++;
                    break;
                case NVRM_CLK_DOMAIN_VIDEO:
                    out->clockVideoMhz = c.currentFreq;
                    found++;
                    break;
                case NVRM_CLK_DOMAIN_SM:
                    out->clockSmMhz = c.currentFreq;
                    found++;
                    break;
                default:
                    break;
            }
        }
        if (found) out->valid |= NVRM_TELEMETRY_VALID_CLOCK;
    }

    NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS ps;
    fbiZero(&ps, sizeof(ps));
    if (fbiControl(state->hClient, state->hSub, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE,
                   &ps, sizeof(ps)) == 0) {
        out->pstate = ps.currPstate;
        out->valid |= NVRM_TELEMETRY_VALID_PSTATE;
    }
}

// PCIe link. BUS_GET_INFO_V2 answers inline, so one call covers both registers, and it
// works on a monolithic build where the per-domain clock calls do not.
static void telReadPcie(NvrmTelemetryState *state, struct NVRMTelemetry *out)
{
    enum { kIdxLinkCaps = 0x03u, kIdxLinkStatus = 0x07u, kIdxCount = 2u };

    fbiZero(&state->bus, sizeof(state->bus));
    state->bus.busInfoListSize = kIdxCount;
    state->bus.busInfoList[0].index = kIdxLinkCaps;
    state->bus.busInfoList[1].index = kIdxLinkStatus;

    const NvV32 st = fbiControl(state->hClient, state->hSub, NV2080_CTRL_CMD_BUS_GET_INFO_V2,
                                &state->bus, sizeof(state->bus));
    if (!state->busLogged) {
        state->busLogged = true;
        FBILOG("pcie: BUS_GET_INFO_V2 -> status 0x%x caps 0x%08x status 0x%08x", (unsigned)st,
               (unsigned)state->bus.busInfoList[0].data, (unsigned)state->bus.busInfoList[1].data);
    }
    if (st != 0) return;

    // Link Capabilities follows the PCIe register layout for its low 12 bits: speed code
    // in 3:0 (1 = 2.5 GT/s, so Gen1), lanes in 9:4. Link Status says what the link
    // actually trained to: speed code in 19:16, lanes in 25:20.
    const NvU32 caps = state->bus.busInfoList[0].data;
    const NvU32 stat = state->bus.busInfoList[1].data;
    out->pcieMaxGen   = caps & 0xfu;
    out->pcieMaxWidth = (caps >> 4) & 0x3fu;
    out->pcieCurGen   = (stat >> 16) & 0xfu;
    out->pcieCurWidth = (stat >> 20) & 0x3fu;
    if (out->pcieMaxGen || out->pcieCurGen) out->valid |= NVRM_TELEMETRY_VALID_PCIE;
}

// The clock table: one call per profile, each answering with every domain the RM knows.
// Only graphics and memory are kept -- the video and SM domains carry the same numbers as
// the live clocks above on this family, and three rows of two columns is what a monitor
// can show.
static void telReadLevels(NvrmTelemetryState *state, struct NVRMTelemetry *out)
{
    static NV2080_CTRL_PERF_GET_LEVEL_INFO_V2_PARAMS level;
    static bool logged = false;

    for (NvU32 profile = 0; profile < NVRM_TELEMETRY_MAX_LEVELS; profile++) {
        fbiZero(&level, sizeof(level));
        level.level = profile;
        level.perfGetClkInfoListSize = NV2080_CTRL_PERF_CLK_MAX_DOMAINS;
        const NvV32 st = fbiControl(state->hClient, state->hSub,
                                    NV2080_CTRL_CMD_PERF_GET_LEVEL_INFO_V2,
                                    &level, sizeof(level));
        if (!logged) {
            FBILOG("levels: profile %u -> status 0x%x, %u domains", (unsigned)profile,
                   (unsigned)st, (unsigned)level.perfGetClkInfoListSize);
        }
        if (st != 0) continue;

        struct NVRMTelemetryLevel *e = &out->levels[out->levelCount];
        e->level = profile;
        NvU32 n = level.perfGetClkInfoListSize;
        if (n > NV2080_CTRL_PERF_CLK_MAX_DOMAINS) n = NV2080_CTRL_PERF_CLK_MAX_DOMAINS;
        bool any = false;
        for (NvU32 i = 0; i < n; i++) {
            const NV2080_CTRL_PERF_GET_CLK_INFO &c = level.perfGetClkInfoList[i];
            if (c.domain == NVRM_CLK_DOMAIN_GRAPHICS) {
                e->graphicsMhz = c.currentFreq; e->graphicsMinMhz = c.minFreq; e->graphicsMaxMhz = c.maxFreq;
                any = true;
            } else if (c.domain == NVRM_CLK_DOMAIN_MEMORY) {
                e->memoryMhz = c.currentFreq; e->memoryMinMhz = c.minFreq; e->memoryMaxMhz = c.maxFreq;
                any = true;
            }
        }
        if (any) {
            out->levelCount++;
            out->valid |= NVRM_TELEMETRY_VALID_LEVELS;
        }
    }
    logged = true;
}

// Framebuffer occupancy. The shared block does not carry it, so this is a control call:
// FB_GET_INFO_V2 answers inline with a list of (index, value) pairs, all in kilobytes.
// used = heap - free (every user allocation the RM tracks); total prefers TOTAL_RAM_SIZE
// (the whole 8 GB, matching what the box label says) and falls back to the heap size.
// The params struct is ~KB of stack with the full list, so it is static, like the clock
// table's -- the telemetry poll is a single serialized timer path.
static void telReadVram(NvrmTelemetryState *state, struct NVRMTelemetry *out)
{
    static NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb;
    static bool logged = false;

    fbiZero(&fb, sizeof(fb));
    enum { kIdxTotalRam = 0, kIdxHeapSize = 1, kIdxHeapFree = 2, kIdxCount = 3 };
    fb.fbInfoListSize = kIdxCount;
    fb.fbInfoList[kIdxTotalRam].index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE; // 0x08
    fb.fbInfoList[kIdxHeapSize].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE;      // 0x09
    fb.fbInfoList[kIdxHeapFree].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;      // 0x16

    const NvV32 st = fbiControl(state->hClient, state->hSub, NV2080_CTRL_CMD_FB_GET_INFO_V2,
                                &fb, sizeof(fb));
    if (!logged) {
        logged = true;
        FBILOG("vram: FB_GET_INFO_V2 -> status 0x%x, total %u KB, heap %u KB, free %u KB",
               (unsigned)st, (unsigned)fb.fbInfoList[kIdxTotalRam].data,
               (unsigned)fb.fbInfoList[kIdxHeapSize].data,
               (unsigned)fb.fbInfoList[kIdxHeapFree].data);
    }
    if (st != 0) return;

    const unsigned totalKb = fb.fbInfoList[kIdxTotalRam].data;
    const unsigned heapKb  = fb.fbInfoList[kIdxHeapSize].data;
    const unsigned freeKb  = fb.fbInfoList[kIdxHeapFree].data;
    // A heap of zero or a free larger than the heap means the call answered garbage;
    // keep the flag clear rather than publish nonsense.
    if (!heapKb || freeKb > heapKb) return;

    out->vramUsedBytes  = (unsigned long long)(heapKb - freeKb) * 1024u;
    out->vramTotalBytes = (unsigned long long)(totalKb ? totalKb : heapKb) * 1024u;
    if (out->vramUsedBytes) out->valid |= NVRM_TELEMETRY_VALID_VRAM;
}

extern "C" bool nvrm_query_telemetry(unsigned int gpuId, void **opaque, struct NVRMTelemetry *out){
    if (!opaque || !out) return false;
    fbiZero(out, sizeof(*out));
    out->version = NVRM_TELEMETRY_ABI_VERSION;

    // Declared here rather than at first use: the fail path jumps over the control-call
    // setup, and a jump past an initialisation is not allowed.
    struct NVRMTelemetryRusd rusd;
    fbiZero(&rusd, sizeof rusd);
    int rusdGot = 0;

    NvrmTelemetryState *state = (NvrmTelemetryState *)*opaque;
    if (!state) {
        state = (NvrmTelemetryState *)IOMalloc(sizeof(*state));
        if (!state) return false;
        fbiZero(state, sizeof(*state));
        *opaque = state;
    }

    if (!state->hSub) {
        FbiAttachIds at;
        NV0080_ALLOC_PARAMETERS dp;
        NV2080_ALLOC_PARAMETERS sp;
        NvV32 st = fbiAlloc(0, 0, kClsRootClient, nullptr, 0, &state->hClient);
        if (st != 0 || !state->hClient) goto fail;
        fbiZero(&at, sizeof at);
        at.gpuIds[0] = gpuId;
        at.gpuIds[1] = kGpuInvalidId;
        st = fbiControl(state->hClient, state->hClient, kCmdGpuAttachIds, &at, sizeof at);
        if (st != 0) goto fail;
        fbiZero(&dp, sizeof dp);
        dp.deviceId = 0;
        dp.hClientShare = state->hClient;
        st = fbiAlloc(state->hClient, state->hClient, kClsDevice0, &dp, sizeof dp, &state->hDevice);
        if (st != 0 || !state->hDevice) goto fail;
        fbiZero(&sp, sizeof sp);
        sp.subDeviceId = 0;
        st = fbiAlloc(state->hClient, state->hDevice, kClsSubdevice0, &sp, sizeof sp, &state->hSub);
        if (st != 0 || !state->hSub) goto fail;
    }

    // The shared block first: the firmware already sampled it, so this costs a memory
    // copy rather than a control call, and it is the only source for power and for the
    // clocks the part is actually running at. Whatever it cannot fill, the control calls
    // below try instead -- on a part whose firmware does not track a field, the control
    // call is the only way to get it, and the other way round.
    // Ask the RM to keep the shared block filled. The firmware only writes the polled
    // fields while a mask is set, and by default nothing is: the RM turns polling on for
    // its own clients, not for a kext that only reads memory. A permanent mask survives
    // this client and is applied again on resume, which is what a monitor wants.
    // Failure is not fatal -- the block may already be polled by something else, and the
    // fields the firmware tracks regardless still read.
    if (!state->rusdAsked && state->hSub) {
        state->rusdAsked = true;
        NV2080_CTRL_GPU_RUSD_SET_FEATURES_PARAMS feats;
        fbiZero(&feats, sizeof feats);
        feats.permanentPolledDataMask = kNvRusdWantMask;
        const NvV32 fst = fbiControl(state->hClient, state->hSub,
                                     NV2080_CTRL_CMD_GPU_RUSD_SET_FEATURES, &feats, sizeof feats);
        if (fst != 0) FBILOG("rusd: SET_FEATURES -> 0x%x (polling stays as it is)", (unsigned)fst);
    }

    rusdGot = nvrm_rusd_read(&rusd, mach_continuous_time(), kNvRusdToleranceNs);
    {
        static int loggedGot = -1;
        if (rusdGot != loggedGot) {
            loggedGot = rusdGot;
            FBILOG("rusd: groups 0x%x, polled mask 0x%x, power %u mW, limit %u mW, "
                   "clk gfx %u MHz mem %u MHz, pstate %u, throttle 0x%x",
                   (unsigned)rusdGot, rusd.polledMask, rusd.powerMw, rusd.powerLimitMw,
                   rusd.clockGraphicsMhz, rusd.clockMemoryMhz, rusd.pstate, rusd.throttleMask);
            // Which fields the firmware actually tracks on this part. A stamp of 0 means
            // "not available here", which is a legitimate answer and the reason a key can
            // be missing from the published dictionary.
            nvrm_rusd_dump_stamps();
        }
    }

    if (rusdGot & NVRM_RUSD_POWER)       { out->powerMw = rusd.powerMw; out->valid |= NVRM_TELEMETRY_VALID_POWER; }
    if (rusdGot & NVRM_RUSD_POWER_SPLIT) { out->gpuPowerMw = rusd.gpuPowerMw; out->memoryPowerMw = rusd.memoryPowerMw; }
    if (rusdGot & NVRM_RUSD_POWER_LIMIT) { out->powerLimitMw = rusd.powerLimitMw; out->valid |= NVRM_TELEMETRY_VALID_POWER_LIMIT; }
    if (rusdGot & NVRM_RUSD_THROTTLE)    { out->throttleMask = rusd.throttleMask; out->valid |= NVRM_TELEMETRY_VALID_THROTTLE; }

    if (rusdGot & NVRM_RUSD_INST_POWER) {
        out->instPowerMw = rusd.instPowerMw;
        out->valid |= NVRM_TELEMETRY_VALID_INST_POWER;
    }

    if (rusdGot & NVRM_RUSD_UTIL) {
        out->utilMemory = rusd.utilMemory;
        out->utilEnc = rusd.utilVideo[0];
        out->utilDec = rusd.utilVideo[1];
        out->utilJpg = rusd.utilVideo[2];
        out->utilOfa = rusd.utilVideo[3];
        out->valid |= NVRM_TELEMETRY_VALID_RUSD_UTIL;
    }

    if (rusdGot & NVRM_RUSD_FAN) {
        unsigned count = 0;
        for (unsigned i = 0; i < NVRM_TELEMETRY_MAX_FANS; i++) {
            if (!rusd.fanRpm[i]) continue;
            if (!count) out->fanRpm = rusd.fanRpm[i];
            count++;
        }
        out->fanCount = count;
        if (count) out->valid |= NVRM_TELEMETRY_VALID_FAN;
    }

    if (rusdGot & NVRM_RUSD_ECC) {
        out->eccCorrectedVolatile   = rusd.ecc[0];
        out->eccCorrectedAggregate  = rusd.ecc[1];
        out->eccUncorrectedVolatile = rusd.ecc[2];
        out->eccUncorrectedAggregate = rusd.ecc[3];
        out->valid |= NVRM_TELEMETRY_VALID_ECC;
    }

    if (rusdGot & NVRM_RUSD_ROW_REMAP) {
        out->rowRemapCorrectable   = rusd.rowRemapCorrectable;
        out->rowRemapUncorrectable = rusd.rowRemapUncorrectable;
        out->rowRemapPending       = rusd.rowRemapPending;
        out->valid |= NVRM_TELEMETRY_VALID_ROW_REMAP;
    }

    if (rusdGot & NVRM_RUSD_PCI) {
        out->pcieLinecodeErrors     = rusd.pciErrors[0];
        out->pcieCrcErrors          = rusd.pciErrors[1];
        out->pcieNaks               = rusd.pciErrors[2];
        out->pcieL0sExits           = rusd.pciErrors[3];
        out->pcieCorrectableErrors  = rusd.pciErrors[4];
        out->pcieNonfatalErrors     = rusd.pciErrors[5];
        out->pcieFatalErrors        = rusd.pciErrors[6];
        out->pcieUnsupportedRequests = rusd.pciErrors[7];
        out->valid |= NVRM_TELEMETRY_VALID_PCIE_ERRORS;
    }

    if (rusdGot & NVRM_RUSD_SENSORS) {
        for (unsigned i = 0; i < rusd.sensorCount && i < NVRM_TELEMETRY_MAX_SENSORS; i++) {
            out->sensors[out->sensorCount].target = rusdSensorTarget(rusd.sensor[i].type);
            out->sensors[out->sensorCount].value = rusd.sensor[i].value;
            out->sensors[out->sensorCount].minimum = 0;
            out->sensors[out->sensorCount].maximum = 0;
            out->sensorCount++;
        }
        if (out->sensorCount) out->valid |= NVRM_TELEMETRY_VALID_SENSORS;
    }

    // The control calls: sensors the firmware did not hand over, and the clocks if the
    // firmware does not track them (the usual answer on a monolithic build).
    if (!out->sensorCount) {
        telReadSensors(state, out);
        if (out->sensorCount) out->valid |= NVRM_TELEMETRY_VALID_SENSORS;
    }

    if (rusdGot & NVRM_RUSD_CLOCK) {
        out->clockGraphicsMhz    = rusd.clockGraphicsMhz;
        out->clockMemoryMhz      = rusd.clockMemoryMhz;
        out->clockVideoMhz       = rusd.clockVideoMhz;
        out->clockSmMhz          = rusd.clockSmMhz;
        out->clockGraphicsMaxMhz = rusd.clockGraphicsTargetMhz;
        out->clockMemoryMaxMhz   = rusd.clockMemoryTargetMhz;
        out->valid |= NVRM_TELEMETRY_VALID_CLOCK;
    } else {
        telReadClocks(state, out);
    }

    if (rusdGot & NVRM_RUSD_PSTATE) {
        out->pstate = rusd.pstate;
        out->valid |= NVRM_TELEMETRY_VALID_PSTATE;
    }

    // The two that always come from control calls: the link registers, and the clock
    // table for the three profiles. Neither is in the shared block in a decodable form
    // (the block keeps raw PCIe counters only), so they never overlap with the reads
    // above.
    telReadPcie(state, out);
    telReadLevels(state, out);
    telReadVram(state, out);

    out->timestamp = mach_continuous_time();
    return out->valid != 0;

fail:
    // Do not leave a partially allocated client behind after setup fails.
    telemetryClose(state);
    return false;
}
