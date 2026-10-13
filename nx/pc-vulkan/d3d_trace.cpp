// BO3 Zombies 0.0.8.5 XDK census. Calls original bodies without altering guest state.
// Evidence: analysis/bo3_d3d_candidates.json and analysis/BO3_NATIVE_FOUNDATION.md.
#include "bo3_zm_init.h"
#include "d3d_trace.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace {
enum Kind { CreatePS, CreateVS, SetPS, SetVS, LoadShaders, Draw, DrawIndexed,
            DrawUP, DrawIndexedUP, Resolve, BeginCommands, RunCommands, Swap, Count };
constexpr std::array<const char*, Count> names = {
    "CreatePixelShader", "CreateVertexShader", "SetPixelShader", "SetVertexShader",
    "LoadShaders", "DrawVertices", "DrawIndexedVertices", "DrawVerticesUP",
    "DrawIndexedVerticesUP", "Resolve", "BeginCommandBuffer", "RunCommandBuffer", "Swap"};
std::array<std::atomic<uint64_t>, Count> counts{};
std::atomic<uint64_t> frame{};
std::filesystem::path directory;
std::ofstream trace;
std::mutex mutex;
std::unordered_set<std::string> dumped;
bool enabled = false;
uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
void Note(Kind kind, PPCContext& ctx, uint8_t* base) {
  if (!enabled) return;
  const auto ordinal = counts[kind].fetch_add(1, std::memory_order_relaxed) + 1;
  if (kind == Swap) frame.fetch_add(1, std::memory_order_relaxed);
  // Bounded logging; counters are diagnostic overhead, never performance evidence.
  if (ordinal > 8 && !(kind == Swap && ordinal % 120 == 0)) return;
  uint32_t vs = 0, ps = 0;
  if (kind == LoadShaders) {
    // Verified literal loads in sub_826FD590 after r31 = r3.
    const uint32_t device = ctx.r3.u32;
    if (device && device <= UINT32_MAX - 13076) {
      vs = Read32(base, device + 13072);
      ps = Read32(base, device + 13068);
    }
  }
  std::lock_guard lock(mutex);
  trace << fmt::format("{{\"kind\":\"{}\",\"call\":{},\"frame\":{},\"lr\":\"{:08X}\","
                       "\"r3\":\"{:08X}\",\"r4\":\"{:08X}\",\"r5\":\"{:08X}\","
                       "\"r6\":\"{:08X}\",\"r7\":\"{:08X}\",\"vs\":\"{:08X}\",\"ps\":\"{:08X}\"}}\n",
                       names[kind], ordinal, frame.load(), uint32_t(ctx.lr), ctx.r3.u32,
                       ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, vs, ps);
  trace.flush();
}
void DumpContainer(uint8_t* base, uint32_t address, bool vertex) {
  if (!enabled || !address || address > UINT32_MAX - 12) return;
  // Both verified Create* bodies read these words, then copy header + physical data.
  const uint32_t header = Read32(base, address + 4);
  const uint32_t physical = Read32(base, address + 8);
  if (header < 12 || header > (1u << 20) || !physical || physical > (1u << 20) ||
      uint64_t(address) + header + physical > UINT32_MAX) return;
  const size_t size = size_t(header) + physical;
  // FNV-1a is a filename key only; authoritative comparison uses all blob bytes.
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ base[address + i]) * 1099511628211ull;
  const std::string name = fmt::format("{}_{:016X}_{}.bin", vertex ? "vs" : "ps", hash, size);
  std::lock_guard lock(mutex);
  if (!dumped.insert(name).second) return;
  const auto path = directory / "containers" / name;
  if (std::filesystem::exists(path)) {
    std::ifstream previous(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(previous)), {});
    if (bytes.size() == size && !std::memcmp(bytes.data(), base + address, size)) return;
    REXLOG_ERROR("BO3 shader dump filename collision: {}", path.string());
    return;
  }
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(base + address), std::streamsize(size));
}
}
namespace bo3 {
void InitializeD3DTrace() {
  const char* root = std::getenv("BO3_D3D_TRACE_DIR");
  if (!root || !*root) return;
  directory = root;
  std::filesystem::create_directories(directory / "containers");
  trace.open(directory / "calls.jsonl", std::ios::trunc);
  enabled = trace.is_open();
  REXLOG_INFO("BO3 XDK trace {}: {}", enabled ? "enabled" : "failed", directory.string());
}
void FinishD3DTrace() {
  if (!enabled) return;
  std::lock_guard lock(mutex);
  std::ofstream summary(directory / "summary.json");
  summary << "{\n";
  for (size_t i = 0; i < Count; ++i) {
    summary << fmt::format("  \"{}\": {}{}\n", names[i], counts[i].load(), i + 1 < Count ? "," : "");
  }
  summary << "}\n";
  trace.flush();
}
}

#define BO3_TRACE_HOOK(address, kind) \
  REX_HOOK_RAW(sub_##address) { Note(kind, ctx, base); __imp__sub_##address(ctx, base); }
BO3_TRACE_HOOK(826EF888, SetPS)
BO3_TRACE_HOOK(826EFA90, SetVS)
BO3_TRACE_HOOK(826FD590, LoadShaders)
BO3_TRACE_HOOK(826F9190, Draw)
BO3_TRACE_HOOK(826F95A8, DrawIndexed)
BO3_TRACE_HOOK(826F8B68, DrawUP)
BO3_TRACE_HOOK(826F90F0, DrawIndexedUP)
BO3_TRACE_HOOK(826EE3D0, Resolve)
BO3_TRACE_HOOK(826F2C98, BeginCommands)
BO3_TRACE_HOOK(826F40F0, RunCommands)
REX_HOOK_RAW(sub_826EAD10) {
  Note(Swap, ctx, base);
  // The host can hard-exit at title termination before OnShutdown is called.
  if (enabled && (frame.load() == 1 || frame.load() % 120 == 0)) bo3::FinishD3DTrace();
  __imp__sub_826EAD10(ctx, base);
}
REX_HOOK_RAW(sub_826F00F0) {
  Note(CreatePS, ctx, base);
  DumpContainer(base, ctx.r3.u32, false);
  __imp__sub_826F00F0(ctx, base);
}
REX_HOOK_RAW(sub_826F0318) {
  Note(CreateVS, ctx, base);
  DumpContainer(base, ctx.r3.u32, true);
  __imp__sub_826F0318(ctx, base);
}
