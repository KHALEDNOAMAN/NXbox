# Native D3D12 backend

## Scope and selection

NXbox currently uses Eden OpenGL (SPIR-V shaders by default), Mesa Gallium d3d12,
then D3D12. The native backend removes the OpenGL state machine and Gallium driver
from this path. It retains Eden's Maxwell decoding, IR and VideoCommon cache algorithms.
The native shader path is Maxwell IR → HLSL → DXC → DXIL. A new textual HLSL
backend will live next to Eden's GLSL backend in `src/shader_recompiler/backend/`.
Neither Mesa nor `spirv_to_dxil` participates in this path. Vulkan is the architectural reference, not a runtime
dependency of the native renderer. Existing Vulkan sources remain part of Eden's build.

The native backend lives in `src/video_core/renderer_d3d12/`. It is compiled only when `WIN32` and
`CMAKE_SYSTEM_NAME=WindowsStore`. OpenGL remains the default. Select either:

```ini
# LocalState/nxbox_env.txt (also accepts a process environment variable)
NXBOX_RENDERER=d3d12
```

or:

```ini
# LocalState/eden_settings.txt
renderer_backend=5
```

Eden's canonical `backend=5` label also works; `renderer_backend` is an NXbox alias.
`Settings::RendererBackend::D3D12` is appended as value 5; values 0–4 are unchanged.
The environment override is case-insensitive and takes precedence over settings.
Unknown environment values preserve the configured backend. Restart after changing
it. Set this globally: a per-title setting which changes graphics API after window
creation is rejected before core loading. A per-title setting can retain the
already selected API. Other frontends do not instantiate this UWP-only backend.

The frontend still uses the `MesaWindow` EmuWindow adapter, but its native branch
returns a no-op `GraphicsContext` and does not load Mesa or WGL. Native presentation loads the packaged `dxcompiler.dll`
and uses DXC validation to produce DXIL. The library
and launch screen release their Direct2D swapchain before handing over CoreWindow.
Native launch-frame uploads remain a frontend TODO; they must use the renderer's
swapchain.

P1 creates a hardware D3D12 device at feature level 11_0, direct queue, event-backed
fence, allocator/list, two RTVs and an opaque RGBA8 flip-discard CoreWindow swapchain.
The integration path uses eight fence-tagged allocators and a presentation descriptor
slice per swapchain buffer. Each is reused only after its own fence completes. `Context` is shared by the renderer and
both cache runtimes; every command retains its resources until completion. Layout
changes retire work before releasing buffers and calling ResizeBuffers. Fence waits
are bounded at 10 seconds and check device removal. Presentation failure latches
presentation off and publishes `NXBOX_D3D12_COMMAND_FAILURE` for frontend shutdown.
Teardown drains submitted work, including partial construction.

### P2 implementation

- `d3d12_buffer_cache.*` and `d3d12_texture_cache.*` supply native parameter traits
  and explicitly instantiate `VideoCommon::BufferCache` / `TextureCache` in separate
  translation units. Channels, CPU invalidation, unmaps, downloads and frame ticks
  are connected to the rasterizer. `d3d12_fence_manager.*` derives from the shared
  `VideoCommon::FenceManager`, matching Vulkan's cache-batch and guest callback
  protocol. `SyncOperation` only queues callbacks; `FlushCommands` only submits.
  Fence release uses the shared completion thread (`HAS_ASYNC_CHECK=true`). It only
  waits on already submitted values with a private event, completes CPU readback
  repacking, then retires cache batches and guest callbacks. Recording stays on the
  GPU thread. Completion batches detach before invocation; memory accounting is
  atomic because leases can retire on either thread. Completion failures propagate
  to the renderer boundary and wake forced drains. Teardown joins the fencing
  thread while the derived manager and caches are still alive.
- Buffers use committed DEFAULT resources. UPLOAD and READBACK rings allocate
  512-byte-aligned slices in 4 MiB pages, retaining allocations by fence tick and
  shared leases. A page resets only after completion and after every lease retires.
  Each ring is capped at 64 MiB; all committed cache allocations share a hard
  512 MiB ceiling, including staging. Large buffer fills submit in 1 MiB chunks.
  Ring pressure submits and waits before reusing completed, unleased pages; larger
  size classes evict idle smaller pages. Oversized requests and leased-page pressure
  use temporary storage under the same 512 MiB device ceiling. Deferred texture
  downloads use zero-offset leases until CPU unswizzling finishes. Image-to-buffer
  DMA downloads copy into a padded DEFAULT footprint, then fan out with GPU row
  copies to the tight guest buffer and to READBACK storage. READBACK heaps remain
  COPY_DEST; CPU repacking finishes before the guest callback retires.
