// SPDX-License-Identifier: GPL-3.0-only
#include "edram_clear.h"
#include <iostream>
#include <stdexcept>
using namespace bo3::native;
void Expect(ClearRectangle actual, ClearRectangle expected) {
  if (actual.x != expected.x || actual.y != expected.y || actual.width != expected.width || actual.height != expected.height)
    throw std::runtime_error("incorrect sample-grid clear rectangle");
}
int main() {
  try {
    // Observed BO3 fast-clear layouts map the same number of EDRAM samples.
    Expect(FromSampleRectangle(ToSampleRectangle({0,0,440,720},2),880,880,1), {0,0,880,720});
    Expect(FromSampleRectangle(ToSampleRectangle({0,0,640,360},2),1280,1280,0), {0,0,1280,720});
    Expect(FromSampleRectangle(ToSampleRectangle({0,0,400,400},2),800,800,0), {0,0,800,800});
    Expect(FromSampleRectangle(ToSampleRectangle({8,16,32,64},2),880,880,1), {16,16,64,64});
    Expect(FromSampleRectangle(ToSampleRectangle({16,16,64,64},1),880,440,2), {8,16,32,64});
    bool bad_pitch = false, partial = false, bad_samples = false;
    try { FromSampleRectangle({0,0,8,8},880,800,0); } catch (...) { bad_pitch = true; }
    try { FromSampleRectangle({1,0,8,8},880,440,2); } catch (...) { partial = true; }
    try { ToSampleRectangle({0,0,8,8},3); } catch (...) { bad_samples = true; }
    if (!bad_pitch || !partial || !bad_samples) throw std::runtime_error("unsafe alias accepted");
    const int32_t edges[]{-129,-128,-127,0,127,128,129,440*256+128};
    const int32_t expected[]{-1,-1,0,0,0,0,1,440};
    for (uint32_t i = 0; i < 8; ++i) if (ClearPixelBoundary(edges[i]) != expected[i])
      throw std::runtime_error("incorrect top-left pixel-center coverage");
    std::cout << "8 EDRAM clear mapping cases and 8 pixel-center boundary cases passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
