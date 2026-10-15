// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include "video_core/renderer_d3d12/d3d12_descriptor.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <fmt/format.h>
#include "common/alignment.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/textures/texture.h"

namespace D3D12 {
using Tegra::Texture::DepthCompareFunc;
using Tegra::Texture::TextureFilter;
using Tegra::Texture::TextureMipmapFilter;
using Tegra::Texture::WrapMode;

DXGI_FORMAT DsvFormat(DXGI_FORMAT view_format) {
    switch (view_format) {
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
        return view_format;
    }
}

namespace {

struct SrvRange {
    UINT mip, levels, first, size, plane;
};

std::optional<SrvRange> ReadSrvRange(const D3D12_SHADER_RESOURCE_VIEW_DESC& d) {
    switch (d.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        return SrvRange{d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels, 0, 1, 0};
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        return SrvRange{d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels,
                        d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, 0};
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        return SrvRange{d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels, 0, 1,
                        d.Texture2D.PlaneSlice};
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        return SrvRange{d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels,
                        d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize,
                        d.Texture2DArray.PlaneSlice};
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        return SrvRange{d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels, 0, 6, 0};
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        return SrvRange{d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels,
                        d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes * 6, 0};
    default:
        return std::nullopt;
    }
}

} // namespace

// The shader's declared texture type, not the guest view type, decides the SRV
// dimension: a mismatch is undefined in D3D12. vkd3d tolerates it, but the
// Xbox driver samples zeros (SM3DW lost its ambient light that way).
std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> AdaptSrvDimension(
    const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, D3D12_SRV_DIMENSION wanted,
    D3D12_RESOURCE_DIMENSION resource) {
    if (desc.ViewDimension == wanted)
        return desc;
    const auto range = ReadSrvRange(desc);
    if (!range)
        return std::nullopt;
    const auto [mip, levels, first, size, plane] = *range;
    const bool is_1d = resource == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is_2d = resource == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    auto out = desc;
    out.ViewDimension = wanted;
    switch (wanted) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        if (!is_1d || first != 0)
            return std::nullopt;
        out.Texture1D = {mip, levels, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        if (!is_1d)
            return std::nullopt;
        out.Texture1DArray = {mip, levels, first, size, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        if (!is_2d || first != 0)
            return std::nullopt;
        out.Texture2D = {mip, levels, plane, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        if (!is_2d)
            return std::nullopt;
        out.Texture2DArray = {mip, levels, first, size, plane, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        if (!is_2d || first != 0 || size < 6)
            return std::nullopt;
        out.TextureCube = {mip, levels, 0.0f};
        return out;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        if (!is_2d || size < 6)
            return std::nullopt;
        out.TextureCubeArray = {mip, levels, first, size / 6, 0.0f};
        return out;
    default:
        return std::nullopt;
    }
}

namespace {
D3D12_TEXTURE_ADDRESS_MODE AddressMode(WrapMode mode) {
    switch (mode) {
    case WrapMode::Wrap:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case WrapMode::Mirror:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case WrapMode::Border:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case WrapMode::MirrorOnceClampToEdge:
    case WrapMode::MirrorOnceBorder:
    case WrapMode::MirrorOnceClampOGL:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: // ClampToEdge, Clamp
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}
} // namespace

D3D12_SAMPLER_DESC MakeSamplerDesc(const Tegra::Texture::TSCEntry& entry) {
    D3D12_SAMPLER_DESC desc{};
    const u32 min_linear = entry.min_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mag_linear = entry.mag_filter == TextureFilter::Linear ? 1 : 0;
    const u32 mip_linear = entry.mipmap_filter == TextureMipmapFilter::Linear ? 1 : 0;
    u32 reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (entry.depth_compare_enabled) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Min) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (entry.reduction_filter == Tegra::Texture::SamplerReduction::Max) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const float anisotropy = std::clamp(entry.MaxAnisotropy(), 1.0f, 16.0f);
    desc.Filter = anisotropy > 1.0f
                      ? static_cast<D3D12_FILTER>(D3D12_ANISOTROPIC_FILTERING_BIT |
                                                  D3D12_ENCODE_BASIC_FILTER(1, 1, 1, reduction))
                      : static_cast<D3D12_FILTER>(D3D12_ENCODE_BASIC_FILTER(min_linear, mag_linear,
                                                                            mip_linear, reduction));
    desc.AddressU = AddressMode(entry.wrap_u);
    desc.AddressV = AddressMode(entry.wrap_v);
    desc.AddressW = AddressMode(entry.wrap_p);
    desc.MipLODBias = entry.LodBias();
    desc.MaxAnisotropy = static_cast<UINT>(anisotropy);
    desc.ComparisonFunc = entry.depth_compare_enabled
                              ? static_cast<D3D12_COMPARISON_FUNC>(
                                    static_cast<u32>(entry.depth_compare_func.Value()) + 1)
                              : D3D12_COMPARISON_FUNC_NEVER;
    const auto border = entry.srgb_conversion ? entry.SrgbBorderColor() : entry.BorderColor();
    std::copy(border.begin(), border.end(), desc.BorderColor);
    // yuzu convention: no mip filter means "sample level 0 only". Match Vulkan and zero
    // MinLOD too; MinLOD > MaxLOD is undefined and AMD drivers sample zeros.
    const bool no_mips = entry.mipmap_filter == TextureMipmapFilter::None;
    desc.MaxLOD = no_mips ? 0.25f : entry.MaxLod();
    desc.MinLOD = no_mips ? 0.0f : std::min(entry.MinLod(), desc.MaxLOD);
    return desc;
}

D3D12DescriptorAllocator::D3D12DescriptorAllocator(Context& context, u32 view_capacity,
                                                   u32 sampler_capacity)
    : context{context}, views{view_capacity}, samplers{sampler_capacity} {
    if (!view_capacity || view_capacity > ViewHeapSize || !sampler_capacity ||
        sampler_capacity > SamplerHeapSize)
        throw std::invalid_argument("D3D12 descriptor heap capacity outside supported limits");
    Init(views, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, view_capacity, true);
    Init(samplers, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_capacity, true);
    Init(rtvs, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvHeapSize, false);
    Init(dsvs, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DsvHeapSize, false);
    InitCache(view_cache, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
    InitCache(sampler_cache, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
    InitCache(bound_rtvs, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8);
    InitCache(bound_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
}

void D3D12DescriptorAllocator::InitCache(DescriptorCache& cache, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                         u32 capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    Check(context.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cache.heap)),
          "create CPU descriptor cache", context.Device());
    cache.stride = context.Device()->GetDescriptorHandleIncrementSize(type);
    cache.capacity = capacity;
}
void D3D12DescriptorAllocator::RefreshCaches() {
    if (cache_tick == context.CurrentTick())
        return;
    cache_tick = context.CurrentTick();
    slot_contents.clear();
    // Cached views hold their resource, so they stay valid across ticks. Clearing them on every
    // submit (~30 per frame in BotW) recreated ~11k descriptors per frame; once per frame bounds
    // how long they keep freed resources alive. Samplers reference no resources at all.
    if (new_frame) {
        new_frame = false;
        view_cache.entries.clear();
        view_cache.cursor = 0;
    }
}
DescriptorKey D3D12DescriptorAllocator::MakeDescriptorKey(const ResourcePtr& resource, u32 kind,
                                                          const void* desc, size_t size) {
    DescriptorKey key;
    if (size > key.description.size())
        throw std::invalid_argument("D3D12 descriptor exceeds cache key size");
    key.resource = reinterpret_cast<std::uintptr_t>(resource ? resource->handle.Get() : nullptr);
    key.kind = kind;
    key.size = static_cast<u32>(size);
    std::memcpy(key.description.data(), desc, size);
    return key;
}

void D3D12DescriptorAllocator::WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           const ResourcePtr& resource, u32 kind, const void* desc,
                                           size_t size, const DescriptorTable& table, u32 slot) {
    if (table.plan) {
        const u32 index = table.first + slot;
        const DescriptorKey key = MakeDescriptorKey(resource, kind, desc, size);
        table.plan->keys.at(index) = key;
        table.plan->resources.at(index) = resource;
        return;
    }
    WriteCached(type, resource, kind, desc, size, table.Slot(slot));
}

bool D3D12DescriptorAllocator::CanReuseDrawTable(const DescriptorPlan& plan, bool sampler) const {
    const auto& previous = previous_draw_tables[sampler ? 1 : 0];
    return previous.tick == context.CurrentTick() && previous.plan.sizes == plan.sizes &&
           previous.plan.keys == plan.keys;
}

DescriptorTable D3D12DescriptorAllocator::FinishDrawTable(DescriptorPlan& plan, bool sampler) {
    auto& previous = previous_draw_tables[sampler ? 1 : 0];
    if (CanReuseDrawTable(plan, sampler)) {
        context.Profile().Count(FrameCount::DescriptorTableReuses);
        return previous.table;
    }
    const u32 count = static_cast<u32>(plan.keys.size());
    const auto table = sampler ? AllocateSamplers(count) : AllocateViews(count);
    const auto type =
        sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    for (u32 slot = 0; slot < count; ++slot) {
        const auto& key = plan.keys[slot];
        WriteCached(type, plan.resources[slot], key.kind, key.description.data(), key.size,
                    table.Slot(slot));
    }
    previous.tick = context.CurrentTick();
    previous.plan = std::move(plan); // Keep resources alive while comparing their addresses.
    previous.table = table;
    return table;
}

void D3D12DescriptorAllocator::WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           const ResourcePtr& resource, u32 kind, const void* desc,
                                           size_t size, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    RefreshCaches();
    const auto key = MakeDescriptorKey(resource, kind, desc, size);
    const auto old_contents = slot_contents.find(destination.ptr);
    if (old_contents != slot_contents.end() && old_contents->second == key)
        return; // The slot has not changed; never overwrite an in-flight table.
    auto& cache = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? sampler_cache : view_cache;
    auto found = cache.entries.find(key);
    if (found == cache.entries.end()) {
        if (cache.cursor == cache.capacity) {
            // CPU descriptors were copied immediately; no fence is needed to recycle them.
            cache.entries.clear();
            slot_contents.clear();
            cache.cursor = 0;
        }
        auto handle = cache.heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(cache.cursor++) * cache.stride;
        switch (kind) {
        case 0: {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            std::memcpy(&cbv, desc, sizeof(cbv));
            context.Device()->CreateConstantBufferView(cbv.BufferLocation ? &cbv : nullptr, handle);
            break;
        }
        case 1: {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            std::memcpy(&srv, desc, sizeof(srv));
            context.Device()->CreateShaderResourceView(resource ? resource->handle.Get() : nullptr,
                                                       &srv, handle);
            break;
        }
        case 2: {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            std::memcpy(&uav, desc, sizeof(uav));
            context.Device()->CreateUnorderedAccessView(resource ? resource->handle.Get() : nullptr,
                                                        nullptr, &uav, handle);
            break;
        }
        case 3: {
            D3D12_SAMPLER_DESC sampler{};
            std::memcpy(&sampler, desc, sizeof(sampler));
            context.Device()->CreateSampler(&sampler, handle);
            break;
        }
        default:
            throw std::invalid_argument("D3D12 unknown cached descriptor kind");
        }
        context.Profile().Count(FrameCount::DescriptorWrites);
        constexpr std::array by_kind{FrameCount::DescriptorWritesCbv, FrameCount::DescriptorWritesSrv,
                                     FrameCount::DescriptorWritesUav,
                                     FrameCount::DescriptorWritesSampler};
        context.Profile().Count(by_kind[kind]);
        found = cache.entries.emplace(key, CachedDescriptor{handle, resource}).first;
    }
    context.Device()->CopyDescriptorsSimple(1, destination, found->second.handle, type);
    context.Profile().Count(FrameCount::DescriptorCopies);
    slot_contents[destination.ptr] = std::move(key);
}
std::optional<DescriptorTable> D3D12DescriptorAllocator::FindTable(std::string_view key,
                                                                   bool sampler) {
    RefreshCaches();
    return table_cache[sampler ? 1 : 0].Find(context.CurrentTick(), key);
}
void D3D12DescriptorAllocator::RememberTable(std::string key, DescriptorTable table, bool sampler) {
    RefreshCaches();
    table_cache[sampler ? 1 : 0].Remember(context.CurrentTick(), std::move(key), table);
}
void D3D12DescriptorAllocator::WriteUavDescriptor(const DescriptorTable& table, u32 slot,
                                                  const ResourcePtr& resource,
                                                  const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc) {
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, 2, &desc, sizeof(desc), table,
                slot);
}
void D3D12DescriptorAllocator::Init(Ring& ring, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity,
                                    bool visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags =
        visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Diagnostic("D3D12 descriptor heap type=" + std::to_string(static_cast<u32>(type)) +
               " capacity=" + std::to_string(capacity) + " visible=" + std::to_string(visible));
    Check(context.Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&ring.heap)),
          "CreateDescriptorHeap", context.Device());
    ring.stride = context.Device()->GetDescriptorHandleIncrementSize(type);
    ring.cpu = ring.heap->GetCPUDescriptorHandleForHeapStart();
    ring.shader_visible = visible;
    if (visible)
        ring.gpu = ring.heap->GetGPUDescriptorHandleForHeapStart();
}