- Textures are committed DEFAULT resources with typeless storage for legal typed
  view casts. The std-only format table covers every Maxwell PixelFormat and marks
  missing mappings explicitly unsupported. CPU ASTC conversion uses VideoCommon,
  including the configured uncompressed/BC1/BC3 result; BC formats stay compressed.
  BC resource dimensions round up to whole blocks; partial BC regions must be
  block aligned. Texture transfers use `GetCopyableFootprints`, 256-byte row pitch
  and 512-byte placement alignment. Tight guest rows are repacked for `CopyTextureRegion`; readback
  unpacks only after fence completion. Mips, layers and 3D slices are addressed
  explicitly. A conservative whole-resource tracker emits transition barriers.
- `Composite` obtains a framebuffer view from the shared texture cache when one
  exists. Otherwise the texture runtime caches native framebuffer resources by
  guest address (bounded to 64 entries, with idle entries reclaimed) and uploads NVFlinger's block-linear guest memory, preserving the guest stride. This
  matches the existing presentation fallback's block-height assumption (log2=4).
  A built-in full-screen triangle, compiled with DXC at startup as `vs_6_0`/`ps_6_0`,
  handles crop, flips, rotation, scaling into the display rectangle and layer blend
  modes. Shader model 6.0 is checked separately from feature level. Each layer gets
  its own fence-retired SRV slot (up to 64 per swapchain buffer). Empty frames retain the P1 test clear.

The integration branch translates guest graphics and compute through Maxwell IR,
HLSL and packaged DXC into native PSOs. Vertex A/B programs merge before emission.
Vertex/fragment/compute Shader::Info owns the descriptor schema; pipelines retain
copies so translation pool retirement cannot invalidate descriptors. Device options
select FP64/int64 lowering and require wave operations with at least 32 lanes.
Graphics keys include guest hashes, HLSL specialization, fixed state and topology;
compute specialization includes local dimensions and shared/local memory sizes.

Draws populate CBV/SRV/UAV/sampler tables and cached RTV/DSV attachments. Indexed
8/16/32-bit draws normalize restart markers, quads, strips and fans to native lists;
nonindexed fans/quads/polygons/line loops generate indices. Clears resolve masked,
scissored, per-layer color/depth/stencil operations through ClearRenderer. Guest
compute dispatch binds the same resource schema and orders UAV writes. ASTC 2D/array
uploads decode on the GPU when storing RGBA8; BC1/BC3 recompression retains CPU
conversion. Query hooks collect host counters, resolve reports and synchronize
before guest fences. Transform-feedback shader linkage remains unsupported.

Disk PSO libraries are scoped to title and adapter/driver/compiler/schema identity.
Versioned pointer-free recipes store fixed state, stages, descriptor counts and HLSL;
warmup recompiles/validates HLSL through DXC before pipeline-library reuse. It does
not trust cached unvalidated DXIL. Warmup and foreground PSO maps share a lock.
Composite samples the same cached guest images that Draw/Clear write, falling back
to guest-memory uploads only when no framebuffer view exists.

Translation exceptions produce a logged, explicit StubHlsl fallback. Its degenerate
vertex shader is diagnostic, not a substitute for a rendered guest scene. DXC/PSO
failures skip the rejected pipeline with diagnostics. Geometry/tessellation, alpha
testing, dual-source blending, transform feedback, unsupported HLSL opcodes and
indirect index conversion still need parity work.
DMA/surface acceleration declines requests so engine CPU fallbacks run; inline
writes and payload/timestamp queries retain CPU behavior. Applet captures and
screenshots still return blank buffers. Unsupported packed/ETC formats, multi-plane
depth/stencil staging transfers and GPU unswizzle remain unsupported. MSAA images,
resolves, sample-grid copies and supported guest blits use the native helpers;
unsupported image combinations are logged and skipped rather than issuing invalid copies.
Converted ASTC has no reverse readback. Full parity for those paths is later work.

## Vulkan to D3D12 ownership map

