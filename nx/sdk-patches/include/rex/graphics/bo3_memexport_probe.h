// SPDX-License-Identifier: BSD-3-Clause
// Optional BO3 native-renderer preparation. Called only on the GPU command
// thread after shader analysis, so register snapshots match PM4 consumption.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <rex/graphics/register_file.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>

namespace rex::graphics::bo3 {
inline void WriteProbeWords(const std::filesystem::path& path,
                            const uint32_t* words, size_t count) {
  if (std::filesystem::exists(path)) throw std::runtime_error("probe output already exists");
  std::ofstream file(path, std::ios::binary);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t word = words[i];
    const char bytes[] = {char(word), char(word >> 8), char(word >> 16), char(word >> 24)};
    file.write(bytes, 4);
  }
  file.close();
  if (!file) throw std::runtime_error("probe output write failed");
}

template <typename IndexInfo>
inline void CaptureMemexportState(const RegisterFile& regs, const Shader& shader,
                                  uint32_t primitive, uint32_t count,
                                  const IndexInfo* indices, bool explicit_major) {
  static const std::string root = [] {
    const char* env = std::getenv("BO3_MEMEXPORT_PROBE_DIR");
    return env ? std::string(env) : std::string();
  }();
  if (root.empty()) return;
  static bool failed = false;
  static std::unordered_map<uint64_t, uint32_t> samples;
  if (failed) return;
  const uint64_t hash = shader.ucode_data_hash(); // filename key, not authoritative identity
  auto found = samples.find(hash);
  // At most 8 programs * 16 snapshots * ~80 KB. No guest buffer reads or writes.
  if (found == samples.end() && samples.size() >= 8) return;
  auto& sample = samples[hash];
  if (sample >= 16) return;
  const uint32_t ordinal = ++sample;
  try {
    const std::filesystem::path directory(root);
    std::filesystem::create_directories(directory);
    const auto name = fmt::format("{:016X}_{:02}", hash, ordinal);
    WriteProbeWords(directory / (name + ".regs.bin"), regs.values, RegisterFile::kRegisterCount);
    WriteProbeWords(directory / (name + ".ucode.bin.vert"), shader.ucode_dwords(), shader.ucode_dword_count());
    const auto record = directory / (name + ".json");
    if (std::filesystem::exists(record)) throw std::runtime_error("probe metadata already exists");
    std::ofstream metadata(record);
    metadata << fmt::format(
        "{{\"version\":1,\"word_order\":\"little-endian\",\"shader_key\":\"{:016X}\","
        "\"sample\":{},\"primitive\":{},\"count\":{},\"explicit_major\":{},"
        "\"indexed\":{},\"index_address\":{},\"index_count\":{},\"index_format\":{},"
        "\"index_endian\":{},\"index_bytes\":{},\"registers\":\"{}.regs.bin\","
        "\"microcode\":\"{}.ucode.bin.vert\"}}\n",
        hash, ordinal, primitive, count, explicit_major, indices != nullptr,
        indices ? indices->guest_base : 0, indices ? indices->count : 0,
        indices ? uint32_t(indices->format) : 0, indices ? uint32_t(indices->endianness) : 0,
        indices ? indices->length : 0, name, name);
    metadata.close();
    if (!metadata) throw std::runtime_error("probe metadata write failed");
    REXGPU_INFO("BO3 memexport state captured: {} (sample {})", name, ordinal);
  } catch (const std::exception& error) {
    failed = true;
    REXGPU_ERROR("BO3 memexport probe stopped: {}", error.what());
  }
}
} // namespace rex::graphics::bo3
