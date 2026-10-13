// SPDX-License-Identifier: GPL-3.0-only
// BO3 adaptation of the pinned MW2 translator. Each instruction selects its
// own typed view; a slot may be used as both 2D and 3D in one program.
    void Translator::DeclareTextureViews()
    {
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            const uint32_t mask = m_result.textureViewMasks[slot];
            if (!mask) continue;
            // Retain the old single-kind summary only for unambiguous slots.
            m_result.textureKinds[slot] = (mask & (mask - 1)) ? 255 :
                                         (mask & 1) ? 0 : (mask & 2) ? 1 : 2;
            for (uint32_t kind = 0; kind < 3; ++kind)
            {
                if (!(mask & (1u << kind))) continue;
                // MW2 kinds: 0 = 2D (also Xenos 1D), 1 = cube, 2 = 3D.
                const uint32_t dim = kind == 2 ? 2 : kind == 1 ? 3 : 1;
                const Id image = m_module.Image(m_float, dim, false);
                const Id sampled = m_module.SampledImage(image);
                m_textureImageTypes[slot][kind] = image;
                const Id pointer = m_module.Pointer(StorageClass::UniformConstant, sampled);
                const Id variable = m_module.Allocate();
                Module::Emit(m_module.Declarations(), Op::Variable,
                             { pointer, variable, uint32_t(StorageClass::UniformConstant) });
                Module::Emit(m_module.Decorations(), Op::Decorate,
                             { variable, uint32_t(Decoration::DescriptorSet), bindings::kTextureSet });
                Module::Emit(m_module.Decorations(), Op::Decorate,
                             { variable, uint32_t(Decoration::Binding), shader::TextureViewBinding(slot, kind) });
                m_textures[slot][kind] = variable;
                m_textureTypes[slot][kind] = sampled;
            }
        }
    }
