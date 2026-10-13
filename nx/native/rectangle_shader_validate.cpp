// SPDX-License-Identifier: GPL-3.0-only
// Emit the actual runtime geometry modules for structural validation.
#include "rectangle_shader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  try {
    if (argc < 3) throw std::runtime_error("usage: rectangle_shader_validate <output-directory> <mask>...");
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    for (int i = 2; i < argc; ++i) {
      const uint32_t mask = uint32_t(std::stoul(argv[i]));
      const auto code = bo3::native::BuildRectangleShader(mask);
      const auto path = directory / ("gs_" + std::to_string(mask) + ".spv");
      if (std::filesystem::exists(path)) throw std::runtime_error("output already exists");
      std::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
      if (!file) throw std::runtime_error("module write failed");
      std::cout << "mask=" << mask << " words=" << code.size() << '\n';
    }
    bool rejected = false;
    try { bo3::native::BuildRectangleShader(0x10000); } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("invalid mask was accepted");
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
