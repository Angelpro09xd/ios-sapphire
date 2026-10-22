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
| Map any IOSurface / user memory into the GPU's address space | done: every iOS frame copied to the panel by the GPU (`sweetfb_gpucopy=1`), 110 µs instead of 5 ms |
| A GPU service with one submission path and a lock | done (`gpu_service=1`) |
| Interrupts (SPI 177) and fences instead of polling | |
| A GPU service user space can open: buffer objects, command submission, waits | |
| Per-process address spaces (TTBR switch from the ring, as msm does) | |

## 2. A Metal driver for the Adreno

What the iOS 16.7.16 binaries say (dyld shared cache, `ipsw` 3.1.739):

- `getMetalPluginClassForService()` in Metal reads `MetalPluginName` from the accelerator service,
  loads `/System/Library/Extensions/<name>.bundle` with `[NSBundle bundleWithPath:]`, takes
  `MetalPluginClassName` with `classNamed:` (else `principalClass`), and **requires
  `[cls isSubclassOfClass:[_MTLDevice class]]`**; the device is `[[cls alloc] initWithAcceleratorPort:]`.
- Apple's driver for this GPU family has exactly that shape: `AGXPrincipalDevice` (entry point,
  `initWithAcceleratorPort:` only) → `AGXA11Device : AGXA11FamilyDevice : IOGPUMetalDevice :
  _MTLDevice`, with queues, command buffers, encoders, buffers and textures subclassing
  IOGPU.framework's `IOGPUMetal*` classes. IOGPU does the generic work and talks to the kernel
  (`IOGPUDeviceCreateWithAPIProperty(port, "Metal")`); AGX adds the GPU-specific encoding.
- `-[_MTLDevice initWithAcceleratorPort:]` is abstract; `IOGPUMetalDevice`'s does `[super init]`
  (a real `-[_MTLDevice init]`) and only then opens IOGPU. So a device of our own needs nothing of
  IOGPU to exist.
- The port already publishes an empty `IOAcceleratorES` (SweetFramebuffer.cpp); with `sweetmetal=1`
  (0147) it names `SapphireMetal` / `SapphireMTLDevice`.

| Step | State |
| --- | --- |
| A device that loads and logs every message iOS sends it (`metal/`, built on Linux with clang + ld64.lld, no SDK) | done: backboardd's compositor runs its frame loop on it; the full list of what it uses is in NOTES (2026-10-22) |
| From that list: queues, command buffers, buffers, textures, blits on layer 1's GPU service | |
| Render encoders and pipelines for what CoreAnimation draws | |

The layering choice ahead: subclass IOGPU's `IOGPUMetal*` classes (their resource and submission
machinery for free, at the price of implementing Apple's private IOGPU kernel interface), or
`_MTLDevice` and friends directly over a small interface of our own (less to match, more to
write). The logging device's trace is what decides it.

Metal looks for an IOService with a `MetalPluginName` and loads that bundle from
`/System/Library/Extensions`. A bundle of our own, loaded in place of `AGXMetalA11`, would
implement the device, command queues, buffers, textures, render and blit encoders on top of
layer 1, building Adreno command streams the way Mesa's turnip (Vulkan) does. CoreAnimation uses
a limited subset: textured quads, blending, blits, a few filters (blur), YUV.

## 3. Shaders

Metal's shaders arrive as AIR (Apple's LLVM IR) in metallibs. CoreAnimation's are in
`QuartzCore.framework/default.metallib`: **141 functions** (39 vertex, 95 fragment, 7 kernel; list
in `metal/quartzcore-functions.txt`, made with `metal/tools/metallib-list.py`), each in an `_lpf`
and an `_lph` (half) variant: `std_vert0/1` (textured and coloured quads: most of the UI), paths
(`path_rect`, `path_stroke_line/curve/round_join/miter_join`), blurs (`variable_blur`,
`single_pass_blur_3`, `downsample`/`upsample`), gradients (`radial_shading`,
`radial_complex_shading`), `read_surf`, the panel's APL/dimming kernels, and `tile_downsample_*`
(A11 tile shaders, used only if the GPU claims family Apple4). Precompiled pipelines for Apple's
GPUs are in `default.pipelinelib` (one file per GPU generation, 94 MB; nothing there runs on an
Adreno, but it names the pipelines).

**Ubuntu's LLVM 18 `llvm-dis` reads AIR**, wrapper and all: `std_vert0` disassembles to plain LLVM
IR (`position = MVP × position`, the 4×4 matrix in buffer 2, colour and texcoord passed through),
with metadata naming every input and output (`air.vertex_input` locations 0/1/3, `air.buffer`
indices 2/3, `air.vertex_output` names). So every shader CoreAnimation uses can be read exactly,
and either rewritten for the Adreno with the same interface, or translated mechanically
(LLVM IR → NIR → Mesa's ir3, which is in `infinity/external/mesa3d`): the shaders are small, and the
`air.*` intrinsics (sampling, interpolation) map onto NIR's.

**The a610's 3D state comes from turnip itself, not from guessing.** Mesa's drm-shim lets turnip
run on the server as this a610 (`metal/capture/`), and `FD_RD_DUMP=full` records every register,
packet and buffer of a submit. The first capture (clear, one textured quad blended source-over, a
blit) replays in the kernel unchanged (`gpu_3d=1`, `metal/tools/rd2replay.py`). Every piece the
real driver needs can be learned the same way, one Vulkan call at a time: what a texture
descriptor, a sampler, a blend state, a scissor or a draw costs in commands, on exactly this GPU,
written by a driver that already works on it.

## Scale

Layer 1 is weeks of work; layers 2 and 3 are the size of a large open-source GPU project. Each
step is verified on the phone before the next, with the sources in this tree's AOSP checkouts
(Mesa in `infinity/external/mesa3d`, Linux 6.18 in `tools/wsl-kernel`) as the reference.
