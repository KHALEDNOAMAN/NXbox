// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

#ifdef _WIN32
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace D3D12 {
class FrameProfile;
using DiskBytes = std::vector<std::uint8_t>;

// Bump emitter/DXC/schema versions whenever generated code, compiler options,
// root bindings or the caller's pointer-free PSO recipe encoding changes.
struct DiskCacheIdentity {
    std::uint32_t emitter_version{}, dxc_version{}, schema_version{};
    std::uint32_t vendor_id{}, device_id{}, subsystem_id{}, revision{};
    std::uint64_t adapter_luid{}, driver_version{};
    bool operator==(const DiskCacheIdentity&) const = default;
};
enum class DiskRecordKind : std::uint32_t {
    Dxil = 1,
    Graphics = 2,
    Compute = 3,
    Library = 4,
    NativeGraphics = 5,
    NativeCompute = 6
};
struct DiskPipelineRecord {
    DiskRecordKind kind{};
    std::uint64_t key{};
    DiskBytes payload;
    // Packed guest request identity and pointer-free draw/dispatch binding metadata.
    DiskBytes request_key{};
    DiskBytes runtime_data{};
};

// Std-only, little-endian, bounded, checksummed format. A miss includes stale,
// truncated and corrupt data; no native struct padding or pointers are persisted.
DiskBytes SerializeDiskRecord(const DiskCacheIdentity& identity, const DiskPipelineRecord& record);
std::optional<DiskPipelineRecord> DeserializeDiskRecord(std::span<const std::uint8_t> bytes,
                                                        const DiskCacheIdentity& identity);

enum class DiskWarmupStage { Begin, Build, Complete, Cancelled, Failed };
struct DiskWarmupProgress {
    DiskWarmupStage stage{};
    std::size_t completed{}, total{}, failed{};
};

// Integration for d3d12_pipeline_cache.cpp:
// 1. Own one instance per title. Pass ApplicationData::Current().LocalFolder().Path()
//    (LocalState), title ID and identity. On Windows pass the device; use
//    QueryIdentity to obtain its adapter/driver fields (failure disables disk reuse).
// 2. Hash the final HLSL PLUS stage target, entry point, DXC flags and specialization
//    using the caller's stable hash. Try FindDxil before DXC, then StoreDxil ONLY
//    after successful DXC validation. A DXIL hit must still pass normal validation.
// 3. Hash all fixed PSO state, root schema and stage hashes; call LoadOrCreateGraphics
//    / Compute with the live desc and a versioned recipe containing state and DXIL
//    keys. Recipes MUST contain no pointers/COM handles; reconstruct root signatures,
//    input layouts and bytecode from them. Never persist raw D3D12_*_DESC structs.
// 4. From LoadDiskResources call StartWarmup; its build callback decodes each recipe,
//    loads stage DXIL and invokes LoadOrCreate*, then safely publishes the resulting
//    PSO into the main cache. Protect that cache against simultaneous foreground work.
//    Map Begin/Build to UI LoadCallbackStage::Build; marshal UI changes to its thread.
//    Callbacks run on the worker without the disk/library lock. Return false on a
//    rejected recipe; failed entries do not prevent remaining pipelines warming.
// 5. StopWarmup before changing titles or destroying callback captures; Flush on
//    suspend/checkpoints and after warmup. Destruction joins and flushes. Lifecycle
//    calls (Start/Stop/destruction) belong to the owner thread, never a callback.
//    Missing/unsupported pipeline libraries fall back to normal PSO creation.
class DiskPipelineCache {
public:
    using BuildCallback = std::function<bool(const DiskPipelineRecord&, std::stop_token)>;
    using ProgressCallback = std::function<void(const DiskWarmupProgress&)>;
    DiskPipelineCache(const std::filesystem::path& local_state, std::uint64_t title_id,
                      DiskCacheIdentity identity);
#ifdef _WIN32
    DiskPipelineCache(const std::filesystem::path& local_state, std::uint64_t title_id,
                      DiskCacheIdentity identity, ID3D12Device* device);
    static std::optional<DiskCacheIdentity> QueryIdentity(ID3D12Device* device,
                                                          DiskCacheIdentity versions);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LoadOrCreateGraphics(
        std::uint64_t key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        std::span<const std::uint8_t> recipe, FrameProfile* profile = nullptr);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LoadOrCreateCompute(
        std::uint64_t key, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
        std::span<const std::uint8_t> recipe, FrameProfile* profile = nullptr);
#endif
    ~DiskPipelineCache();
    DiskPipelineCache(const DiskPipelineCache&) = delete;
    DiskPipelineCache& operator=(const DiskPipelineCache&) = delete;
    std::optional<DiskBytes> FindDxil(std::uint64_t hlsl_hash);
    bool StoreDxil(std::uint64_t hlsl_hash, std::span<const std::uint8_t> validated_dxil);
    bool RememberPipeline(const DiskPipelineRecord& record);
    void StartWarmup(BuildCallback build, ProgressCallback progress,
                     std::stop_token stop_loading = {});
    // Owner thread: join without cancelling; all progress callbacks finish before return.
    void WaitWarmup();
    void StopWarmup();
    bool Flush();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace D3D12
