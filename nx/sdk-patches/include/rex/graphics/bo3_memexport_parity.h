// SPDX-License-Identifier: BSD-3-Clause
// Bounded diagnostic capture around the existing synchronous GPU readback.
// This probe never supplies replacement output to the game.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <rex/graphics/bo3_memexport_probe.h>
#include <rex/memory.h>
#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::graphics::bo3 {
struct MemexportParityPending {
  std::filesystem::path path;
  uint32_t destination = 0, size = 0;
  bool valid = false;
};
inline MemexportParityPending& ParityPending() { static MemexportParityPending p; return p; }
inline bool ParityReadable(const void* pointer, size_t bytes) {
#if REX_PLATFORM_WIN32
  uintptr_t at = uintptr_t(pointer), end = at + bytes;
  if (!bytes || end < at) return false;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t next = uintptr_t(info.BaseAddress) + info.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
#else
  return false;  // PC-only validation tool.
#endif
}
inline void ParityBytes(const std::filesystem::path& path, const void* data, size_t bytes) {
  if (std::filesystem::exists(path)) throw std::runtime_error("parity output exists");
  std::ofstream file(path, std::ios::binary); file.write(static_cast<const char*>(data), bytes); file.close();
  if (!file) throw std::runtime_error("parity write failed");
}
template <typename IndexInfo>
inline void CaptureMemexportBefore(const RegisterFile& regs, const Shader& shader,
    rex::memory::Memory& memory, uint32_t primitive, uint32_t count, const IndexInfo* indices, bool major) {
  static const std::string root = [] { const char* p = std::getenv("BO3_MEMEXPORT_PARITY_DIR"); return p ? std::string(p) : std::string(); }();
  if (root.empty()) return;
  auto& pending = ParityPending(); pending.valid = false;
  const uint64_t key = shader.ucode_data_hash();
  if (key != 0x5B9B7484417FB9B6ull && key != 0xA4827826E2589916ull) return;
  static bool failed = false; static std::map<uint64_t, uint32_t> samples;
  if (failed || samples[key] >= 4) return;
  try {
    auto vf = regs.GetVertexFetch(95);
    uint32_t source = vf.address << 2, source_bytes = vf.size << 2;
    auto c0 = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
    uint32_t destination = c0[0] << 2, output_bytes = (c0[3] & 0x7fffff) * 8;
    if (!source_bytes || !output_bytes || source_bytes > 2 * 1024 * 1024 || output_bytes > 2 * 1024 * 1024 || uint64_t(source) + source_bytes > 0x20000000 || uint64_t(destination) + output_bytes > 0x20000000) return;
    const void* input = memory.TranslatePhysical(source); const void* original = memory.TranslatePhysical(destination);
    if (!ParityReadable(input, source_bytes) || !ParityReadable(original, output_bytes)) return;
    const std::filesystem::path directory(root); std::filesystem::create_directories(directory);
    const auto name = fmt::format("{:016X}_{:02}", key, ++samples[key]);
    WriteProbeWords(directory / (name + ".regs.bin"), regs.values, RegisterFile::kRegisterCount);
    WriteProbeWords(directory / (name + ".ucode.bin.vert"), shader.ucode_dwords(), shader.ucode_dword_count());
    ParityBytes(directory / (name + ".input.bin"), input, source_bytes);
    ParityBytes(directory / (name + ".before.bin"), original, output_bytes);
    std::ofstream meta(directory / (name + ".json"));
    meta << fmt::format("{{\"version\":1,\"word_order\":\"little-endian\",\"primitive\":{},\"count\":{},\"indexed\":{},\"explicit_major\":{},\"source\":{},\"destination\":{},\"registers\":\"{}.regs.bin\",\"microcode\":\"{}.ucode.bin.vert\",\"input\":\"{}.input.bin\",\"before\":\"{}.before.bin\",\"output\":\"{}.output.bin\"}}\n", primitive, count, indices != nullptr, major, source, destination, name, name, name, name, name);
    meta.close(); if (!meta) throw std::runtime_error("parity metadata failed");
    pending.path = directory / (name + ".output.bin"); pending.destination = destination; pending.size = output_bytes; pending.valid = true;
    REXGPU_INFO("BO3 memexport parity input captured: {} bytes={} outputBytes={}", name, source_bytes, output_bytes);
  } catch (const std::exception& error) { failed = true; REXGPU_ERROR("BO3 parity capture stopped: {}", error.what()); }
}
inline void CaptureMemexportAfter(rex::memory::Memory& memory) {
  auto& pending = ParityPending(); if (!pending.valid) return; pending.valid = false;
  try { ParityBytes(pending.path, memory.TranslatePhysical(pending.destination), pending.size); REXGPU_INFO("BO3 memexport parity synchronous output captured: {}", pending.path.filename().string()); }
  catch (const std::exception& error) { REXGPU_ERROR("BO3 parity output failed: {}", error.what()); }
}
}  // namespace rex::graphics::bo3
