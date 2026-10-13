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
#include <vector>
#include <windows.h>

namespace {
enum Kind { CreatePS, CreateVS, SetPS, SetVS, LoadShaders, Draw, DrawIndexed,
            DrawUP, DrawIndexedUP, Resolve, BeginCommands, RunCommands, Swap,
            InitializePS, InitializeVS, Count };
constexpr std::array<const char*, Count> names = {
    "CreatePixelShader", "CreateVertexShader", "SetPixelShader", "SetVertexShader",
    "LoadShaders", "DrawVertices", "DrawIndexedVertices", "DrawVerticesUP",
    "DrawIndexedVerticesUP", "Resolve", "BeginCommandBuffer", "RunCommandBuffer", "Swap",
    "InitializePixelShader", "InitializeVertexShader"};
std::array<std::atomic<uint64_t>, Count> counts{};
std::atomic<uint64_t> frame{};
std::filesystem::path directory;
std::ofstream trace;
std::mutex mutex;
std::unordered_set<std::string> dumped;
std::unordered_set<uint64_t> observed_objects;
std::atomic<uint64_t> bound_vs{}, bound_ps{}, rejected_objects{};
std::atomic<uint64_t> initialized_vs{}, initialized_ps{}, rejected_initializers{};
std::ofstream objects;
std::ofstream initializers;
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
bool Readable(uint8_t* base, uint32_t address, size_t size) {
  if (!size || uint64_t(address) + size > (uint64_t(1) << 32)) return false;
  const auto end = reinterpret_cast<uintptr_t>(base + address) + size;
  auto cursor = reinterpret_cast<uintptr_t>(base + address);
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= cursor) return false;
    cursor = next;
  }
  return true;
}
std::string SaveContainer(const uint8_t* bytes, size_t size, bool vertex) {
  // FNV-1a is a filename key only; authoritative comparison uses all blob bytes.
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
  const std::string name = fmt::format("{}_{:016X}_{}.bin", vertex ? "vs" : "ps", hash, size);
  std::lock_guard lock(mutex);
  if (!dumped.insert(name).second) return name;
  const auto path = directory / "containers" / name;
  if (std::filesystem::exists(path)) {
    std::ifstream previous(path, std::ios::binary);
    std::string previous_bytes((std::istreambuf_iterator<char>(previous)), {});
    if (previous_bytes.size() == size && !std::memcmp(previous_bytes.data(), bytes, size)) return name;
    REXLOG_ERROR("BO3 shader dump filename collision: {}", path.string());
    return {};
  }
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes), std::streamsize(size));
  if (!file) return {};
  return name;
}
void DumpContainer(uint8_t* base, uint32_t address, bool vertex) {
  if (!enabled || !address || address > UINT32_MAX - 12) return;
  if (!Readable(base, address, 12)) return;
  // Both verified Create* bodies read these words, then copy header + physical data.
  const uint32_t header = Read32(base, address + 4);
  const uint32_t physical = Read32(base, address + 8);
  if (header < 12 || header > (1u << 20) || !physical || physical > (1u << 20) ||
      uint64_t(address) + header + physical > UINT32_MAX) return;
  const size_t size = size_t(header) + physical;
  if (Readable(base, address, size)) SaveContainer(base + address, size, vertex);
}
void DumpBoundObject(uint8_t* base, uint32_t object, bool vertex) {
  if (!object) return;
  // Only the first sighting of an object is a pre-patcher candidate. Identity
  // against authoritative PM4 shader loads is a later, separate validation gate.
  const uint64_t key = (uint64_t(object) << 1) | uint64_t(vertex);
  {
    std::lock_guard lock(mutex);
    if (!observed_objects.insert(key).second) return;
  }
  const uint32_t prefix = vertex ? 872 : 40;
  if (!Readable(base, object, prefix + 12) || (Read32(base, object) & 0xFFFF) != (vertex ? 6u : 7u)) {
    rejected_objects.fetch_add(1);
    return;
  }
  const uint32_t header_address = object + prefix;
  const uint32_t signature = Read32(base, header_address);
  const uint32_t header_size = Read32(base, header_address + 4);
  const uint32_t physical_size = Read32(base, header_address + 8);
  // The original InitializePixelShader stores its physical allocation in +24;
  // InitializeVertexShader stores it in +32. Header and physical data are split.
  const uint32_t physical_address = Read32(base, object + (vertex ? 32 : 24));
  if ((signature & 0xFFFFFF00) != 0x102A1100 || bool(signature & 1) != vertex ||
      header_size < 12 || header_size > (1u << 20) ||
      !physical_size || physical_size > (1u << 20) ||
      !Readable(base, header_address, header_size) || !Readable(base, physical_address, physical_size)) {
    rejected_objects.fetch_add(1);
    return;
  }
  std::vector<uint8_t> container(size_t(header_size) + physical_size);
  std::memcpy(container.data(), base + header_address, header_size);
  std::memcpy(container.data() + header_size, base + physical_address, physical_size);
  const auto filename = SaveContainer(container.data(), container.size(), vertex);
  if (filename.empty()) { rejected_objects.fetch_add(1); return; }
  (vertex ? bound_vs : bound_ps).fetch_add(1);
  std::lock_guard lock(mutex);
  objects << fmt::format("{{\"object\":\"{:08X}\",\"vertex\":{},\"frame\":{},\"header_address\":\"{:08X}\","
                         "\"physical_address\":\"{:08X}\",\"header_bytes\":{},\"physical_bytes\":{},\"file\":\"{}\"}}\n",
                         object, vertex, frame.load(), header_address, physical_address, header_size, physical_size, filename);
  objects.flush();
}
void DumpInitialization(uint8_t* base, uint32_t object, uint32_t physical, bool vertex) {
  if (!enabled) return;
  const uint32_t prefix = vertex ? 872 : 40;
  // Verified Initialize* bodies receive r3 = object and r4 = physical block.
  // Callers already copied the header at +prefix. Capture before VS initializer
  // builds patch records / changes any instruction fields.
  if (object > UINT32_MAX - prefix || !Readable(base, object + prefix, 36)) {
    rejected_initializers.fetch_add(1);
    return;
  }
  const uint32_t header = object + prefix;
  const uint32_t signature = Read32(base, header);
  const uint32_t header_size = Read32(base, header + 4);
  const uint32_t physical_size = Read32(base, header + 8);
  if ((signature & 0xFFFFFF00) != 0x102A1100 || bool(signature & 1) != vertex ||
      header_size < 36 || header_size > (1u << 20) ||
      !physical_size || physical_size > (1u << 20) ||
      !Readable(base, header, header_size) || !Readable(base, physical, physical_size)) {
    rejected_initializers.fetch_add(1);
    return;
  }
  std::vector<uint8_t> container(size_t(header_size) + physical_size);
  std::memcpy(container.data(), base + header, header_size);
  std::memcpy(container.data() + header_size, base + physical, physical_size);
  const auto filename = SaveContainer(container.data(), container.size(), vertex);
  if (filename.empty()) { rejected_initializers.fetch_add(1); return; }
  (vertex ? initialized_vs : initialized_ps).fetch_add(1);
  std::lock_guard lock(mutex);
  // A reused object address must be observed again at the next shader load.
  observed_objects.erase((uint64_t(object) << 1) | uint64_t(vertex));
  initializers << fmt::format("{{\"object\":\"{:08X}\",\"vertex\":{},\"frame\":{},"
                             "\"header_address\":\"{:08X}\",\"physical_address\":\"{:08X}\","
                             "\"file\":\"{}\"}}\n",
                             object, vertex, frame.load(), header, physical, filename);
  initializers.flush();
}
}
namespace bo3 {
void InitializeD3DTrace() {
  const char* root = std::getenv("BO3_D3D_TRACE_DIR");
  if (!root || !*root) return;
  directory = root;
  std::filesystem::create_directories(directory / "containers");
  trace.open(directory / "calls.jsonl", std::ios::trunc);
  objects.open(directory / "objects.jsonl", std::ios::trunc);
  initializers.open(directory / "initializers.jsonl", std::ios::trunc);
  enabled = trace.is_open();
  REXLOG_INFO("BO3 XDK trace {}: {}", enabled ? "enabled" : "failed", directory.string());
}
void FinishD3DTrace() {
  if (!enabled) return;
  std::lock_guard lock(mutex);
  std::ofstream summary(directory / "summary.json");
  summary << "{\n";
  for (size_t i = 0; i < Count; ++i) {
    summary << fmt::format("  \"{}\": {},\n", names[i], counts[i].load());
  }
  summary << fmt::format("  \"BoundVertexObjects\": {},\n  \"BoundPixelObjects\": {},\n  \"RejectedBoundObjects\": {},\n"
                         "  \"InitializedVertexObjects\": {},\n  \"InitializedPixelObjects\": {},\n  \"RejectedInitializers\": {}\n",
                         bound_vs.load(), bound_ps.load(), rejected_objects.load(),
                         initialized_vs.load(), initialized_ps.load(), rejected_initializers.load());
  summary << "}\n";
  trace.flush();
}
}

#define BO3_TRACE_HOOK(address, kind) \
  REX_HOOK_RAW(sub_##address) { Note(kind, ctx, base); __imp__sub_##address(ctx, base); }
BO3_TRACE_HOOK(826EF888, SetPS)
BO3_TRACE_HOOK(826EFA90, SetVS)
REX_HOOK_RAW(sub_826FD590) {
  Note(LoadShaders, ctx, base);
  if (enabled && Readable(base, ctx.r3.u32, 13076)) {
    DumpBoundObject(base, Read32(base, ctx.r3.u32 + 13072), true);
    DumpBoundObject(base, Read32(base, ctx.r3.u32 + 13068), false);
  }
  __imp__sub_826FD590(ctx, base);
}
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
REX_HOOK_RAW(sub_826EF850) {
  Note(InitializePS, ctx, base);
  DumpInitialization(base, ctx.r3.u32, ctx.r4.u32, false);
  __imp__sub_826EF850(ctx, base);
}
REX_HOOK_RAW(sub_826F0220) {
  Note(InitializeVS, ctx, base);
  DumpInitialization(base, ctx.r3.u32, ctx.r4.u32, true);
  __imp__sub_826F0220(ctx, base);
}