| Vulkan reference | Native D3D12 equivalent and responsibility |
| --- | --- |
| `vulkan_common/vulkan_device.*`, `VkDevice` | Device owns `ID3D12Device`, adapter capabilities, format/heap support and device-removal diagnostics. Probe shader model independently of feature level. |
| `vk_scheduler.*`, queue, command buffers/pools | Direct queue initially; command lists plus allocators. Allocators, descriptors, upload slices and resources retire against a monotonically increasing fence value. Never reset an allocator while submitted work references it. Add copy/compute queues only with explicit cross-queue waits. |
| Submission semaphores and scheduler ticks | Queue `Signal(ID3D12Fence, value)`, GPU queue waits and bounded CPU event waits. CPU completion, submission and guest syncpoints are separate concepts. |
| `vulkan_memory_allocator.*`, buffers/images | DEFAULT, UPLOAD and READBACK resources. Start with committed resources, later pooled placed resources. Respect `GetCopyableFootprints`, texture row/placement alignment and CBV 256-byte alignment; track budget within Xbox's shared process limit. |
| Descriptor sets/pools/update templates, `vk_update_descriptor.*` | Shader-visible CBV/SRV/UAV heap and separate sampler heap, plus CPU-only RTV/DSV heaps. Descriptor tables point to contiguous ranges; null descriptors and per-submission lifetime are required. No overwriting descriptors still used by the GPU. |
| Pipeline layout and push constants | Versioned root signature describes descriptor ranges/register spaces and root constants. Cache by binding schema, not by an opaque Vulkan layout handle. Root constants consume the shared 64-DWORD root budget; keep large blocks in CBVs. |
| `vk_pipeline_cache.*`, graphics/compute pipelines | Graphics/compute PSOs combine validated DXIL, root signature and fixed state. Include vertex layout, topology type, blend/raster/depth state, attachment formats and samples in the graphics key. Dynamic viewport/scissor, stencil reference and topology are set on the command list. |
| Image layouts and access/stage barriers | Per-subresource D3D12 resource-state tracker. Emit transition, UAV and aliasing barriers as applicable. Vulkan stage masks cannot simply be copied; distinguish ordered access from state transitions. Texture feedback must use legal nonoverlapping views or explicit copies. |
| Render-pass/framebuffer cache | RTV/DSV views plus `OMSetRenderTargets`; explicit load clears, store/discard policy and resolves. End incompatible attachment usage before sampling/copying. Render-pass APIs are optional later; no Vulkan render-pass dependency. |
| `vk_swapchain.*`, WSI and present passes | `IDXGIFactory2::CreateSwapChainForCoreWindow` receives the direct queue and CoreWindow ABI `IUnknown`, never HWND. `IDXGISwapChain3` selects the actual backbuffer index. Single owner per CoreWindow, flip model, opaque alpha, PRESENT state before DXGI Present. |
| Present layers/window adaptation | P2 uploads or looks up guest block-linear framebuffers and composes crop, transforms, scaling and layers with a built-in DXC shader. Guest graphics pipelines use the Maxwell/HLSL path on the integration branch. |
| Queries, conditional rendering, transform feedback | Query heaps plus ResolveQueryData/readback, predication where equivalent, and stream-output buffers/counters. Guest observable results and fence ordering must remain correct, even with software fallback. |

## VideoCommon template contracts

These are compile-time duck-typed interfaces, not abstract C++ runtime base classes.
Implement D3D12 resource wrappers and parameter structs, then instantiate the same
templates. Do not alias Vulkan resource handles or advertise an unsupported trait.
The file:line anchors below refer to the current Eden reference implementation;
method overloads and `if constexpr` branches determine the precise required set.

| Contract | Declaration / reference | Required behavior |
| --- | --- | --- |
| `TextureCache<P>` | `src/video_core/texture_cache/texture_cache_base.h:131`, constructor `:159`; Vulkan traits `src/video_core/renderer_vulkan/vk_texture_cache.h:581` | P supplies Runtime, Image, ImageAlloc, ImageView, Sampler, Framebuffer, AsyncBuffer, BufferType and capability flags (validation, framebuffer blits, emulated copies, memory reporting, async downloads, MSAA downloads). Cache owns guest identity, overlap/alias tracking, invalidation, swizzling, format conversion and collection. |
| `TextureCacheRuntime` | `src/video_core/renderer_vulkan/vk_texture_cache.h:43` | Finish/TickFrame/FlushDeferredClear; UploadStagingBuffer/DownloadStagingBuffer/FreeDeferredStagingBuffer; GetDeviceLocalMemory/GetDeviceMemoryUsage/CanReportMemoryUsage; BlitImage, CopyImage/CopyImageMSAA, ShouldReinterpret/ReinterpretImage, ConvertImage; upload/MSAA capability queries, AccelerateImageUpload, InsertUploadMemoryBarrier, TransitionImageLayout, HasBrokenTextureViewFormats/HasNativeBgr, BarrierFeedbackLoop. Disabled traits avoid BlitFramebuffer/EmulateCopyImage paths. Optional NXbox budget/ASTC hooks are detected with `requires`. |
| Texture resource wrappers | `src/video_core/renderer_vulkan/vk_texture_cache.h:193` (Framebuffer), `:308` (Image), `:399` (ImageView), `:503` (ImageAlloc), `:525` (Sampler) | Image derives ImageBase and implements staging upload/download, scale and modification hooks. Views derive ImageViewBase and expose host views without losing guest metadata. Provide null-image/view constructors, framebuffer attachment metadata and sampler translation. Fence-retire all host views/resources. |
| `BufferCache<P>` | `src/video_core/buffer_cache/buffer_cache_base.h:173`; Vulkan traits `src/video_core/renderer_vulkan/vk_buffer_cache.h:272` | Runtime, Buffer, Async_Buffer, MemoryTracker; IS_OPENGL=false. Select persistent UBO binding, primitive support, binding indices, memory-map/upload and separate image-buffer flags honestly. Preserve dirty guest ranges, download scheduling and channel state. Sparse multi-range hooks are optional, so start with ordinary buffers. |
| `BufferCacheRuntime` | `src/video_core/renderer_vulkan/vk_buffer_cache.h:98` | TickFrame(slot_buffers), CurrentTick/IsFree/Wait/Finish; memory reporting; uniform/storage alignment; staging allocation/free; CanReorderUpload, PreCopyBarrier/CopyBuffer/PostCopyBarrier/ClearBuffer; index/quad/vertex/transform-feedback bindings; BindMappedUniformBuffer, BindUniformBuffer/BindStorageBuffer/BindTextureBuffer. With Vulkan-style flags, compute uses the same binding methods. Optional storage-limit/multi-range extensions can follow later. |
| Buffer / staging wrappers | `src/video_core/renderer_vulkan/vk_buffer_cache.h:34`; `src/video_core/renderer_vulkan/vk_staging_buffer_pool.h:22` | BufferBase metadata, null constructor, handle/view, mapped access, range-use/write ticks and CPU upload/download operations. Async staging carries resource, offset, mapped span and retirement identity. Replace VkBuffer/device addresses with native resource and GPU virtual address types. |
| `QueryCacheBase<Traits>` / `QueryCacheRuntime` | `src/video_core/query_cache/query_cache_base.h:39`, `:56`; `src/video_core/renderer_vulkan/vk_query_cache.h:29`, traits `:72` | Traits::RuntimeType. GetStreamerInterface returns a StreamerInterface for each QueryType; SyncValues writes guest values; Bind3DEngine/View3DRegs, Barriers, End/Pause/ResumeHostConditionalRendering and compare-one/compare-two conditional operations. Streamer ownership, bank recycling and CPU guest writes must survive invalidation and channel changes. |
| Query streamers | `src/video_core/query_cache/query_cache.h:48`, `:119`, `:179` | GuestStreamer/StubStreamer and QueryCacheBase implementation expose actual runtime calls. Follow `query_stream.h`/`bank_base.h` for host streamer/bank contracts; this checkout is not the older HostCounter-only query cache. |
| `FenceManager<Traits>` | `src/video_core/fence_manager.h:42`, protected hooks `:154`; Vulkan traits `src/video_core/renderer_vulkan/vk_fence_manager.h:43` | FenceType and three cache types; CreateFence, QueueFence, IsFenceSignaled, WaitFence and HAS_ASYNC_CHECK. Flush cache batches before signalling; release guest callbacks/syncpoints only after the represented work completes. |
| Rasterizer integration | `src/video_core/rasterizer_interface.h` and `src/video_core/renderer_vulkan/vk_rasterizer.h` | Bind channels/engines, invoke caches, handle CPU writes/unmaps, draw/dispatch/clear, barriers, acceleration and disk-resource loading. P1 implements the complete virtual interface with explicit stubs instead of instantiating incomplete templates. |