void D3D12DescriptorAllocator::Reserve(u32 view_count, u32 sampler_count, u32 rtv_count,
                                       u32 dsv_count) {
    const std::array rings{&views, &samplers, &rtvs, &dsvs};
    const std::array counts{view_count, sampler_count, rtv_count, dsv_count};
    for (size_t i = 0; i < rings.size(); ++i) {
        if (counts[i] > rings[i]->math.Capacity())
            throw std::runtime_error("D3D12 descriptor batch larger than its heap");
    }
    for (;;) {
        // Retire callbacks before the preflight, then keep this recording open.
        // Every subsequent allocation in this batch must use the same fence tick.
        context.Commands();
        bool ready = true;
        for (size_t i = 0; i < rings.size(); ++i) {
            auto& ring = *rings[i];
            while (const auto oldest = ring.math.OldestTick()) {
                if (!context.IsFree(*oldest))
                    break;
                ring.math.Retire(*oldest);
            }
            ready &= counts[i] == 0 || ring.math.CanAllocate(counts[i]);
        }
        if (ready)
            return;
        // Submit the producer if needed, then retire only the oldest blocking
        // allocation rather than draining every in-flight command allocator.
        u64 oldest_blocked = context.CurrentTick();
        for (size_t i = 0; i < rings.size(); ++i) {
            if (counts[i] && !rings[i]->math.CanAllocate(counts[i]))
                oldest_blocked = std::min(oldest_blocked, *rings[i]->math.OldestTick());
        }
        if (oldest_blocked == context.CurrentTick())
            context.Submit(FrameCount::ForcedSubmitDescriptorExhaustion);
        context.Wait(oldest_blocked);
    }
}

