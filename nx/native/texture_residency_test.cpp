// SPDX-License-Identifier: GPL-3.0-only
#include "texture_residency.h"
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
using namespace bo3::native;
struct Texture { uint64_t batch, use; };
static unsigned checks = 0;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
  ++checks;
}
int main() {
  try {
    constexpr uint64_t mib = 1024 * 1024;
    std::map<unsigned, std::unique_ptr<Texture>> textures;
    uint64_t bytes = 128 * mib;
    for (unsigned i = 0; i < 400; ++i) {
      if(!ImageBudgetFits(bytes, mib / 2))throw std::runtime_error("small asset working set cannot fit");
      textures[i] = std::make_unique<Texture>(Texture{1, i});
      bytes += mib / 2;
    }
    Check(bytes == 328 * mib && textures.size() == 400, "working set retained above old entry cap");
    Check(ImageBudgetFits(kNativeImageBudget - 4, 4), "exact budget accepted");
    Check(!ImageBudgetFits(kNativeImageBudget - 4, 5), "pressure detected in allocation bytes");
    Check(!ImageBudgetFits(1, UINT64_MAX), "allocation size overflow refused");
    Check(!ImageBudgetFits(UINT64_MAX, 1), "accounting overflow refused");
    textures[0]->batch = 2;
    Check(SelectRetiredTexture(textures, true, 2)->first == 1, "oldest recorded texture protected");
    Check(SelectRetiredTexture(textures, false, 2)->first == 0, "retired batch may be evicted");
    for (auto& [key, value] : textures) value->batch = 2;
    Check(SelectRetiredTexture(textures, true, 2) == textures.end(), "all recorded resources protected");
    textures[401] = nullptr;
    Check(SelectRetiredTexture(textures, true, 2) == textures.end(), "allocation placeholder cannot become victim");
    textures[399]->batch = 1;
    Check(SelectRetiredTexture(textures, true, 2)->first == 399, "only retired resource eligible under pressure");
    std::cout << checks << " texture residency checks passed; no GPU launched\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