Use `texture_cache/texture_cache.h`, `buffer_cache/buffer_cache.h` and
`query_cache/query_cache.h` as the final authority for instantiated calls. Vulkan
helpers such as MSAA resolve shadows, VkFormat lists and render-pass caching are
backend implementation details, not requirements to reproduce by name.

## Maxwell IR to HLSL to DXIL integration (owner decision)

The native path is **Maxwell → Eden IR → textual HLSL → DXC → DXIL**. Implement a
new HLSL emitter next to `src/shader_recompiler/backend/glsl`, using the GLSL
backend as the textual-emission reference. Vulkan remains the reference for stage
chaining, cache ownership and runtime specialization. Do not emit SPIR-V, call Mesa,
load `spirv_to_dxil`, or add those dependencies to the native renderer. Existing
Mesa artifacts belong only to the unchanged OpenGL path.

`dxcompiler.dll` is already packaged with the app. P2 loads it with
`LoadPackagedLibrary` and `DxcCreateInstance` for the built-in presentation shader.
It uses the public `IDxcCompiler3` ABI, checks both invocation and compilation status,
retains diagnostics, and obtains the validated DXIL container before PSO creation.
The minimal local ABI declarations follow Microsoft's
[`dxcapi.h`](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/dxcapi.h).
No shader compilation import library or Mesa compiler library is linked.

P3 must extend this to guest shaders:

1. Reuse Maxwell decoding/IR, stage chaining and dual VA/VB merging. Emit HLSL from
   the same IR and Shader::Info, with a D3D12-specific Profile/RuntimeInfo derived
   from actual device capabilities. Lower unsupported IR operations explicitly or
   reject the shader with diagnostics. P2 does not implement the guest HLSL emitter.
2. Derive one binding schema from Shader::Info and the shared backend binding
   counters. HLSL `register`/`space` declarations, root signatures and descriptor
   population must use that schema: CBVs for uniform buffers, SRVs/samplers for
   sampled textures, UAVs for writable buffers/images. Descriptor arrays need
   nonoverlapping register ranges; reserve a space for runtime constants.
3. Match HLSL cbuffer layout and host constant offsets with shader tests. Keep root
   constants and tables within the 64-DWORD root budget; larger blocks use aligned
   upload CBVs. Handle base vertex/instance, draw ID and viewport/compute corrections
   in explicitly defined runtime data, without Mesa metadata.
4. Compile stage-appropriate HLSL profiles with the packaged DXC, targeting supported
   shader model/validator versions. Preserve errors, verify signed DXIL before PSO
   creation, and fail rejected guest pipelines cleanly. Test packaged UWP imports
   on Windows/Xbox; host tests cannot establish runtime DLL compatibility.
