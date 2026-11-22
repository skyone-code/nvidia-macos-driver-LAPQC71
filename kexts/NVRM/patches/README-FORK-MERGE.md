# Retained fork changes for upstream merges

Performance candidates from the same fork (AIR hash reuse, WindowServer image pool,
convolution diagnostics, small-BAR encoder headroom) are covered separately in
`docs/FORK-PERF-MERGE.md`; their source patches are archived under `docs/fork-merge/`.
That directory is a staging list, not part of the ogkm patch chain.

## Hardware vblank after partial modesets, 2026-11-22

Candidate: retain `620-vblank-restore-unchanged-heads.patch`, taken from
HighDelay/nvidia-macos-driver-nvk (commit `0f49ac8`, 2026-11-17). NVIDIA 620.70.06
disables RG semaphore interrupts on every API head before applying a committed
modeset, but the unchanged-head early return skips re-enabling them, so the
framebuffer keeps a registered callback that stops firing and falls back to its
software timer.

Anchor verified in this tree: `ApplyProposedModeSetStateOneApiHeadPreUpdate()`
line 3268, `if (!pProposedApiHead->changed) { return; }` line ~3292,
`DisableVBlankRgSemaphoreInterruptsForModeset()` 3205,
`EnableVBlankRgSemaphoreInterruptOneApiHead()` 3228, update point 3384.
Same NVIDIA version (620.70.06), so the patch should apply with `--fuzz=0`.

Relevance here: the LAPQC71 B-scene log shows `hw vblank on head 0: REFUSED by
NVKMS - trying again after 2 latched flips` and `hw vblank on head 1: deferred
until NVKMS latches a flip` never resolving. Not yet proven to be our break, but
it is the only patch in this family we do not already carry.

Upstream caveat: the trial machine (RTX 2060 SUPER) reports this interrupt
method unsupported, so the repaired path itself is still unqualified on hardware.

## Already carried from the same patch family

`610-display-completion`, `610-setmode-reason`, `615-viewport-scaling`,
`610-backlight-register-log`, `610-fsp-timer-double-destroy`, plus our own
`620-yuv420-hw` (general port of upstream PR #459) and `620-color-range`.
Not carried, evaluated as not applicable: `620-dvi-rgb-scanout` (digital DVI and
rotation; HDMI here).
