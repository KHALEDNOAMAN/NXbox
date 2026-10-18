// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_recompiler/backend/hlsl/emit_hlsl.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "shader_recompiler/frontend/ir/modifiers.h"

using namespace Shader;
using Shader::Backend::Bindings;
namespace {
void Require(bool value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
}
void Contains(const std::string &text, std::string_view token) {
  Require(text.find(token) != text.npos, token);
}
struct Fixture {
  ObjectPool<IR::Inst> pool;
  IR::Block block{pool};
  IR::Program program;
  Fixture(Stage stage) {
    program.stage = stage;
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block},
         .type = IR::AbstractSyntaxNode::Type::Block});
    program.workgroup_size = {8, 4, 1};
  }
  IR::Value Add(IR::Opcode opcode, std::initializer_list<IR::Value> args = {},
                u32 flags = 0) {
    auto it = block.PrependNewInst(block.end(), opcode, args, flags);
    return IR::Value{&*it};
  }
  std::string Emit(Bindings &bindings, const Profile &profile = {},
                   const RuntimeInfo &runtime = {}) {
    return Backend::HLSL::EmitHLSL(profile, runtime, program, bindings);
  }
};
void Vertex() {
  Fixture f{Stage::VertexB};
  f.program.info.loads.Set(IR::Attribute::Generic0X);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto input =
      f.Add(IR::Opcode::GetAttribute,
            {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
  const auto sum = f.Add(IR::Opcode::FPAdd32, {input, IR::Value{2.0f}});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, sum, IR::Value{0u}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "Output main(Input input)");
  Contains(text, "SV_Position");
  Contains(text, "TEXCOORD0");
  Contains(text, "output.position.x =");
  const auto second = f.Emit(bindings);
  Require(text == second, "Emission must preserve the IR");
  std::ofstream("hlsl_vertex.hlsl") << text;
}
void Fragment() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.program.info.stores_frag_depth = true;
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  const auto coords = f.Add(IR::Opcode::CompositeConstructF32x2,
                            {IR::Value{0.5f}, IR::Value{0.25f}});
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  const auto sample = f.Add(IR::Opcode::ImageSampleImplicitLod,
                            {IR::Value{0u}, coords, {}, {}}, flags.raw);
  const auto red =
      f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
  f.Add(IR::Opcode::SetFragDepth, {IR::Value{0.75f}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "Texture2D<float4> tex0 : register(t0, space0)");
  Contains(text, "SamplerState samp0 : register(s0, space0)");
  Contains(text, ".Sample(samp0,");
  Contains(text, "SV_Target0");
  Contains(text, "SV_Depth");
  Require(bindings.texture == 1, "Texture counter");
  std::ofstream("hlsl_fragment.hlsl") << text;
  // Attachment specialization clears export metadata but keeps the original IR.
  // Stores into unbound targets must disappear from both declarations and body.
  f.Add(IR::Opcode::SetFragColor, {IR::Value{3u}, IR::Value{0u}, red});
  Bindings masked_bindings;
  const auto masked = f.Emit(masked_bindings);
  Require(masked.find("SV_Target3") == masked.npos, "Unbound target signature");
  Require(masked.find("output.color3") == masked.npos, "Unbound target store");
  f.program.info.stores_frag_color[0] = false;
  Bindings depth_bindings;
  const auto depth_only = f.Emit(depth_bindings);
  Require(depth_only.find("SV_Target") == depth_only.npos,
          "Depth-only color signature");
  Require(depth_only.find("output.color0") == depth_only.npos,
          "Depth-only color store");
  Contains(depth_only, "SV_Depth");
}
void MultisampleFetch() {
  for (const auto type : {TextureType::Color2D, TextureType::ColorArray2D}) {
    for (const bool depth : {false, true}) {
      Fixture f{Stage::Fragment};
      f.program.info.stores_frag_color[0] = true;
      f.program.info.texture_descriptors.push_back({.type = type,
                                                    .is_depth = depth,
                                                    .is_multisample = true,
                                                    .count = 1});
      const bool array = type == TextureType::ColorArray2D;
      const auto coords =
          array ? f.Add(IR::Opcode::CompositeConstructU32x3,
                        {IR::Value{1u}, IR::Value{2u}, IR::Value{0u}})
                : f.Add(IR::Opcode::CompositeConstructU32x2,
                        {IR::Value{1u}, IR::Value{2u}});
      IR::TextureInstInfo flags{};
      flags.type.Assign(type);
      flags.is_depth.Assign(depth);
      const auto sample = f.Add(
          IR::Opcode::ImageFetch,
          {IR::Value{0u}, coords, {}, IR::Value{0u}, IR::Value{2u}}, flags.raw);
      const auto red =
          f.Add(IR::Opcode::CompositeExtractF32x4, {sample, IR::Value{0u}});
      f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, red});
      // An omitted sample operand still selects the MSAA overload at sample
      // zero.
      const auto zero =
          f.Add(IR::Opcode::ImageFetch,
                {IR::Value{0u}, coords, {}, IR::Value{0u}, {}}, flags.raw);
      const auto green =
          f.Add(IR::Opcode::CompositeExtractF32x4, {zero, IR::Value{0u}});
      f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{1u}, green});
      Bindings bindings;
      const auto text = f.Emit(bindings);
      Contains(text, array ? "Texture2DMSArray<" : "Texture2DMS<");
      Contains(text, "tex0.Load(");
      Contains(text, ", int(2u)");
      Contains(text, ", int(0)");
      Require(text.find("SamplerState") == text.npos,
              "MSAA has no sampler declaration");
      const std::string name = std::string{"hlsl_msaa"} +
                               (array ? "_array" : "") +
                               (depth ? "_depth" : "") + ".hlsl";
      std::ofstream(name) << text;
    }
  }
}
void Compute() {
  Fixture f{Stage::Compute};
  f.program.shared_memory_size = 128;
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 3, .count = 1});
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto value =
      f.Add(IR::Opcode::GetCbufU32, {IR::Value{3u}, IR::Value{16u}});
  const auto old = f.Add(IR::Opcode::StorageAtomicIAdd32,
                         {IR::Value{0u}, IR::Value{0u}, value});
  f.Add(IR::Opcode::WriteSharedU32, {IR::Value{4u}, old});
  f.Add(IR::Opcode::Barrier);
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{4u}, old});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "register(b0, space1)");
  Contains(text, "nx_cbuf3 : register(b0, space0)");
  Contains(text, "RWByteAddressBuffer ssbo0 : register(u0, space0)");
  Contains(text, ".InterlockedAdd(");
  Contains(text, "groupshared uint smem[32]");
  Contains(text, "GroupMemoryBarrierWithGroupSync");
  Contains(text, "[numthreads(8, 4, 1)]");
  Require(bindings.uniform_buffer == 1 && bindings.storage_buffer == 1,
          "Buffer counters");
  std::ofstream("hlsl_compute.hlsl") << text;
}
void ControlFlowAndWaves() {
  Fixture f{Stage::Compute};
  f.program.info.uses_subgroup_vote = true;
  f.program.info.uses_subgroup_shuffles = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  IR::Block header{f.pool};
  IR::Block body{f.pool};
  f.program.blocks.push_back(&header);
  f.program.blocks.push_back(&body);
  auto first_it = header.PrependNewInst(header.end(), IR::Opcode::Phi, {},
                                        static_cast<u32>(IR::Type::U32));
  auto second_it = header.PrependNewInst(header.end(), IR::Opcode::Phi, {},
                                         static_cast<u32>(IR::Type::U32));
  first_it->AddPhiOperand(&f.block, IR::Value{11u});
  second_it->AddPhiOperand(&f.block, IR::Value{22u});
  first_it->AddPhiOperand(&body, IR::Value{&*second_it});
  second_it->AddPhiOperand(&body, IR::Value{&*first_it});
  const auto condition = f.Add(IR::Opcode::ConditionRef, {IR::Value{true}});
  f.Add(IR::Opcode::SubgroupBallot, {condition});
  f.Add(IR::Opcode::VoteAll, {condition});
  const auto shuffle =
      f.Add(IR::Opcode::ShuffleIndex,
            {IR::Value{7u}, IR::Value{1u}, IR::Value{31u}, IR::Value{0u}});
  const auto flag = f.Add(IR::Opcode::GetInBoundsFromOp, {shuffle});
  f.Add(IR::Opcode::LogicalNot, {flag});
  body.AppendNewInst(IR::Opcode::WriteStorage32,
                     {IR::Value{0u}, IR::Value{0u}, IR::Value{&*first_it}});
  using Node = IR::AbstractSyntaxNode;
  f.program.syntax_list.push_back(
      {.data = {.loop = {.body = &header}}, .type = Node::Type::Loop});
  f.program.syntax_list.push_back(
      {.data = {.block = &header}, .type = Node::Type::Block});
  f.program.syntax_list.push_back(
      {.data = {.if_node = {.cond = IR::U1{condition}}},
       .type = Node::Type::If});
  f.program.syntax_list.push_back(
      {.data = {.block = &body}, .type = Node::Type::Block});
  f.program.syntax_list.push_back({.type = Node::Type::EndIf});
  f.program.syntax_list.push_back(
      {.data = {.repeat = {.cond = IR::U1{IR::Value{false}}}},
       .type = Node::Type::Repeat});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "incoming_v");
  Contains(text, "while (true)");
  Contains(text, "int loop0 = 0x2000");
  Contains(text, "nx_shuffle(");
  Contains(text, "nx_ballot(");
  Contains(text, "WaveReadLaneAt");
  // Check parallel phi assignments: every snapshot precedes the first write.
  const auto start = text.find("uint incoming_");
  const auto next = text.find("uint incoming_", start + 1);
  const auto write = text.find(" = incoming_", start);
  Require(start < next && next < write,
          "Phi snapshots must precede phi writes");
  Bindings repeated;
  Require(text == f.Emit(repeated), "Wave/pseudo/phi emission modified IR");
  std::ofstream("hlsl_waves.hlsl") << text;
}
void ResourceOrderAndCompare() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .is_depth = true, .count = 1});
  f.program.info.texture_buffer_descriptors.push_back({.count = 1});
  f.program.info.storage_buffers_descriptors.push_back({.count = 1});
  f.program.info.image_descriptors.push_back({.type = TextureType::Color2D,
                                              .format = ImageFormat::R32_UINT,
                                              .is_written = true,
                                              .is_integer = true,
                                              .count = 1});
  const auto coords = f.Add(IR::Opcode::CompositeConstructF32x2,
                            {IR::Value{0.5f}, IR::Value{0.25f}});
  IR::TextureInstInfo depth{};
  depth.type.Assign(TextureType::Color2D);
  depth.is_depth.Assign(1);
  const auto compare = f.Add(
      IR::Opcode::ImageSampleDrefExplicitLod,
      {IR::Value{0u}, coords, IR::Value{0.5f}, IR::Value{0.0f}, {}}, depth.raw);
  f.Add(IR::Opcode::SetFragColor, {IR::Value{0u}, IR::Value{0u}, compare});
  f.Add(IR::Opcode::ImageQueryDimensions,
        {IR::Value{0u}, IR::Value{0u}, IR::Value{false}}, depth.raw);
  IR::TextureInstInfo gather{};
  gather.type.Assign(TextureType::Color2D);
  gather.is_depth.Assign(1);
  f.Add(IR::Opcode::ImageGatherDref,
        {IR::Value{0u}, coords, {}, {}, IR::Value{0.5f}}, gather.raw);
  const auto int_coords = f.Add(IR::Opcode::CompositeConstructU32x2,
                                {IR::Value{1u}, IR::Value{2u}});
  IR::TextureInstInfo image{};
  image.type.Assign(TextureType::Color2D);
  image.image_format.Assign(ImageFormat::R32_UINT);
  const auto value =
      f.Add(IR::Opcode::CompositeConstructU32x4,
            {IR::Value{4u}, IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
  f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, int_coords, value}, image.raw);
  f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, int_coords}, image.raw);
  f.Add(IR::Opcode::ImageAtomicIAdd32,
        {IR::Value{0u}, int_coords, IR::Value{3u}}, image.raw);
  Bindings bindings{
      .uniform_buffer = 2, .storage_buffer = 3, .texture = 4, .image = 2};
  const auto text = f.Emit(bindings);
  Contains(text, "tex4 : register(t4, space0)");
  Contains(text, "tex5 : register(t5, space0)");
  Contains(text, "ssbo0 : register(u5, space0)");
  Contains(text, "img6 : register(u6, space0)");
  Contains(text, "SamplerComparisonState samp4");
  Contains(text, ".SampleCmpLevelZero(");
  Contains(text, ".GatherCmp(");
  Contains(text, ".GetDimensions(");
  Contains(text, "InterlockedAdd(img6[");
  Require(bindings.texture == 6 && bindings.storage_buffer == 4 &&
              bindings.image == 3,
          "Shared UAV/SRV counters");
  std::ofstream("hlsl_resources.hlsl") << text;
}
void StorageImageFormats() {
  for (const auto format :
       {ImageFormat::Typeless, ImageFormat::R8_SINT, ImageFormat::R16_SINT}) {
    for (const bool buffer : {false, true}) {
      Fixture f{Stage::Compute};
      if (buffer)
        f.program.info.image_buffer_descriptors.push_back({.format = format,
                                                           .is_written = true,
                                                           .is_read = true,
                                                           .count = 1});
      else
        f.program.info.image_descriptors.push_back(
            {.type = TextureType::Color2D,
             .format = format,
             .is_written = true,
             .is_read = true,
             .count = 1});
      IR::TextureInstInfo flags{};
      flags.type.Assign(buffer ? TextureType::Buffer : TextureType::Color2D);
      flags.image_format.Assign(format);
      const auto coords = buffer ? IR::Value{0u}
                                 : f.Add(IR::Opcode::CompositeConstructU32x2,
                                         {IR::Value{0u}, IR::Value{0u}});
      const auto color =
          f.Add(IR::Opcode::ImageRead, {IR::Value{0u}, coords}, flags.raw);
      f.Add(IR::Opcode::ImageWrite, {IR::Value{0u}, coords, color}, flags.raw);
      Bindings bindings;
      Profile profile{};
      profile.support_typeless_image_loads = true;
      const auto text = f.Emit(bindings, profile);
      const bool typeless = format == ImageFormat::Typeless;
      Contains(text, std::string(buffer ? "RWBuffer<" : "RWTexture2D<") +
                         (typeless ? "float4> img0" : "int> img0"));
      if (typeless) {
        Contains(text, "= asuint(img0[");
        Contains(text, "] = asfloat(");
        Bindings fallback_bindings;
        const auto fallback = f.Emit(fallback_bindings);
        Contains(fallback, "= uint4(0u, 0u, 0u, 0u);");
        Require(fallback.find("= asuint(img0[") == fallback.npos,
                "Unsupported typeless load must return zero");
        Contains(fallback, "] = asfloat(");
      } else {
        Contains(text, ", 0u, 0u, 1u)");
        Contains(text, "] = asint(");
        Contains(text, ".x);");
      }
      std::ofstream("hlsl_storage_" + std::to_string(static_cast<int>(format)) +
                    (buffer ? "_buffer.hlsl" : "_texture.hlsl"))
          << text;
      // Narrow integer UAVs cannot perform D3D12 32-bit image atomics. Reject
      // deliberately rather than emitting uint atomic operands against an int
      // UAV.
      if (!typeless) {
        f.Add(IR::Opcode::ImageAtomicIAdd32,
              {IR::Value{0u}, coords, IR::Value{1u}}, flags.raw);
        Bindings rejected;
        try {
          f.Emit(rejected);
          throw std::runtime_error("Narrow signed image atomic accepted");
        } catch (const NotImplementedException &error) {
          Contains(error.what(), "32-bit UAV");
        }
      }
    }
  }
}
void ArithmeticAndPseudoResults() {
  Fixture f{Stage::Compute};
  f.program.info.uses_int64 = true;
  f.program.info.uses_fp64 = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto sum =
      f.Add(IR::Opcode::IAdd32, {IR::Value{0xffffffffu}, IR::Value{1u}});
  const auto carry = f.Add(IR::Opcode::GetCarryFromOp, {sum});
  f.Add(IR::Opcode::GetZeroFromOp, {sum});
  f.Add(IR::Opcode::GetSignFromOp, {sum});
  f.Add(IR::Opcode::GetOverflowFromOp, {sum});
  const auto select =
      f.Add(IR::Opcode::SelectU32, {carry, IR::Value{1u}, IR::Value{0u}});
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{0u}, select});
  const auto wide = f.Add(IR::Opcode::IAdd64,
                          {IR::Value{u64{0x100000001}}, IR::Value{u64{2}}});
  f.Add(IR::Opcode::IAbs64, {wide});
  const auto pair = f.Add(IR::Opcode::UnpackUint2x32, {wide});
  f.Add(IR::Opcode::WriteStorage64, {IR::Value{0u}, IR::Value{8u}, pair});
  const auto rounded = f.Add(IR::Opcode::FPRoundEven64, {IR::Value{2.5}});
  const auto narrowed = f.Add(IR::Opcode::ConvertF32F64, {rounded});
  const auto equal =
      f.Add(IR::Opcode::FPOrdEqual32, {narrowed, IR::Value{2.0f}});
  f.Add(IR::Opcode::SelectU32, {equal, IR::Value{3u}, IR::Value{4u}});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "uaddCarry(");
  Contains(text, "carry!=0");
  Contains(text, "nx_abs_int64(");
  Contains(text, "(uint64_t(1u) | (uint64_t(1u) << 32))");
  Contains(text, "nx_round64(");
  Contains(text, "&&!isnan(");
  Bindings repeated;
  Require(text == f.Emit(repeated),
          "Arithmetic pseudo-result emission modified IR");
  std::ofstream("hlsl_arithmetic.hlsl") << text;
}
void ChainedResourceBindings() {
  Fixture vertex{Stage::VertexB};
  vertex.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  vertex.program.info.texture_buffer_descriptors.push_back({.count = 1});
  vertex.program.info.storage_buffers_descriptors.push_back({.count = 1});
  vertex.program.info.image_descriptors.push_back(
      {.type = TextureType::Color2D,
       .format = ImageFormat::R32_UINT,
       .count = 1});
  vertex.program.info.image_buffer_descriptors.push_back(
      {.format = ImageFormat::R32_UINT, .count = 1});
  Bindings bindings;
  const auto first = vertex.Emit(bindings);
  Contains(first, "samp0 : register(s0, space0)");
  Contains(first, "ssbo0 : register(u0, space0)");
  Contains(first, "img1 : register(u1, space0)");
  Contains(first, "img2 : register(u2, space0)");
  Fixture fragment{Stage::Fragment};
  fragment.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  fragment.program.info.storage_buffers_descriptors.push_back({.count = 1});
  const auto second = fragment.Emit(bindings);
  Contains(second, "tex2 : register(t2, space0)");
  Contains(second, "samp2 : register(s1, space0)");
  Contains(second, "ssbo0 : register(u3, space0)");
  Require(bindings.texture == 3 && bindings.hlsl_sampler == 2 &&
              bindings.storage_buffer + bindings.image == 4,
          "Stage append order must match native descriptor tables");
}
IR::Value Argument(Fixture &f, IR::Type type) {
  switch (type) {
  case IR::Type::U1:
    return IR::Value{true};
  case IR::Type::U8:
    return IR::Value{u8{1}};
  case IR::Type::U16:
    return IR::Value{u16{1}};
  case IR::Type::U32:
    return IR::Value{1u};
  case IR::Type::U64:
    return IR::Value{u64{1}};
  case IR::Type::F16:
    return f.Add(IR::Opcode::BitCastF16U16, {IR::Value{u16{0x3c00}}});
  case IR::Type::F32:
    return IR::Value{1.0f};
  case IR::Type::F64:
    return IR::Value{1.0};
  default:
    break;
  }
  const auto construct =
      type == IR::Type::U32x2   ? IR::Opcode::CompositeConstructU32x2
      : type == IR::Type::U32x3 ? IR::Opcode::CompositeConstructU32x3
      : type == IR::Type::U32x4 ? IR::Opcode::CompositeConstructU32x4
      : type == IR::Type::F16x2 ? IR::Opcode::CompositeConstructF16x2
      : type == IR::Type::F16x3 ? IR::Opcode::CompositeConstructF16x3
      : type == IR::Type::F16x4 ? IR::Opcode::CompositeConstructF16x4
      : type == IR::Type::F32x2 ? IR::Opcode::CompositeConstructF32x2
      : type == IR::Type::F32x3 ? IR::Opcode::CompositeConstructF32x3
      : type == IR::Type::F32x4 ? IR::Opcode::CompositeConstructF32x4
      : type == IR::Type::F64x2 ? IR::Opcode::CompositeConstructF64x2
      : type == IR::Type::F64x3 ? IR::Opcode::CompositeConstructF64x3
                                : IR::Opcode::CompositeConstructF64x4;
  const auto value = Argument(f, IR::ArgTypeOf(construct, 0));
  const auto count = IR::NumArgsOf(construct);
  return count == 2   ? f.Add(construct, {value, value})
         : count == 3 ? f.Add(construct, {value, value, value})
                      : f.Add(construct, {value, value, value, value});
}
void NumericCoverage() {
  Fixture f{Stage::Compute};
  f.Add(IR::Opcode::IsHelperInvocation);
  f.program.info.uses_int64 = true;
  f.program.info.uses_fp64 = true;
  size_t count = 0;
#define OPCODE(name, ...) IR::Opcode::name,
  constexpr std::array opcodes{
#include "shader_recompiler/frontend/ir/opcodes.inc"
  };
#undef OPCODE
  for (const auto op : opcodes) {
    const auto name = IR::NameOf(op);
    if (!name.starts_with("FP") && !name.starts_with("Convert") &&
        !name.starts_with("Composite") && !name.starts_with("Select") &&
        !name.starts_with("BitCast"))
      continue;
    std::vector<IR::Value> args;
    for (size_t i = 0; i < IR::NumArgsOf(op); ++i)
      args.push_back(Argument(f, IR::ArgTypeOf(op, i)));
    switch (args.size()) {
    case 0:
      f.Add(op);
      break;
    case 1:
      f.Add(op, {args[0]});
      break;
    case 2:
      f.Add(op, {args[0], args[1]});
      break;
    case 3:
      f.Add(op, {args[0], args[1], args[2]});
      break;
    case 4:
      f.Add(op, {args[0], args[1], args[2], args[3]});
      break;
    default:
      throw std::runtime_error("Unsupported numeric fixture arity");
    }
    ++count;
  }
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "nx_half(");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 60u,
          "Non-fragment helper queries require no pixel intrinsic");
  Contains(text, "double4");
  Contains(text, "nx_rsqrt64(");
  Require(count == 189, "Numeric coverage unexpectedly decreased");
  std::cout << "Numeric opcodes emitted: " << count << '\n';
  std::ofstream("hlsl_numeric.hlsl") << text;
}
void GlobalAndRegisterState() {
  Fixture f{Stage::Compute};
  f.program.info.uses_global_memory = true;
  f.program.info.uses_int64 = true;
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 0, .count = 1});
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  const auto address = IR::Value{u64{0x1000}};
  const auto value = f.Add(IR::Opcode::LoadGlobalS8, {address});
  f.Add(IR::Opcode::WriteGlobalU16, {address, value});
  f.Add(IR::Opcode::GlobalAtomicIAdd32, {address, value});
  f.Add(IR::Opcode::GlobalAtomicSMin32, {address, value});
  f.Add(IR::Opcode::GlobalAtomicAddF32, {address, IR::Value{1.0f}});
  f.Add(IR::Opcode::SetRegister, {IR::Value{IR::Reg::R0}, value});
  const auto reg = f.Add(IR::Opcode::GetRegister, {IR::Value{IR::Reg::R0}});
  f.Add(IR::Opcode::WriteStorage32, {IR::Value{0u}, IR::Value{0u}, reg});
  f.Add(IR::Opcode::SetPred, {IR::Value{IR::Pred::P0}, IR::Value{true}});
  f.Add(IR::Opcode::GetPred, {IR::Value{IR::Pred::P0}});
  f.Add(IR::Opcode::GetPred, {IR::Value{IR::Pred::PT}});
  f.Add(IR::Opcode::SetGotoVariable, {IR::Value{17u}, IR::Value{true}});
  f.Add(IR::Opcode::GetGotoVariable, {IR::Value{17u}});
  Bindings bindings;
  for (size_t i = 0; i < std::size(IR::Detail::META_TABLE); ++i) {
    const auto op = static_cast<IR::Opcode>(i);
    if (!IR::NameOf(op).starts_with("GlobalAtomic"))
      continue;
    const auto operand = Argument(f, IR::ArgTypeOf(op, 1));
    f.Add(op, {address, operand});
  }
  Profile profile{};
  profile.support_int64_atomics = true;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "nx_global_load_subword(");
  Contains(text, "nx_global_size - (address - nx_global_base) >= bits / 8u");
  Contains(text, "nx_global_write_subword(");
  Contains(text, "nx_global_matched");
  Contains(text, "nx_regs[0]");
  Contains(text, "nx_goto_17");
  std::ofstream("hlsl_global.hlsl") << text;
}
void SparseResidency() {
  Fixture f{Stage::Fragment};
  f.program.info.texture_descriptors.push_back(
      {.type = TextureType::Color2D, .count = 1});
  const auto coords = Argument(f, IR::Type::F32x2);
  IR::TextureInstInfo flags{};
  flags.type.Assign(TextureType::Color2D);
  const auto sample = f.Add(IR::Opcode::ImageSampleImplicitLod,
                            {IR::Value{0u}, coords, {}, {}}, flags.raw);
  f.Add(IR::Opcode::GetSparseFromOp, {sample});
  Bindings bindings;
  Contains(f.Emit(bindings), "= true;");
}
void Geometry() {
  Fixture f{Stage::Geometry};
  f.program.output_topology = OutputTopology::TriangleStrip;
  f.program.output_vertices = 3;
  f.program.invocations = 1;
  f.program.info.loads.Set(IR::Attribute::PositionX);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto x = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::PositionX}, IR::Value{2u}});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, x, IR::Value{0u}});
  f.Add(IR::Opcode::InvocationId);
  f.Add(IR::Opcode::EmitVertex, {IR::Value{0u}});
  f.Add(IR::Opcode::EndPrimitive, {IR::Value{0u}});
  RuntimeInfo runtime{};
  runtime.input_topology = InputTopology::Triangles;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "inputs[2u].position.x");
  Contains(text, "[maxvertexcount(3)]");
  Contains(text, "inout TriangleStream<Output>");
  Contains(text, "nx_stream.Append(nx_vertex)");
  Contains(text, "nx_stream.RestartStrip()");
  std::ofstream("hlsl_geometry.hlsl") << text;
}
void NativeWideAtomics() {
  Fixture f{Stage::Compute};
  f.program.info.uses_int64 = true;
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  f.Add(IR::Opcode::StorageAtomicIAdd64,
        {IR::Value{0u}, IR::Value{0u}, IR::Value{u64{1}}});
  f.Add(IR::Opcode::StorageAtomicSMin64,
        {IR::Value{0u}, IR::Value{8u}, IR::Value{u64{1}}});
  Profile profile{};
  profile.support_int64_atomics = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "Load<uint64_t>");
  Contains(text, "InterlockedCompareExchange");
  std::ofstream("hlsl_atomic64.hlsl") << text;
  Bindings failed{.storage_buffer = 7};
  try {
    f.Emit(failed);
    throw std::runtime_error(
        "Native wide atomics accepted without device capability");
  } catch (const NotImplementedException &error) {
    Contains(error.what(), "SM 6.6");
  }
  Require(failed.storage_buffer == 7, "Failure advanced bindings");
}
void MixedSharedMemory() {
  Fixture f{Stage::Compute};
  f.program.shared_memory_size = 128;
  f.program.info.uses_int64 = true;
  const auto pair = Argument(f, IR::Type::U32x2);
  f.Add(IR::Opcode::SharedAtomicExchange64, {IR::Value{0u}, IR::Value{u64{1}}});
  f.Add(IR::Opcode::SharedAtomicExchange32x2, {IR::Value{8u}, pair});
  for (size_t i = 0; i < std::size(IR::Detail::META_TABLE); ++i) {
    const auto op = static_cast<IR::Opcode>(i);
    const auto name = IR::NameOf(op);
    if (!name.starts_with("LoadShared") && !name.starts_with("WriteShared") &&
        !(name.starts_with("SharedAtomic") && name.ends_with("32")))
      continue;
    if (IR::NumArgsOf(op) == 1) {
      f.Add(op, {IR::Value{0u}});
    } else {
      const auto value = Argument(f, IR::ArgTypeOf(op, 1));
      f.Add(op, {IR::Value{0u}, value});
    }
  }
  Profile profile{};
  profile.support_shared_int64_atomics = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, profile);
  Contains(text, "groupshared uint64_t smem64[16]");
  Contains(text, "nx_shared_write_subword(");
  Contains(text, "nx_shared_atomic32(");
  Require(text.find("smem[") == text.npos,
          "Mixed-width shared accesses use different backing arrays");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 66u,
          "Shared wide atomics need SM 6.6");
  std::ofstream("hlsl_shared64.hlsl") << text;
}
void PixelHelperAndAlphaTest() {
  Fixture f{Stage::Fragment};
  f.program.info.stores_frag_color[0] = true;
  f.Add(IR::Opcode::IsHelperInvocation);
  f.Add(IR::Opcode::SetFragColor,
        {IR::Value{0u}, IR::Value{3u}, IR::Value{0.75f}});
  RuntimeInfo runtime{};
  runtime.alpha_test_func = CompareFunction::Greater;
  runtime.alpha_test_reference = 0.5f;
  runtime.xfb_count = 1;
  runtime.dual_source_blend = true;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "IsHelperLane()");
  Contains(text, "if (!(nx_alpha > asfloat(");
  Require(Backend::HLSL::RequiredShaderModel(f.program) == 66u,
          "Helper queries need SM 6.6");
  std::ofstream("hlsl_helper.hlsl") << text;
}
void UnsupportedRollback() {
  Fixture f{Stage::TessellationControl};
  f.program.info.constant_buffer_descriptors.push_back(
      {.index = 0, .count = 1});
  Bindings bindings{.uniform_buffer = 7};
  try {
    f.Emit(bindings);
    throw std::runtime_error("Unlinked hull stage silently accepted");
  } catch (const NotImplementedException &error) {
    Contains(error.what(), "stage linkage");
  }
  Require(bindings.uniform_buffer == 7, "Failure advanced bindings");
}
void PackedSignedAttributes() {
  // Maxwell A2B10G10R10 SNorm/SInt vertex formats have no D3D12 IA spelling; the
  // input assembler fetches R10G10B10A2_UINT and the shader decodes each component.
  Fixture f{Stage::VertexB};
  f.program.info.loads.Set(IR::Attribute::Generic0X);
  f.program.info.loads.Set(IR::Attribute::Generic0W);
  f.program.info.loads.Set(IR::Attribute::Generic1Y);
  f.program.info.stores.Set(IR::Attribute::PositionX);
  const auto x = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic0X}, IR::Value{0u}});
  const auto w = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic0W}, IR::Value{0u}});
  const auto y = f.Add(IR::Opcode::GetAttribute,
                       {IR::Value{IR::Attribute::Generic1Y}, IR::Value{0u}});
  const auto sum = f.Add(IR::Opcode::FPAdd32, {x, w});
  const auto all = f.Add(IR::Opcode::FPAdd32, {sum, y});
  f.Add(IR::Opcode::SetAttribute,
        {IR::Value{IR::Attribute::PositionX}, all, IR::Value{0u}});
  RuntimeInfo runtime{};
  runtime.generic_input_types[0] = AttributeType::SignedNorm10;
  runtime.generic_input_types[1] = AttributeType::SignedInt10;
  Bindings bindings;
  const auto text = f.Emit(bindings, {}, runtime);
  Contains(text, "uint4 attr0 : TEXCOORD0");
  Contains(text, "uint4 attr1 : TEXCOORD1");
  Contains(text, "max(float((int(input.attr0.x << 22u) >> 22u)) * (1.0f / 511.0f), -1.0f)");
  Contains(text, "max(float((int(input.attr0.w << 30u) >> 30u)) * 1.0f, -1.0f)");
  Contains(text, "asfloat((int(input.attr1.y << 22u) >> 22u))");
  std::ofstream("hlsl_packed10.hlsl") << text;
}
} // namespace
// A return nested in structured control flow must become a flag plus guards: DXC keeps it as an
// extra exit edge and the Xbox driver's compile of such a shader runs out of memory.
void NestedReturns() {
  using Node = IR::AbstractSyntaxNode;
  Fixture f{Stage::Compute};
  f.program.info.storage_buffers_descriptors.push_back(
      {.count = 1, .is_written = true});
  IR::Block inner{f.pool};
  IR::Block after{f.pool};
  f.program.blocks.push_back(&inner);
  f.program.blocks.push_back(&after);
  const auto condition = f.Add(IR::Opcode::ConditionRef, {IR::Value{true}});
  inner.AppendNewInst(IR::Opcode::WriteStorage32,
                      {IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
  after.AppendNewInst(IR::Opcode::WriteStorage32,
                      {IR::Value{0u}, IR::Value{4u}, IR::Value{2u}});
  const auto push = [&](Node node) { f.program.syntax_list.push_back(node); };
  // { if (c) { { if (c) { inner; return; } } after; return; } }
  push({.data = {.if_node = {.cond = IR::U1{condition}}},
        .type = Node::Type::If});
  push({.data = {.if_node = {.cond = IR::U1{condition}}},
        .type = Node::Type::If});
  push({.data = {.block = &inner}, .type = Node::Type::Block});
  push({.type = Node::Type::Return});
  push({.type = Node::Type::EndIf});
  push({.data = {.block = &after}, .type = Node::Type::Block});
  push({.type = Node::Type::EndIf});
  push({.type = Node::Type::Return});
  Bindings bindings;
  const auto text = f.Emit(bindings);
  Contains(text, "bool nx_returned = false;");
  Contains(text, "nx_returned = true;");
  Contains(text, "if (!nx_returned) {");
  const auto count = [&](std::string_view token) {
    size_t n = 0;
    for (auto at = text.find(token); at != text.npos;
         at = text.find(token, at + 1))
      ++n;
    return n;
  };
  // Only the top-level return and the function epilogue may leave the function.
  Require(count("return;") == 2, "Nested returns must not leave the function");
  // The guard opened after the inner region closes before the outer region does.
  Require(count("if (!nx_returned) {") == 2 && count("{") == count("}"),
          "Return guards must be balanced");
  std::ofstream("hlsl_returns.hlsl") << text;
  Bindings repeated;
  Require(text == f.Emit(repeated), "Return lowering modified IR");
  // A program whose only returns are at the top level keeps the plain form.
  Fixture plain{Stage::Compute};
  plain.program.syntax_list.push_back({.type = Node::Type::Return});
  Bindings plain_bindings;
  Require(plain.Emit(plain_bindings).find("nx_returned") == std::string::npos,
          "Top-level returns must not use the flag");
}
int main() {
  try {
    Vertex();
    PackedSignedAttributes();
    Fragment();
    MultisampleFetch();
    Compute();
    ControlFlowAndWaves();
    ResourceOrderAndCompare();
    StorageImageFormats();
    ArithmeticAndPseudoResults();
    ChainedResourceBindings();
    NumericCoverage();
    GlobalAndRegisterState();
    SparseResidency();
    Geometry();
    NativeWideAtomics();
    MixedSharedMemory();
    PixelHelperAndAlphaTest();
    UnsupportedRollback();
    NestedReturns();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
