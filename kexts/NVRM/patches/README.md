# NVIDIA 610 display completion archive

The framebuffer and accelerator allocation interface changes must be built and installed together. Use the NVRMFB framebuffer source and the matching NVRM headers from this revision.

`610-display-completion.patch` implements the mapped flip notifier query and resets the selected slot for each committed surface. NVIDIA modeset treats the BEGUN notifier state as the latched flip; FINISHED is also accepted. Invalid or unavailable notifier state keeps the transaction pending.

`610-setmode-reason.patch` applies after it. A refused mode set logged only "commit rejected" in release builds (the reason lines are debug-only), so a WindowServer that could not set its first mode stopped with no cause on record. The patch logs one warning with the set-mode status, the display status, and each head's status.

`615-viewport-scaling.patch` applies to `kapi/src/nvkms-kapi.c` of NVIDIA 615.78.08 (from PR #273 by Leaf-lsgtky, the finding and implementation by lsCoding666 in #221). Primary-layer scaling (`sizeIn != sizeOut`) is not offered on GPUs that scale at the head, so NVKMS refused every scaled desktop and HiDPI mode. The primary layer stays 1:1 and the desktop size becomes the head's viewport input, so the head's scaler fits it to the display's timings.

`610-backlight-register-log.patch` applies to `src/nvkms-rm.c`. A laptop panel driven by the GPU has no brightness control when NVKMS registers no backlight, and both inputs to that decision (the internal display mask and the brightness query) are answered by the GPU firmware with nothing on record. The patch logs the internal display status and masks, each display's brightness query status and level, and the case where none answered. No behaviour change.

With the original NVIDIA 610.57.04 Darwin modeset archive and compile commands already built, run:

```
python3 kexts/NVRM/build-completion-archive.py --vendor /build/vendor/src/nvidia-modeset --output /build/paired-completion
```

Link NVRM against the resulting `libnvkms.a`, not the original archive. The script preserves the original archive and replaces only the completion object and release build metadata. Build NVAccel with the matching allocation cookie interface, once for macOS 15 and once for macOS 26.

## RM boot-failure cleanup

`610-fsp-timer-double-destroy.patch` applies to the open-gpu-kernel-modules 610.57.04 tree (`git apply` from its root) before the RM objects are built. When the FSP boot commands fail, `kfspCleanupBootState` runs from both the failure path and `kfspStateDestroy`; the second `tmrEventDestroy` faulted on the freed OS timer (page fault at 0x1010 in `nv_destroy_nano_timer`). The patch destroys the poll event once and clears the OS timer handle after cancelling it.

`620-yuv420-hw.patch` applies to `kapi/src/nvkms-kapi.c` of NVIDIA 620.73.04 with the patches above already applied. HDMI 4K60 that RGB 4:4:4 cannot carry (a DisplayPort-to-HDMI adapter, or a TMDS link limited to HDMI 1.4 rates) was refused by NVKMS, so such a display stayed at 4K30. A mode refused as RGB is now listed, validated and set again with the YUV 4:2:0 preference when that retry is valid, decided as hardware 4:2:0, and has the same timing; the head is then set with `yuv420Mode` hardware. Modes RGB already accepts are untouched. Idea and first implementation by backdash756 (pull request 459, limited to one rig's handles); this one is general.

Applying the chain: every patch here applies with `--fuzz=0` to a pristine open-gpu-kernel-modules 620.73.04, in file-name order
(`610-` and `615-` patches keep their names). `610-display-completion.patch` was rewritten for 620 (its flip-pending function is
inserted before `FramebufferConsoleDisabled`, and the function table entry was added; the 620 header already declares
`getFlipPendingStatus`). `610-fsp-timer-double-destroy.patch` keeps only its `timer_ostimer.c` hunk: 620 already guards the
poll event in `kern_fsp.c`. `tools/test_patch_chain.sh` runs the whole chain against a pristine tree.

The check is `tools/test_patch_chain.py` (SKIP without a pristine tree) and an optional step of `build/build_nvrm.sh`:
`NVRM_PRISTINE_OGKM=<pristine 620.73.04 tree> bash build/build_nvrm.sh ...` stops before building if any patch no longer applies.

`630-head-hwstate-debug.patch` applies to `interface/nvkms-api.h`, `src/nvkms.c`, `kapi/interface/nvkms-kapi.h` and
`kapi/src/nvkms-kapi.c` of NVIDIA 620.73.04 with the patches above already applied. It adds one debug ioctl
(`NVKMS_IOCTL_QUERY_HEAD_HW_STATE`) that snapshots an api head's EVO state -- head active, connector driven, OR
assignment (type/protocol/mask/owner), hwHeadsMask, activeDpys, activeRmId, color format and display rate -- plus
two kapi function-table entries (`getHeadHwState`, `getEventCounters`) and kapi-side counters for the display
events that reach the event dispatcher (dpy-changed, dynamic-dpy-connected, flip-occurred). NVAccel exposes all of
it as `debug.nvaccel_hs_*` and `debug.nvaccel_evt_*` sysctls; `tools/nv-scene.sh` samples them as extra columns.
Purpose: with a dynamically-connected display, an accepted modeset with a frozen encoder looks identical whether
the head is inactive or active-but-never-programmed; the CRC query path refuses inactive heads, so a successful
CRC read plus a frozen output CRC already proves "active compositor, dead SOR" -- these counters pin down which
stage owns the break and whether disconnect events are being delivered at all (the unplug leak symptom).