5. Include IR/shader hashes, Profile/RuntimeInfo specialization, emitter and DXC
   versions, register/root schema and fixed PSO state in native cache keys. Never
   reuse GL/Vulkan disk shader blobs. Verify coordinate/depth conventions, texture
   buffer emulation and all stage combinations at this boundary.

## Phases and acceptance criteria

| Phase | Deliverable | Acceptance criteria |
| --- | --- | --- |
| P1: skeleton, clear, present | Opt-in RendererD3D12 and complete stub rasterizer; CoreWindow swapchain, direct submission/fence; architecture | Existing host suite passes with its real exit code; OpenGL remains selected without opt-in; Windows UWP build/link succeeds; console displays the documented color, frame notifications advance, suspend/resume and shutdown do not hang. No Mesa load or duplicate CoreWindow swapchain in native mode. Current host work does not establish the Windows/console criteria. |
| P2: buffers and textures | Native TextureCache/BufferCache traits, committed resources, staging rings, state tracker, allocation budget and DXC framebuffer compositor | Instantiate shared templates; round-trip linear and block-linear data, mips/layers, BC and CPU ASTC, null views, format aliases, CPU invalidation and fence-retired reuse. Present a known guest framebuffer with crop/transform/scaling and compare readback. No shader-based guest rendering required yet. |
| P3: graphics pipelines | Maxwell IR HLSL emitter, DXC compilation, validated DXIL, root signatures, descriptor heaps and graphics PSO cache | Known shaders demonstrate constants, descriptor arrays, vertex inputs and sampled/storage textures; triangle and guest scene draw correctly without GL. Validate all stage combinations used; rejected shaders/PSOs retain diagnostics. Warm disk cache reconstructs identical output; no stale descriptor reuse. |
| P4: compute | Native compute PSOs, runtime data, Dispatch and UAV synchronization | Deterministic SSBO/image workloads match CPU/Vulkan references; guest compute-to-graphics dependencies and barriers work. No compute silently dropped; unsupported features are reported. |
| P5: queries and transform feedback | Query heaps/readback, guest reports, predication and stream output | Occlusion/timestamp/report ordering and conditional draws match reference; stream-output bytes and counters match; guest waits complete correctly and no query/bank is reused before fence retirement. Test software fallbacks for unavailable statistics. |
| P6: parity | Presentation layers, remaining formats/state/features, cache robustness, diagnostics and performance | Compare representative games/scenes against the existing backend; screenshots and saves/session behavior remain correct; exercise resize/suspend/resume/device loss, long-run memory and cold/warm shader cache. No silent unsupported paths; establish budgets/performance before any default-backend change. |

## Validation commands and outstanding evidence

```sh
cd tests/port && python3 -m unittest discover -s . -p 'test_*.py'
```

The selection test compiles std-only C++ on the host and checks precedence,
case handling and persistence of backend enum values. Resource tests execute the
format table, odd-sized BC footprints, mip/layer placement, overflow rejection and
row packing round trips under AddressSanitizer/UndefinedBehaviorSanitizer. Texture
layout rules follow Microsoft's [upload/readback documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data)
and [`GetCopyableFootprints`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints). Full Windows compilation
requires the existing Visual Studio UWP toolchain (`tools/nxbox/build-uwp.cmd`).
The host HLSL IR test generates vertex/fragment/compute artifacts and exercises
mixed-resource stage chaining. Host tests do not execute D3D12 guest draw commands.
Windows compilation, packaged DXC loading, GPU round trips and console framebuffer
comparisons remain required acceptance evidence; passing host tests alone does not
complete that evidence. For console evidence, keep startup logs, the test-color
capture, frame progression and teardown/removal logs. Do not treat passing host
checks as proof that the Xbox accepts the swapchain or shader compiler imports.

## Early device removal bisect

With `NXBOX_D3D12_DEBUG=1`, diagnostics capture all InfoQueue severities through
an unfiltered InfoQueue1 callback, with polling when the callback is unavailable.
Creation messages are drained before registration. Each ExecuteCommandLists logs
its sequence, fence tick and recorded work categories; command lists have matching
names and string markers. Present and pipeline-library creation have explicit
before/after removal checks. UWP diagnostics also append to `nxbox_crash.txt`.
DRED enables breadcrumbs, page faults and breadcrumb contexts before device creation;
removal reports list/queue names, the last completed operation, nearby operations,
contexts and page-fault allocations. Availability HRESULTs are retained.

Set each knob to exactly `1` before starting a fresh process. They deliberately
change emulation behavior and are intended only to isolate device removal:

