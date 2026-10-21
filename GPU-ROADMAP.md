# Rendering iOS on the Adreno 610

iOS never drives a GPU directly: CoreAnimation draws through Metal, and Metal loads the
iPhone X's driver (`AGXMetalA11`), which expects Apple's kernel interfaces (IOGPU) and compiles
shaders to Apple's ISA. None of that exists on this phone, so CoreAnimation falls back to its CPU
renderer, which tops out around 17 fps here (`NOTES.md`, frame timing of 2026-10-21). For iOS to
render on the Adreno, three layers have to exist. This file is the plan; `NOTES.md` is the log.

## 1. Kernel: a GPU iOS can use

| Step | State |
| --- | --- |
| Power-up, zap shader through TrustZone, SQE microcode, ring, CP commands | done (patch 0145) |
| Park the GPU cleanly (GBIF drain before collapse) | done |
| 2D blitter: fills and copies, an image on the panel | done |
| GPU virtual memory: stage-1 page tables in the GPU's SMMU context bank | done (`gpu_mmu=1`): scattered memory above 4 GB filled right |
| Map any IOSurface / user memory into the GPU's address space | written: surface windows (`gpu_service=1`), first consumer the frame copy (`sweetfb_gpucopy=1`) |
| A GPU service with one submission path and a lock | written (`gpu_service=1`) |
| Interrupts (SPI 177) and fences instead of polling | |
| A GPU service user space can open: buffer objects, command submission, waits | |
| Per-process address spaces (TTBR switch from the ring, as msm does) | |

## 2. A Metal driver for the Adreno

Metal looks for an IOService with a `MetalPluginName` and loads that bundle from
`/System/Library/Extensions`. A bundle of our own, loaded in place of `AGXMetalA11`, would
implement the device, command queues, buffers, textures, render and blit encoders on top of
layer 1, building Adreno command streams the way Mesa's turnip (Vulkan) does. CoreAnimation uses
a limited subset: textured quads, blending, blits, a few filters (blur), YUV.

## 3. Shaders

Metal's shaders arrive as AIR (Apple's LLVM IR) in metallibs. The plausible road to the Adreno
ISA reuses Mesa's compiler: AIR → SPIR-V → NIR → ir3, with Apple's intrinsics lowered on the way.

## Scale

Layer 1 is weeks of work; layers 2 and 3 are the size of a large open-source GPU project. Each
step is verified on the phone before the next, with the sources in this tree's AOSP checkouts
(Mesa in `infinity/external/mesa3d`, Linux 6.18 in `tools/wsl-kernel`) as the reference.
