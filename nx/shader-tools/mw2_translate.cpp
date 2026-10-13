// SPDX-License-Identifier: GPL-3.0-only
// Isolated BO3 comparison driver for PaulCombal/mw2-recompiled's translator.
// Input is a Windows SDK DumpUcode file: little-endian dwords, explicit stage.
// The game runtime and its rendering behavior are not changed by this tool.
#include "shader_translator.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::string Quote(const std::string& value) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += char(c);
  }
  return out + '"';
}
template<typename T, size_t N>
static void Array(const T (&values)[N]) {
  std::cout << '[';
  for (size_t i = 0; i < N; ++i) {
    if (i) std::cout << ',';
    std::cout << uint64_t(values[i]);
  }
  std::cout << ']';
}
int main(int argc, char** argv) try {
  if (argc != 4 || (std::string(argv[1]) != "vertex" && std::string(argv[1]) != "fragment"))
    throw std::runtime_error("usage: mw2_translate <vertex|fragment> <SDK dump> <output.spv>");
  const std::filesystem::path input = argv[2], output = argv[3];
  if (std::filesystem::exists(output)) throw std::runtime_error("output already exists");
  if (std::filesystem::absolute(input).lexically_normal() == std::filesystem::absolute(output).lexically_normal())
    throw std::runtime_error("input and output must differ");
  const auto bytes = std::filesystem::file_size(input);
  if (bytes < 12 || bytes % 4 || bytes > 4 * 1024 * 1024)
    throw std::runtime_error("input is truncated, unaligned or oversized");
  std::ifstream file(input, std::ios::binary);
  std::vector<uint8_t> data(size_t(bytes), 0);
  if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes)))
    throw std::runtime_error("input read failed");
  std::vector<uint32_t> words(data.size() / 4);
  for (size_t i = 0; i < words.size(); ++i)
    words[i] = uint32_t(data[i * 4]) | (uint32_t(data[i * 4 + 1]) << 8) |
               (uint32_t(data[i * 4 + 2]) << 16) | (uint32_t(data[i * 4 + 3]) << 24);
  const bool vertex = std::string(argv[1]) == "vertex";
  const auto start = std::chrono::steady_clock::now();
  const auto result = shader::Translate(vertex ? shader::Type::Vertex : shader::Type::Pixel,
                                        words.data(), words.size(), 1);
  const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (result.ok) {
    std::ofstream module(output, std::ios::binary);
    for (uint32_t word : result.spirv) {
      const char encoded[] = {char(word), char(word >> 8), char(word >> 16), char(word >> 24)};
      module.write(encoded, 4);
    }
    module.close();
    if (!module) throw std::runtime_error("SPIR-V write failed");
  }
  std::cout << "{\"ok\":" << (result.ok ? "true" : "false")
            << ",\"error\":" << Quote(result.error) << ",\"translate_ms\":" << elapsed
            << ",\"description\":" << Quote(shader::Describe(result))
            << ",\"spirv_words\":" << result.spirv.size()
            << ",\"instructions\":" << result.instructionCount
            << ",\"interpolator_mask\":" << result.interpolatorMask
            << ",\"texture_mask\":" << result.textureMask
            << ",\"vertex_fetch_mask\":" << result.vertexFetchMask
            << ",\"constants_read\":" << result.constantsRead
            << ",\"addressed_constants\":" << (result.addressedConstants ? "true" : "false")
            << ",\"colour_mask\":" << result.colourMask
            << ",\"writes_depth\":" << (result.writesDepth ? "true" : "false")
            << ",\"writes_memory\":" << (result.writesMemory ? "true" : "false")
            << ",\"dropped_exports\":" << result.droppedExports
            << ",\"dropped_memory_exports\":" << result.droppedMemoryExports
            << ",\"texture_kinds\":";
  Array(result.textureKinds);
  std::cout << ",\"vertex_fetch_subs\":"; Array(result.vertexFetchSubs);
  std::cout << ",\"vertex_fetch_stride\":"; Array(result.vertexFetchStride);
  std::cout << ",\"vertex_fetch_tail\":"; Array(result.vertexFetchTail);
  std::cout << "}\n";
  return result.ok ? 0 : 2;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