| Variable | Effect |
| --- | --- |
| `NXBOX_D3D12_PSO_LIBRARY` | Opt in to native pipeline libraries on Xbox/UWP. Disabled by default because even an empty library removes the Xbox device; DXIL disk caching remains active. |
| `NXBOX_D3D12_NO_PREWARM` | Set to `1` to disable background disk-recipe PSO prewarming. By default async workers rebuild cached requests at lowest priority; ready results share the runtime cache. |
| `NXBOX_D3D12_NO_PSO_LIBRARY` | Skip native pipeline-library creation/load/store/serialization; keep direct PSOs and validated DXIL/recipe caching. |
| `NXBOX_D3D12_GPU_ASTC` | Enable the GPU ASTC decoder (off by default: it corrupts small textures such as Mii face parts; the CPU conversion path is used instead). |
| `accelerate_astc=2` (in `eden_settings.txt`) | Decode CPU ASTC on the texture decode worker instead of inline on the GPU thread; a texture samples empty until its upload lands at a later frame tick. Off by default until measured on the console. |
| `NXBOX_D3D12_NO_COMPUTE` | Skip guest compute dispatches; retain helper compute and GPU work accounting. |
| `NXBOX_D3D12_NO_DRAWS` | Skip direct/indirect guest draws; retain clears, compositor presentation and GPU work accounting. |
| `NXBOX_D3D12_NO_QUERIES` | Disable native query heaps, begin/end/resolve, timestamps and host predication; return zero counters/timestamps and retain guest report writes. |
| `NXBOX_D3D12_SYNC_QUERIES` | Set to `1` to restore synchronous query collection (submit and CPU wait at each report/reset). Otherwise native query results, accumulated totals and reset baselines stay on the GPU. Reports are copied only to readback snapshots; after the batch fence completes, the query cache writes their values directly to guest memory and marks them `IsGuestSynced`. Counter reports do not copy into guest GPU buffers or mark buffer-cache regions GPU-modified. A guest read of an unfinished report drains the batch once; subsequent reports in that batch are already available. Query mode is selected when the runtime is created. `NXBOX_D3D12_NO_QUERIES` takes precedence. `gpu_resolved_query_reports` counts GPU snapshots; `query_report_fence_waits` counts guest reads that request a fence drain because the final query value is still pending (not completion-thread waits); `forced_submits_query_bank` still counts actual forced collection submits. |
| `NXBOX_D3D12_CPU_PREDICATE` | Evaluate every conditional render on the CPU (readback and fence wait per check). Host-managed counter reports use Maxwell's guest-memory comparison path. For other values, single values and comparisons against a known zero are resolved on the GPU by default, and other two-report comparisons render unconditionally unless GPU accuracy is high, as in the Vulkan backend. |

Initialization order is device/SM6 support, direct queue/fence/allocator/list,
rasterizer heaps and transform-feedback counter upload recording, swapchain and
SM6.0 presentation PSOs, counter upload submission, test clear/present submission
and completion, then disk shader-cache warmup. All root signatures use version 1.0.
The view heap has 262144 descriptors and the sampler heap has 2048; creation logs
record these capacities rather than assuming console-specific limits.

The third SM3DW console capture (`/tmp/d3d12crash3`) establishes removal reason
`0x887a0020`, but cannot identify the offending work. The old “from disk” label
was also used for an empty blob. Existing disk records already validate adapter
LUID, driver version, emitter/DXC/schema/format versions and checksum, deleting
stale/corrupt records before passing their payload to the driver. New logs include
identity and actual blob size, and native-library fallback cannot swallow a removed
device. ASTC image stores already check output dimensions; no out-of-bounds store
was established by this capture. A new console run is required to locate the fault.

### Native console draw contracts

The HLSL emitter and PSO input layout share `GenericSemantic` (`TEXCOORD` plus
numeric guest attribute index). Only shader-read attributes enter the layout;
constant/missing inputs have a dedicated zero-stride buffer slot. Disabled or
unmapped guest streams also bind a zero buffer rather than inheriting stale IA
bindings. The buffer uses committed upload memory, not host-pointer heap imports.

Each draw snapshots the actual framebuffer views before requesting its PSO. The
same snapshot supplies the RTV/DSV descriptors and the format key, preserving
sRGB views and interior unbound output slots (UNKNOWN), dropping trailing null
slots, and disabling depth/stencil tests when no DSV exists. DXIL/recipe cache
versions invalidate older input layouts and register-derived attachment keys.

In `/tmp/d3d12run4/run4_only_NO_PSO_LIBRARY/diag.txt`, id 1086 is severity 3
(INFO), first emitted between command-allocator and command-list creation and
before guest buffers exist. There is no `OpenExistingHeapFromAddress` call or
host-address heap import in the native renderer or its buffer allocator: resources
use `CreateCommittedResource`. That evidence does not identify the internal
runtime caller, nor prove a failing guest import path. Xbox debug callbacks now
capture the first such message's caller stack with module offsets. A new console
run is needed to attribute it; no error message is suppressed or speculative
allocation workaround enabled.

### Render target tracking and syncpoint completion

Console run 6 (`74b83c8c14`, SM3DW, `NXBOX_D3D12_DEBUG=1`) survived without
removal but displayed black and stopped guest progress after about 100 seconds.
PSO creation reported empty RTV/DSV formats. The draw/clear ordering already
updated render targets before taking framebuffer snapshots; native channel
initialization was missing the common Maxwell dirty tables. Each native channel
now installs those tables and initially invalidates all tracked state, allowing
RT address/format, draw-buffer control, surface size and zeta changes to reach the
texture cache and the attachment-derived PSO key.

