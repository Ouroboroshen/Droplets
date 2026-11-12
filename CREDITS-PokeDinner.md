# PokeDinner fixes in Droplet

Ported from PokeDinner (https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork, GPLv3), an Eglantine fork by Franseven, on 2026-11-03 (archie's Switch 2 mouse on 2026-11-05). Commits marked "from Eden-NS2" are Franseven's ports from the Eden-NS2 fork; "#4558"-style numbers are Eden pull requests. "In part" marks commits of which Droplet takes only some changes.


**999sian**

- vulkan: only force the guest subgroup size for warp-level compute

**archie**

- hid: Switch 2 mouse (Nmouse), so Joy-Con 2 mouse mode works

**BXYMartin**

- ns: Fix the Switch 2 GetBuiltInFontData request and reply layout
- host1x: Decode Switch 2 movies with frame threads
- ns: Document the Switch 2 built-in font type numbering
- shader_recompiler: Accept any cache hint and RZ in local spills
- shader_recompiler: Lower LDC.IS and more I2F source forms
- shader_recompiler: Merge the I2F lowering paths

**Dee1**

- audio_core: Unregister audio in/out managers before they are destroyed
- audio_core: Keep audio in/out managers alive while their sessions are open

**Franseven**

- kernel: Switch 2 processes get an 8 GiB larger heap region
- kernel: allow 12 GiB main memory for Switch 2 layout
- kernel: SetHeapSize bounds by max heap when it exceeds the heap region
- ngc: serve stpl:u/stpl:sys regardless of firmware version
- shader: lower signed SGXT (from Eden-NS2 00aecd6)
- nvdrv: clear the error notifier when it is installed (from Eden-NS2 de4f497)
- shader: translate F2I to 8-bit integers (from Eden-NS2 de4f497)
- shader: translate PRMT in its default mode (from Eden-NS2 de4f497)
- shader: lower RED with register address, offset, type and 32-bit addressing (from Eden-NS2 0884090)
- shader: return TMML levels in 8.8 fixed point, in Maxwell order (from Eden-NS2 f3f422e)
- shader: allow SUST with a partial channel mask (from Eden-NS2 f3f422e)
- shader: saturate F2I F32 to U64 and map NaN to zero (from Eden-NS2 f3f422e)
- texture_cache: map G8R24 UNORM/UINT to D24_UNORM_S8_UINT (from Eden-NS2 f3f422e)
- renderer_vulkan: rotate three resource sets for the applet capture (from Eden-NS2 e2f285a)
- shader: round I2F U64 to F32 and U32 to F32 RZ exactly (from Eden-NS2 f3f422e)
- nfp: add IUser InitializeEx (cmd 300) (from Eden-NS2 f3f422e)
- shader: lower VOTEU into any mapped uniform register (UR16-UR27)
- vk_rasterizer: perform the indirect draw reset program's stores
- native: reject an atomic byte offset Maxwell cannot encode
- buffer_cache: qualify DEVICE_PAGEMASK
- shader: pack FP16 texture results with c0 in the low half
- Donkey Kong Bananza: optional 30 fps lock
- nx2: copy buffers back at every fence for Pokemon Pokopia
- native: lower LEA.HI with a constant base
- shader: lower indexed LDC of 8 to 32 bits (from Eden-NS2 de4f497)
- shader: lower F2I to 8/16-bit and all 32-bit roundings (from Eden-NS2 de4f497)
- shader: lower ULOP3 with a uniform second source, 0x092 (from Eden-NS2 de4f497)
- shader: lower MUFU with immediate and uniform sources (from Eden-NS2 0884090)
- shader: accept any cache hint on native LDG (from Eden-NS2 0884090)
- native: accept inverted uniform predicates on uniform arithmetic
- shader: lower native geometry shaders (OUT, ISBERD, LEPC, invocation ids) (from Eden-NS2 de4f497)
- ampere: 2D gather with per-texel offsets (.PTP)
- shader: accumulate HMMA.1688/.16816 in f32, round every eight products (in part: the HMMA.1688 form and the predicate check)
- ampere: emulate HMMA by default instead of rejecting the program (for Donkey Kong Bananza only)
- nx2: gpu 41/42 devices, vi2 notifications/client state, gsv 115 (in part: the vi2 notifications)
- nvdrv: stub nvhost-tsg-gpu subcontext ioctls
- nvdrv: accept AS MAPPING_MODIFY so NVN can place compressed textures
- sockets: answer bsd Unknown36 and refuse non-IPv4 families with EAFNOSUPPORT (from Eden-NS2 f3f422e; in part: EAFNOSUPPORT)
- dma_pusher: flush macro parameters the GPU wrote (for Donkey Kong Bananza only)
- vulkan_memory_allocator: fall back to host memory when VRAM is full (from Eden-NS2 1cf63e0)
- buffer_cache: re-upload a native program's 64 KiB uniform bank once per frame (monkeefunnee's, from Eglantine's raiders-fix branch)
- texture_cache: re-upload only the changed pages of large streamed images (from Eden-NS2 1cf63e0)
- texture_cache: wrap std::max/min against the Windows macros in partial uploads (fix for 7e8c527)

**gabrielskaminski**

- shader_recompiler: include all components in IsGeneric upper bound

**k-start**

- audio_core: keep libopus state on the host, report fixed Opus work buffer sizes

**KaitonGxx**

- native: accept guarded uniform arithmetic and predicate combines

**MamamaNanana**

- amd-bananza-rdna (Donkey Kong Bananza on AMD's Windows driver; in part: the driver workarounds)

**PavelBARABANOV**

- [vk] handles unreachable unknown format 255 error (#4591)
- [emit_spirv_image] remove the fix for LA/EOW (#4589)

**Wael3rd**

- shader_recompiler: drop unused identities at the end of the SSA rewrite
- shader_recompiler: skip instructions failed storage buffer searches already walked
- shader_recompiler: keep SSA definitions of predicates and flags in blocks
- spirv: remember the ids of scalar constants
- shader_recompiler: find the visited block containing a label by lookup
- nx2: back sparse regions whole on AMD's proprietary driver
- shader_environment: let graphics environments detach from the 3D engine
- vk_pipeline_cache: translate native graphics pipelines on worker threads
- vk_graphics_pipeline: share shader modules between pipelines
- vulkan: measurement-only pipeline creation timing (PipelineStats)
- vk_pipeline_cache: reuse translated shaders between graphics pipelines