void D3D12DescriptorAllocator::SetHeaps(ID3D12GraphicsCommandList* commands,
                                        bool cache_draw_state) {
    const std::array heaps{views.heap.Get(), samplers.heap.Get()};
    if (cache_draw_state) {
        auto& state = context.DrawState();
        if (!context.RecordState(state.heaps.Update(heaps)))
            return;
        // Changing descriptor heaps invalidates descriptor tables. Clearing all
        // root arguments is conservative and keeps their tracking simple.
        state.ClearRootArguments();
    } else {
        context.InvalidateRecordedState();
    }
    commands->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
}

DescriptorTable D3D12DescriptorAllocator::Allocate(Ring& ring, u32 count) {
    if (!count || count > ring.math.Capacity())
        throw std::invalid_argument("D3D12 invalid descriptor allocation size");
    for (;;) {
        context.Commands();
        while (const auto oldest = ring.math.OldestTick()) {
            if (!context.IsFree(*oldest))
                break;
            ring.math.Retire(*oldest);
        }
        if (const auto offset = ring.math.Allocate(count, context.CurrentTick())) {
            DescriptorTable table;
            table.count = count;
            table.stride = ring.stride;
            table.cpu = {ring.cpu.ptr + static_cast<SIZE_T>(*offset) * ring.stride};
            if (ring.shader_visible)
                table.gpu = {ring.gpu.ptr + static_cast<UINT64>(*offset) * ring.stride};
            return table;
        }
        const auto oldest = ring.math.OldestTick();
        if (!oldest)
            throw std::runtime_error("D3D12 descriptor table larger than its heap");
        // Everything live belongs to the open command list: close it so it can retire.
        if (*oldest >= context.CurrentTick())
            context.Submit(FrameCount::ForcedSubmitDescriptorExhaustion);
        else
            context.Wait(*oldest, "descriptor");
    }
}