`D3D12_ATTACHMENTS` records the first eight draw snapshots, including every output
slot's view format, the depth format and any pixel shader output without an
attachment. `D3D12_DISPLAY` records the first compositor source's display address,
resource identity, format, dimensions, scaling and whether it came from the texture
cache or guest-memory fallback. Presentation resolves `FramebufferConfig` through
`TryFindFramebufferImageView`; it does not select the most recently bound RT.

Native guest fences always submit and require GPU completion, including when no
cache readbacks exist and fence settings are relaxed. The GPU thread drains each
native signal before returning, so a final Host1x callback cannot depend on another
guest command arriving. WFI also drains pending fences after finishing GPU work.
`D3D12_SYNCPOINTS` reports pending and signaled counters for early signals and every
120 completions. This synchronous policy preserves Context's thread ownership but
may reduce CPU/GPU overlap; measure it in the next console run. Host regression
tests cover dirty-table invalidation, no-readback fence ordering and the actual
native manager's last-syncpoint completion. They do not establish console pixels
or sustained guest progress; both require another hardware capture.

### Console run 7 follow-up

Run 7 (`a24fbd3b7`) rendered the game-select screen correctly at 1920x1080.
The final process (`pid=5268`) records GPU-thread `SIGABRT` at tick 265707359,
then a 5078 ms heartbeat gap and `Suspending` at tick 265712843. Memory was
1753100288 bytes against a 5368709120-byte limit, with `device_lost=0`.
This is an abort preceding suspension, not evidence of a swapchain timeout or
process memory-limit termination. The captured 24 handler frames stop inside
C++ runtime unwinding and do not identify the original throwing operation.

The follow-up removes synchronous syncpoint drains, single-allocator serialization
and per-frame compositor drains. It handles staging pressure without throwing on
the 64 MiB ring cap, filters informational resource-creation traffic, samples
submission/presentation diagnostics, and emits one marker per work label per batch.
Fragment export metadata and HLSL stores are masked by the bound attachment formats;
the existing framebuffer PSO key keeps each signature paired with its RTV layout.
Fatal diagnostics now capture 96 frames and any active C++ exception. The GPU and
fencing threads install their own MSVC terminate handlers, which were previously
installed only on the main thread.

Query reports register their cache entry before publishing an asynchronous callback.
The native completion requirement also prevents low-accuracy settings from recycling
a query slot while its callback still references it. Pending-list locks are released
before D3D12 uploads to allow completion reentry. Host-managed counter predicates use
Maxwell's guest-memory comparison path so they cannot read stale GPU buffer contents.
Other predicates retain native D3D12 predication.

Host tests cover asynchronous callback ordering without another guest command,
completion failure waking a forced drain, staging pressure with live CPU leases,
query publication/retirement at low accuracy, GPU DMA row layouts and destination
contracts, and HLSL export/store masking. A new console run is still required to establish
whether these fixes remove the run-7 abort and to measure the resulting frame rate.

### MSAA depth-target termination fix

Native cached 2D images now retain the guest sample count (1/2/4/8/16), subject to
`D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS` for the typed color/DSV format. Quality
zero keeps attachment formats compatible. MSAA resources have one mip and no UAV
flag. Guest image dimensions include the expanded sample grid; the D3D12 resource
uses logical pixel dimensions. SRV/RTV/DSV bindings, layered clears, null SRVs and
graphics PSO keys all carry the actual resource sample count. MSAA framebuffer
views resolve before the single-sample compositor; rejected presentation layers
leave the cleared swapchain intact. Mixed attachments
skip the draw. HLSL texel fetches select `Texture2DMS.Load`/`Texture2DMSArray.Load`
from the resource descriptor, defaulting an omitted sample index to zero.

Two-plane D24S8/D32FS8 resources are valid depth/stencil targets, including MSAA.
Their packed guest staging upload/readback remains unimplemented: upload logs and
skips, and download eligibility excludes them so untouched staging bytes cannot
be written back to guest memory. Color and depth-only MSAA staging uses a
single-sample grid intermediate and the existing scatter/gather shader helpers,
preserving each sample. Color blit resolves average normalized/floating samples;
integer and depth/stencil resolves select sample zero. Supported depth/color
reinterpretations and copies also route through the existing helpers. Unsupported
formats, dimensions, view casts, multisample support and helper operations log
once per `(PixelFormat, planes, samples, type)`, with the format name and failing
condition, and leave an invalid image/view or skip the operation. Device/resource
failures still propagate to the renderer failure boundary.

