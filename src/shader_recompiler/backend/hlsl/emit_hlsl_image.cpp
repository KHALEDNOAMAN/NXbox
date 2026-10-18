// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl_instructions.h"
#include "shader_recompiler/backend/hlsl/hlsl_emit_context.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/profile.h"
namespace Shader::Backend::HLSL {
namespace {
IR::TextureInstInfo TextureInfo(EmitContext& ctx, IR::Inst& inst) {
    auto info = inst.Flags<IR::TextureInstInfo>();
    if (info.has_lod_clamp)
        throw NotImplementedException("HLSL texture LOD clamp");
    if (info.type != TextureType::Buffer &&
        ctx.info.texture_descriptors.at(info.descriptor_index).is_multisample &&
        inst.GetOpcode() != IR::Opcode::ImageFetch &&
        inst.GetOpcode() != IR::Opcode::ImageQueryDimensions)
        throw NotImplementedException("HLSL multisample resource requires Load");
    return info;
}
u32 CoordinateCount(TextureType type) {
    switch (type) {
    case TextureType::Color1D:
    case TextureType::Buffer:
        return 1;
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        return 2;
    case TextureType::ColorArray2D:
    case TextureType::Color3D:
    case TextureType::ColorCube:
        return 3;
    case TextureType::ColorArrayCube:
        return 4;
    }
    throw NotImplementedException("HLSL texture coordinate count");
}
std::string IntCoordinates(std::string_view coords, TextureType type) {
    const auto count = CoordinateCount(type);
    return fmt::format("int{}({})", count == 1 ? "" : std::to_string(count), coords);
}
std::string Offset(const IR::Value& offset) {
    if (offset.IsEmpty())
        return "";
    if (offset.IsImmediate())
        return fmt::format("int({})", static_cast<s32>(offset.U32()));
    const auto* inst = offset.InstRecursive();
    if (!inst->AreAllArgsImmediates() || inst->NumArgs() < 2 || inst->NumArgs() > 4)
        throw NotImplementedException("HLSL sampled texture offsets must be constant");
    std::string result = fmt::format("int{}(", inst->NumArgs());
    for (size_t i = 0; i < inst->NumArgs(); ++i)
        result += fmt::format("{}{}", i ? ", " : "", static_cast<s32>(inst->Arg(i).U32()));
    return result + ")";
}
std::string OffsetArgument(const IR::Value& offset) {
    return offset.IsEmpty() ? "" : ", " + Offset(offset);
}
std::string Texture(EmitContext& ctx, const IR::TextureInstInfo& info, const IR::Value& index,
                    bool image = false) {
    return ctx.Resource(index, info.descriptor_index, image, info.type == TextureType::Buffer);
}
bool EmitCubeOne(EmitContext& ctx, IR::Inst& inst, const IR::TextureInstInfo& info,
                 bool scalar = false, bool depth_compare = false) {
    const bool cube =
        info.type == TextureType::ColorCube || info.type == TextureType::ColorArrayCube;
    if (!(ctx.profile.hlsl_cube_one && cube) && !(ctx.profile.hlsl_shadow_one && depth_compare))
        return false;
    if (scalar)
        ctx.AddF32("{} = 1.0f;", inst);
    else
        ctx.AddF32x4("{} = float4(1, 1, 1, 1);", inst);
    return true;
}
void Sample(EmitContext& ctx, IR::Inst& inst, const IR::Value& index, std::string_view coords,
            std::string_view lod, const IR::Value& offset, bool explicit_lod,
            std::string_view dref = {}) {
    const auto info = TextureInfo(ctx, inst);
    if (EmitCubeOne(ctx, inst, info, !dref.empty(), !dref.empty()))
        return;
    const auto tex = Texture(ctx, info, index);
    if (info.type == TextureType::Buffer)
        throw NotImplementedException("HLSL buffer sampling must use fetch");
    const auto sampler = ctx.Sampler(index, info.descriptor_index);
    if (info.type == TextureType::Buffer)
        throw NotImplementedException("HLSL buffer sampling must use fetch");
    if (ctx.info.texture_descriptors.at(info.descriptor_index).is_integer)
        throw NotImplementedException("HLSL integer texture filtering must use fetch");
    std::string method;
    std::string arguments = fmt::format("{}, {}", sampler, coords);
    if (!dref.empty()) {
        if (info.has_bias)
            throw NotImplementedException("HLSL compare sampling with bias");
        if (explicit_lod && !lod.empty()) {
            const auto& arg = inst.Arg(3);
            if (!arg.IsImmediate() || arg.F32() != 0.0f)
                throw NotImplementedException(
                    "HLSL SM 6.0 comparison sampling supports explicit LOD zero only");
        }
        method = explicit_lod || ctx.stage != Stage::Fragment ? "SampleCmpLevelZero" : "SampleCmp";
        arguments += fmt::format(", {}", dref);
    } else if (explicit_lod || ctx.stage != Stage::Fragment) {
        method = "SampleLevel";
        arguments += fmt::format(", {}", explicit_lod && !lod.empty() ? lod : "0.0f");
    } else if (info.has_bias) {
        method = "SampleBias";
        arguments += fmt::format(", {}", lod);
    } else
        method = "Sample";
    arguments += OffsetArgument(offset);
    if (dref.empty())
        ctx.AddF32x4("{} = {}.{}({});", inst, tex, method, arguments);
    else
        ctx.AddF32("{} = {}.{}({});", inst, tex, method, arguments);
}
void Gather(EmitContext& ctx, IR::Inst& inst, const IR::Value& index, std::string_view coords,
            const IR::Value& offset, const IR::Value& offset2, std::string_view dref = {}) {
    const auto info = TextureInfo(ctx, inst);
    if (EmitCubeOne(ctx, inst, info, false, !dref.empty()))
        return;
    const auto tex = Texture(ctx, info, index);
    const auto sampler = ctx.Sampler(index, info.descriptor_index);
    const auto type = info.type.Value();
    if (type != TextureType::Color2D && type != TextureType::ColorArray2D &&
        type != TextureType::ColorCube && type != TextureType::ColorArrayCube)
        throw NotImplementedException("HLSL gather resource dimensions");
    std::string method =
        dref.empty() ? std::string("Gather") +
                           std::array{"Red", "Green", "Blue", "Alpha"}.at(info.gather_component)
                     : "GatherCmp";
    auto arguments = fmt::format("{}, {}", sampler, coords);
    if (!dref.empty())
        arguments += fmt::format(", {}", dref);
    if (offset2.IsEmpty())
        arguments += OffsetArgument(offset);
    else {
        const std::array values{offset.InstRecursive(), offset2.InstRecursive()};
        for (const auto* value : values) {
            if (value->GetOpcode() != IR::Opcode::CompositeConstructU32x4 ||
                !value->AreAllArgsImmediates())
                throw NotImplementedException("HLSL gather four offsets must be constants");
            for (size_t i = 0; i < 4; i += 2)
                arguments += fmt::format(", int2({}, {})", static_cast<s32>(value->Arg(i).U32()),
                                         static_cast<s32>(value->Arg(i + 1).U32()));
        }
    }
    auto expression = fmt::format("{}.{}({})", tex, method, arguments);
    if (info.is_integer)
        expression = "asfloat(" + expression + ")";
    ctx.AddF32x4("{} = {};", inst, expression);
}
ImageFormat StorageImageFormat(EmitContext& ctx, const IR::TextureInstInfo& info) {
    return info.type == TextureType::Buffer
               ? ctx.info.image_buffer_descriptors.at(info.descriptor_index).format
               : ctx.info.image_descriptors.at(info.descriptor_index).format;
}
bool IsSignedImage(EmitContext& ctx, const IR::TextureInstInfo& info) {
    const auto format = StorageImageFormat(ctx, info);
    return format == ImageFormat::R8_SINT || format == ImageFormat::R16_SINT;
}
u32 ImageComponents(EmitContext& ctx, const IR::TextureInstInfo& info) {
    const auto format = StorageImageFormat(ctx, info);
    if (IsSignedImage(ctx, info))
        return 1;
    if (format == ImageFormat::R32_UINT || format == ImageFormat::R8_UINT ||
        format == ImageFormat::R16_UINT)
        return 1;
    if (format == ImageFormat::R32G32_UINT)
        return 2;
    return 4;
}
void CheckImageAtomic(EmitContext& ctx, const IR::TextureInstInfo& info) {
    if (IsSignedImage(ctx, info))
        throw NotImplementedException("HLSL signed 8/16-bit image atomics require a 32-bit UAV");
    if (ImageComponents(ctx, info) != 1)
        throw NotImplementedException("HLSL image atomics require a scalar UAV");
}
} // namespace
void EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                std::string_view coords, std::string_view bias_lc,
                                const IR::Value& offset) {
    Sample(ctx, inst, index, coords, bias_lc, offset, false);
}
void EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                std::string_view coords, std::string_view lod_lc,
                                const IR::Value& offset) {
    Sample(ctx, inst, index, coords, lod_lc, offset, true);
}
void EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                    std::string_view coords, std::string_view dref,
                                    std::string_view bias_lc, const IR::Value& offset) {
    Sample(ctx, inst, index, coords, bias_lc, offset, false, dref);
}
void EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                                    std::string_view coords, std::string_view dref,
                                    std::string_view lod_lc, const IR::Value& offset) {
    Sample(ctx, inst, index, coords, lod_lc, offset, true, dref);
}
void EmitImageGather(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                     std::string_view coords, const IR::Value& offset, const IR::Value& offset2) {
    Gather(ctx, inst, index, coords, offset, offset2);
}
void EmitImageGatherDref(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                         std::string_view coords, const IR::Value& offset, const IR::Value& offset2,
                         std::string_view dref) {
    Gather(ctx, inst, index, coords, offset, offset2, dref);
}
void EmitImageFetch(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                    std::string_view coords, const IR::Value& offset, std::string_view lod,
                    std::string_view ms) {
    const auto info = TextureInfo(ctx, inst);
    if (info.type == TextureType::ColorCube || info.type == TextureType::ColorArrayCube)
        throw NotImplementedException("HLSL cube texel fetch requires a 2D-array SRV");
    const auto tex = Texture(ctx, info, index);
    std::string arguments = IntCoordinates(coords, info.type);
    const bool multisample = info.type != TextureType::Buffer &&
                             ctx.info.texture_descriptors.at(info.descriptor_index).is_multisample;
    if (info.type != TextureType::Buffer && !multisample)
        arguments = fmt::format("int{}({}, int({}))", CoordinateCount(info.type) + 1, arguments,
                                lod.empty() ? "0" : lod);
    if (multisample)
        arguments += fmt::format(", int({})", ms.empty() ? "0" : ms);
    arguments += OffsetArgument(offset);
    auto expression = fmt::format("{}.Load({})", tex, arguments);
    if (info.is_integer)
        expression = "asfloat(" + expression + ")";
    if (info.is_depth)
        expression = "float4(" + expression + ", 0, 0, 1)";
    ctx.AddF32x4("{} = {};", inst, expression);
}
void EmitImageQueryDimensions(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                              std::string_view lod, const IR::Value& skip_mips) {
    const auto info = TextureInfo(ctx, inst);
    const auto tex = Texture(ctx, info, index);
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32x4);
    const auto type = info.type.Value();
    const bool msaa = type != TextureType::Buffer &&
                      ctx.info.texture_descriptors.at(info.descriptor_index).is_multisample;
    const auto dims = type == TextureType::ColorCube        ? 2
                      : type == TextureType::ColorArrayCube ? 3
                                                            : CoordinateCount(type);
    std::string arguments;
    if (type != TextureType::Buffer && !msaa)
        arguments += fmt::format("uint({}), ", lod);
    for (u32 i = 0; i < dims; ++i)
        arguments += fmt::format("{}{}.{}", i ? ", " : "", result, "xyz"[i]);
    if (type != TextureType::Buffer)
        arguments += ", mip_count";
    ctx.Add("{} = uint4(0, 0, 0, 0); {{ uint mip_count = 0; {}.GetDimensions({}); {}.w = {}; }}",
            result, tex, arguments, result, skip_mips.U1() || msaa ? "0u" : "mip_count");
}
void EmitImageQueryLod(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                       std::string_view coords) {
    const auto info = TextureInfo(ctx, inst);
    const auto tex = Texture(ctx, info, index);
    const auto sampler = ctx.Sampler(index, info.descriptor_index);
    ctx.AddF32x4("{} = float4({}.CalculateLevelOfDetail({}, {}), "
                 "{}.CalculateLevelOfDetailUnclamped({}, {}), 0, 0);",
                 inst, tex, sampler, coords, tex, sampler, coords);
}
void EmitImageGradient(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                       std::string_view coords, const IR::Value& derivatives,
                       const IR::Value& offset, const IR::Value&) {
    const auto info = TextureInfo(ctx, inst);
    if (EmitCubeOne(ctx, inst, info))
        return;
    const auto tex = Texture(ctx, info, index);
    const auto sampler = ctx.Sampler(index, info.descriptor_index);
    const auto deriv = ctx.var_alloc.Consume(derivatives);
    std::string dx, dy, offsets;
    if (info.num_derivatives == 1) {
        dx = deriv + ".x";
        dy = deriv + ".y";
        offsets = OffsetArgument(offset);
    } else if (info.num_derivatives == 2) {
        dx = deriv + ".xz";
        dy = deriv + ".yw";
        offsets = OffsetArgument(offset);
    } else {
        const auto extra = ctx.var_alloc.Consume(offset);
        dx = fmt::format("float3({}.xz, {}.x)", deriv, extra);
        dy = fmt::format("float3({}.yw, {}.y)", deriv, extra);
    }
    ctx.AddF32x4("{} = {}.SampleGrad({}, {}, {}, {}{});", inst, tex, sampler, coords, dx, dy,
                 offsets);
}
void EmitImageRead(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                   std::string_view coords) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    if (StorageImageFormat(ctx, info) == ImageFormat::Typeless &&
        !ctx.profile.support_typeless_image_loads) {
        // Match SPIR-V's fallback without emitting an unsupported typed UAV load.
        ctx.AddU32x4("{} = uint4(0u, 0u, 0u, 0u);", inst);
        return;
    }
    const auto image = Texture(ctx, info, index, true);
    const auto components = ImageComponents(ctx, info);
    const auto padding = components == 1 ? ", 0u, 0u, 1u" : components == 2 ? ", 0u, 1u" : "";
    const auto load = fmt::format("{}[{}]", image, IntCoordinates(coords, info.type));
    if (StorageImageFormat(ctx, info) == ImageFormat::Typeless)
        ctx.AddU32x4("{} = asuint({});", inst, load);
    else
        ctx.AddU32x4("{} = uint4({}{});", inst, load, padding);
}
void EmitImageWrite(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                    std::string_view coords, std::string_view color) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    const auto components = ImageComponents(ctx, info);
    auto value = fmt::format("{}{}", color, components == 1 ? ".x" : components == 2 ? ".xy" : "");
    if (StorageImageFormat(ctx, info) == ImageFormat::Typeless)
        value = fmt::format("asfloat({})", value);
    else if (IsSignedImage(ctx, info))
        value = fmt::format("asint({})", value);
    ctx.Add("{}[{}] = {};", Texture(ctx, info, index, true), IntCoordinates(coords, info.type),
            value);
}
void EmitImageAtomicIAdd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                           std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAdd({}, {}, {});", address, value, result);
}

void EmitImageAtomicSMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                           std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add(
        "{{ uint old_value = {}; uint previous; do {{ uint new_value = uint(min(int(old_value), "
        "int({}))); InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
        "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
        address, value, address, result);
}

void EmitImageAtomicUMin32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                           std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMin({}, {}, {});", address, value, result);
}

void EmitImageAtomicSMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                           std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add(
        "{{ uint old_value = {}; uint previous; do {{ uint new_value = uint(max(int(old_value), "
        "int({}))); InterlockedCompareExchange({}, old_value, new_value, previous); if (previous "
        "== old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
        address, value, address, result);
}

void EmitImageAtomicUMax32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                           std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedMax({}, {}, {});", address, value, result);
}

void EmitImageAtomicInc32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                          std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add(
        "{{ uint old_value = {}; uint previous; do {{ uint new_value = old_value >= {} ? 0u : "
        "old_value + 1u; InterlockedCompareExchange({}, old_value, new_value, previous); if "
        "(previous == old_value) break; old_value = previous; }} while (true); {} = old_value; }}",
        address, value, address, result);
}

void EmitImageAtomicDec32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                          std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("{{ uint old_value = {}; uint previous; do {{ uint new_value = old_value == 0u || "
            "old_value > {} ? {} : old_value - 1u; InterlockedCompareExchange({}, old_value, "
            "new_value, previous); if (previous == old_value) break; old_value = previous; }} "
            "while (true); {} = old_value; }}",
            address, value, value, address, result);
}

void EmitImageAtomicAnd32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                          std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedAnd({}, {}, {});", address, value, result);
}

void EmitImageAtomicOr32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                         std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedOr({}, {}, {});", address, value, result);
}

void EmitImageAtomicXor32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                          std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedXor({}, {}, {});", address, value, result);
}

void EmitImageAtomicExchange32(EmitContext& ctx, IR::Inst& inst, const IR::Value& index,
                               std::string_view coords, std::string_view value) {
    const auto info = inst.Flags<IR::TextureInstInfo>();
    CheckImageAtomic(ctx, info);
    const auto address =
        fmt::format("{}[{}]", Texture(ctx, info, index, true), IntCoordinates(coords, info.type));
    const auto result = ctx.var_alloc.Define(inst, HlslVarType::U32);
    ctx.Add("InterlockedExchange({}, {}, {});", address, value, result);
}
} // namespace Shader::Backend::HLSL
