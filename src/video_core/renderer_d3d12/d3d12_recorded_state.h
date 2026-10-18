// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace D3D12 {
// Compare object representations: floating-point state must preserve signed zero
// and NaN payloads. Only use this with initialized, trivially copyable API values.
template <class T>
struct RecordedValue {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    bool valid{};
    bool Update(const T& next) {
        if (valid && std::memcmp(&value, &next, sizeof(T)) == 0)
            return false;
        std::memcpy(&value, &next, sizeof(T));
        valid = true;
        return true;
    }
};

template <class T, size_t Capacity>
struct RecordedArray {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<T, Capacity> values{};
    size_t count{};
    bool valid{};
    bool Update(const T* next, size_t next_count) {
        // An oversized future caller must record rather than truncate its state.
        if (next_count > Capacity) {
            valid = false;
            return true;
        }
        if (valid && count == next_count &&
            (next_count == 0 || std::memcmp(values.data(), next, next_count * sizeof(T)) == 0))
            return false;
        if (next_count != 0)
            std::memcpy(values.data(), next, next_count * sizeof(T));
        count = next_count;
        valid = true;
        return true;
    }
};
} // namespace D3D12

#ifdef _WIN32
#include <d3d12.h>

namespace D3D12 {
// Owned by Context, never shared across command lists. All arrays are bounded by
// D3D12 API limits; recording state performs no allocations.
struct RecordedDrawState {
    std::uint64_t generation{};
    RecordedValue<std::array<ID3D12DescriptorHeap*, 2>> heaps;
    RecordedValue<ID3D12RootSignature*> root_signature;
    RecordedValue<ID3D12PipelineState*> pipeline;
    RecordedValue<D3D_PRIMITIVE_TOPOLOGY> topology;
    struct RootArguments {
        std::array<RecordedValue<D3D12_GPU_VIRTUAL_ADDRESS>, 64> tables;
        std::array<RecordedValue<D3D12_GPU_VIRTUAL_ADDRESS>, 64> cbvs;
        std::array<RecordedArray<std::uint32_t, 16>, 64> constants;
    } root_args;
    // Count, contiguous RTV base, DSV handle, DSV presence. The rasterizer also
    // invalidates on fixed RTV/DSV descriptor rewrites, even if handles match.
    RecordedValue<std::array<std::uint64_t, 4>> targets;
    RecordedArray<D3D12_VIEWPORT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports;
    RecordedArray<D3D12_RECT, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors;
    RecordedArray<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
    RecordedValue<D3D12_INDEX_BUFFER_VIEW> index;
    RecordedValue<std::array<UINT, 3>> stencil;
    RecordedValue<std::array<float, 4>> blend;
    RecordedValue<std::array<float, 2>> depth_bounds;
    RecordedArray<D3D12_STREAM_OUTPUT_BUFFER_VIEW, 4> stream_output;

    // One bit per tracked member, for Forget().
    enum Category : std::uint32_t {
        kHeaps = 1u << 0,
        kRootSignature = 1u << 1,
        kPipeline = 1u << 2,
        kTopology = 1u << 3,
        kRootTables = 1u << 4,
        kRootCbvs = 1u << 5,
        kRootConstants = 1u << 6,
        kTargets = 1u << 7,
        kViewports = 1u << 8,
        kScissors = 1u << 9,
        kVertices = 1u << 10,
        kIndex = 1u << 11,
        kStencil = 1u << 12,
        kBlend = 1u << 13,
        kDepthBounds = 1u << 14,
        kStreamOutput = 1u << 15,
    };
    void Refresh(std::uint64_t current_generation) {
        if (generation != current_generation) {
            *this = {};
            generation = current_generation;
        }
    }
    void ClearRootArguments() {
        root_args = {};
    }
    // Makes the next draw record the selected members again and keeps the others.
    void Forget(std::uint32_t categories) {
        if (categories & kHeaps)
            heaps = {};
        if (categories & kRootSignature)
            root_signature = {};
        if (categories & kPipeline)
            pipeline = {};
        if (categories & kTopology)
            topology = {};
        if (categories & kRootTables)
            root_args.tables = {};
        if (categories & kRootCbvs)
            root_args.cbvs = {};
        if (categories & kRootConstants)
            root_args.constants = {};
        if (categories & kTargets)
            targets = {};
        if (categories & kViewports)
            viewports = {};
        if (categories & kScissors)
            scissors = {};
        if (categories & kVertices)
            vertices = {};
        if (categories & kIndex)
            index = {};
        if (categories & kStencil)
            stencil = {};
        if (categories & kBlend)
            blend = {};
        if (categories & kDepthBounds)
            depth_bounds = {};
        if (categories & kStreamOutput)
            stream_output = {};
    }
};
} // namespace D3D12
#endif // _WIN32