DescriptorTable D3D12DescriptorAllocator::AllocateViews(u32 count) {
    return Allocate(views, count);
}
DescriptorTable D3D12DescriptorAllocator::AllocateSamplers(u32 count) {
    return Allocate(samplers, count);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateRtv(u32 count) {
    return Allocate(rtvs, count).cpu;
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorAllocator::AllocateDsv() {
    return Allocate(dsvs, 1).cpu;
}

void D3D12DescriptorAllocator::WriteNullCbv(const DescriptorTable& table, u32 slot) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {}, 0, &desc, sizeof(desc), table, slot);
}
void D3D12DescriptorAllocator::WriteNullSrv(const DescriptorTable& table, u32 slot,
                                            D3D12_SRV_DIMENSION dimension) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = dimension;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dimension) {
    case D3D12_SRV_DIMENSION_BUFFER:
        desc.Buffer.NumElements = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.Texture1D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.Texture1DArray.MipLevels = desc.Texture1DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.Texture2DArray.MipLevels = desc.Texture2DArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        desc.Texture2DMSArray.ArraySize = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.Texture3D.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        desc.TextureCube.MipLevels = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        desc.TextureCubeArray.MipLevels = desc.TextureCubeArray.NumCubes = 1;
        break;
    default:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        break;
    }
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, {}, 1, &desc, sizeof(desc), table, slot);
}
void D3D12DescriptorAllocator::WriteNullUav(const DescriptorTable& table, u32 slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    WriteUavDescriptor(table, slot, {}, desc);
}
void D3D12DescriptorAllocator::WriteNullSampler(const DescriptorTable& table, u32 slot) {
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {}, 3, &desc, sizeof(desc), table, slot);
}

