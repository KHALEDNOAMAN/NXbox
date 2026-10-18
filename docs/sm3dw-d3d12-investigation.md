# SM3DW first-use PSOs and shadow streaks

Title: `010028600EBDA000`. This investigation used existing local artifacts only;
no console run, Windows build, CI, network access, commit, or package was produced.
Artifact directory names are not reliable title identification: for example,
`probe-mario-play/diag.txt` actually identifies BotW. Measurements below come from
logs that identify the requested title.

## Measured evidence

Paths below are relative to the local NXbox artifact directory.

- `sm3dw-p2d/diag.txt:23-24`: requested title; `astc=0`,
  `asynchronous_shaders=false`, `astc_sync=false`.
- Lines 47-48: native per-PSO blobs unsupported/disabled; pipeline library disabled
  on Xbox/UWP, DXIL disk cache active. These are direct creates, not measured
  pipeline-library lookup misses.
- Line 655: 284 frames, one new guest pipeline/PSO, lookup 332.663 ms,
  translation/DXC/native PSO recording-thread buckets all zero, texture upload
  0.061 ms, repack 0.001 ms, zero pending draws skipped.
- Line 739: zero presented frames in the profile window, 122 guest misses and
  PSOs, lookup 12985.048 ms, upload 25.939 ms, repack 7.298 ms, zero pending draws
  skipped. Line 747 reports a 14342 ms worst gap; line 748 reports GPU work
  `17525/13674/78`, upload `9/1/23`, conversion `0/0/0`.
- Lines 751/818: lookup 4077.802/2697.854 ms with 110/8 new PSOs; texture upload
  0.565/0.336 ms and repack 0.017/0.005 ms. These are interval totals, not timings
  of individual compilations, and the windows are not necessarily aligned to
  GAME_PRESENT windows.
- Independent `sm3dw-p1d/diag.txt:739`: lookup 12938.781 ms, 122 misses/PSOs,
  upload 26.258 ms, repack 7.273 ms, zero pending draws skipped.
- Earlier `mario-PAD/diag.txt:550`: 131 misses/PSOs, lookup 322.880 ms and
  30798 pending draws skipped. It is a different build/run, not a controlled A/B.

The dominant measured first-use cost is waiting inside pipeline lookup. GPU-thread
translation/DXC buckets being zero does not prove compilation was free: async
workers have no recording-thread profiler sink. The code already snapshots guest
inputs and compiles translation, HLSL, DXC, and native PSOs on workers, then the
recording thread waits up to 5000 ms once per new request. Upload and conversion
measurements do not support ASTC decode as the dominant cause in these windows.
The old logs cannot apportion worker time among translation, DXC, and driver PSO
creation, or time an individual animation's hitch.

## Change and limits

`NXBOX_D3D12_ASYNC_SHADERS=1` with `NXBOX_D3D12_ASYNC_PSO=1` bypasses that wait
for pending graphics draws only when the drawn count (indices for an indexed draw,
vertices otherwise) exceeds six and transform feedback is disabled. The rule
started as a copy of `renderer_vulkan/vk_pipeline_cache.cpp::BuiltPipeline`, which
tests both counts; one of them is always left over from an earlier draw, so a
3-vertex fullscreen pass made every later indexed mesh wait (Wonder: 123 waits,
69 s of a 380 s run). It preserves the bounded wait for likely one-shot
texture-generation quads. `PSO_WAIT` lines record `indexed`, `count`, `depth`
and the first render target size of each waited draw. Compute retains its
existing wait. Pending requests remain retryable, including queue saturation;
a later small draw of a previously skipped PSO can still take its bounded wait.

This is an opt-in visual tradeoff, not a proof that a draw has no side effects.
Transient geometry/effects can disappear until ready, and large one-shot draws
or shader storage writes are not identified by this heuristic. Small draws and
compute can still hitch; guest snapshot costs and texture work remain on the
recording thread. Defaults are unchanged. The added timing reads run only with
PSO_TRACE enabled. PSO_TRACE now includes cumulative worker wall time and thread
ID; PSO_WAIT records recording-thread wait wall time.
Subtract adjacent phase timestamps for translation, HLSL, per-stage DXC, root
signature, and native PSO durations. Disk recipe persistence after `done` is not
included in that marker; PSO_WAIT includes the wait for final publication.

## Shader compile probe

- `NXBOX_D3D12_SHADER_PROBE_DIR=<folder>` enables one background probe per process over `*.hlsl` files in name order; absolute paths or paths relative to UWP LocalState are accepted, and unset/empty disables it.
- `NXBOX_D3D12_SHADER_PROBE_TIMEOUT_S=<seconds>` sets the native compute PSO timeout (default 45; clamped to 1..600; invalid values use 45).

Files may start with `// target: <profile>` (default `cs_6_0`), use entry point
`main`, and must embed a root signature on `main`. The probe uses the game's DXC
arguments, including `-Gis` with `NXBOX_D3D12_DXC_STRICT=1`, without reading or
writing DXIL caches. `SHADER_PROBE` diagnostics report DXC/PSO timing, HRESULT,
and UWP app memory before/after each variant, followed by result totals.
Timed-out driver threads retain their resources; the probe stops after four
timeouts to bound the driver's leaked memory. Shutdown or suspend stops new files
without waiting for an in-flight compilation.

## Shadow diagnosis

No artifact establishes the specific shadow-producing pass or its bound depth
format, bias, sampler, clear, and border together. No shadow rendering behavior
was changed. The first hypothesis to test is depth-bias precision/scale at shadow
map rasterization: FillGraphicsState rounds `regs.depth_bias / 2` to an integer,
where Vulkan keeps a float and has format-dependent handling. This is a
low-confidence lead, not a demonstrated bug; the native D3D12 API itself takes
an integer bias and uses format-dependent units.

