// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_d3d12/d3d12_compute_pass.h"
#include "video_core/renderer_d3d12/d3d12_pso_tables.h"

#include <limits>
#include <string_view>
#include <vector>
#include "video_core/host_shaders/hlsl/astc_decoder_hlsl.h"
#include "video_core/host_shaders/hlsl/block_linear_unswizzle_2d_hlsl.h"
#include "video_core/host_shaders/hlsl/block_linear_unswizzle_3d_bcn_hlsl.h"
#include "video_core/host_shaders/hlsl/block_linear_unswizzle_3d_hlsl.h"
#include "video_core/host_shaders/hlsl/quad_indexed_hlsl.h"
#include "video_core/host_shaders/hlsl/queries_prefix_hlsl.h"
#include "video_core/host_shaders/hlsl/uint8_to_uint16_hlsl.h"
#include "video_core/renderer_d3d12/d3d12_footprint.h"
#include "video_core/renderer_d3d12/d3d12_shader.h"

namespace D3D12 {
namespace {
void ValidateBuffer(const ComputeBufferView& view) {
    if (!view.resource || view.resource->heap != D3D12_HEAP_TYPE_DEFAULT)
        throw std::invalid_argument("D3D12 helper UAV requires a DEFAULT buffer");
    const auto desc = view.resource->handle->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !view.size || view.size % 4 ||
        view.offset % 4 || view.offset > desc.Width || view.size > desc.Width - view.offset ||
        view.size / 4 > UINT32_MAX)
        throw std::invalid_argument("D3D12 invalid helper raw UAV range");
}
void RequireSize(const ComputeBufferView& view, std::uint64_t size) {
    if (size > view.size)
        throw std::out_of_range("D3D12 helper buffer is too small");
}
} // namespace
ComputePasses::ComputePasses(Context& context)
    : context{context}, dispatch{context}, descriptors{context, 4096, 256},
      root_signature{CreateComputeRootSignature(context.Device(), {0, 0, 3, 0})} {
    const std::array sources{HostShaders::ASTC_DECODER_HLSL,
                             HostShaders::BLOCK_LINEAR_UNSWIZZLE_2D_HLSL,
                             HostShaders::BLOCK_LINEAR_UNSWIZZLE_3D_HLSL,
                             HostShaders::BLOCK_LINEAR_UNSWIZZLE_3D_BCN_HLSL,
                             HostShaders::QUERIES_PREFIX_HLSL,
                             HostShaders::UINT8_TO_UINT16_HLSL,
                             HostShaders::QUAD_INDEXED_HLSL};
    const FrameProfile::Scope profile_pso{context.Profile(), FramePhase::Pso};
    ShaderCompiler compiler;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (i == static_cast<size_t>(HelperComputePass::DecodeAstc) &&
            !BisectEnabled("NXBOX_D3D12_GPU_ASTC"))
            continue;
        const auto dxil = compiler.Compile(sources[i], L"main", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_signature.Get();
        desc.CS = {dxil.data(), dxil.size()};
        context.Profile().Count(FrameCount::PsoCompiles);
        CheckPipeline(
            context.Device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[i])),
            "create helper compute pipeline", context.Device(),
            Pso::HashBytes(desc.CS.pShaderBytecode, desc.CS.BytecodeLength));
    }
}
ResourcePtr ComputePasses::UploadInput(const ResourcePtr& staging, std::uint64_t offset,
                                       std::uint64_t size) {
    const FrameProfile::Scope profile_upload{context.Profile(), FramePhase::BufferUpload};
    if (!staging || staging->heap == D3D12_HEAP_TYPE_READBACK || !size || size % 4 || offset % 4)
        throw std::invalid_argument("D3D12 invalid helper upload");
    const auto source = staging->handle->GetDesc();
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > source.Width ||
        size > source.Width - offset)
        throw std::out_of_range("D3D12 helper upload range");
    auto desc = source;
    desc.Width = size;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto input =
        context.CreateResource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    context.Transition(staging, D3D12_RESOURCE_STATE_COPY_SOURCE);
    context.Retain(input);
    context.MarkWork("upload/copy");
    context.Profile().Count(staging->heap == D3D12_HEAP_TYPE_UPLOAD
                                ? FrameCount::StagingCopyBytes
                                : FrameCount::GpuBufferCopyBytes,
                            size);
    context.BeforeCopyWrite(input, 0, size);
    context.Commands()->CopyBufferRegion(input->handle.Get(), 0, staging->handle.Get(), offset,
                                         size);
    return input;
}
void ComputePasses::Run(HelperComputePass pass, ComputeConstants constants, ComputeGroups extent,
                        std::span<const ComputeBufferView> buffers, ComputeTextureView texture) {
    const auto groups = HelperGroupCount(pass, extent);
    if (!HasComputeWork(groups))
        return;
    const bool image = pass == HelperComputePass::DecodeAstc ||
                       pass == HelperComputePass::Unswizzle2D ||
                       pass == HelperComputePass::Unswizzle3D;
    const std::size_t expected = image ? 1 : pass == HelperComputePass::QueriesPrefix ? 3 : 2;
    if (buffers.size() != expected || static_cast<bool>(texture.resource) != image)
        throw std::invalid_argument("D3D12 helper descriptor count/type mismatch");
    for (const auto& view : buffers)
        ValidateBuffer(view);
    // Helpers require disjoint input/output resources. Guest dispatch uses its
    // own alias policy; a helper must not overwrite unread source bytes.
    for (std::size_t i = 0; i < buffers.size(); ++i)
        for (std::size_t j = i + 1; j < buffers.size(); ++j)
            if (buffers[i].resource == buffers[j].resource)
                throw std::invalid_argument("D3D12 helper input/output alias");
    if (image) {
        const auto desc = texture.resource->handle->GetDesc();
        const bool volume = pass == HelperComputePass::Unswizzle3D;
        if (texture.resource == buffers[0].resource ||
            texture.resource->heap != D3D12_HEAP_TYPE_DEFAULT ||
            !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            desc.Dimension != (volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                      : D3D12_RESOURCE_DIMENSION_TEXTURE2D) ||
            texture.descriptor.ViewDimension !=
                (volume ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2DARRAY))
            throw std::invalid_argument("D3D12 helper needs a typed array/volume UAV");
    }
    if (pass == HelperComputePass::DecodeAstc) {
        if (texture.descriptor.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            throw std::invalid_argument("D3D12 ASTC decode requires an RGBA8 UNORM UAV");
        if (constants[0] < 4 || constants[0] > 12 || constants[1] < 4 || constants[1] > 12 ||
            constants[4] > 31 || constants[5] > 5 || constants[6] != (1u << constants[5]) - 1u)
            throw std::invalid_argument("D3D12 invalid ASTC parameters");
    } else if (pass == HelperComputePass::Uint8ToUint16) {
        constants[0] = extent[0];
        RequireSize(buffers[0], (std::uint64_t(extent[0]) + 3) / 4 * 4);
        RequireSize(buffers[1], (std::uint64_t(extent[0]) + 1) / 2 * 4);
    } else if (pass == HelperComputePass::QuadIndexed) {
        if (constants[2] > 2 || constants[3] > 1)
            throw std::invalid_argument("D3D12 invalid quad index parameters");
        constants[0] = extent[0];
        const auto indexes =
            constants[3] ? std::uint64_t(extent[0]) * 2 + 2 : std::uint64_t(extent[0]) * 4;
        RequireSize(buffers[0], ((indexes << constants[2]) + 3) / 4 * 4);
        RequireSize(buffers[1], std::uint64_t(extent[0]) * 24);
    } else if (pass == HelperComputePass::QueriesPrefix) {
        constants[2] = extent[0] - 1;
        if (std::uint64_t(constants[3]) + extent[0] > UINT32_MAX / 8 ||
            (constants[0] < extent[0] && (!constants[1] || constants[1] > extent[0])))
            throw std::invalid_argument("D3D12 invalid prefix accumulation range");
        RequireSize(buffers[0], (std::uint64_t(constants[3]) + extent[0]) * 8);
        RequireSize(buffers[1], (std::uint64_t(constants[3]) + extent[0]) * 8);
        RequireSize(buffers[2], 8);
    } else {
        const bool bcn = pass == HelperComputePass::Unswizzle3DBcn;
        if (constants[3] > 4 || (bcn && constants[3] < 3) || constants[9] > 31 ||
            constants[10] > 5 || constants[12] > 5 || constants[11] != (1u << constants[10]) - 1u ||
            constants[13] != (1u << constants[12]) - 1u)
            throw std::invalid_argument("D3D12 invalid unswizzle parameters");
        if (!bcn) {
            constants[14] = extent[0];
            constants[15] = extent[1];
        }
        if (bcn) {
            for (std::size_t i = 0; i < 3; ++i)
                constants[i] = extent[i];
            const auto row_pitch = Align(std::uint64_t(extent[0]) << constants[3], 256);
            const auto slice_pitch = CheckedMultiply(row_pitch, extent[1]);
            const auto size = CheckedMultiply(slice_pitch, extent[2]);
            if (size > UINT32_MAX || buffers[1].offset % 512)
                throw std::out_of_range("D3D12 BCn output requires an aligned copy footprint");
            constants[14] = static_cast<std::uint32_t>(row_pitch);
            constants[15] = static_cast<std::uint32_t>(slice_pitch);
            RequireSize(buffers[1], size);
        }
    }
    const FrameProfile::Scope profile_descriptors{context.Profile(), FramePhase::DescriptorWrites};
    std::string view_key{"helper views"};
    const auto append = [&](const auto& value) {
        view_key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    for (const auto& view : buffers) {
        const auto identity = reinterpret_cast<std::uintptr_t>(view.resource.get());
        append(identity);
        append(view.offset);
        append(view.size);
    }
    const auto texture_identity = reinterpret_cast<std::uintptr_t>(texture.resource.get());
    append(texture_identity);
    append(texture.descriptor);
    auto views = descriptors.FindTable(view_key);
    auto samplers = descriptors.FindTable("helper null sampler", true);
    if (!views || !samplers) {
        // Reserve both: a submission invalidates a hit from the previous tick.
        descriptors.Reserve(5, 1);
        views = descriptors.FindTable(view_key);
        samplers = descriptors.FindTable("helper null sampler", true);
    }
    if (!samplers) {
        samplers = descriptors.AllocateSamplers(1);
        descriptors.WriteNullSampler(*samplers, 0);
        descriptors.RememberTable("helper null sampler", *samplers, true);
    }
    if (!views) {
        views = descriptors.AllocateViews(5);
        descriptors.WriteNullCbv(*views, 0);
        descriptors.WriteNullSrv(*views, 1, D3D12_SRV_DIMENSION_BUFFER);
        for (u32 i = 0; i < 3; ++i) {
            if (image && i == 1) {
                descriptors.WriteUavDescriptor(*views, i + 2, texture.resource, texture.descriptor);
            } else if (i < buffers.size()) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_R32_TYPELESS;
                uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                uav.Buffer.FirstElement = buffers[i].offset / 4;
                uav.Buffer.NumElements = static_cast<UINT>(buffers[i].size / 4);
                descriptors.WriteUavDescriptor(*views, i + 2, buffers[i].resource, uav);
            } else {
                descriptors.WriteNullUav(*views, i + 2);
            }
        }
        descriptors.RememberTable(view_key, *views);
    }
    ComputeBindings bindings;
    bindings.pipeline = pipelines[static_cast<std::size_t>(pass)];
    bindings.root_signature = root_signature;
    bindings.resource_heap = descriptors.ResourceHeap();
    bindings.sampler_heap = descriptors.SamplerHeap();
    bindings.constants = constants;
    bindings.tables = {views->gpu, views->Slice(1, 1).gpu, views->Slice(2, 3).gpu, samplers->gpu};
    std::vector<ComputeResourceUse> resources;
    resources.reserve(buffers.size() + (image ? 1 : 0));
    for (const auto& view : buffers)
        resources.push_back({view.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
    if (image)
        resources.push_back({texture.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
    bindings.resources = resources;
    context.MarkWork(pass == HelperComputePass::DecodeAstc ? "ASTC compute" : "helper compute");
    context.Profile().Count(FrameCount::HelperDispatches);
    dispatch.Direct(bindings, groups);
}
} // namespace D3D12