void D3D12DescriptorAllocator::WriteCbv(const DescriptorTable& table, u32 slot,
                                        const BoundBuffer& buffer) {
    if (!buffer.resource || !buffer.size) {
        WriteNullCbv(table, slot);
        return;
    }
    const u64 width = buffer.resource->handle->GetDesc().Width;
    u64 size = Common::AlignUp<u64>(buffer.size, 256);
    // Diagnostic: view the whole 64 KiB the HLSL cbuffer declares, so reads past the guest
    // size see memory instead of the zeros AMD returns for out-of-bounds CBV reads.
    static const bool full_cbv = BisectEnabled("NXBOX_D3D12_CBV_FULL");
    if (full_cbv)
        size = 65536;
    if (buffer.offset >= width) {
        size = 0;
    } else if (buffer.offset + size > width) {
        // CBV ranges must be 256-byte multiples: drop the (sub-256) tail past the allocation.
        size = (width - buffer.offset) & ~u64{255};
    }
    if (!size) {
        WriteNullCbv(table, slot);
        return;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = buffer.resource->handle->GetGPUVirtualAddress() + buffer.offset;
    desc.SizeInBytes = static_cast<UINT>(size);
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, buffer.resource, 0, &desc, sizeof(desc),
                table, slot);
}

void D3D12DescriptorAllocator::WriteStorageUav(const DescriptorTable& table, u32 slot,
                                               const BoundBuffer& buffer) {
    const u32 size = buffer.size & ~3u;
    if (!buffer.resource || !size) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / 4;
    desc.Buffer.NumElements = size / 4;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    WriteUavDescriptor(table, slot, buffer.resource, desc);
}

void D3D12DescriptorAllocator::WriteTexelSrv(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format =
        MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullSrv(table, slot, D3D12_SRV_DIMENSION_BUFFER);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, buffer.resource, 1, &desc, sizeof(desc),
                table, slot);
}

void D3D12DescriptorAllocator::WriteTexelUav(const DescriptorTable& table, u32 slot,
                                             const BoundBuffer& buffer) {
    const auto format =
        MaxwellFormat(static_cast<VideoCore::Surface::PixelFormat>(buffer.pixel_format));
    if (!buffer.resource || !format.Supported() || !format.bytes_per_block ||
        buffer.size < format.bytes_per_block) {
        WriteNullUav(table, slot);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format.view_format);
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = buffer.offset / format.bytes_per_block;
    desc.Buffer.NumElements = buffer.size / format.bytes_per_block;
    WriteUavDescriptor(table, slot, buffer.resource, desc);
}

bool D3D12DescriptorAllocator::WriteImageSrv(const DescriptorTable& table, u32 slot,
                                             const ImageView& view,
                                             D3D12_SRV_DIMENSION fallback_dimension,
                                             const ResourcePtr& replacement) {
    const auto resource = replacement ? replacement : view.ImageHandle();
    if (!resource) {
        WriteNullSrv(table, slot, fallback_dimension);
        return false;
    }
    auto desc = view.Description();
    if (desc.ViewDimension != fallback_dimension) {
        if (const auto adapted = AdaptSrvDimension(desc, fallback_dimension,
                                                   resource->handle->GetDesc().Dimension)) {
            desc = *adapted;
        } else if (logged_srv_mismatches.insert(desc.ViewDimension * 16 + fallback_dimension)
                       .second) {
            Diagnostic(fmt::format("D3D12 SRV dimension {} cannot become shader dimension {}",
                                   static_cast<int>(desc.ViewDimension),
                                   static_cast<int>(fallback_dimension)));
        }
    }
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, resource, 1, &desc, sizeof(desc), table,
                slot);
    return true;
}

