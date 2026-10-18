// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_dispatch.h"

#include <algorithm>

#include "video_core/renderer_d3d12/d3d12_root_signature.h"

namespace D3D12 {
ComPtr<ID3D12RootSignature> CreateComputeRootSignature(ID3D12Device* device,
                                                       ComputeBindingCounts counts) {
    const std::array types{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                           D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER};
    const std::array sizes{counts.cbv, counts.srv, counts.uav, counts.sampler};
    std::array<D3D12_DESCRIPTOR_RANGE, 4> ranges{};
    std::array<D3D12_ROOT_PARAMETER, 5> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 1, ComputeRootDwords};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        ranges[i] = {types[i], std::max(1u, sizes[i]), 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i + 1].DescriptorTable = {1, &ranges[i]};
        parameters[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob, errors;
    Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
          "serialize compute root signature", device);
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)),
          "create compute root signature", device);
    return root;
}
ComputeDispatch::ComputeDispatch(Context& context) : context{context} {
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = 12;
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &argument;
    Check(
        context.Device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&indirect_signature)),
        "create indirect compute signature", context.Device());
}
void ComputeDispatch::UavBarrier() {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    // Global ordering also covers aliasing guest storage descriptors.
    const FrameProfile::Scope profile_barrier{context.Profile(), FramePhase::Barriers};
    context.Commands()->ResourceBarrier(1, &barrier);
    context.Profile().Count(FrameCount::Barriers);
}
void ComputeDispatch::Bind(const ComputeBindings& bindings) {
    if (!bindings.pipeline || !bindings.root_signature || !bindings.resource_heap ||
        !bindings.sampler_heap)
        throw std::invalid_argument("D3D12 incomplete compute bindings");
    const auto resource_heap = bindings.resource_heap->GetDesc();
    const auto sampler_heap = bindings.sampler_heap->GetDesc();
    if (resource_heap.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        sampler_heap.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
        !(resource_heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
        !(sampler_heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
        throw std::invalid_argument("D3D12 invalid compute descriptor heaps");
    for (std::size_t i = bindings.root_cbvs ? 1u : 0u; i < bindings.tables.size(); ++i) {
        auto* heap = i == 3 ? bindings.sampler_heap.Get() : bindings.resource_heap.Get();
        const auto desc = heap->GetDesc();
        const auto start = heap->GetGPUDescriptorHandleForHeapStart().ptr;
        const auto stride = context.Device()->GetDescriptorHandleIncrementSize(desc.Type);
        const auto table = bindings.tables[i].ptr;
        if (!table || table < start || (table - start) % stride ||
            (table - start) / stride >= desc.NumDescriptors)
            throw std::invalid_argument("D3D12 compute table is outside its descriptor heap");
    }
    for (const auto& use : bindings.resources) {
        if (!use.resource)
            throw std::invalid_argument("D3D12 missing compute resource");
        if ((use.state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            (use.resource->heap != D3D12_HEAP_TYPE_DEFAULT ||
             !(use.resource->handle->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            throw std::invalid_argument("D3D12 compute storage requires a DEFAULT UAV resource");
        // A whole-resource tracker cannot represent conflicting views.
        for (const auto& other : bindings.resources)
            if (other.resource == use.resource && other.state != use.state)
                throw std::invalid_argument("D3D12 conflicting compute resource states");
    }
    static const bool fast_roots = BisectEnabled("NXBOX_D3D12_FAST_ROOT_BINDS");
    if (fast_roots)
        context.InvalidateGraphicsState();
    else
        context.InvalidateRecordedState();
    auto* commands = context.Commands();
    // Commands() may retire the previous recording before these objects are held.
    context.AfterCompletion([pipeline = bindings.pipeline, root = bindings.root_signature,
                             resources = bindings.resource_heap, samplers = bindings.sampler_heap,
                             signature = indirect_signature] {});
    commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    static const bool fast_barriers = BisectEnabled("NXBOX_D3D12_FAST_BARRIERS");
    if (!fast_barriers)
        UavBarrier();
    std::vector<std::pair<ResourcePtr, D3D12_RESOURCE_STATES>> transitions;
    transitions.reserve(bindings.resources.size());
    for (const auto& use : bindings.resources)
        transitions.emplace_back(use.resource, use.state);
    context.TransitionBatch(transitions, fast_barriers);
    ID3D12DescriptorHeap* heaps[]{bindings.resource_heap.Get(), bindings.sampler_heap.Get()};
    commands->SetDescriptorHeaps(2, heaps);
    auto& roots = context.ComputeRoots();
    if (!fast_roots || roots.signature.Update(bindings.root_signature.Get())) {
        commands->SetComputeRootSignature(bindings.root_signature.Get());
        roots.cbvs = {};
    }
    commands->SetPipelineState(bindings.pipeline.Get());
    commands->SetComputeRoot32BitConstants(0, ComputeRootDwords, bindings.constants.data(), 0);
    for (UINT i = bindings.root_cbvs ? 1u : 0u; i < bindings.tables.size(); ++i)
        commands->SetComputeRootDescriptorTable(i + (bindings.root_cbvs ? 0u : 1u),
                                                bindings.tables[i]);
    if (bindings.root_cbvs) {
        for (UINT i = 0; i < bindings.root_cbv_addresses.size(); ++i) {
            // An unchanged slot/address under the same signature retains the same binding.
            if (!fast_roots ||
                roots.cbvs[kFirstRootCbv + i].Update(bindings.root_cbv_addresses[i])) {
                commands->SetComputeRootConstantBufferView(kFirstRootCbv + i,
                                                           bindings.root_cbv_addresses[i]);
                context.Profile().Count(FrameCount::RootCbvBinds);
            }
        }
    }
}
void ComputeDispatch::Direct(const ComputeBindings& bindings, ComputeGroups groups) {
    const FrameProfile::Scope profile_dispatch{context.Profile(), FramePhase::DrawDispatch};
    if (!HasComputeWork(groups))
        return;
    Bind(bindings);
    context.Commands()->Dispatch(groups[0], groups[1], groups[2]);
    context.Profile().Count(FrameCount::Dispatches);
    UavBarrier();
}
void ComputeDispatch::Indirect(const ComputeBindings& bindings, const ResourcePtr& arguments,
                               std::uint64_t offset) {
    if (!arguments || arguments->heap == D3D12_HEAP_TYPE_READBACK ||
        arguments->handle->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset % 4 ||
        offset > arguments->handle->GetDesc().Width ||
        arguments->handle->GetDesc().Width - offset < 12)
        throw std::invalid_argument("D3D12 invalid indirect dispatch range");
    for (const auto& use : bindings.resources)
        if (use.resource == arguments && use.state != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
            throw std::invalid_argument("D3D12 indirect arguments alias compute storage");
    Bind(bindings);
    context.Transition(arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    context.Commands()->ExecuteIndirect(indirect_signature.Get(), 1, arguments->handle.Get(),
                                        offset, nullptr, 0);
    context.Profile().Count(FrameCount::Dispatches);
    UavBarrier();
}
} // namespace D3D12
