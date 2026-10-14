// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace bo3::native {
// Existing aggregate prototype cap, including targets and resolves. This is
// not a validated Switch memory budget.
constexpr uint64_t kNativeImageBudget = 512ull * 1024 * 1024;
inline bool ImageBudgetFits(uint64_t used, uint64_t requested) {
  return requested <= kNativeImageBudget && used <= kNativeImageBudget - requested;
}
// Submit currently waits for completion. Protect resources referenced by the
// recorded batch, including uploads belonging to rejected draws.
template <typename Map>
auto SelectRetiredTexture(Map& textures, bool recording, uint64_t batch) {
  auto victim = textures.end();
  for (auto it = textures.begin(); it != textures.end(); ++it) {
    if (!it->second || (recording && it->second->batch == batch)) continue;
    if (victim == textures.end() || it->second->use < victim->second->use) victim = it;
  }
  return victim;
}
}
