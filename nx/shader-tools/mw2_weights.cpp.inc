// SPDX-License-Identifier: GPL-3.0-only
// BO3's observed getWeights profile. Coordinate/fraction rules follow the
// staged RexGlue/Xenia spirv_translator_fetch.cpp (BSD-3-Clause), including
// its 1.5/1024 rounding offset. No general mip-LOD implementation is claimed.
    void Translator::TranslateGetWeights(const Fetch& op)
    {
        // Every captured BO3 use has this profile. Refuse broader behavior
        // rather than assigning results to mip/3D cases we have not validated.
        if (op.Dimension() != FetchDimension::D2 || op.UseComputedLod() ||
            op.UseRegisterLod() || op.CoordinatesUnnormalised() || op.LodBias() ||
            op.MagFilter() != 1 || op.MinFilter() != 1)
        {
            Fail("unsupported BO3 getWeights profile", uint32_t(op.Dimension()));
            return;
        }
        for (uint32_t i = 0; i < 4; ++i)
            if (op.DestComponent(i) == 2 || op.DestComponent(i) == 3)
            {
                Fail("unsupported BO3 getWeights Z/W component", op.DestComponent(i));
                return;
            }
        const uint32_t slot = op.ConstantIndex() & 31;
        Id dimensions = TextureFetchWord(slot, 2);
        Id source = LoadTemp(op.SourceRegister());
        const uint32_t swizzle = op.TextureSourceSwizzle();
        const int32_t offsets[2] = { op.OffsetX(), op.OffsetY() };
        Id components[2];
        for (uint32_t axis = 0; axis < 2; ++axis)
        {
            // The draw must supply a 2D-compatible fetch constant. Its width
            // and height are 13-bit size-minus-one fields, not host-image size.
            Id encoded = Emit(Op::ShiftRightLogical, m_uint,
                              { dimensions, m_module.ConstantU(axis * 13) });
            encoded = Emit(Op::BitwiseAnd, m_uint, { encoded, m_module.ConstantU(8191) });
            Id extent = Emit(Op::ConvertUToF, m_float,
                             { Emit(Op::IAdd, m_uint, { encoded, m_module.ConstantU(1) }) });
            Id coordinate = Component(source, (swizzle >> (axis * 2)) & 3);
            Id texel = Emit(Op::FMul, m_float, { coordinate, extent });
            Id shifted = Emit(Op::FAdd, m_float,
                              { texel, m_module.ConstantF(float(offsets[axis]) * 0.5f +
                                                          1.5f / 1024.0f - 0.5f) });
            // Preserve the multiply/add rounding at texel boundaries.
            Module::Emit(m_module.Decorations(), Op::Decorate, { texel, 42 }); // NoContraction
            Module::Emit(m_module.Decorations(), Op::Decorate, { shifted, 42 });
            Id weight = Glsl450(Glsl::Fract, m_float, { shifted });
            Id ordered = Emit(Op::FOrdEqual, m_bool, { coordinate, coordinate });
            components[axis] = Emit(Op::Select, m_float,
                                    { ordered, weight, m_module.ConstantF(0.0f) });
        }
        // WriteFetchResult retains the instruction's swizzle and predicate.
        WriteFetchResult(op, components, 2);
    }