The reported `MakeTexture` exception could not reach the GPU catch because
`Common::SlotVector::insert` was `noexcept` around placement construction.
Allocation helpers were also `noexcept`. Construction now rolls the slot back and
propagates; growth reserves metadata first and cleans up partially relocated
storage. Maxwell clear calls have no `noexcept` boundary; macro JIT callbacks
already capture exceptions with `DeferredException` before returning to generated
code and rethrow in a compiled C++ frame. `RendererD3D12::ReportFailure` records
`event=renderer_exception what=...` through `Diagnostic`, including
`nxbox_crash.txt`, and the GPU thread continues consuming cancellation/fence work.

Host regressions exercise throwing image construction, slot reuse, the macro
callback transport, sample-sensitive and depth-only PSO keys, and HLSL MSAA fetches
for 2D/array color/depth textures. Console rendering and packed depth/stencil
staging parity still require hardware validation. OpenGL remains the default.

### Asynchronous guest PSOs and per-pipeline blobs

`NXBOX_D3D12_ASYNC_PSO` defaults to enabled on UWP and disabled on desktop.
Set it to `0` for the original synchronous path. One two-thread pool serves guest
both graphics and compute compilation, with at most four outstanding jobs. The
GPU thread snapshots guest instructions, constant buffers, TIC descriptors and
register specialization; workers own these copies through translation, HLSL,
DXC and native PSO creation. Active graphics stages share the captured TIC table.
The requesting thread polls a mutex-protected result map and skips pending work;
`draws_skipped_pending_pso` and `dispatches_skipped_pending_pso` expose this cost.
Failed requests are negatively cached until a title reload. Shutdown cancels
queued jobs, joins idle workers, detaches busy workers, and retires GPU references
before releasing results. A driver call already running cannot be safely interrupted;
detached jobs retain their inputs and disk-cache lifetime.

Async mode prewarms disk recipes in the background on the existing lowest-priority
workers without retaining the loading UI callback. Record format 3 (identity schema 6,
recipe version 4) adds length-delimited guest request keys and pointer-free resource,
primitive and stream-output metadata; older records are discarded. The recipe's
fixed-state/stage/source layout is otherwise unchanged. Records have no usage timestamp,
so replay keeps filename order. Admission uses the runtime cache's capacity and key
insertion: a foreground request shares an in-progress prewarm instead of compiling
again or waiting on it. Requests replay has not reached retain the normal first-use
wait. Known stalled requests are skipped; loading cancellation and shutdown stop replay.

`NXBOX_D3D12_NO_PREWARM=1` disables replay. Start diagnostics report the record count;
`D3D12_PSO_PREWARM built=N skipped=N failed=N ms=N` reports completion. Frame profiles
count runtime lookups of ready prewarmed pipelines as `prewarmed_pso_hits`, including
subsequent guest-map/last-request hits. Worker compilers are thread-local. Frame CPU
phases still describe the GPU thread, so async translation/DXC CPU time does not appear
there; `pso_compiles` counts completed foreground async results when first published.

Native pipeline libraries remain disabled on Xbox: earlier console evidence showed
that even creating an empty library removed the device. Per-pipeline persistence
uses the separate `D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO` capability,
`ID3D12PipelineState::GetCachedBlob` and `CachedPSO`. Adapter/LUID/driver, emitter,
DXC and schema identity plus checksums protect `.gpso`/`.cpso` files beside the DXIL
cache in the title's LocalState shader directory. A rejected blob is retried once
without cached data while the device is healthy. Unsupported blob retrieval leaves
the successfully created live PSO intact. The blob cache is off by default: on the
Xbox Series X (build 558) reading a stored blob back made the driver remove the device
(0x887A0020) within the first frames. `NXBOX_D3D12_CACHED_PSO=1` turns it on for
experiments, independently of pipeline libraries. Depth-bounds/independent
stencil PSO streams still use direct creation and do not persist native blobs.
Public Windows API support does not establish Xbox driver behavior; first/second
launch console measurements remain necessary.

`NXBOX_D3D12_PSO_DIAGNOSTICS=1` logs each pipeline taking at least one second once,
including its hash, native wall time, root signature/schema and descriptor counts,
per-stage DXIL/HLSL sizes, LLVM instruction count, allocation/indexing/switch counts,
explicit unroll/loop markers, TEXCOORD declaration count and local/register arrays.
LLVM counts describe the DXIL program rather than native GPU ISA. The counts expose
possible causes without claiming that a GEP necessarily spills or that each
TEXCOORD declaration is a live hardware interpolant. Guest structured loops now
carry `[loop]` to prevent speculative unrolling in DXC/driver lowering.

Guest `EXIT` nested in structured control flow is not emitted as `return`. DXC turns each
such return into an extra branch to the shared exit block, and the Xbox driver's compile of
that unstructured CFG needs memory without bound: the firmware Mii editor pixel shader
(six nested returns at depth up to 13, only 40 KB of DXIL) took the process past its 5 GB
limit within 25 s, while the same shader with its returns removed or turned into flags
compiled in about a second. The emitter now writes `nx_returned = true;` and guards what
follows the region with `if (!nx_returned) {`, so every exit stays structured (a loop's
repeat test also checks the flag). Programs whose returns are all at the top level are
emitted as before. `NXBOX_HLSL_RAW_RETURNS=1` restores the old form for bisecting.
