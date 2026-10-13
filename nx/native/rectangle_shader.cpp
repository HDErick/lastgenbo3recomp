// SPDX-License-Identifier: GPL-3.0-only
// Uses the MW2 SPIR-V writer and the rectangle algorithm documented in the
// BSD-licensed RexGlue/Xenia Vulkan pipeline_cache.cpp (longest-edge mirror).
#include "rectangle_shader.h"
#include "spirv.h"
#include <array>
#include <stdexcept>

namespace bo3::native {
std::vector<uint32_t> BuildRectangleShader(uint32_t interpolators) {
  using namespace spirv;
  if (interpolators & ~0xffffu) throw std::runtime_error("rectangle interpolator mask");
  Module m;
  m.RequireCapability(2); // Geometry.
  const Id f = m.Float(), f4 = m.Float4(), u = m.Int(false), b = m.Bool(), v = m.Void();
  std::vector<Id> interface;
  auto variable = [&](StorageClass storage, Id type) {
    Id id = m.Allocate();
    Module::Emit(m.Declarations(), Op::Variable, {m.Pointer(storage, type), id, uint32_t(storage)});
    interface.push_back(id); return id;
  };
  const Id input_pos = variable(StorageClass::Input, m.ArrayOf(f4, 3));
  const Id output_pos = variable(StorageClass::Output, f4);
  for (Id id : {input_pos, output_pos}) Module::Emit(m.Decorations(), Op::Decorate, {id, uint32_t(Decoration::BuiltIn), uint32_t(BuiltIn::Position)});
  std::array<Id, 16> inputs{}, outputs{};
  for (uint32_t i = 0; i < 16; ++i) if (interpolators & (1u << i)) {
    inputs[i] = variable(StorageClass::Input, m.ArrayOf(f4, 3));
    outputs[i] = variable(StorageClass::Output, f4);
    for (Id id : {inputs[i], outputs[i]}) Module::Emit(m.Decorations(), Op::Decorate, {id, uint32_t(Decoration::Location), i});
  }
  const Id function_type = m.Allocate(), function = m.Allocate();
  Module::Emit(m.Declarations(), Op::TypeFunction, {function_type, v});
  m.EntryPoint(3, function, "main", interface); // Geometry.
  Module::Emit(m.ExecutionModes(), Op::ExecutionMode, {function, 22}); // Triangles.
  Module::Emit(m.ExecutionModes(), Op::ExecutionMode, {function, 29}); // OutputTriangleStrip.
  Module::Emit(m.ExecutionModes(), Op::ExecutionMode, {function, 26, 4}); // OutputVertices.
  Module::Emit(m.ExecutionModes(), Op::ExecutionMode, {function, 0, 1}); // Invocations.
  Module::Emit(m.Code(), Op::Function, {v, function, 0, function_type});
  Module::Emit(m.Code(), Op::Label, {m.Allocate()});
  auto emit = [&](Op op, Id type, std::initializer_list<Id> args) {
    Id id = m.Allocate();
    auto& code = m.Code(); code.push_back(Head(op, uint32_t(args.size()) + 3));
    code.push_back(type); code.push_back(id); code.insert(code.end(), args.begin(), args.end());
    return id;
  };
  auto load = [&](Id input, Id index) {
    return emit(Op::Load, f4, {emit(Op::AccessChain, m.Pointer(StorageClass::Input, f4), {input, index})});
  };
  const std::array<Id, 3> positions{load(input_pos, m.ConstantU(0)), load(input_pos, m.ConstantU(1)), load(input_pos, m.ConstantU(2))};
  std::array<Id, 3> length{};
  for (uint32_t i = 0; i < 3; ++i) {
    const Id a = positions[(i + 1) % 3], c = positions[(i + 2) % 3];
    const Id dx = emit(Op::FSub, f, {emit(Op::CompositeExtract, f, {c, 0}), emit(Op::CompositeExtract, f, {a, 0})});
    const Id dy = emit(Op::FSub, f, {emit(Op::CompositeExtract, f, {c, 1}), emit(Op::CompositeExtract, f, {a, 1})});
    length[i] = emit(Op::FAdd, f, {emit(Op::FMul, f, {dx, dx}), emit(Op::FMul, f, {dy, dy})});
  }
  const Id first = emit(Op::Select, u, {
      emit(Op::LogicalAnd, b, {emit(Op::FOrdGreaterThan, b, {length[0], length[1]}), emit(Op::FOrdGreaterThan, b, {length[0], length[2]})}),
      m.ConstantU(0), emit(Op::Select, u, {emit(Op::FOrdGreaterThan, b, {length[1], length[2]}), m.ConstantU(1), m.ConstantU(2)})});
  const std::array<Id, 3> indices{first,
      emit(Op::UMod, u, {emit(Op::IAdd, u, {first, m.ConstantU(1)}), m.ConstantU(3)}),
      emit(Op::UMod, u, {emit(Op::IAdd, u, {first, m.ConstantU(2)}), m.ConstantU(3)})};
  constexpr Op emit_vertex = Op(218), end_primitive = Op(219);
  for (Id index : indices) {
    Module::Emit(m.Code(), Op::Store, {output_pos, load(input_pos, index)});
    for (uint32_t i = 0; i < 16; ++i) if (inputs[i]) Module::Emit(m.Code(), Op::Store, {outputs[i], load(inputs[i], index)});
    Module::Emit(m.Code(), emit_vertex, {});
  }
  auto mirror = [&](Id input) {
    const Id a = load(input, indices[0]), c = load(input, indices[1]), d = load(input, indices[2]);
    const Id delta = emit(Op::FSub, f4, {c, a}), result = emit(Op::FAdd, f4, {delta, d});
    for (Id id : {delta, result}) Module::Emit(m.Decorations(), Op::Decorate, {id, 42}); // NoContraction.
    return result;
  };
  Module::Emit(m.Code(), Op::Store, {output_pos, mirror(input_pos)});
  for (uint32_t i = 0; i < 16; ++i) if (inputs[i]) Module::Emit(m.Code(), Op::Store, {outputs[i], mirror(inputs[i])});
  Module::Emit(m.Code(), emit_vertex, {}); Module::Emit(m.Code(), end_primitive, {});
  Module::Emit(m.Code(), Op::Return, {}); Module::Emit(m.Code(), Op::FunctionEnd, {});
  return m.Finish();
}
}