void D3D12DescriptorAllocator::WriteSampler(const DescriptorTable& table, u32 slot,
                                            const Sampler& sampler) {
    const auto desc = MakeSamplerDesc(sampler.Entry());
    WriteCached(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, {}, 3, &desc, sizeof(desc), table, slot);
}

void D3D12DescriptorAllocator::CreateRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                         const ImageView* view_ptr, const ResourcePtr& replacement,
                                         u32 layer_count) {
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    context.Profile().Count(FrameCount::DescriptorWrites);
    const auto resource = replacement ? replacement : view_ptr ? view_ptr->ImageHandle() : nullptr;
    if (!resource) {
        // Null render target: a valid RTV that discards writes.
        D3D12_RENDER_TARGET_VIEW_DESC null_desc{};
        null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        null_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        context.Device()->CreateRenderTargetView(nullptr, &null_desc, handle);
        return;
    }
    const ImageView& view = *view_ptr;
    const auto desc = resource->handle->GetDesc();
    const UINT level = replacement ? 0 : static_cast<UINT>(view.range.base.level);
    const UINT layer = replacement ? 0 : static_cast<UINT>(view.range.base.layer);
    const UINT layers = layer_count ? layer_count : static_cast<UINT>(view.range.extent.layers);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = view.Description().Format;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtv.Texture3D = {level, 0, static_cast<UINT>(-1)};
    } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        rtv.Texture1DArray = {level, layer, layers};
    } else if (desc.SampleDesc.Count > 1) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rtv.Texture2DMSArray = {layer, layers};
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray = {level, layer, layers, 0};
    }
    context.Device()->CreateRenderTargetView(resource->handle.Get(), &rtv, handle);
}

void D3D12DescriptorAllocator::CreateDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ImageView& view,
                                         D3D12_DSV_FLAGS flags, u32 layer_count) {
    const FrameProfile::Scope profile_write{context.Profile(), FramePhase::DescriptorWrites};
    context.Profile().Count(FrameCount::DescriptorWrites);
    const auto resource = view.ImageHandle();
    if (!resource)
        throw std::runtime_error("D3D12 depth view without image");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DsvFormat(view.Description().Format);
    dsv.Flags = flags;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    if (resource->handle->GetDesc().SampleDesc.Count > 1) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dsv.Texture2DMSArray = {static_cast<UINT>(view.range.base.layer),
                                layer_count ? layer_count
                                            : static_cast<UINT>(view.range.extent.layers)};
    } else {
        dsv.Texture2DArray = {
            static_cast<UINT>(view.range.base.level), static_cast<UINT>(view.range.base.layer),
            layer_count ? layer_count : static_cast<UINT>(view.range.extent.layers)};
    }
    context.Device()->CreateDepthStencilView(resource->handle.Get(), &dsv, handle);
}
bool D3D12DescriptorAllocator::WriteImageUav(const DescriptorTable& table, u32 slot,
                                             const ImageView& view, DXGI_FORMAT format) {
    if (!view.ImageHandle()) {
        WriteNullUav(table, slot);
        return false;
    }
    if (!(view.ImageHandle()->handle->GetDesc().Flags &
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        view.LogUnsupported(
            "storage image lacks ALLOW_UNORDERED_ACCESS (MSAA UAVs are unsupported)");
        WriteNullUav(table, slot);
        return false;
    }
    const auto& srv = view.Description();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = format;
    switch (srv.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        desc.Texture1D.MipSlice = srv.Texture1D.MostDetailedMip;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {srv.Texture1DArray.MostDetailedMip,
                               srv.Texture1DArray.FirstArraySlice, srv.Texture1DArray.ArraySize};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {srv.Texture2D.MostDetailedMip, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {srv.Texture2DArray.MostDetailedMip,
                               srv.Texture2DArray.FirstArraySlice, srv.Texture2DArray.ArraySize, 0};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {srv.Texture3D.MostDetailedMip, 0, static_cast<UINT>(-1)};
        break;
    default:
        throw std::runtime_error("D3D12 unsupported storage image dimension");
    }
    WriteUavDescriptor(table, slot, view.ImageHandle(), desc);
    return true;
}
} // namespace D3D12