Comparison sampling is already gated by both TSC compare enable and shader
comparison instructions (`MakeSamplerDesc`); comparison function is mapped with
`+1`, border components are copied from the guest, D16/D24/D32 format families
are represented explicitly, and clear depth comes from `regs.clear_depth`.
Those paths provide no evidence for replacing them with a constant or changing
the bias sign. The selected-frame PSO trace now includes the actual integer bias,
slope bias, and clamp so the hypothesis can be checked against the offending pass.

## Console experiment for the tester

Build/package run IDs: none; the lead must build/package the uncommitted change.

1. For hitch A/B, put the following KEY=VALUE lines in `LocalState\nxbox_env.txt`:

   ```text
   NXBOX_RENDERER=d3d12
   NXBOX_D3D12_ASYNC_PSO=1
   NXBOX_D3D12_ASYNC_SHADERS=0
   NXBOX_D3D12_FRAME_PROFILE=1
   NXBOX_D3D12_PSO_TRACE=1
   ```

   Fully restart NXbox, select the requested title, enter the same save/level,
   then trigger jump, attack, transformation and a new particle effect once,
   then repeat them. Save `eden_uwp_diag.txt`, `eden_log.txt` if present, and a
   video with action times. Repeat from the same scene with ASYNC_SHADERS=1.
   Preserve/restore an identical copy of the title's cache before each run for
   a controlled comparison; do not compare a cold baseline to a warmed candidate.
   Do not use PSO_WAIT_MS=0 as a substitute: it also drops small one-shot passes.
   Success: eligible misses produce pending-draw skips and resume when Ready,
   with reduced worst gaps, and ambient lighting remains correct after entering
   and re-entering a level. Check first-run missing effects and persistent black
   textures explicitly. Trace/log I/O affects timing; repeat timing runs with
   PSO_TRACE removed after the phase diagnosis.

2. Separate shadow capture (capture downloads stall rendering, so do not use it
   for hitch measurements). Replace the environment file with:

   ```text
   NXBOX_RENDERER=d3d12
   NXBOX_D3D12_ASYNC_PSO=1
   NXBOX_D3D12_ASYNC_SHADERS=0
   NXBOX_D3D12_DRAW_TRACE_FRAME=1800
   NXBOX_D3D12_DEPTH_TRACE=1
   NXBOX_PASS_DUMP_FRAME=1800
   ```

   Leave NXBOX_PASS_DUMP_DIR unset: `mesa_window.cpp::LoadRuntimeEnvironment`
   supplies the absolute writable `LocalState\passdump` directory. Fully restart,
   select SM3DW, and reproduce the streak scene before presented frame 1800.
   Keep the camera still through that frame; retain the save, input script/timing,
   screenshot/video and the entire `passdump` directory (`manifest.jsonl`,
   `sampled.jsonl`, `pass_*.raw`) plus diagnostics. If the streak is absent at
   frame 1800, repeat with both frame variables set to the same later frame
   while holding that scene; an unrelated frame does not establish the cause.
   Depth trace is limited to four draws/clears per frame. Draw trace is capped at
   3000 draws; vertex-byte tracing is further limited to small non-indexed draws.
   Missing draws beyond the cap are inconclusive. Pass dumps include sampled depth
   textures but color attachments only in manifest.jsonl, not every depth write.
   Capture a reference using the same save, timing and NXBOX_PASS_DUMP_FRAME on
   a working Vulkan build; collect its passdump under a separate directory.
   Run `python3 tools/nxbox/pass_diff.py reference native --out shadow-diff`.
   Inspect the first divergent color pass, its sampled depth raw data and the
   matching D3D12_DRAW_TRACE PSO bias/DSV values. The diff tool cannot decode all
   packed depth formats; retain raw data for direct depth analysis. If the map
   is already corrupt, inspect depth write/clear/format and bias; if it is clean,
   inspect comparison sampling, coordinates and border at the consuming pass.

## PSO compile policy knobs

- `NXBOX_D3D12_PREWARM_THREADS`: integer `1`–`6`, default `2`; invalid values use `2`. Sets PSO compile parallelism only while the loading screen waits for disk warmup, then restores two workers to limit gameplay compiler memory.
- `NXBOX_D3D12_SKIP_GLOBAL_FALLBACK_COMPUTE`: defaults to ON for `NXBOX_UWP`, OFF elsewhere; `0` disables it. Rejects compute programs with global-memory fallback before DXC or worker admission to avoid Xbox driver hangs; graphics stages are unaffected. With this protection enabled, compute translation runs before submission and its IR is retained for the worker. Disk prewarm checks the equivalent helper declaration in persisted HLSL, since older metadata does not store `uses_global_memory`.
- `NXBOX_D3D12_PSO_WORKER_PRIORITY`: `lowest`, `below`, or `normal`, default `below`; invalid values use `below`. The pool applies the base priority and promotes queued keys and boosts running keys to above normal while the GPU thread waits, restoring base priority when the job finishes or its last waiter leaves.
- `NXBOX_D3D12_COMPILE_HEADROOM_MB`: default `448` MiB on `NXBOX_UWP`, disabled elsewhere; `0` disables it. Defers new graphics, compute, and prewarm async compiles below this available app-memory threshold; keys remain Missing for later frame retries. UWP memory readings are cached for 100 ms and deferral diagnostics are limited to one per five seconds.
- `NXBOX_D3D12_PSO_TRACE`: default OFF. Enables graphics/compute `PSO_WAIT` diagnostics with kind, elapsed milliseconds, status, and thread, plus compute phase marks with elapsed milliseconds, to distinguish waiting from compiler work.
