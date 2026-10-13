        // Reconstruct clip coordinates from the guest's W / reciprocal-W and
        // pre-divided XY/Z formats, then apply the SDK's host NDC conversion.
        // Push ABI: XY scale/offset at 16, Z scale/offset + format bits at 32.
        if (m_type == shader::Type::Vertex && m_ndc && m_position)
        {
            Id pointer = m_module.Pointer(StorageClass::PushConstant, m_float4);
            auto load = [&](uint32_t member) {
                return Emit(Op::Load, m_float4,
                            { Emit(Op::AccessChain, pointer,
                                   { m_ndc, m_module.ConstantU(member) }) });
            };
            Id ndc = load(0), depth = load(1);
            Id flags = Emit(Op::ConvertFToU, m_uint,
                            { Emit(Op::CompositeExtract, m_float, { depth, 2 }) });
            auto flag = [&](uint32_t bit) {
                return Emit(Op::INotEqual, m_bool,
                            { Emit(Op::BitwiseAnd, m_uint, { flags, m_module.ConstantU(bit) }),
                              m_module.ConstantU(0) });
            };
            Id value = Emit(Op::Load, m_float4, { m_position });
            Id guest_w = Emit(Op::CompositeExtract, m_float, { value, 3 });
            Id w = Emit(Op::Select, m_float,
                        { flag(4), guest_w,
                          Emit(Op::FDiv, m_float, { m_module.ConstantF(1.0f), guest_w }) });
            auto formatted = [&](uint32_t component) {
                Id v = Emit(Op::CompositeExtract, m_float, { value, component });
                return Emit(Op::Select, m_float,
                            { flag(component == 2 ? 2 : 1),
                              Emit(Op::FMul, m_float, { v, w }), v });
            };
            auto xy = [&](uint32_t component) {
                return Glsl450(Glsl::Fma, m_float,
                               { formatted(component),
                                 Emit(Op::CompositeExtract, m_float, { ndc, component }),
                                 Emit(Op::FMul, m_float,
                                      { Emit(Op::CompositeExtract, m_float, { ndc, component + 2 }), w }) });
            };
            Id z = Glsl450(Glsl::Fma, m_float,
                           { formatted(2), Emit(Op::CompositeExtract, m_float, { depth, 0 }),
                             Emit(Op::FMul, m_float,
                                  { Emit(Op::CompositeExtract, m_float, { depth, 1 }), w }) });
            Module::Emit(m_module.Code(), Op::Store,
                         { m_position, Emit(Op::CompositeConstruct, m_float4, { xy(0), xy(1), z, w }) });
        }

