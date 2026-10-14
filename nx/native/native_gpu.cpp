// SPDX-License-Identifier: GPL-3.0-only
// BO3 PC native Vulkan prototype. The MW2-derived translator has GPL-3.0-only
// provenance; SDK command parsing and host presentation retain their BSD license.
#include "shader_translator.h"
#include "present_spirv.h"
#include "memexport_copy.h"
#include "bc_decode.h"
#include "physical_access.h"
#include "rectangle_shader.h"
#include "edram_clear.h"
#include "texture_upload.h"
#include "resolve_ownership.h"
#include "color_resolve.h"
#include <mutex>
#include "sha256.h"
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/graphics_util.h>
#include <xxhash.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace bo3::native {
using namespace rex::graphics;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanProvider;
using rex::ui::vulkan::VulkanPresenter;
constexpr uint32_t kArenaBytes = 16 * 1024 * 1024;
constexpr uint32_t kBatchDraws = 32;
using PerfClock = std::chrono::steady_clock;
static double Milliseconds(PerfClock::time_point start) { return std::chrono::duration<double, std::milli>(PerfClock::now() - start).count(); }
struct TimedScope {
  double& total; PerfClock::time_point start = PerfClock::now();
  explicit TimedScope(double& value) : total(value) {}
  ~TimedScope() { total += Milliseconds(start); }
};
// Windows thread time excludes time spent waiting or descheduled. Keep it
// separate from the elapsed scopes, which may include guest page faults.
static double ThreadMilliseconds() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
  auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10000.0;
}
struct ThreadTimedScope {
  double& total; double start = ThreadMilliseconds();
  explicit ThreadTimedScope(double& value) : total(value) {}
  ~ThreadTimedScope() { const double end = ThreadMilliseconds(); if (start >= 0 && end >= start) total += end - start; }
};
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
static void Check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw std::runtime_error(fmt::format("{} returned {}", operation, int(result)));
}
static uint32_t EndianWord(uint32_t v, uint32_t endian) {
  if (endian == 1) return ((v & 0xff00ff) << 8) | ((v >> 8) & 0xff00ff);
  if (endian == 2) return __builtin_bswap32(v);
  if (endian == 3) return (v << 16) | (v >> 16);
  return v;
}
struct NativeShader : Shader {
  shader::Translation native;
  VkShaderModule module = VK_NULL_HANDLE;
  std::string canonical_sha256;
  NativeShader(xenos::ShaderType type, const uint32_t* words, size_t count)
      : Shader(type, XXH3_64bits(words, count * 4), words, count) {
    rex::string::StringBuffer disassembly;
    AnalyzeUcode(disassembly);
    canonical_sha256 = sha256::SHA256()(words, count * 4);
    native = shader::Translate(type == xenos::ShaderType::kVertex ? shader::Type::Vertex : shader::Type::Pixel,
                              ucode_dwords(), ucode_dword_count());
  }
};
struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
  uint64_t allocation = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};
// Depth ownership is independent of color attachments: a depth prepass and
// subsequent equal-depth color pass must observe the same image and layout.
struct DepthSurface { Image image; VkImageView sampled{}; };
struct DepthClear { VkRect2D rectangle; VkClearDepthStencilValue value; uint32_t packed; VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; uint64_t sequence = 0; };
struct Target { Image color, display; DepthSurface* depth{}; bool depth_only = false; VkRenderPass pass{}; VkFramebuffer framebuffer{}; uint64_t draws = 0, color_sequence = 0; };
struct Pipeline {
  VkDescriptorSetLayout sets[3]{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  std::vector<VkDescriptorSetLayoutBinding> textures;
  uint32_t probes = 0;
  uint64_t vs_hash = 0, ps_hash = 0;
  uint32_t depth_state = 0, color_mask = 0;
  bool rectangle = false;
};
struct Texture { Image image; VkSampler sampler{}; std::array<uint64_t, 2> content{}; bool gpu_resolved = false; };

struct ResolvePipeline { Pipeline gpu; VkRenderPass pass{}; };
struct ResolvedSurface {
  Image image; VkFramebuffer framebuffer{}; ColorResolvePlan plan{};
  std::vector<ResolveRect> coverage; uint64_t sequence=0;
};
class NativeCommandProcessor final : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* gs, rex::system::KernelState* ks) : CommandProcessor(gs, ks) {}
  void IssueSwap(uint32_t address, uint32_t width, uint32_t height) override;
 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  Shader* LoadShader(xenos::ShaderType type, uint32_t, const uint32_t* data, uint32_t count) override;
  bool IssueDraw(xenos::PrimitiveType primitive, uint32_t count, IndexBufferInfo* indices, bool major) override;
  bool IssueCopy() override;
 private:
  const VulkanDevice* gpu_ = nullptr;
  PFN_vkCmdClearDepthStencilImage clear_depth_ = nullptr;
  PFN_vkCmdClearAttachments clear_attachments_ = nullptr;
  PFN_vkCmdResolveImage resolve_image_ = nullptr;
  PFN_vkCmdCopyImage copy_image_ = nullptr;
  PFN_vkGetPhysicalDeviceImageFormatProperties image_properties_ = nullptr;
  PFN_vkGetQueryPoolResults query_results_ = nullptr;
  VkCommandPool command_pool_{};
  VkCommandBuffer command_{};
  VkFence fence_{};
  VkDescriptorPool descriptors_{};
  VkQueryPool coverage_queries_{};
  struct CoverageProbe { uint32_t query; Pipeline* pipeline; Target* target; uint64_t swap; };
  std::vector<CoverageProbe> coverage_probes_;
  VkBuffer arena_{};
  VkDeviceMemory arena_memory_{};
  uint8_t* mapped_ = nullptr;
  uint32_t cursor_ = 16384;
  std::vector<std::unique_ptr<NativeShader>> shaders_;
  std::map<std::array<uint32_t, 8>, std::unique_ptr<Target>> targets_;
  std::map<std::array<uint32_t, 5>, std::unique_ptr<DepthSurface>> depths_;
  // Clears can precede the first draw that allocates a matching host image.
  // Replay retained rectangles in order; newer covering clears replace old ones.
  std::map<std::array<uint32_t, 4>, std::vector<DepthClear>> depth_clears_;
  // Reviewed XDK clear shader writes physical EDRAM samples through fake MSAA
  // views. Histories here are keyed by base, format, and sample-row pitch.
  std::map<std::array<uint32_t, 3>, std::vector<DepthClear>> fast_depth_clears_;
  std::map<std::vector<uint64_t>, std::unique_ptr<Pipeline>> pipelines_;
  std::map<uint32_t, VkShaderModule> rectangle_shaders_;
  std::map<std::array<uint32_t, 7>, std::unique_ptr<Texture>> textures_;
  std::map<std::array<uint32_t, 7>, TextureUploadPlan> texture_plans_;
  UnavailableResolveRanges unavailable_resolves_;
  uint64_t resolve_destinations_ = 0, resolve_completed_ = 0, resolved_reads_ = 0;
  uint64_t color_sequence_ = 0, resolved_sequence_ = 0;
  std::map<std::array<uint32_t, 2>, std::unique_ptr<ResolvePipeline>> resolve_pipelines_;
  std::map<std::array<uint32_t, 5>, std::unique_ptr<ResolvedSurface>> resolved_surfaces_;
  VkShaderModule resolve_vs_{}, resolve_ps_{}, resolve_ms_ps_{};
  void* invalidation_handle_ = nullptr;
  std::mutex invalidation_mutex_;
  std::vector<std::pair<uint32_t,uint32_t>> invalidation_queue_;
  bool invalidate_all_resolves_ = false;
  std::array<std::unique_ptr<Texture>, 3> null_textures_;
  Target* latest_ = nullptr;
  Target* scene_ = nullptr;
  uint64_t scene_draw_swap_ = 0;
  Target* depth_scene_ = nullptr;
  uint64_t depth_scene_draw_swap_ = 0;
  uint64_t depth_submitted_ = 0;
  uint64_t pixel_shader_bypassed_ = 0;
  uint64_t depth_clear_requests_ = 0, depth_clear_applied_ = 0;
  uint64_t rectangles_submitted_ = 0;
  uint64_t fast_clear_requests_ = 0, fast_clear_aliases_ = 0;
  uint64_t depth_clear_sequence_ = 0;
  std::map<std::string, uint64_t> unsupported_;
  uint64_t draws_ = 0, submitted_ = 0, swaps_ = 0, copies_ = 0;
  uint64_t image_bytes_ = 0;
  bool failed_ = false;
  bool no_cull_diagnostic_ = false;
  bool scene_view_diagnostic_ = false;
  bool scene_view_enabled_ = false, scene_key_down_ = false;
  bool depth_view_enabled_ = false, depth_key_down_ = false;
  bool readback_diagnostic_ = false, recording_ = false;
  bool draw_probe_diagnostic_ = false;
  bool pending_work_ = false;
  uint32_t batch_draws_ = 0;
  uint64_t export_copies_ = 0;
  std::vector<uint8_t> export_snapshot_;
  struct Timings {
    double texture = 0, exports = 0, shader = 0, pipeline = 0, wait = 0;
    double export_plan = 0, export_guards = 0, export_snapshot = 0, export_write = 0, export_thread = 0;
    uint64_t submissions = 0, upload_bytes = 0, linear_bytes = 0, tiled_bytes = 0, export_snapshot_bytes = 0;
  } timings_;
  PerfClock::time_point last_perf_ = PerfClock::now();
  uint64_t last_perf_swaps_ = 0;
  double last_thread_ms_ = -1;
  VkRenderPass present_pass_{};
  Pipeline present_;
  Pipeline present_depth_;
  VkShaderModule present_vs_{}, present_ps_{};
  VkShaderModule present_depth_ps_{};
  VkSampler present_sampler_{};
  const VulkanDevice::Functions& F() const { return gpu_->functions(); }
  VkDevice D() const { return gpu_->device(); }
  uint32_t MemoryType(uint32_t allowed, uint32_t preferred);
  void Begin();
  void Submit();
  void Transition(Image& image, VkImageLayout layout, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
  Image CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkComponentMapping mapping = {},
                    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                    uint32_t depth = 1, uint32_t layers = 1, uint32_t levels = 1, uint32_t kind = 0);
  void DestroyImage(Image& image);
  uint32_t Upload(const void* source, uint32_t bytes, uint32_t align = 16);
  const uint8_t* Guest(uint32_t address, uint32_t size);
  bool PendingResolve(uint32_t address, uint32_t size) const;
  bool Reject(const std::string& reason);
  Target* GetTarget(bool depth_only);
  void ApplyDepthClear(Target& target, const DepthClear& clear);
  void IssueDepthClear();
  draw_util::ResolveInfo DecodeResolveInfo();
  void TrackResolveDestination(const draw_util::ResolveInfo& info);
  bool ResolveColor(const draw_util::ResolveInfo& info);
  void ClearResolvedColor(const draw_util::ResolveInfo& info);
  ResolvePipeline* GetResolvePipeline(VkFormat format, bool multisampled);
  Texture* GetResolvedTexture(const TextureUploadPlan& plan, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind);
  Texture* EnsureTexture(const TextureUploadPlan& plan, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind);
  Target* FindResolveSource(const draw_util::ResolveInfo& info, ResolveRect rectangle);
  void DrainResolveInvalidations();
  void InvalidateResolvedRange(uint32_t address, uint32_t size);
  static std::pair<uint32_t,uint32_t> MemoryInvalidated(void* context, uint32_t address, uint32_t size, bool exact);
  void PropagateFastDepthClear(NativeShader& vs, NativeShader* ps, Target& target, const uint32_t* fetch, uint32_t base, bool indexed, uint32_t count);
  Texture* GetTexture(uint32_t slot, uint32_t kind);
  const TextureUploadPlan& GetTexturePlan(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind);
  Texture* GetNullTexture(uint32_t kind);
  Pipeline* GetPipeline(NativeShader& vs, NativeShader* ps, Target& target, VkPrimitiveTopology topology, bool rectangle);
  VkShaderModule GetRectangleShader(uint32_t mask);
};
class NativeGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "BO3 native Vulkan prototype"; }
 protected:
  // The SDK names its full graphics feature profile "with_gpu_emulation".
  // Request it for native shader/depth/geometry features, while retaining this
  // class's own command processor; false creates a presentation-only device.
  void CreateProvider(bool presentation) override { provider_ = VulkanProvider::Create(true, presentation); }
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};

uint32_t NativeCommandProcessor::MemoryType(uint32_t allowed, uint32_t preferred) {
  uint32_t mask = allowed & preferred;
  if (!mask) throw std::runtime_error("No compatible Vulkan memory type");
  return std::countr_zero(mask);
}
const uint8_t* NativeCommandProcessor::Guest(uint32_t address, uint32_t size) {
  if (uint64_t(address) + size > 0x20000000) throw Unsupported("physical_range");
  const auto* pointer = memory_->TranslatePhysical<const uint8_t*>(address);
  if (!PhysicalAccessible(*memory_->GetPhysicalHeap(), address, size)) throw Unsupported("unreadable_guest_range");
  return pointer;
}
bool NativeCommandProcessor::PendingResolve(uint32_t address, uint32_t size) const {
  if (uint64_t(address) + size > 0x20000000) throw Unsupported("physical_range");
  return unavailable_resolves_.Intersects(address, size);
}
uint32_t NativeCommandProcessor::Upload(const void* source, uint32_t bytes, uint32_t align) {
  cursor_ = (cursor_ + align - 1) & ~(align - 1);
  if (uint64_t(cursor_) + bytes > kArenaBytes) throw Unsupported("upload_arena_limit");
  const uint32_t offset = cursor_;
  std::memcpy(mapped_ + offset, source, bytes);
  cursor_ += bytes;
  return offset;
}
bool NativeCommandProcessor::SetupContext() {
  try {
    last_thread_ms_ = ThreadMilliseconds();
    last_perf_ = PerfClock::now();
    gpu_ = static_cast<VulkanProvider*>(graphics_system_->provider())->vulkan_device();
    no_cull_diagnostic_ = std::getenv("BO3_NATIVE_NO_CULL") != nullptr;
    scene_view_diagnostic_ = std::getenv("BO3_NATIVE_SCENE_VIEW") != nullptr;
    if (scene_view_diagnostic_) REXGPU_WARN("BO3 NATIVE diagnostic: F8 toggles scene color, F9 toggles scene depth; normal presentation initially active");
    readback_diagnostic_ = std::getenv("BO3_NATIVE_READBACK") != nullptr;
    draw_probe_diagnostic_ = std::getenv("BO3_NATIVE_DRAW_PROBE") != nullptr;
    if (no_cull_diagnostic_) REXGPU_WARN("BO3 NATIVE diagnostic: culling disabled for comparison");
    clear_depth_ = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(gpu_->device(), "vkCmdClearDepthStencilImage"));
    clear_attachments_ = reinterpret_cast<PFN_vkCmdClearAttachments>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(gpu_->device(), "vkCmdClearAttachments"));
    copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(D(), "vkCmdCopyImage"));
    if(!copy_image_)throw std::runtime_error("Missing Vulkan image copy function");
    resolve_image_ = reinterpret_cast<PFN_vkCmdResolveImage>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(gpu_->device(), "vkCmdResolveImage"));
    image_properties_ = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(gpu_->vulkan_instance()->functions().vkGetInstanceProcAddr(gpu_->vulkan_instance()->instance(), "vkGetPhysicalDeviceImageFormatProperties"));
    if (!clear_depth_ || !clear_attachments_ || !resolve_image_ || !image_properties_) throw std::runtime_error("Missing core Vulkan image functions");
    REXGPU_INFO("BO3 native framebuffer sample masks: color={:X} depth={:X} stencil={:X}", gpu_->properties().framebufferColorSampleCounts, gpu_->properties().framebufferDepthSampleCounts, gpu_->properties().framebufferStencilSampleCounts);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu_->queue_family_graphics_compute();
    Check(F().vkCreateCommandPool(D(), &pool, nullptr, &command_pool_), "command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = command_pool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    Check(F().vkAllocateCommandBuffers(D(), &allocation, &command_), "command buffer");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(F().vkCreateFence(D(), &fence, nullptr, &fence_), "fence");
    if (draw_probe_diagnostic_) {
      query_results_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(D(), "vkGetQueryPoolResults"));
      if (!query_results_) throw std::runtime_error("Missing Vulkan query result function");
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; query.queryType = VK_QUERY_TYPE_OCCLUSION; query.queryCount = kBatchDraws;
      Check(F().vkCreateQueryPool(D(), &query, nullptr, &coverage_queries_), "native coverage queries");
      REXGPU_INFO("BO3 native coverage probe active: first four draws per scene/rectangle pipeline; precise={}", gpu_->properties().occlusionQueryPrecise);
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5 * kBatchDraws},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBatchDraws}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 * kBatchDraws}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}};
    VkDescriptorPoolCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    desc.maxSets = 3 * kBatchDraws; desc.poolSizeCount = 4; desc.pPoolSizes = sizes;
    Check(F().vkCreateDescriptorPool(D(), &desc, nullptr, &descriptors_), "descriptor pool");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = kArenaBytes; buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Check(F().vkCreateBuffer(D(), &buffer, nullptr, &arena_), "upload buffer");
    VkMemoryRequirements requirements{};
    F().vkGetBufferMemoryRequirements(D(), arena_, &requirements);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, gpu_->memory_types().host_visible & gpu_->memory_types().host_coherent);
    Check(F().vkAllocateMemory(D(), &memory, nullptr, &arena_memory_), "upload memory");
    Check(F().vkBindBufferMemory(D(), arena_, arena_memory_, 0), "bind upload");
    void* mapping = nullptr;
    Check(F().vkMapMemory(D(), arena_memory_, 0, VK_WHOLE_SIZE, 0, &mapping), "map upload"); mapped_ = static_cast<uint8_t*>(mapping);
    REXGPU_INFO("BO3 NATIVE backend active: own Vulkan targets/pipelines/submissions, 16-MiB upload arena; no Xenos draw backend. Device={} geometryShader={} depthClamp={} preciseCoverage={}", gpu_->properties().deviceName, gpu_->properties().geometryShader, gpu_->properties().depthClamp, gpu_->properties().occlusionQueryPrecise);
    invalidation_queue_.reserve(4096);
    invalidation_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidated, this);
    return true;
  } catch (const std::exception& error) { REXGPU_ERROR("BO3 native setup failed: {}", error.what()); return false; }
}
void NativeCommandProcessor::Begin() {
  if (recording_) throw std::runtime_error("Native command buffer already recording");
  // The previous Submit retired its fence. Reset uploads/descriptors only at
  // batch boundaries, never while recorded draws still reference them.
  cursor_ = 16384; batch_draws_ = 0;
  pending_work_ = false;
  Check(F().vkResetDescriptorPool(D(), descriptors_, 0), "reset batch descriptors");
  Check(F().vkResetCommandPool(D(), command_pool_, 0), "reset command pool");
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(F().vkBeginCommandBuffer(command_, &info), "begin command");
  coverage_probes_.clear();
  if (coverage_queries_) F().vkCmdResetQueryPool(command_, coverage_queries_, 0, kBatchDraws);
  recording_ = true;
}
void NativeCommandProcessor::Submit() {
  TimedScope time(timings_.wait);
  if (!recording_) throw std::runtime_error("Native command buffer is not recording");
  Check(F().vkEndCommandBuffer(command_), "end command");
  recording_ = false;
  // CPU snapshots/descriptors alone don't require a queue submission. A
  // rejected draw may have recorded no GPU command at all.
  if (!pending_work_) return;
  Check(F().vkResetFences(D(), 1, &fence_), "reset fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
  { auto queue = gpu_->AcquireQueue(gpu_->queue_family_graphics_compute(), 0);
    Check(F().vkQueueSubmit(queue.queue(), 1, &submit, fence_), "queue submit"); }
  Check(F().vkWaitForFences(D(), 1, &fence_, VK_TRUE, UINT64_MAX), "native completion fence");
  ++timings_.submissions;
  for (const auto& probe : coverage_probes_) {
    uint64_t samples = 0;
    Check(query_results_(D(), coverage_queries_, probe.query, 1, sizeof(samples), &samples, sizeof(samples), VK_QUERY_RESULT_64_BIT), "native coverage result");
    const auto& p = *probe.pipeline; const auto& t = *probe.target;
    REXGPU_INFO("BO3 NATIVE coverage: swap={} rectangle={} VS={:016X} PS={:016X} extent={}x{} msaa={} depth={:08X} mask={:08X} passedSamples={} precise={}", probe.swap, p.rectangle, p.vs_hash, p.ps_hash, t.color.width, t.color.height, uint32_t(t.color.samples), p.depth_state, p.color_mask, samples, gpu_->properties().occlusionQueryPrecise);
  }
  coverage_probes_.clear();
}
Image NativeCommandProcessor::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                         VkImageAspectFlags aspect, VkComponentMapping mapping, VkSampleCountFlagBits samples,
                                         uint32_t depth, uint32_t layers, uint32_t levels, uint32_t kind) {
  Image result; result.width = width; result.height = height; result.depth = depth; result.layers = layers; result.levels = levels; result.format = format; result.samples = samples;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = kind == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D; info.format = format;
  info.flags = kind == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.extent = {width, height, depth}; info.mipLevels = levels; info.arrayLayers = layers; info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
  VkFormatProperties properties{};
  gpu_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(gpu_->physical_device(), format, &properties);
  VkFormatFeatureFlags required = 0;
  if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if ((properties.optimalTilingFeatures & required) != required) throw Unsupported("host_image_format");
  VkImageFormatProperties supported{};
  if (image_properties_(gpu_->physical_device(), format, info.imageType, VK_IMAGE_TILING_OPTIMAL, usage, info.flags, &supported) != VK_SUCCESS ||
      !(supported.sampleCounts & samples) || width > supported.maxExtent.width || height > supported.maxExtent.height || depth > supported.maxExtent.depth || levels > supported.maxMipLevels || layers > supported.maxArrayLayers) throw Unsupported("host_image_samples_or_extent");
  Check(F().vkCreateImage(D(), &info, nullptr, &result.image), "image");
  try {
  VkMemoryRequirements requirements{}; F().vkGetImageMemoryRequirements(D(), result.image, &requirements);
  VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.allocationSize = requirements.size;
  if (image_bytes_ + requirements.size > 512ull * 1024 * 1024) throw Unsupported("image_memory_budget");
  memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, gpu_->memory_types().device_local);
  Check(F().vkAllocateMemory(D(), &memory, nullptr, &result.memory), "image memory");
  result.allocation = requirements.size; image_bytes_ += result.allocation;
  Check(F().vkBindImageMemory(D(), result.image, result.memory, 0), "bind image");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.image; view.viewType = kind == 2 ? VK_IMAGE_VIEW_TYPE_3D : kind == 1 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  view.format = format; view.components = mapping; view.subresourceRange = {aspect, 0, levels, 0, layers};
  Check(F().vkCreateImageView(D(), &view, nullptr, &result.view), "image view");
  } catch (...) { DestroyImage(result); throw; }
  return result;
}
void NativeCommandProcessor::Transition(Image& image, VkImageLayout layout, VkImageAspectFlags aspect) {
  if (image.layout == layout) return;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = image.layout; barrier.newLayout = layout; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image; barrier.subresourceRange = {aspect, 0, image.levels, 0, image.layers};
  F().vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  pending_work_ = true;
  image.layout = layout;
}
void NativeCommandProcessor::DestroyImage(Image& image) {
  if (image.view) F().vkDestroyImageView(D(), image.view, nullptr);
  if (image.image) F().vkDestroyImage(D(), image.image, nullptr);
  if (image.memory) F().vkFreeMemory(D(), image.memory, nullptr);
  image_bytes_ -= image.allocation;
  image = {};
}
bool NativeCommandProcessor::Reject(const std::string& reason) {
  auto& count = unsupported_[reason];
  if (++count <= 2) REXGPU_WARN("BO3 NATIVE unsupported: {} (packet consumed, draw not submitted)", reason);
  return true;
}
Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType type, uint32_t, const uint32_t* data, uint32_t count) {
  if (!count || count > 65536) { Reject("shader_size"); return nullptr; }
  const uint64_t hash = XXH3_64bits(data, count * 4);
  for (const auto& cached : shaders_) {
    if (cached->type() != type || cached->ucode_dword_count() != count || cached->ucode_data_hash() != hash) continue;
    bool same = true;
    for (size_t i = 0; i < count; ++i) if (cached->ucode_dwords()[i] != __builtin_bswap32(data[i])) { same = false; break; }
    if (same) return cached.get();
  }
  if (shaders_.size() >= 1024) { Reject("shader_cache_limit"); return nullptr; }
  TimedScope time(timings_.shader);
  auto result = std::make_unique<NativeShader>(type, data, count);
  if (result->native.ok && !result->native.droppedExports && !result->native.droppedMemoryExports && !result->native.writesMemory) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = result->native.spirv.size() * 4; info.pCode = result->native.spirv.data();
    const auto status = F().vkCreateShaderModule(D(), &info, nullptr, &result->module);
    if (status != VK_SUCCESS) REXGPU_ERROR("BO3 native shader module failed: {}", int(status));
  }
  REXGPU_INFO("BO3 native shader {} {:016X}: {}", type == xenos::ShaderType::kVertex ? "VS" : "PS", result->ucode_data_hash(), shader::Describe(result->native));
  auto* pointer = result.get(); shaders_.push_back(std::move(result)); return pointer;
}

static VkFormat ColorFormat(uint32_t format) {
  format = uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(format)));
  switch (format) {
    case 0: return VK_FORMAT_R8G8B8A8_UNORM;
    case 1: throw Unsupported("gamma_target_blending");
    case 2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 6: return VK_FORMAT_R16G16_SFLOAT;
    case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: throw Unsupported(fmt::format("color_format_{}", format));
  }
}

Target* NativeCommandProcessor::GetTarget(bool depth_only) {
  const auto& r = *register_file_;
  const auto surface = r.Get<reg::RB_SURFACE_INFO>();
  const auto color = r.Get<reg::RB_COLOR_INFO>();
  const auto depth = r.Get<reg::RB_DEPTH_INFO>();
  const uint32_t guest_samples = uint32_t(surface.msaa_samples);
  if (guest_samples > 2) throw Unsupported("reserved_multisample_state");
  const auto samples = VkSampleCountFlagBits(1u << guest_samples);
  const auto supported_samples = gpu_->properties().framebufferDepthSampleCounts & gpu_->properties().framebufferStencilSampleCounts &
      (depth_only ? ~VkSampleCountFlags(0) : gpu_->properties().framebufferColorSampleCounts);
  if (!(supported_samples & samples)) throw Unsupported(fmt::format("host_msaa_{}", 1u << guest_samples));
  uint32_t width = surface.surface_pitch;
  float yscale = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  float yoffset = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  uint32_t height = 0;
  if (std::isfinite(yscale) && std::isfinite(yoffset) && std::abs(yscale) > 0)
    height = uint32_t(std::ceil(std::max(yoffset + std::abs(yscale), 0.0f)));
  if (!height || height > 2048) height = (r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] >> 16) & 0x3fff;
  if (!width || width > 4096 || !height || height > 2048) throw Unsupported("target_extent");
  std::array<uint32_t, 8> key{depth_only ? 0u : color.color_base, depth_only ? 0u : uint32_t(xenos::GetStorageColorFormat(color.color_format)), width, height, depth.depth_base, uint32_t(depth.depth_format), guest_samples, uint32_t(depth_only)};
  auto found = targets_.find(key);
  if (found != targets_.end()) return found->second.get();
  if (targets_.size() >= 64) throw Unsupported("target_cache_limit");
  const std::array<uint32_t, 5> depth_key{depth.depth_base, uint32_t(depth.depth_format), width, height, guest_samples};
  auto depth_found = depths_.find(depth_key);
  const bool new_depth = depth_found == depths_.end();
  if (depth_found == depths_.end()) {
    if (depths_.size() >= 64) throw Unsupported("depth_cache_limit");
    auto surface_result = std::make_unique<DepthSurface>();
    try {
      // Float24 uses D32 storage until exact packed quantization is supported.
      const auto depth_format = uint32_t(depth.depth_format) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
      surface_result->image = CreateImage(width, height, depth_format,
          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (samples != VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
          VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, {}, samples);
      if (samples != VK_SAMPLE_COUNT_1_BIT) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = surface_result->image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = surface_result->image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        Check(F().vkCreateImageView(D(), &view, nullptr, &surface_result->sampled), "sampled depth view");
      }
      if (recording_) Submit();
      Begin();
      Transition(surface_result->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
      VkClearDepthStencilValue one{1, 0}; clear_depth_(command_, surface_result->image.image, surface_result->image.layout, &one, 1, &range);
      Submit();
    } catch (...) {
      if (surface_result->sampled) F().vkDestroyImageView(D(), surface_result->sampled, nullptr);
      DestroyImage(surface_result->image); throw;
    }
    depth_found = depths_.emplace(depth_key, std::move(surface_result)).first;
    REXGPU_INFO("BO3 native shared depth {}x{} base={} samples={} format={}", width, height, depth.depth_base, uint32_t(samples), int(depth_found->second->image.format));
  }
  auto result = std::make_unique<Target>();
  result->depth = depth_found->second.get(); result->depth_only = depth_only;
  try {
    // Keep dimensions available to viewport/scissor calculations without
    // allocating or accessing a color attachment for depth-only passes.
    result->color.width = width; result->color.height = height; result->color.samples = samples;
    if (!depth_only) {
      result->color = CreateImage(width, height, ColorFormat(uint32_t(color.color_format)),
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, {}, samples);
      if (samples != VK_SAMPLE_COUNT_1_BIT) result->display = CreateImage(width, height, result->color.format,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = result->color.format; attachments[1].format = result->depth->image.format;
    for (auto& a : attachments) { a.samples = samples; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE; }
    attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr{depth_only ? 0u : 1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = depth_only ? 0 : 1; sub.pColorAttachments = depth_only ? nullptr : &cr; sub.pDepthStencilAttachment = &dr;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.attachmentCount = depth_only ? 1 : 2; pass.pAttachments = attachments + (depth_only ? 1 : 0); pass.subpassCount = 1; pass.pSubpasses = &sub;
    Check(F().vkCreateRenderPass(D(), &pass, nullptr, &result->pass), "target pass");
    VkImageView views[]{result->color.view, result->depth->image.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass = result->pass; fb.attachmentCount = depth_only ? 1 : 2; fb.pAttachments = views + (depth_only ? 1 : 0); fb.width = width; fb.height = height; fb.layers = 1;
    Check(F().vkCreateFramebuffer(D(), &fb, nullptr, &result->framebuffer), "target framebuffer");
    if (!depth_only) {
      if (recording_) Submit();
      Begin();
      Transition(result->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      F().vkCmdClearColorImage(command_, result->color.image, result->color.layout, &zero, 1, &range);
      Submit();
    }
  } catch (...) {
    if (result->framebuffer) F().vkDestroyFramebuffer(D(), result->framebuffer, nullptr);
    if (result->pass) F().vkDestroyRenderPass(D(), result->pass, nullptr);
    DestroyImage(result->color); DestroyImage(result->display); throw;
  }
  auto* pointer = result.get(); targets_.emplace(key, std::move(result));
  if (new_depth) {
    std::vector<DepthClear> pending_clears;
    const std::array<uint32_t, 4> clear_key{depth.depth_base, uint32_t(depth.depth_format), width, guest_samples};
    auto clears = depth_clears_.find(clear_key);
    if (clears != depth_clears_.end()) pending_clears = clears->second;
    const std::array<uint32_t, 3> sample_key{depth.depth_base, uint32_t(depth.depth_format), width * SampleX(guest_samples)};
    auto fast_clears = fast_depth_clears_.find(sample_key);
    if (fast_clears != fast_depth_clears_.end()) for (const auto& clear : fast_clears->second) {
      const auto rectangle = FromSampleRectangle({uint32_t(clear.rectangle.offset.x), uint32_t(clear.rectangle.offset.y), clear.rectangle.extent.width, clear.rectangle.extent.height}, sample_key[2], width, guest_samples);
      auto mapped_clear = clear; mapped_clear.rectangle = {{int32_t(rectangle.x), int32_t(rectangle.y)}, {rectangle.width, rectangle.height}};
      pending_clears.push_back(mapped_clear); ++fast_clear_aliases_;
    }
    std::stable_sort(pending_clears.begin(), pending_clears.end(), [](const DepthClear& a, const DepthClear& b) { return a.sequence < b.sequence; });
    for (const auto& clear : pending_clears) ApplyDepthClear(*pointer, clear);
  }
  REXGPU_INFO("BO3 native target {}x{} format {} color base {} depth base {} samples={} depthOnly={}", width, height, int(pointer->color.format), key[0], depth.depth_base, uint32_t(samples), depth_only);
  if (uint32_t(depth.depth_format)) REXGPU_INFO("BO3 native float24 depth uses D32 storage; packed quantization/clear parity unverified");
  return pointer;
}

const TextureUploadPlan& NativeCommandProcessor::GetTexturePlan(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind) {
  std::array<uint32_t, 7> key{}; std::memcpy(key.data(), &fetch, sizeof(fetch)); key[6] = kind;
  auto found = texture_plans_.find(key);
  if (found != texture_plans_.end()) return found->second;
  // These records contain immutable layout arithmetic only, never permissions
  // or pixels. No caller keeps a reference across another plan lookup.
  if (texture_plans_.size() >= 512) texture_plans_.erase(texture_plans_.begin());
  try { return texture_plans_.emplace(key, PlanTextureUpload(fetch, kind, kArenaBytes)).first->second; }
  catch (const TextureUploadError& error) { throw Unsupported(error.what()); }
}
Texture* NativeCommandProcessor::GetTexture(uint32_t slot, uint32_t kind) {
  TimedScope time(timings_.texture);
  const auto fetch = register_file_->GetTextureFetch(slot);
  if (!TextureDimensionsCompatible(fetch, kind)) return GetNullTexture(kind);
  const auto& plan = GetTexturePlan(fetch, kind);
  // GPU-produced ranges must be handled by native resolve ownership; never
  // turn their stale CPU shadows into asset uploads.
  if (PendingResolve(plan.base_address, plan.base_bytes) || PendingResolve(plan.mip_address, plan.mip_bytes))
    return GetResolvedTexture(plan, fetch, kind);
  std::span<const uint8_t> base, mips;
  if (plan.base_bytes) base = {Guest(plan.base_address, plan.base_bytes), plan.base_bytes};
  if (plan.mip_bytes) mips = {Guest(plan.mip_address, plan.mip_bytes), plan.mip_bytes};
  const std::array<uint64_t, 2> hash{base.empty() ? 0 : XXH3_64bits(base.data(), base.size()), mips.empty() ? 0 : XXH3_64bits(mips.data(), mips.size())};
  std::array<uint32_t, 7> key{}; std::memcpy(key.data(), &fetch, sizeof(fetch)); key[6] = kind;
  auto& cached = textures_[key];
  if (cached && !cached->gpu_resolved && cached->content == hash) return cached.get();
  if (!cached && textures_.size() > 256) { textures_.erase(key); throw Unsupported("texture_cache_limit"); }
  std::vector<uint8_t> pixels;
  try { pixels = DecodeTextureUpload(plan, base, mips); }
  catch (const TextureUploadError& error) { throw Unsupported(error.what()); }
  if (fetch.tiled || plan.bc) timings_.tiled_bytes += pixels.size(); else timings_.linear_bytes += pixels.size();
  EnsureTexture(plan, fetch, kind);
  const uint32_t saved = cursor_, offset = Upload(pixels.data(), uint32_t(pixels.size()), 16);
  const bool standalone = !recording_;
  if (standalone) Begin();
  Transition(cached->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // Base-less mip chains have no valid guest data below the sampler's minimum
  // LOD. Initialize those allocated levels too, rather than exposing garbage.
  VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, cached->image.levels, 0, cached->image.layers};
  F().vkCmdClearColorImage(command_, cached->image.image, cached->image.layout, &zero, 1, &range);
  VkMemoryBarrier order{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; order.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; order.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  F().vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &order, 0, nullptr, 0, nullptr);
  std::vector<VkBufferImageCopy> copies;
  for (const auto& slice : plan.slices) {
    VkBufferImageCopy copy{}; copy.bufferOffset = offset + slice.output_offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, slice.mip, slice.layer, 1}; copy.imageExtent = {slice.width, slice.height, slice.depth};
    copies.push_back(copy);
  }
  F().vkCmdCopyBufferToImage(command_, arena_, cached->image.image, cached->image.layout, uint32_t(copies.size()), copies.data());
  Transition(cached->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if (standalone) { Submit(); cursor_ = saved; }
  timings_.upload_bytes += pixels.size(); cached->content = hash; cached->gpu_resolved = false; return cached.get();
}

Texture* NativeCommandProcessor::EnsureTexture(const TextureUploadPlan& plan, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t kind) {
  std::array<uint32_t, 7> key{}; std::memcpy(key.data(), &fetch, sizeof(fetch)); key[6] = kind;
  auto& cached = textures_[key];
  if (!cached && textures_.size() > 256) { textures_.erase(key); throw Unsupported("texture_cache_limit"); }
  if (!cached) {
    auto result = std::make_unique<Texture>();
    VkComponentMapping mapping{};
    VkComponentSwizzle* channels[]{&mapping.r, &mapping.g, &mapping.b, &mapping.a};
    const VkComponentSwizzle swizzles[]{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    for (uint32_t i = 0; i < 4; ++i) *channels[i] = swizzles[(fetch.swizzle >> (3 * i)) & 7];
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = fetch.mag_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.minFilter = fetch.min_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto clamp = [](uint32_t c) { switch (c) { case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; default: throw Unsupported("texture_clamp"); } };
    sampler.addressModeU = kind == 1 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : clamp(uint32_t(fetch.clamp_x));
    sampler.addressModeV = kind == 1 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : clamp(uint32_t(fetch.clamp_y));
    sampler.addressModeW = kind == 2 ? clamp(uint32_t(fetch.clamp_z)) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.minLod = float(plan.min_level); sampler.maxLod = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? sampler.minLod : float(plan.max_level);
    // The translator already adds the fetch and instruction LOD biases in
    // shaders for both implicit and explicit LOD. Do not apply it twice.
    sampler.mipLodBias = 0;
    if (sampler.magFilter == VK_FILTER_LINEAR || sampler.minFilter == VK_FILTER_LINEAR || sampler.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) {
      VkFormatProperties filtering{};
      gpu_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(gpu_->physical_device(), plan.format, &filtering);
      if (!(filtering.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) throw Unsupported("host_texture_linear_filter");
    }
    Check(F().vkCreateSampler(D(), &sampler, nullptr, &result->sampler), "texture sampler");
    try { result->image = CreateImage(plan.width, plan.height, plan.format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, mapping, VK_SAMPLE_COUNT_1_BIT, plan.depth, plan.layers, plan.levels, kind); }
    catch (...) { F().vkDestroySampler(D(), result->sampler, nullptr); throw; }
    cached = std::move(result);
    if (kind || plan.levels > 1) REXGPU_INFO("BO3 native texture allocation: kind={} {}x{}x{} layers={} levels={} guestBase={:08X} guestMips={:08X} decodedBytes={}", kind, plan.width, plan.height, plan.depth, plan.layers, plan.levels, plan.base_address, plan.mip_address, plan.output_bytes);
  }
  return cached.get();
}

// Typed zero descriptors are used only for unbound/incompatible guest bindings,
// as in the SDK. A compatible resource with unsupported data is still rejected.
Texture* NativeCommandProcessor::GetNullTexture(uint32_t kind) {
  if (kind > 2) throw Unsupported("texture_binding_kind");
  auto& cached = null_textures_[kind];
  if (cached) return cached.get();
  auto result = std::make_unique<Texture>();
  result->image = CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, {}, VK_SAMPLE_COUNT_1_BIT, 1, kind == 1 ? 6 : 1, 1, kind);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  try { Check(F().vkCreateSampler(D(), &sampler, nullptr, &result->sampler), "typed null sampler"); }
  catch (...) { DestroyImage(result->image); throw; }
  if (!recording_) Begin();
  Transition(result->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, result->image.layers};
  F().vkCmdClearColorImage(command_, result->image.image, result->image.layout, &zero, 1, &range);
  Transition(result->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  cached = std::move(result); REXGPU_INFO("BO3 native typed null texture: kind={}", kind); return cached.get();
}

static VkBlendFactor BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR; case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR; case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA; case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: throw Unsupported("blend_factor");
  }
}
static VkBlendOp BlendOp(uint32_t op) {
  const VkBlendOp ops[]{VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
  if (op > 4) throw Unsupported("blend_op"); return ops[op];
}

VkShaderModule NativeCommandProcessor::GetRectangleShader(uint32_t mask) {
  if (!gpu_->properties().geometryShader) throw Unsupported("rectangle_geometry_shader_unavailable");
  auto found = rectangle_shaders_.find(mask);
  if (found != rectangle_shaders_.end()) return found->second;
  if (rectangle_shaders_.size() >= 128) throw Unsupported("rectangle_shader_cache_limit");
  auto code = BuildRectangleShader(mask);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code.size() * 4; info.pCode = code.data();
  VkShaderModule module{}; Check(F().vkCreateShaderModule(D(), &info, nullptr, &module), "native rectangle shader");
  rectangle_shaders_.emplace(mask, module); return module;
}

Pipeline* NativeCommandProcessor::GetPipeline(NativeShader& vs, NativeShader* ps, Target& target, VkPrimitiveTopology topology, bool rectangle) {
  const auto& r = *register_file_;
  const auto depth = r.Get<reg::RB_DEPTHCONTROL>();
  const auto raster = r.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (!rectangle && raster.poly_mode == xenos::PolygonModeEnable::kDualMode &&
      ((!raster.cull_front && raster.polymode_front_ptype != xenos::PolygonType::kTriangles) ||
       (!raster.cull_back && raster.polymode_back_ptype != xenos::PolygonType::kTriangles))) throw Unsupported("nonfill_polygon_mode");
  // Unwritten or masked-off MRTs do not require host attachments. Guest clears
  // may leave all four masks enabled while the PS exports only target zero.
  const uint32_t color_mask = ps ? draw_util::GetNormalizedColorMask(r, ps->native.colourMask) : 0;
  if (color_mask & 0xfff0) throw Unsupported("shader_mrt");
  if (rectangle && ps && (ps->native.interpolatorMask & ~vs.native.interpolatorMask)) throw Unsupported("rectangle_missing_interpolators");
  std::vector<uint64_t> key{uint64_t(&vs), uint64_t(ps), uint64_t(target.color.format), uint64_t(target.depth->image.format), uint64_t(target.color.samples), uint64_t(topology), depth.value, raster.value, target.depth_only ? 0u : r[XE_GPU_REG_RB_BLENDCONTROL0], color_mask & 15, uint64_t(rectangle), r[XE_GPU_REG_PA_CL_CLIP_CNTL]};
  auto found = pipelines_.find(key); if (found != pipelines_.end()) return found->second.get();
  if (pipelines_.size() >= 1024) throw Unsupported("pipeline_cache_limit");
  TimedScope time(timings_.pipeline);
  auto result = std::make_unique<Pipeline>();
  result->vs_hash = vs.ucode_data_hash(); result->ps_hash = ps ? ps->ucode_data_hash() : 0;
  result->depth_state = depth.value; result->color_mask = color_mask; result->rectangle = rectangle;
  try {
    VkDescriptorSetLayoutBinding uniforms[5]{};
    for (uint32_t i = 0; i < 5; ++i) uniforms[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = 5; layout.pBindings = uniforms;
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[0]), "uniform layout");
    for (uint32_t slot = 0; slot < 32; ++slot) {
      uint32_t mask = vs.native.textureViewMasks[slot] | (ps ? ps->native.textureViewMasks[slot] : 0);
      for (uint32_t kind = 0; kind < 3; ++kind) if (mask & (1u << kind))
        result->textures.push_back({shader::TextureViewBinding(slot, kind), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    layout.bindingCount = uint32_t(result->textures.size()); layout.pBindings = result->textures.data();
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[1]), "texture layout");
    VkDescriptorSetLayoutBinding storage{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    layout.bindingCount = 1; layout.pBindings = &storage;
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[2]), "arena layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 3; pl.pSetLayouts = result->sets; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    Check(F().vkCreatePipelineLayout(D(), &pl, nullptr, &result->layout), "pipeline layout");
    VkPipelineShaderStageCreateInfo stages[3]{};
    for (auto& s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
    uint32_t stage_count = 1;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs.module;
    if (rectangle) { stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT; stages[stage_count++].module = GetRectangleShader(vs.native.interpolatorMask); }
    if (ps) { stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[stage_count++].module = ps->module; }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; input.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
    rs.cullMode = (raster.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (raster.cull_back ? VK_CULL_MODE_BACK_BIT : 0); rs.frontFace = raster.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (no_cull_diagnostic_) rs.cullMode = VK_CULL_MODE_NONE;
    if (rectangle) rs.cullMode = VK_CULL_MODE_NONE;
    rs.depthClampEnable = gpu_->properties().depthClamp && r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
    rs.depthBiasEnable = VK_TRUE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = target.color.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = depth.z_enable; ds.depthWriteEnable = depth.z_write_enable; ds.depthCompareOp = VkCompareOp(depth.zfunc);
    ds.stencilTestEnable = depth.stencil_enable;
    ds.front = {VkStencilOp(depth.stencilfail), VkStencilOp(depth.stencilzpass), VkStencilOp(depth.stencilzfail), VkCompareOp(depth.stencilfunc), 255, 255, 0};
    ds.back = !rectangle && depth.backface_enable
        ? VkStencilOpState{VkStencilOp(depth.stencilfail_bf), VkStencilOp(depth.stencilzpass_bf), VkStencilOp(depth.stencilzfail_bf), VkCompareOp(depth.stencilfunc_bf), 255, 255, 0} : ds.front;
    const auto blend = r.Get<reg::RB_BLENDCONTROL>();
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = color_mask & 15;
    if (!target.depth_only && ps) {
      ba.blendEnable = true; ba.srcColorBlendFactor = BlendFactor(uint32_t(blend.color_srcblend)); ba.dstColorBlendFactor = BlendFactor(uint32_t(blend.color_destblend)); ba.colorBlendOp = BlendOp(uint32_t(blend.color_comb_fcn));
      ba.srcAlphaBlendFactor = BlendFactor(uint32_t(blend.alpha_srcblend)); ba.dstAlphaBlendFactor = BlendFactor(uint32_t(blend.alpha_destblend)); ba.alphaBlendOp = BlendOp(uint32_t(blend.alpha_comb_fcn));
    }
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; bs.attachmentCount = target.depth_only ? 0 : 1; bs.pAttachments = target.depth_only ? nullptr : &ba;
    VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 7; dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = stage_count; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &input; info.pViewportState = &viewport; info.pRasterizationState = &rs; info.pMultisampleState = &ms; info.pDepthStencilState = &ds; info.pColorBlendState = &bs; info.pDynamicState = &dynamic; info.layout = result->layout; info.renderPass = target.pass;
    Check(F().vkCreateGraphicsPipelines(D(), VK_NULL_HANDLE, 1, &info, nullptr, &result->pipeline), "native pipeline");
  } catch (...) {
    if (result->pipeline) F().vkDestroyPipeline(D(), result->pipeline, nullptr);
    if (result->layout) F().vkDestroyPipelineLayout(D(), result->layout, nullptr);
    for (auto s : result->sets) if (s) F().vkDestroyDescriptorSetLayout(D(), s, nullptr);
    throw;
  }
  auto* pointer = result.get(); pipelines_.emplace(std::move(key), std::move(result));
  if (target.color.samples != VK_SAMPLE_COUNT_1_BIT) REXGPU_INFO("BO3 native scene pipeline: VS={:016X} PS={:016X} depth={:08X} mask={:08X} raster={:08X} vte={:08X} clip={:08X}", vs.ucode_data_hash(), ps ? ps->ucode_data_hash() : 0, depth.value, r[XE_GPU_REG_RB_COLOR_MASK], raster.value, r[XE_GPU_REG_PA_CL_VTE_CNTL], r[XE_GPU_REG_PA_CL_CLIP_CNTL]);
  return pointer;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType primitive, uint32_t count, IndexBufferInfo* indices, bool major) {
  DrainResolveInvalidations();
  ++draws_;
  if (failed_) return Reject("backend_disabled_after_failure");
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) return IssueCopy();
  bool touched_batch = false;
  try {
    if (!count) return true;
    if (count > 1000000) throw Unsupported("draw_count");
    const auto mode = register_file_->Get<reg::RB_MODECONTROL>().edram_mode;
    const bool depth_only = mode == xenos::EdramMode::kDepthOnly;
    if (!depth_only && mode != xenos::EdramMode::kColorDepth) return Reject("edram_draw_mode");
    auto* vs = static_cast<NativeShader*>(active_vertex_shader_);
    auto* ps = depth_only ? nullptr : static_cast<NativeShader*>(active_pixel_shader_);
    // Color/depth mode is also used for prepasses with color writes masked
    // off. Only omit a PS when SDK analysis proves it has no side effects.
    const bool bypass_ps = ps && !draw_util::IsPixelShaderNeededWithRasterization(*ps, *register_file_);
    if (bypass_ps) ps = nullptr;
    if (vs && vs->native.writesMemory) {
      if (recording_) Submit();
      TimedScope time(timings_.exports);
      ThreadTimedScope thread_time(timings_.export_thread);
      CopyPlan plan;
      try { TimedScope phase(timings_.export_plan); plan = PlanMemexport(register_file_->values, vs->canonical_sha256, uint32_t(primitive), count, indices != nullptr, major); }
      catch (const std::exception& error) { throw Unsupported(error.what()); }
      if (plan.ranges.empty()) return true;
      const auto last = plan.ranges.back();
      const uint32_t source_bytes = last.source + last.bytes;
      const uint8_t* source;
      uint8_t* destination;
      {
        TimedScope phase(timings_.export_guards);
        if (PendingResolve(plan.source, source_bytes)) throw Unsupported("memexport_pending_native_resolve");
        source = Guest(plan.source, source_bytes);
        destination = memory_->TranslatePhysical<uint8_t*>(plan.destination);
        if (!PhysicalAccessible(*memory_->GetPhysicalHeap(), plan.destination, last.destination + last.bytes, true)) throw Unsupported("memexport_destination_not_writable");
      }
      // Prior native submissions have retired. Snapshot every source byte
      // before exposing any output; all range guards have already passed.
      {
        TimedScope phase(timings_.export_snapshot);
        export_snapshot_.resize(source_bytes);
        std::memcpy(export_snapshot_.data(), source, source_bytes);
      }
      timings_.export_snapshot_bytes += source_bytes;
      uint64_t bytes = 0;
      {
        TimedScope phase(timings_.export_write);
        for (auto range : plan.ranges) {
          std::memcpy(destination + range.destination, export_snapshot_.data() + range.source, range.bytes);
          // Native writes use the physical mapping directly and may bypass
          // guest virtual-alias fault callbacks. Invalidate GPU coverage here.
          InvalidateResolvedRange(plan.destination + range.destination, range.bytes);
          unavailable_resolves_.CpuWritten(plan.destination + range.destination, range.bytes); bytes += range.bytes;
        }
      }
      if (++export_copies_ <= 5) REXGPU_INFO("BO3 native verified memexport copy {}: sha={} invocations={} bytes={} ranges={}", export_copies_, vs->canonical_sha256, count, bytes, plan.ranges.size());
      return true;
    }
    if (!vs || !vs->module || (!depth_only && !bypass_ps && (!ps || !ps->module))) return Reject("missing_or_unsupported_shader");
    VkPrimitiveTopology topology{};
    const bool rectangle = primitive == xenos::PrimitiveType::kRectangleList;
    if (rectangle && count % 3) return Reject("rectangle_vertex_count");
    switch (primitive) {
      case xenos::PrimitiveType::kTriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
      case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
      case xenos::PrimitiveType::kTriangleFan: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
      case xenos::PrimitiveType::kRectangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
      default: return Reject(fmt::format("primitive_{}", uint32_t(primitive)));
    }
    const auto& r = *register_file_;
    if (r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) throw Unsupported("primitive_restart");
    if (uint32_t(r.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode) == 7) throw Unsupported("memory_export_draw");
    if (ps && (draw_util::GetNormalizedColorMask(r, ps->native.colourMask) & 0xfff0)) return Reject("mrt_write_mask");
    auto* target = GetTarget(depth_only);
    auto* pipeline = GetPipeline(*vs, ps, *target, topology, rectangle);
    // Reject unsupported bindings before any uploads, descriptor allocation,
    // or command recording. Earlier supported draws keep their shared batch.
    for (const auto& binding : pipeline->textures) {
      const auto tf = r.GetTextureFetch(binding.binding % 32);
      if (TextureDimensionsCompatible(tf, binding.binding / 32)) GetTexturePlan(tf, binding.binding / 32);
    }
    uint32_t base = r[XE_GPU_REG_VGT_INDX_OFFSET] & 0xffffff;
    uint32_t highest = count - 1;
    std::vector<uint32_t> host_indices;
    uint32_t index_offset = 0;
    if (indices) {
      uint32_t size = indices->format == xenos::IndexFormat::kInt16 ? 2 : 4;
      if (uint64_t(count) * size > indices->length) throw Unsupported("index_buffer_length");
      const uint32_t bytes = (count * size + 3) & ~3u;
      if (PendingResolve(indices->guest_base, bytes)) throw Unsupported("index_pending_native_resolve");
      const auto* source = Guest(indices->guest_base, bytes);
      host_indices.resize(count); highest = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t word; std::memcpy(&word, source + ((i * size) & ~3u), 4); word = EndianWord(word, uint32_t(indices->endianness));
        uint32_t value = size == 2 ? (word >> ((i & 1) * 16)) & 0xffff : word;
        if (uint64_t(value) + base >= 1000000) throw Unsupported("vertex_index_range");
        host_indices[i] = value + base; highest = std::max(highest, host_indices[i]);
      }
    } else { if (uint64_t(highest) + base >= 1000000) throw Unsupported("vertex_index_range"); highest += base; }
    const uint32_t uniform_align = std::max(16u, uint32_t(gpu_->properties().minUniformBufferOffsetAlignment));
    // Reserve the full decoded plan, including faces, volume slices, mip levels
    // and copy alignment, since every texture may have changed. Upload still
    // checks its actual range after any earlier batch has retired.
    uint64_t budget = (indices ? uint64_t(count) * 4 + 4 : 0) + 9888 + uint64_t(uniform_align) * 5;
    for (uint32_t slot = 0; slot < 96; ++slot) if (vs->native.vertexFetchSubs[slot % 3] & (1u << (slot / 3)))
      budget += (uint64_t(highest) * vs->native.vertexFetchStride[slot] + vs->native.vertexFetchTail[slot]) * 4 + 4;
    for (const auto& binding : pipeline->textures) {
      const auto tf = r.GetTextureFetch(binding.binding % 32);
      if (TextureDimensionsCompatible(tf, binding.binding / 32)) budget += GetTexturePlan(tf, binding.binding / 32).output_bytes + 16;
    }
    if (recording_ && (batch_draws_ >= kBatchDraws || budget > kArenaBytes - cursor_)) Submit();
    if (!recording_) Begin();
    touched_batch = true;
    VkDescriptorSet sets[3]{}; VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptors_; allocation.descriptorSetCount = 3; allocation.pSetLayouts = pipeline->sets;
    Check(F().vkAllocateDescriptorSets(D(), &allocation, sets), "draw descriptors");
    if (indices) index_offset = Upload(host_indices.data(), count * 4, 4);
    std::array<uint32_t, 96 * 4> fetch{};
    for (uint32_t i = 0; i < 96; ++i) { fetch[i * 4] = r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 2]; fetch[i * 4 + 1] = r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 2 + 1]; }
    for (uint32_t slot = 0; slot < 96; ++slot) if (vs->native.vertexFetchSubs[slot % 3] & (1u << (slot / 3))) {
      const auto vf = r.GetVertexFetch(slot);
      if (uint32_t(vf.type) != 3) throw Unsupported("vertex_fetch_type");
      uint64_t needed = uint64_t(highest) * vs->native.vertexFetchStride[slot] + vs->native.vertexFetchTail[slot];
      if (!needed || needed > vf.size || needed * 4 > kArenaBytes) {
        if (unsupported_["vertex_fetch_bounds"] < 2) REXGPU_WARN("BO3 native VF{} bounds: highest={} stride={} tail={} neededDwords={} declaredDwords={} address={:08X}", slot, highest, vs->native.vertexFetchStride[slot], vs->native.vertexFetchTail[slot], needed, vf.size, vf.address << 2);
        throw Unsupported("vertex_fetch_bounds");
      }
      if (PendingResolve(vf.address << 2, uint32_t(needed * 4))) throw Unsupported("vertex_fetch_pending_native_resolve");
      uint32_t offset = Upload(Guest(vf.address << 2, uint32_t(needed * 4)), uint32_t(needed * 4), 4);
      fetch[slot * 4] = offset | 3;
    }
    const uint32_t starts[]{r.Get<reg::SQ_VS_CONST>().base, r.Get<reg::SQ_PS_CONST>().base};
    uint32_t constants[2][1024]{};
    for (uint32_t s = 0; s < 2; ++s) { uint32_t available = std::min(256u, 512u - starts[s]); std::memcpy(constants[s], &r.values[XE_GPU_REG_SHADER_CONSTANT_000_X + starts[s] * 4], available * 16); }
    uint32_t offsets[]{Upload(constants[0], 4096, uniform_align), Upload(fetch.data(), 1536, uniform_align), Upload(&r.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 32, uniform_align), Upload(&r.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 128, uniform_align), Upload(constants[1], 4096, uniform_align)};
    const uint32_t sizes[]{4096, 1536, 32, 128, 4096};
    VkDescriptorBufferInfo buffers[6]{}; VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
      buffers[i] = {arena_, i < 5 ? offsets[i] : 0, i < 5 ? sizes[i] : kArenaBytes};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sets[i < 5 ? 0 : 2]; writes[i].dstBinding = i < 5 ? i : 0; writes[i].descriptorCount = 1; writes[i].descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
    }
    F().vkUpdateDescriptorSets(D(), 6, writes, 0, nullptr);
    // Each draw receives immutable arena ranges and descriptor sets. They
    // remain alive until the common batch fence retires.
    for (const auto& binding : pipeline->textures) {
      auto* texture = GetTexture(binding.binding % 32, binding.binding / 32);
      VkDescriptorImageInfo image{texture->sampler, texture->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = sets[1]; write.dstBinding = binding.binding; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
      F().vkUpdateDescriptorSets(D(), 1, &write, 0, nullptr);
    }
    const auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
    // SDK normalization handles independently disabled axes, clip-disabled
    // window coordinates, negative scales, half pixels, and -W..W guest Z.
    // The translated VS then reconstructs W and pre-divided XYZ before NDC.
    draw_util::ViewportInfo host_viewport{};
    draw_util::GetHostViewportInfo(r, 1, 1, false, target->color.width, target->color.height,
        true, draw_util::GetNormalizedDepthControl(r), false, false, ps && ps->writes_depth(), host_viewport);
    VkViewport viewport{float(host_viewport.xy_offset[0]), float(host_viewport.xy_offset[1]),
        float(host_viewport.xy_extent[0]), float(host_viewport.xy_extent[1]), host_viewport.z_min, host_viewport.z_max};
    struct Push { uint32_t alpha; float ref, scale; uint32_t gamma; float ndc[4]; float depth[4]; } push{7, 0, 1, 0,
        {host_viewport.ndc_scale[0], host_viewport.ndc_scale[1], host_viewport.ndc_offset[0], host_viewport.ndc_offset[1]},
        {host_viewport.ndc_scale[2], host_viewport.ndc_offset[2], float((vte.value >> 8) & 7), 0}};
    static_assert(sizeof(Push) == 48);
    const auto cc = r.Get<reg::RB_COLORCONTROL>(); push.alpha = cc.alpha_test_enable ? uint32_t(cc.alpha_func) : 7; push.ref = r.Get<float>(XE_GPU_REG_RB_ALPHA_REF); push.scale = std::ldexp(1.f, r.Get<reg::RB_COLOR_INFO>().color_exp_bias);
    if (!host_viewport.xy_extent[0] || !host_viewport.xy_extent[1]) { Submit(); return true; }
    for (float value : push.ndc) if (!std::isfinite(value)) throw Unsupported("nonfinite_ndc_transform");
    for (float value : push.depth) if (!std::isfinite(value)) throw Unsupported("nonfinite_ndc_transform");
    if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 || std::abs(viewport.height) <= 0 || viewport.width > 4096 || std::abs(viewport.height) > 2048) throw Unsupported("viewport_range");
    draw_util::Scissor guest_scissor{}; draw_util::GetScissor(r, guest_scissor);
    int32_t left = std::min(guest_scissor.offset[0], target->color.width), top = std::min(guest_scissor.offset[1], target->color.height);
    int32_t right = std::min(uint64_t(guest_scissor.offset[0]) + guest_scissor.extent[0], uint64_t(target->color.width));
    int32_t bottom = std::min(uint64_t(guest_scissor.offset[1]) + guest_scissor.extent[1], uint64_t(target->color.height));
    if (left == right || top == bottom) { Submit(); return true; }
    VkRect2D scissor{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
    float blend[4]; for (uint32_t i = 0; i < 4; ++i) blend[i] = r.Get<float>(XE_GPU_REG_RB_BLEND_RED + i);
    float bias_scale, bias_offset;
    draw_util::GetPreferredFacePolygonOffset(r, !rectangle, bias_scale, bias_offset);
    bias_scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
    bias_offset *= r.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
        ? draw_util::kD3D10PolygonOffsetFactorUnorm24 : draw_util::kD3D10PolygonOffsetFactorFloat24;
    if (!std::isfinite(bias_scale) || !std::isfinite(bias_offset)) throw Unsupported("nonfinite_depth_bias");
    if (!depth_only) Transition(target->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Transition(target->depth->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    // Preserve attachment load/blend/depth ordering across separate passes in
    // the same submission, including unchanged image layouts.
    VkMemoryBarrier attachment_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    attachment_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    attachment_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    constexpr VkPipelineStageFlags attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    F().vkCmdPipelineBarrier(command_, attachment_stages, attachment_stages, 0, 1, &attachment_barrier, 0, nullptr, 0, nullptr);
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = target->pass; render.framebuffer = target->framebuffer; render.renderArea.extent = {target->color.width, target->color.height};
    F().vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE); F().vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
    F().vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0, 3, sets, 0, nullptr);
    F().vkCmdPushConstants(command_, pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    F().vkCmdSetViewport(command_, 0, 1, &viewport); F().vkCmdSetScissor(command_, 0, 1, &scissor); F().vkCmdSetBlendConstants(command_, blend);
    F().vkCmdSetDepthBias(command_, bias_offset, 0, bias_scale);
    const auto depth_state = r.Get<reg::RB_DEPTHCONTROL>();
    const bool separate = !rectangle && depth_state.backface_enable && gpu_->properties().separateStencilMaskRef;
    const auto front_reg = !rectangle && depth_state.backface_enable && !separate && r.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front
        ? XE_GPU_REG_RB_STENCILREFMASK_BF : XE_GPU_REG_RB_STENCILREFMASK;
    for (uint32_t face = 0; face < 2; ++face) {
      const auto sm = r.Get<reg::RB_STENCILREFMASK>(face && separate ? XE_GPU_REG_RB_STENCILREFMASK_BF : front_reg);
      const auto flags = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
      F().vkCmdSetStencilCompareMask(command_, flags, sm.stencilmask);
      F().vkCmdSetStencilWriteMask(command_, flags, sm.stencilwritemask);
      F().vkCmdSetStencilReference(command_, flags, sm.stencilref);
    }
    const bool probe = coverage_queries_ && pipeline->probes < 4 && (rectangle || target->color.samples != VK_SAMPLE_COUNT_1_BIT);
    if (probe) F().vkCmdBeginQuery(command_, coverage_queries_, batch_draws_, gpu_->properties().occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    if (indices) { F().vkCmdBindIndexBuffer(command_, arena_, index_offset, VK_INDEX_TYPE_UINT32); F().vkCmdDrawIndexed(command_, count, 1, 0, 0, 0); }
    else F().vkCmdDraw(command_, count, 1, base, 0);
    if (probe) {
      F().vkCmdEndQuery(command_, coverage_queries_, batch_draws_);
      coverage_probes_.push_back({batch_draws_, pipeline, target, swaps_}); ++pipeline->probes;
    }
    F().vkCmdEndRenderPass(command_); ++batch_draws_; ++submitted_; ++target->draws;
    if (depth_only) ++depth_submitted_; else latest_ = target;
    if (ps && (pipeline->color_mask & 15)) target->color_sequence = ++color_sequence_;
    if (bypass_ps && ++pixel_shader_bypassed_ <= 5) REXGPU_INFO("BO3 native PS bypass: VS={:016X} mask={:08X} depth={:08X} vte={:08X} clip={:08X} viewport={},{} {}x{} z={},{} ndcZ={},{}", vs->ucode_data_hash(), r[XE_GPU_REG_RB_COLOR_MASK], r[XE_GPU_REG_RB_DEPTHCONTROL], vte.value, r[XE_GPU_REG_PA_CL_CLIP_CNTL], viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth, push.depth[0], push.depth[1]);
    if (rectangle && ++rectangles_submitted_ <= 8) REXGPU_INFO("BO3 native rectangle: VS={:016X} PS={:016X} count={} depth={:08X} mask={:08X} vte={:08X} clip={:08X} stencil={:08X} viewport={},{} {}x{} z={},{}", pipeline->vs_hash, pipeline->ps_hash, count, depth_state.value, pipeline->color_mask, vte.value, r[XE_GPU_REG_PA_CL_CLIP_CNTL], r[XE_GPU_REG_RB_STENCILREFMASK], viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth);
    if (rectangle) PropagateFastDepthClear(*vs, ps, *target, fetch.data(), base, indices != nullptr, count);
    pending_work_ = true;
    if (target->color.samples != VK_SAMPLE_COUNT_1_BIT && target->color.width >= 480 && target->color.height >= 360) {
      depth_scene_ = target; depth_scene_draw_swap_ = swaps_;
      if (!depth_only) { scene_ = target; scene_draw_swap_ = swaps_; }
    }
    if (batch_draws_ >= kBatchDraws) Submit();
    if (submitted_ <= 5 || (depth_only && depth_submitted_ <= 5)) REXGPU_INFO("BO3 NATIVE submitted draw {}: VS {:016X} PS {:016X} {} vertices {}x{} depthOnly={} depthState={:08X}", submitted_, vs->ucode_data_hash(), ps ? ps->ucode_data_hash() : 0, count, target->color.width, target->color.height, depth_only, r[XE_GPU_REG_RB_DEPTHCONTROL]);
    if (readback_diagnostic_ && submitted_ <= 5 && target->color.format == VK_FORMAT_R8G8B8A8_UNORM && target->color.samples == VK_SAMPLE_COUNT_1_BIT) {
      if (recording_) Submit();
      REXGPU_INFO("BO3 native draw state: depth={:08X} blend={:08X} raster={:08X} mask={:08X} vte={:08X} viewport={},{} {},{} alpha={} scale={}", r[XE_GPU_REG_RB_DEPTHCONTROL], r[XE_GPU_REG_RB_BLENDCONTROL0], r[XE_GPU_REG_PA_SU_SC_MODE_CNTL], r[XE_GPU_REG_RB_COLOR_MASK], r[XE_GPU_REG_PA_CL_VTE_CNTL], viewport.x, viewport.y, viewport.width, viewport.height, push.alpha, push.scale);
      const uint32_t bytes = target->color.width * target->color.height * 4;
      if (bytes <= kArenaBytes) {
        Begin(); Transition(target->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {target->color.width, target->color.height, 1};
        F().vkCmdCopyImageToBuffer(command_, target->color.image, target->color.layout, arena_, 1, &copy);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = arena_; barrier.size = bytes;
        F().vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
        Submit();
        uint32_t rgb = 0, alpha = 0;
        for (uint32_t i = 0; i < bytes; i += 4) { if (mapped_[i] || mapped_[i + 1] || mapped_[i + 2]) ++rgb; if (mapped_[i + 3]) ++alpha; }
        REXGPU_INFO("BO3 native target readback: draw={} nonzeroRGBpixels={} nonzeroAlphaPixels={} totalPixels={}", submitted_, rgb, alpha, bytes / 4);
      }
    }
    return true;
  } catch (const Unsupported& error) {
      // A later unsupported texture/state may follow recorded uploads. Submit
      // those before reusing memory so tracked layouts/content stay truthful.
      if (recording_ && touched_batch) { try { Submit(); } catch (const std::exception& failure) { failed_ = true; REXGPU_ERROR("BO3 native partial upload submission failed: {}", failure.what()); } }
      return Reject(error.what());
    }
    catch (const std::exception& error) { failed_ = true; REXGPU_ERROR("BO3 native draw backend stopped: {}", error.what()); return Reject("native_draw_failure"); }
}

void NativeCommandProcessor::PropagateFastDepthClear(NativeShader& vs, NativeShader* ps, Target& target,
                                                   const uint32_t* fetch, uint32_t base, bool indexed, uint32_t count) {
  // Bounded diagnostics for this observed clear program only. Other rectangle
  // shaders still execute normally and are never approximated as clears.
  if (vs.ucode_data_hash() != 0x1E6883FCCDE1F688ull) return;
  static std::map<std::string, uint64_t> skipped;
  auto skip = [&](const char* reason) {
    if (++skipped[reason] == 1) REXGPU_INFO("BO3 native fast clear not recognized: reason={} VSsha={} PSsha={} indexed={} count={} base={}", reason, vs.canonical_sha256, ps ? ps->canonical_sha256 : "none", indexed, count, base);
  };
  // Exact captured XDK pass-through clear VS: VF0 = float3 position followed
  // by float4 color, stride seven dwords, position W=1. No general VS emulation.
  // The saved SDK text disassembly names VF95, but the captured binary and the
  // executable native translation use VF0. Verify actual translated metadata.
  if (vs.canonical_sha256 != "abfec28e9858fd1b9b27afd527f08421c38f288e9333cc7fc10d6391ac190fa9" || indexed || count != 3) { skip("program_or_draw"); return; }
  if (vs.native.vertexFetchSubs[0] != 1 || vs.native.vertexFetchSubs[1] || vs.native.vertexFetchSubs[2] || vs.native.vertexFetchStride[0] != 7 || vs.native.vertexFetchTail[0] != 7) { skip("vertex_metadata"); return; }
  if (ps && ps->canonical_sha256 != "9d4a64e81064abb794c5cba3642feda86a5a1e6b75a13cd4c4d7917bea83e01e") { skip("pixel_program"); return; }
  const auto& r = *register_file_;
  const auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
  const auto clip = r.Get<reg::PA_CL_CLIP_CNTL>();
  const auto depth = r.Get<reg::RB_DEPTHCONTROL>();
  const auto color = r.Get<reg::RB_COLORCONTROL>();
  // The observed fast-clear disables MSAA raster coverage, replicating pixel
  // center coverage to all stored samples. Per-sample edge clears need another path.
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().msaa_enable) { skip("per_sample_coverage"); return; }
  if ((vte.value & 0x7ff) != 0x300 || !clip.clip_disable || draw_util::DoesCoverageDependOnAlpha(color)) { skip("format_clip_alpha"); return; }
  if (depth.z_enable && depth.zfunc != xenos::CompareFunction::kAlways) { skip("depth_compare"); return; }
  VkImageAspectFlags aspects = depth.z_enable && depth.z_write_enable ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
  const auto stencil = r.Get<reg::RB_STENCILREFMASK>();
  if (depth.stencil_enable) {
    if (depth.stencilfunc != xenos::CompareFunction::kAlways) { skip("stencil_compare"); return; }
    if (depth.stencilzpass == xenos::StencilOp::kReplace && stencil.stencilwritemask == 255) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    else if (depth.stencilzpass != xenos::StencilOp::kKeep) { skip("stencil_operation"); return; }
  }
  if (!aspects) { skip("no_writes"); return; }
  const auto vf = r.GetVertexFetch(0);
  const uint32_t offset = fetch[0] & ~3u;
  if (uint64_t(offset) + uint64_t(base + 3) * 28 > cursor_) throw Unsupported("fast_clear_snapshot_bounds");
  draw_util::ViewportInfo viewport{};
  draw_util::GetHostViewportInfo(r, 1, 1, false, target.color.width, target.color.height,
      true, draw_util::GetNormalizedDepthControl(r), false, false, false, viewport);
  float xs[3], ys[3], zs[3];
  for (uint32_t i = 0; i < 3; ++i) {
    float position[3];
    for (uint32_t c = 0; c < 3; ++c) {
      uint32_t word; std::memcpy(&word, mapped_ + offset + (base + i) * 28 + c * 4, 4);
      position[c] = std::bit_cast<float>(EndianWord(word, uint32_t(vf.endian)));
      if (!std::isfinite(position[c])) { skip("nonfinite_position"); return; }
    }
    xs[i] = (std::fma(position[0], viewport.ndc_scale[0], viewport.ndc_offset[0]) + 1) * float(viewport.xy_extent[0]) * .5f + viewport.xy_offset[0];
    ys[i] = (std::fma(position[1], viewport.ndc_scale[1], viewport.ndc_offset[1]) + 1) * float(viewport.xy_extent[1]) * .5f + viewport.xy_offset[1];
    zs[i] = std::fma(position[2], viewport.ndc_scale[2], viewport.ndc_offset[2]);
    xs[i] = float(rex::ui::FloatToD3D11Fixed16p8(xs[i])) / 256;
    ys[i] = float(rex::ui::FloatToD3D11Fixed16p8(ys[i])) / 256;
  }
  if (zs[0] != zs[1] || zs[0] != zs[2] || (zs[0] != 0 && zs[0] != 1)) { skip("nonconstant_endpoint_depth"); return; }
  const float x0 = *std::min_element(xs, xs + 3), x1 = *std::max_element(xs, xs + 3);
  const float y0 = *std::min_element(ys, ys + 3), y1 = *std::max_element(ys, ys + 3);
  if (!(x1 > x0 && y1 > y0)) {
    if (!skipped.contains("empty_rectangle")) REXGPU_INFO("BO3 native empty clear: hostXY=({},{}),({},{}),({},{}) VFaddress={:08X} endian={} uploadOffset={}", xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], vf.address << 2, uint32_t(vf.endian), offset);
    skip("empty_rectangle"); return;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    if ((xs[i] != x0 && xs[i] != x1) || (ys[i] != y0 && ys[i] != y1)) { skip("nonrectangle_vertices"); return; }
    for (uint32_t j = 0; j < i; ++j) if (xs[i] == xs[j] && ys[i] == ys[j]) { skip("duplicate_vertices"); return; }
  }
  draw_util::Scissor scissor{}; draw_util::GetScissor(r, scissor);
  const uint32_t scissor_left = std::min(scissor.offset[0], target.color.width);
  const uint32_t scissor_top = std::min(scissor.offset[1], target.color.height);
  const uint32_t scissor_right = uint32_t(std::min(uint64_t(scissor.offset[0]) + scissor.extent[0], uint64_t(target.color.width)));
  const uint32_t scissor_bottom = uint32_t(std::min(uint64_t(scissor.offset[1]) + scissor.extent[1], uint64_t(target.color.height)));
  auto boundary = [](float edge) { return ClearPixelBoundary(int32_t(edge * 256)); };
  const uint32_t left = uint32_t(std::clamp(boundary(x0), int32_t(scissor_left), int32_t(scissor_right)));
  const uint32_t top = uint32_t(std::clamp(boundary(y0), int32_t(scissor_top), int32_t(scissor_bottom)));
  const uint32_t right = uint32_t(std::clamp(boundary(x1), int32_t(left), int32_t(scissor_right)));
  const uint32_t bottom = uint32_t(std::clamp(boundary(y1), int32_t(top), int32_t(scissor_bottom)));
  if (right <= left || bottom <= top) { skip("empty_scissor"); return; }
  const auto surface = r.Get<reg::RB_SURFACE_INFO>();
  const auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
  const uint32_t samples = uint32_t(surface.msaa_samples), pitch = surface.surface_pitch * SampleX(samples);
  const auto physical = ToSampleRectangle({left, top, right - left, bottom - top}, samples);
  const uint32_t stencil_value = stencil.stencilref;
  const uint32_t depth_bits = zs[0] == 0 ? 0 : (uint32_t(depth_info.depth_format) ? xenos::Float32To20e4(1, true) : 0xffffff);
  DepthClear clear{{{int32_t(physical.x), int32_t(physical.y)}, {physical.width, physical.height}}, {zs[0], stencil_value}, (depth_bits << 8) | stencil_value, aspects, ++depth_clear_sequence_};
  const std::array<uint32_t, 3> key{depth_info.depth_base, uint32_t(depth_info.depth_format), pitch};
  const bool fresh = !fast_depth_clears_.contains(key);
  if (fresh && fast_depth_clears_.size() >= 64) throw Unsupported("fast_clear_cache_limit");
  auto& history = fast_depth_clears_[key];
  std::erase_if(history, [&](const DepthClear& old) {
    return (clear.aspects & old.aspects) == old.aspects && clear.rectangle.offset.x <= old.rectangle.offset.x && clear.rectangle.offset.y <= old.rectangle.offset.y &&
        uint64_t(physical.x) + physical.width >= uint64_t(old.rectangle.offset.x) + old.rectangle.extent.width && uint64_t(physical.y) + physical.height >= uint64_t(old.rectangle.offset.y) + old.rectangle.extent.height;
  });
  if (history.size() >= 64) throw Unsupported("fast_clear_history_limit");
  history.push_back(clear); ++fast_clear_requests_;
  for (auto& [depth_key, destination] : depths_) {
    if (destination.get() == target.depth || depth_key[0] != key[0] || depth_key[1] != key[1] || depth_key[2] * SampleX(depth_key[4]) != pitch) continue;
    const auto rectangle = FromSampleRectangle(physical, pitch, depth_key[2], depth_key[4]);
    auto mapped_clear = clear; mapped_clear.rectangle = {{int32_t(rectangle.x), int32_t(rectangle.y)}, {rectangle.width, rectangle.height}};
    for (auto& [target_key, alias] : targets_) if (alias->depth == destination.get()) { ApplyDepthClear(*alias, mapped_clear); ++fast_clear_aliases_; break; }
  }
  if (fresh || fast_clear_requests_ <= 8) REXGPU_INFO("BO3 native fast depth clear: base={} format={} samplePitch={} source={}x{} msaa={} sampleRect={},{} {}x{} depth={} stencil={} aspects={} requests={} aliases={}", key[0], key[1], pitch, target.color.width, target.color.height, uint32_t(target.color.samples), physical.x, physical.y, physical.width, physical.height, zs[0], stencil_value, aspects, fast_clear_requests_, fast_clear_aliases_);
}

void NativeCommandProcessor::ApplyDepthClear(Target& target, const DepthClear& clear) {
  auto& image = target.depth->image;
  const uint32_t x = std::min(uint32_t(clear.rectangle.offset.x), image.width);
  const uint32_t y = std::min(uint32_t(clear.rectangle.offset.y), image.height);
  const uint32_t right = uint32_t(std::min(uint64_t(clear.rectangle.offset.x) + clear.rectangle.extent.width, uint64_t(image.width)));
  const uint32_t bottom = uint32_t(std::min(uint64_t(clear.rectangle.offset.y) + clear.rectangle.extent.height, uint64_t(image.height)));
  if (right <= x || bottom <= y) return;
  if (!recording_) Begin();
  if (!target.depth_only) Transition(target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  Transition(image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  constexpr VkPipelineStageFlags stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  F().vkCmdPipelineBarrier(command_, stages, stages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = target.pass; render.framebuffer = target.framebuffer;
  render.renderArea.extent = {image.width, image.height};
  VkClearAttachment attachment{}; attachment.aspectMask = clear.aspects; attachment.clearValue.depthStencil = clear.value;
  VkClearRect rectangle{{{int32_t(x), int32_t(y)}, {right - x, bottom - y}}, 0, 1};
  F().vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
  clear_attachments_(command_, 1, &attachment, 1, &rectangle);
  F().vkCmdEndRenderPass(command_); pending_work_ = true; ++depth_clear_applied_;
}

draw_util::ResolveInfo NativeCommandProcessor::DecodeResolveInfo() {
  const auto& r = *register_file_;
  const auto control = r.Get<reg::RB_COPY_CONTROL>();
  const auto surface = r.Get<reg::RB_SURFACE_INFO>();
  if (!surface.surface_pitch || surface.surface_pitch > 4096 || uint32_t(surface.msaa_samples) > 2 || control.copy_src_select > 4 ||
      (control.copy_command != xenos::CopyCommand::kRaw && control.copy_command != xenos::CopyCommand::kConvert)) throw Unsupported("resolve_copy_state");
  const auto fetch = r.GetVertexFetch(0);
  if (uint32_t(fetch.type) != 3 || fetch.size != 6) throw Unsupported("resolve_vertex_format");
  const auto* vertices = Guest(fetch.address << 2, 24);
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t word; std::memcpy(&word, vertices + i * 4, 4);
    const float coordinate = std::bit_cast<float>(EndianWord(word, uint32_t(fetch.endian)));
    if (!std::isfinite(coordinate) || std::abs(coordinate) > 16384) throw Unsupported("resolve_vertex_range");
  }
  draw_util::ResolveInfo info{};
  if (!draw_util::GetResolveInfo(r, *memory_, 1, 1, false, false, info)) throw Unsupported("resolve_rectangle");
  return info;
}
std::pair<uint32_t,uint32_t> NativeCommandProcessor::MemoryInvalidated(void* context, uint32_t address, uint32_t size, bool) {
  auto& self=*static_cast<NativeCommandProcessor*>(context);
  std::lock_guard lock(self.invalidation_mutex_);
  if (self.invalidation_queue_.size()==4096) { self.invalidate_all_resolves_=true; self.invalidation_queue_.clear(); }
  else self.invalidation_queue_.push_back({address,size});
  return {address,size};
}
void NativeCommandProcessor::InvalidateResolvedRange(uint32_t address,uint32_t size) {
  const uint64_t end=uint64_t(address)+size;
  for (auto& [key,surface]:resolved_surfaces_) if (address<uint64_t(surface->plan.extent_start)+surface->plan.extent_bytes && end>surface->plan.extent_start)
    surface->coverage.clear();
}
void NativeCommandProcessor::DrainResolveInvalidations() {
  std::lock_guard lock(invalidation_mutex_);
  // Metadata only: no Memory/Vulkan calls under this lock. Keep the reserved
  // queue allocation so guest fault callbacks never grow it.
  if (invalidate_all_resolves_) for (auto& [key,surface]:resolved_surfaces_) surface->coverage.clear();
  else for (auto [address,size]:invalidation_queue_) InvalidateResolvedRange(address,size);
  invalidate_all_resolves_=false;invalidation_queue_.clear();
}
Target* NativeCommandProcessor::FindResolveSource(const draw_util::ResolveInfo& info,ResolveRect rectangle) {
  Target* source=nullptr;
  const auto surface=register_file_->Get<reg::RB_SURFACE_INFO>();
  const uint32_t format=uint32_t(xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(info.color_edram_info.format)));
  for (auto& [key,target]:targets_) {
    if(target->depth_only || key[0]!=info.color_original_base || key[1]!=format || key[2]!=surface.surface_pitch || key[6]!=uint32_t(surface.msaa_samples) ||
       !ResolveRectContains({0,0,target->color.width,target->color.height},rectangle) || !target->color_sequence) continue;
    if(!source || target->color_sequence>source->color_sequence) source=target.get();
  }
  if(!source) throw Unsupported("native_resolve_source_missing");
  return source;
}
ResolvePipeline* NativeCommandProcessor::GetResolvePipeline(VkFormat format,bool multisampled) {
  const std::array<uint32_t,2> key{uint32_t(format),uint32_t(multisampled)};
  auto found=resolve_pipelines_.find(key); if(found!=resolve_pipelines_.end()) return found->second.get();
  auto result=std::make_unique<ResolvePipeline>(); auto& p=result->gpu;
  try {
    auto module=[&](VkShaderModule& handle,const uint32_t* code,size_t bytes) {
      if(handle) return; VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; m.codeSize=bytes;m.pCode=code;
      Check(F().vkCreateShaderModule(D(),&m,nullptr,&handle),"resolve shader");
    };
    module(resolve_vs_,kPresentVS,sizeof(kPresentVS)); module(resolve_ps_,kPresentResolvePS,sizeof(kPresentResolvePS)); module(resolve_ms_ps_,kPresentResolveMSPS,sizeof(kPresentResolveMSPS));
    VkAttachmentDescription attachment{}; attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&sub;
    Check(F().vkCreateRenderPass(D(),&pass,nullptr,&result->pass),"resolve render pass");
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;
    Check(F().vkCreateDescriptorSetLayout(D(),&sl,nullptr,&p.sets[0]),"resolve descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=p.sets;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    Check(F().vkCreatePipelineLayout(D(),&layout,nullptr,&p.layout),"resolve layout");
    VkPipelineShaderStageCreateInfo stages[2]{};for(auto& stage:stages) stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=resolve_vs_;stages[0].pName="VS";
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=multisampled ? resolve_ms_ps_ : resolve_ps_;stages[1].pName=multisampled ? "ResolveMSPS" : "ResolvePS";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};input.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&ba;
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages;info.pVertexInputState=&vertex;info.pInputAssemblyState=&input;
    info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&ms;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=p.layout;info.renderPass=result->pass;
    Check(F().vkCreateGraphicsPipelines(D(),VK_NULL_HANDLE,1,&info,nullptr,&p.pipeline),"resolve pipeline");
  } catch(...) {
    if(p.pipeline)F().vkDestroyPipeline(D(),p.pipeline,nullptr);if(p.layout)F().vkDestroyPipelineLayout(D(),p.layout,nullptr);
    if(p.sets[0])F().vkDestroyDescriptorSetLayout(D(),p.sets[0],nullptr);if(result->pass)F().vkDestroyRenderPass(D(),result->pass,nullptr);throw;
  }
  auto* pointer=result.get();resolve_pipelines_.emplace(key,std::move(result));return pointer;
}
bool NativeCommandProcessor::ResolveColor(const draw_util::ResolveInfo& info) {
  ColorResolvePlan plan;
  try {plan=PlanColorResolve(info,register_file_->Get<reg::RB_SURFACE_INFO>().surface_pitch,(*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE]);}
  catch(const ColorResolveError& error){throw Unsupported(error.what());}
  auto* source=FindResolveSource(info,plan.source);
  auto* pipeline=GetResolvePipeline(plan.format,plan.samples>1);
  const std::array<uint32_t,5> key{plan.address,plan.width,plan.height,plan.guest_format,plan.endian};
  auto found=resolved_surfaces_.find(key);
  if(found==resolved_surfaces_.end()) {
    if(resolved_surfaces_.size()>=64) throw Unsupported("resolved_surface_cache_limit");
    auto created=std::make_unique<ResolvedSurface>();
    created->image=CreateImage(plan.width,plan.height,plan.format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
    try {
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pipeline->pass;fb.attachmentCount=1;fb.pAttachments=&created->image.view;fb.width=plan.width;fb.height=plan.height;fb.layers=1;
      Check(F().vkCreateFramebuffer(D(),&fb,nullptr,&created->framebuffer),"resolved framebuffer");
      Begin();Transition(created->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      F().vkCmdClearColorImage(command_,created->image.image,created->image.layout,&zero,1,&range);Submit();
    }catch(...){if(created->framebuffer)F().vkDestroyFramebuffer(D(),created->framebuffer,nullptr);DestroyImage(created->image);throw;}
    found=resolved_surfaces_.emplace(key,std::move(created)).first;
  }
  auto& cached=found->second;
  // Only compatible updates preserve coverage; a differently pitched/formatted
  // alias invalidates the whole older view, never just its CPU hash.
  const uint64_t end=uint64_t(plan.extent_start)+plan.extent_bytes;
  for(auto& [other_key,other]:resolved_surfaces_) if(other.get()!=cached.get() && plan.extent_start<uint64_t(other->plan.extent_start)+other->plan.extent_bytes && end>other->plan.extent_start) other->coverage.clear();
  memory_->EnablePhysicalMemoryAccessCallbacks(plan.extent_start,plan.extent_bytes,true,false);
  auto aggregate=plan;
  if(!cached->coverage.empty()) {
    aggregate.extent_start=std::min(plan.extent_start,cached->plan.extent_start);
    aggregate.extent_bytes=uint32_t(std::max(uint64_t(plan.extent_start)+plan.extent_bytes,uint64_t(cached->plan.extent_start)+cached->plan.extent_bytes)-aggregate.extent_start);
  }
  cached->plan=aggregate;
  Begin();
  const bool direct=plan.samples==1 && source->color.format==plan.format && !plan.exponent && !plan.swap_rb;
  if(direct) {
    Transition(source->color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);Transition(cached->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.srcOffset={int32_t(plan.source.x),int32_t(plan.source.y),0};copy.dstOffset={int32_t(plan.destination.x),int32_t(plan.destination.y),0};copy.extent={plan.source.width,plan.source.height,1};
    copy_image_(command_,source->color.image,source->color.layout,cached->image.image,cached->image.layout,1,&copy);pending_work_=true;
  } else {
    Transition(source->color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);Transition(cached->image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkDescriptorSet set{};VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptors_;allocate.descriptorSetCount=1;allocate.pSetLayouts=pipeline->gpu.sets;
    Check(F().vkAllocateDescriptorSets(D(),&allocate,&set),"resolve descriptor");
    VkDescriptorImageInfo image{VK_NULL_HANDLE,source->color.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&image;F().vkUpdateDescriptorSets(D(),1,&write,0,nullptr);
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};render.renderPass=pipeline->pass;render.framebuffer=cached->framebuffer;render.renderArea.extent={plan.width,plan.height};
    F().vkCmdBeginRenderPass(command_,&render,VK_SUBPASS_CONTENTS_INLINE);F().vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->gpu.pipeline);
    F().vkCmdBindDescriptorSets(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->gpu.layout,0,1,&set,0,nullptr);
    struct Push {int32_t source[2],destination[2];float scale;uint32_t swap,padding[2];} push{{int32_t(plan.source.x),int32_t(plan.source.y)},{int32_t(plan.destination.x),int32_t(plan.destination.y)},std::ldexp(1.f,plan.exponent),uint32_t(plan.swap_rb),{0,0}};
    static_assert(sizeof(Push)==32);F().vkCmdPushConstants(command_,pipeline->gpu.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    VkViewport viewport{0,0,float(plan.width),float(plan.height),0,1};VkRect2D scissor{{int32_t(plan.destination.x),int32_t(plan.destination.y)},{plan.destination.width,plan.destination.height}};
    F().vkCmdSetViewport(command_,0,1,&viewport);F().vkCmdSetScissor(command_,0,1,&scissor);F().vkCmdDraw(command_,3,1,0,0);F().vkCmdEndRenderPass(command_);pending_work_=true;
  }
  Transition(cached->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);Submit();
  AddResolveCoverage(cached->coverage,plan.destination);cached->sequence=++resolved_sequence_;++resolve_completed_;
  if(resolve_completed_<=8) REXGPU_INFO("BO3 native color resolve completed: address={:08X} format={} rect={},{} {}x{} samples={} exponent={} direct={}",plan.address,plan.guest_format,plan.destination.x,plan.destination.y,plan.destination.width,plan.destination.height,plan.samples,plan.exponent,direct);
  DrainResolveInvalidations();return true;
}
void NativeCommandProcessor::ClearResolvedColor(const draw_util::ResolveInfo& info) {
  if(!info.IsClearingColor())return;
  ResolveRect rectangle;
  try {rectangle=ColorResolveSourceRectangle(info);}catch(const ColorResolveError& e){throw Unsupported(e.what());}
  auto* target=FindResolveSource(info,rectangle);
  const auto format=xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(info.color_edram_info.format));
  VkClearColorValue value{};
  const uint32_t first=info.rb_color_clear,second=info.rb_color_clear_lo;
  switch(uint32_t(format)) {
    case 0:for(uint32_t i=0;i<4;++i)value.float32[i]=float((first>>(i*8))&255)/255;break;
    case 2:for(uint32_t i=0;i<3;++i)value.float32[i]=float((first>>(i*10))&1023)/1023;value.float32[3]=float(first>>30)/3;break;
    case 6:case 7:
      for(uint32_t i=0;i<(uint32_t(format)==6 ? 2u : 4u);++i) {
        const uint32_t half=((i<2 ? first : second)>>((i&1)*16))&65535;
        if(half!=0 && half!=0x3c00 && half!=0x8000)throw Unsupported("color_clear_half_nonendpoint");
        value.float32[i]=half==0x3c00 ? 1.f : half==0x8000 ? -0.f : 0.f;
      }break;
    case 14:case 15:value.float32[0]=std::bit_cast<float>(first);value.float32[1]=std::bit_cast<float>(second);break;
    default:throw Unsupported("color_clear_format");
  }
  if(!recording_)Begin();Transition(target->color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  Transition(target->depth->image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);
  VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};render.renderPass=target->pass;render.framebuffer=target->framebuffer;render.renderArea.extent={target->color.width,target->color.height};
  VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;attachment.colorAttachment=0;attachment.clearValue.color=value;
  VkClearRect clear{{{int32_t(rectangle.x),int32_t(rectangle.y)},{rectangle.width,rectangle.height}},0,1};
  F().vkCmdBeginRenderPass(command_,&render,VK_SUBPASS_CONTENTS_INLINE);clear_attachments_(command_,1,&attachment,1,&clear);F().vkCmdEndRenderPass(command_);
  pending_work_=true;target->color_sequence=++color_sequence_;
}

Texture* NativeCommandProcessor::GetResolvedTexture(const TextureUploadPlan& plan,const xenos::xe_gpu_texture_fetch_t& fetch,uint32_t kind) {
  DrainResolveInvalidations();
  if(kind!=0 || plan.kind!=0 || !fetch.tiled || plan.bc) throw Unsupported("resolved_texture_shape_unimplemented");
  std::vector<ResolvedSurface*> sources;
  std::vector<uint64_t> versions;
  for(const auto& slice:plan.slices) {
    const uint32_t address=(slice.from_mips ? plan.mip_address : plan.base_address)+slice.source_offset;
    const ResolveRect rectangle{slice.x_blocks*plan.block_width,slice.y_blocks*plan.block_width,slice.width,slice.height};
    ResolvedSurface* found=nullptr;
    for(auto& [key,surface]:resolved_surfaces_) {
      if(surface->plan.address!=address || surface->plan.endian!=uint32_t(fetch.endianness) || surface->image.format!=plan.format ||
         surface->image.width!=slice.row_pitch/plan.bytes_per_block || !std::any_of(surface->coverage.begin(),surface->coverage.end(),[&](ResolveRect r){return ResolveRectContains(r,rectangle);})) continue;
      if(!found || surface->sequence>found->sequence) found=surface.get();
    }
    if(!found) throw Unsupported("resolved_texture_coverage_or_layout");
    sources.push_back(found);versions.push_back(found->sequence);
  }
  const std::array<uint64_t,2> version{XXH3_64bits(versions.data(),versions.size()*sizeof(uint64_t)),0xB03B03B03B03B03ull};
  auto* texture=EnsureTexture(plan,fetch,kind);
  if(texture->gpu_resolved && texture->content==version) return texture;
  if(!recording_)Begin();
  Transition(texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // Levels outside the guest's valid mip range have no backing data. Clear
  // them consistently with CPU uploads without granting resolve coverage.
  VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,texture->image.levels,0,texture->image.layers};
  F().vkCmdClearColorImage(command_,texture->image.image,texture->image.layout,&zero,1,&range);
  VkMemoryBarrier order{VK_STRUCTURE_TYPE_MEMORY_BARRIER};order.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;order.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  F().vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&order,0,nullptr,0,nullptr);
  for(uint32_t i=0;i<plan.slices.size();++i) {
    auto& source=*sources[i];const auto& slice=plan.slices[i];Transition(source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,slice.mip,slice.layer,1};
    copy.srcOffset={int32_t(slice.x_blocks*plan.block_width),int32_t(slice.y_blocks*plan.block_width),0};copy.extent={slice.width,slice.height,1};
    copy_image_(command_,source.image.image,source.image.layout,texture->image.image,texture->image.layout,1,&copy);pending_work_=true;
  }
  Transition(texture->image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);texture->content=version;texture->gpu_resolved=true;++resolved_reads_;
  if(resolved_reads_<=8) REXGPU_INFO("BO3 native resolved texture reader: address={:08X} {}x{} levels={}",plan.base_address,plan.width,plan.height,plan.levels);
  return texture;
}

void NativeCommandProcessor::TrackResolveDestination(const draw_util::ResolveInfo& info) {
  if (!info.copy_dest_extent_length) return;
  // A rejected resolve has no fresh native reader or completed CPU output.
  // Mark its potential destination, including partial/tiled aliases, instead
  // of sampling stale CPU bytes from it through a different fetch constant.
  unavailable_resolves_.Add(info.copy_dest_extent_start, info.copy_dest_extent_length);
  if (++resolve_destinations_ <= 8) REXGPU_INFO("BO3 native pending resolve: destination={:08X} bytes={} depth={} sourceBase={} sourceFormat={} destFormat={} samples={} select={} exponent={} endian={} rect={}x{}", info.copy_dest_extent_start, info.copy_dest_extent_length, info.IsCopyingDepth(), info.IsCopyingDepth() ? info.depth_original_base : info.color_original_base, info.IsCopyingDepth() ? uint32_t(info.depth_edram_info.format) : uint32_t(info.color_edram_info.format), uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.IsCopyingDepth() ? info.depth_edram_info.msaa_samples : info.color_edram_info.msaa_samples), uint32_t(info.rb_copy_control.copy_sample_select), info.copy_dest_info.copy_dest_exp_bias, uint32_t(info.copy_dest_info.copy_dest_endian), info.coordinate_info.width_div_8 * 8, info.height_div_8 * 8);
}

void NativeCommandProcessor::IssueDepthClear() {
  const auto& r = *register_file_;
  const auto control = r.Get<reg::RB_COPY_CONTROL>();
  if (!control.depth_clear_enable) return;
  const auto surface = r.Get<reg::RB_SURFACE_INFO>();
  const auto info = DecodeResolveInfo();
  const auto edram = info.depth_edram_info;
  if (!edram.pitch_tiles) throw Unsupported("depth_clear_pitch");
  // Recover the absolute pixel origin from the SDK's tile-relative rectangle.
  // The existing shared cache only supports surfaces with this exact base and
  // pitch; EDRAM overlap ownership transfers remain explicitly unsupported.
  const uint32_t tile_offset = (edram.base_tiles + xenos::kEdramTileCount - info.depth_original_base) % xenos::kEdramTileCount;
  const uint32_t sx = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sy = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint32_t x = ((tile_offset % edram.pitch_tiles) * xenos::kEdramTileWidthSamples >> sx) + info.coordinate_info.edram_offset_x_div_8 * 8;
  const uint32_t y = ((tile_offset / edram.pitch_tiles) * xenos::kEdramTileHeightSamples >> sy) + info.coordinate_info.edram_offset_y_div_8 * 8;
  const uint32_t width = info.coordinate_info.width_div_8 * 8, height = info.height_div_8 * 8;
  if (!width || !height || x + width > surface.surface_pitch || y + height > 2048) throw Unsupported("depth_clear_extent");
  const uint32_t packed = info.rb_depth_clear;
  const uint32_t depth_bits = packed >> 8;
  const float value = edram.format ? xenos::Float20e4To32(depth_bits) : xenos::UNorm24To32(depth_bits);
  // D32 currently stores the guest float24 value without the SDK's half-range
  // remapping. Values above 1 need that remapping and are not approximated here.
  if (!std::isfinite(value) || value < 0 || value > 1) throw Unsupported("depth_clear_value_range");
  DepthClear clear{{{int32_t(x), int32_t(y)}, {width, height}}, {value, packed & 255}, packed};
  clear.sequence = ++depth_clear_sequence_;
  const std::array<uint32_t, 4> key{info.depth_original_base, edram.format, surface.surface_pitch, uint32_t(surface.msaa_samples)};
  const bool fresh = !depth_clears_.contains(key);
  if (fresh && depth_clears_.size() >= 64) throw Unsupported("depth_clear_cache_limit");
  auto& history = depth_clears_[key];
  std::erase_if(history, [&](const DepthClear& old) {
    return clear.rectangle.offset.x <= old.rectangle.offset.x && clear.rectangle.offset.y <= old.rectangle.offset.y &&
        uint64_t(x) + width >= uint64_t(old.rectangle.offset.x) + old.rectangle.extent.width &&
        uint64_t(y) + height >= uint64_t(old.rectangle.offset.y) + old.rectangle.extent.height;
  });
  if (history.size() >= 64) throw Unsupported("depth_clear_history_limit");
  history.push_back(clear); ++depth_clear_requests_;
  for (auto& [depth_key, depth] : depths_) {
    if (depth_key[0] != key[0] || depth_key[1] != key[1] || depth_key[2] != key[2] || depth_key[4] != key[3]) continue;
    // Each shared image is cleared once, even when several targets attach it.
    for (auto& [target_key, target] : targets_) if (target->depth == depth.get()) { ApplyDepthClear(*target, clear); break; }
  }
  if (recording_) Submit();
  if (fresh || depth_clear_requests_ <= 8) REXGPU_INFO("BO3 native guest depth clear: base={} pitch={} format={} samples={} rect={},{} {}x{} packed={:08X} depth={} stencil={} requests={} applied={}", key[0], key[2], key[1], 1u << key[3], x, y, width, height, packed, value, packed & 255, depth_clear_requests_, depth_clear_applied_);
}

bool NativeCommandProcessor::IssueCopy() {
  if(recording_)Submit(); DrainResolveInvalidations(); ++copies_;
  if(failed_)return Reject("backend_disabled_after_failure");
  bool copied=false;
  draw_util::ResolveInfo info{};bool decoded=false;
  try {info=DecodeResolveInfo();decoded=true;TrackResolveDestination(info);copied=ResolveColor(info);}
  catch(const Unsupported& error){if(decoded)InvalidateResolvedRange(info.copy_dest_extent_start,info.copy_dest_extent_length);Reject(error.what());}
  catch(const ColorResolveError& error){if(decoded)InvalidateResolvedRange(info.copy_dest_extent_start,info.copy_dest_extent_length);Reject(error.what());}
  catch(const std::exception& error){failed_=true;REXGPU_ERROR("BO3 native resolve stopped: {}",error.what());}
  // The source must be copied before any requested post-resolve clear.
  if(!failed_) {
    try {if(decoded)ClearResolvedColor(info);}
    catch(const Unsupported& error){Reject(error.what());}
    catch(const std::exception& error){failed_=true;REXGPU_ERROR("BO3 native post-resolve color clear stopped: {}",error.what());}
    if(!failed_) {
      try {IssueDepthClear();}
      catch(const Unsupported& error){Reject(error.what());}
      catch(const std::exception& error){failed_=true;REXGPU_ERROR("BO3 native post-resolve depth clear stopped: {}",error.what());}
    }
  }
  return copied;
}

void NativeCommandProcessor::IssueSwap(uint32_t address, uint32_t width, uint32_t height) {
  if (recording_) Submit();
  ++swaps_;
  if (scene_view_diagnostic_) {
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    const bool down = foreground_pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000);
    if (down && !scene_key_down_) {
      scene_view_enabled_ = !scene_view_enabled_;
      REXGPU_INFO("BO3 NATIVE scene view {} at swap {}; current-frame scene available={}", scene_view_enabled_ ? "ON" : "OFF", swaps_, scene_ && scene_draw_swap_ + 1 == swaps_);
    }
    scene_key_down_ = down;
    const bool depth_down = foreground_pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (depth_down && !depth_key_down_) {
      depth_view_enabled_ = !depth_view_enabled_;
      REXGPU_INFO("BO3 NATIVE depth view {} at swap {}; current-frame scene available={} depthOnlySubmitted={}", depth_view_enabled_ ? "ON" : "OFF", swaps_, depth_scene_ && depth_scene_draw_swap_ + 1 == swaps_, depth_submitted_);
    }
    depth_key_down_ = depth_down;
  }
  const bool current_scene = scene_ && scene_draw_swap_ + 1 == swaps_;
  const bool current_depth = current_scene || (depth_scene_ && depth_scene_draw_swap_ + 1 == swaps_);
  const bool show_depth = depth_view_enabled_ && current_depth;
  auto* presentation_target = show_depth ? (current_scene ? scene_ : depth_scene_) : (scene_view_enabled_ && current_scene ? scene_ : latest_);
  if (swaps_ % 30 == 0) {
    double wall = Milliseconds(last_perf_);
    const double thread_ms = ThreadMilliseconds();
    REXGPU_INFO("BO3 NATIVE timing: {:.2f} swaps/s over {:.0f}ms; queue/fence={:.1f}ms textureWall={:.1f}ms exportWall={:.1f}ms shader={:.1f}ms pipeline={:.1f}ms submissions={} uploadMiB={:.2f} linearMiB={:.2f} tiledMiB={:.2f} exportSnapshotMiB={:.2f}", (swaps_ - last_perf_swaps_) * 1000.0 / std::max(wall, .001), wall, timings_.wait, timings_.texture, timings_.exports, timings_.shader, timings_.pipeline, timings_.submissions, timings_.upload_bytes / 1048576.0, timings_.linear_bytes / 1048576.0, timings_.tiled_bytes / 1048576.0, timings_.export_snapshot_bytes / 1048576.0);
    REXGPU_INFO("BO3 NATIVE export phases: plan={:.1f}ms guards={:.1f}ms snapshot={:.1f}ms writes={:.1f}ms exportThreadCPU={:.1f}ms commandThreadCPU={:.1f}ms", timings_.export_plan, timings_.export_guards, timings_.export_snapshot, timings_.export_write, timings_.export_thread, thread_ms >= 0 && last_thread_ms_ >= 0 ? thread_ms - last_thread_ms_ : -1.0);
    last_thread_ms_ = thread_ms;
    timings_ = {}; last_perf_ = PerfClock::now(); last_perf_swaps_ = swaps_;
  }
  if (swaps_ == 1 || swaps_ % 120 == 0) {
    REXGPU_INFO("BO3 NATIVE progress: swaps={} packets={} submitted={} depthOnlySubmitted={} pixelShaderBypassed={} copies={} exports={} targets={} depthSurfaces={} textures={} pipelines={} imageMiB={} frontbuffer={:08X}", swaps_, draws_, submitted_, depth_submitted_, pixel_shader_bypassed_, copies_, export_copies_, targets_.size(), depths_.size(), textures_.size(), pipelines_.size(), image_bytes_ / (1024 * 1024), address);
    REXGPU_INFO("BO3 NATIVE depth clears: requests={} applied={} histories={}", depth_clear_requests_, depth_clear_applied_, depth_clears_.size());
    REXGPU_INFO("BO3 NATIVE rectangles: submitted={} shaderVariants={}", rectangles_submitted_, rectangle_shaders_.size());
    REXGPU_INFO("BO3 NATIVE resolve ownership: destinations={} completed={} gpuReads={} surfaces={} unavailableCpuRanges={}", resolve_destinations_, resolve_completed_, resolved_reads_, resolved_surfaces_.size(), unavailable_resolves_.ranges().size());
    REXGPU_INFO("BO3 NATIVE fast clears: requests={} aliasApplications={} histories={}", fast_clear_requests_, fast_clear_aliases_, fast_depth_clears_.size());
    for (const auto& [reason, count] : unsupported_) REXGPU_INFO("BO3 NATIVE skipped {}: {}", reason, count);
    for (const auto& [key, target] : targets_) if (target->draws || target->color.samples != VK_SAMPLE_COUNT_1_BIT) {
      REXGPU_INFO("BO3 NATIVE target activity: base={} depth={} {}x{} format={} samples={} draws={} presented={} depthOnly={}", key[0], key[4], key[2], key[3], int(target->color.format), uint32_t(target->color.samples), target->draws, target.get() == presentation_target, target->depth_only);
      target->draws = 0;
    }
  }
  if (failed_ || !width || !height || width > 4096 || height > 2048 || !graphics_system_->presenter()) return;
  try {
    // Until resolves are implemented this is a diagnostic view of the most
    // recently drawn native target, not the resolved guest frontbuffer.
    if (!present_pass_) {
      VkAttachmentDescription attachment{}; attachment.format = VulkanPresenter::kGuestOutputFormat; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &reference;
      VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.attachmentCount = 1; pass.pAttachments = &attachment; pass.subpassCount = 1; pass.pSubpasses = &sub;
      Check(F().vkCreateRenderPass(D(), &pass, nullptr, &present_pass_), "presentation pass");
      VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl.bindingCount = 1; sl.pBindings = &binding;
      Check(F().vkCreateDescriptorSetLayout(D(), &sl, nullptr, &present_.sets[0]), "present texture layout");
      VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = present_.sets;
      Check(F().vkCreatePipelineLayout(D(), &pl, nullptr, &present_.layout), "present layout");
      VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = sizeof(kPresentVS); module.pCode = kPresentVS;
      Check(F().vkCreateShaderModule(D(), &module, nullptr, &present_vs_), "present VS"); module.codeSize = sizeof(kPresentPS); module.pCode = kPresentPS;
      Check(F().vkCreateShaderModule(D(), &module, nullptr, &present_ps_), "present PS");
      VkPipelineShaderStageCreateInfo stages[2]{};
      stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = present_vs_; stages[0].pName = "VS";
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = present_ps_; stages[1].pName = "PS";
      VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; input.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; bs.attachmentCount = 1; bs.pAttachments = &ba;
      VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamics;
      VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &input; info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &ms; info.pColorBlendState = &bs; info.pDynamicState = &dynamic; info.layout = present_.layout; info.renderPass = present_pass_;
      Check(F().vkCreateGraphicsPipelines(D(), VK_NULL_HANDLE, 1, &info, nullptr, &present_.pipeline), "presentation pipeline");
      binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      Check(F().vkCreateDescriptorSetLayout(D(), &sl, nullptr, &present_depth_.sets[0]), "depth texture layout");
      pl.pSetLayouts = present_depth_.sets;
      Check(F().vkCreatePipelineLayout(D(), &pl, nullptr, &present_depth_.layout), "depth present layout");
      module.codeSize = sizeof(kPresentDepthPS); module.pCode = kPresentDepthPS;
      Check(F().vkCreateShaderModule(D(), &module, nullptr, &present_depth_ps_), "depth diagnostic shader");
      stages[1].module = present_depth_ps_; stages[1].pName = "DepthPS";
      info.layout = present_depth_.layout;
      Check(F().vkCreateGraphicsPipelines(D(), VK_NULL_HANDLE, 1, &info, nullptr, &present_depth_.pipeline), "depth diagnostic pipeline");
      VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      Check(F().vkCreateSampler(D(), &sampler, nullptr, &present_sampler_), "present sampler");
      REXGPU_INFO("BO3 NATIVE diagnostic presentation ready; guest resolve parity is pending");
    }
    auto* presenter = graphics_system_->presenter();
    presenter->RefreshGuestOutput(width, height, width, height, [&](rex::ui::Presenter::GuestOutputRefreshContext& base) {
      auto& context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
      VkFramebuffer framebuffer{};
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass = present_pass_; fb.attachmentCount = 1; fb.pAttachments = &view; fb.width = width; fb.height = height; fb.layers = 1;
      Check(F().vkCreateFramebuffer(D(), &fb, nullptr, &framebuffer), "present framebuffer");
      try {
        Begin();
        Image* source_image = presentation_target ? &presentation_target->color : nullptr;
        if (show_depth) {
          Transition(presentation_target->depth->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
          source_image = nullptr;
        } else if (presentation_target && presentation_target->color.samples != VK_SAMPLE_COUNT_1_BIT) {
          Transition(presentation_target->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          Transition(presentation_target->display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          VkImageResolve resolve{}; resolve.srcSubresource = resolve.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          resolve.extent = {presentation_target->color.width, presentation_target->color.height, 1};
          resolve_image_(command_, presentation_target->color.image, presentation_target->color.layout, presentation_target->display.image, presentation_target->display.layout, 1, &resolve);
          pending_work_ = true;
          source_image = &presentation_target->display;
        }
        if (source_image) Transition(*source_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorSet set{};
        auto& display_pipeline = show_depth ? present_depth_ : present_;
        if (presentation_target) {
          VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptors_; allocation.descriptorSetCount = 1; allocation.pSetLayouts = display_pipeline.sets;
          Check(F().vkAllocateDescriptorSets(D(), &allocation, &set), "present descriptors");
          VkDescriptorImageInfo source{show_depth ? VK_NULL_HANDLE : present_sampler_, show_depth ? presentation_target->depth->sampled : source_image->view, show_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1; write.descriptorType = show_depth ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &source;
          F().vkUpdateDescriptorSets(D(), 1, &write, 0, nullptr);
        }
        Image output; output.image = context.image(); output.layout = context.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkClearValue clear{}; clear.color.float32[2] = .04f; clear.color.float32[3] = 1;
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = present_pass_; render.framebuffer = framebuffer; render.renderArea.extent = {width, height}; render.clearValueCount = 1; render.pClearValues = &clear;
        F().vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
        if (presentation_target) {
          VkViewport viewport{0, 0, float(width), float(height), 0, 1}; VkRect2D scissor{{0, 0}, {width, height}};
          F().vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, display_pipeline.pipeline); F().vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, display_pipeline.layout, 0, 1, &set, 0, nullptr);
          F().vkCmdSetViewport(command_, 0, 1, &viewport); F().vkCmdSetScissor(command_, 0, 1, &scissor); F().vkCmdDraw(command_, 3, 1, 0, 0);
        }
        F().vkCmdEndRenderPass(command_); pending_work_ = true; Transition(output, VulkanPresenter::kGuestOutputInternalLayout); Submit();
      } catch (...) { F().vkDestroyFramebuffer(D(), framebuffer, nullptr); throw; }
      F().vkDestroyFramebuffer(D(), framebuffer, nullptr); return true;
    });
  } catch (const std::exception& error) { failed_ = true; REXGPU_ERROR("BO3 native presentation stopped: {}", error.what()); }
}

void NativeCommandProcessor::ShutdownContext() {
  if (invalidation_handle_) { memory_->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_); invalidation_handle_ = nullptr; }
  if (!gpu_) return;
  if (recording_) Submit();
  REXGPU_INFO("BO3 NATIVE final: packets={} submitted={} depthOnlySubmitted={} swaps={} copies={}", draws_, submitted_, depth_submitted_, swaps_, copies_);
  for (const auto& [reason, count] : unsupported_) REXGPU_INFO("BO3 NATIVE final skipped {}: {}", reason, count);
  for(auto& [key,surface]:resolved_surfaces_) {F().vkDestroyFramebuffer(D(),surface->framebuffer,nullptr);DestroyImage(surface->image);} resolved_surfaces_.clear();
  for(auto& [key,pipeline]:resolve_pipelines_) {
    auto& p=pipeline->gpu;F().vkDestroyPipeline(D(),p.pipeline,nullptr);F().vkDestroyPipelineLayout(D(),p.layout,nullptr);F().vkDestroyDescriptorSetLayout(D(),p.sets[0],nullptr);F().vkDestroyRenderPass(D(),pipeline->pass,nullptr);
  } resolve_pipelines_.clear();
  for(auto module:{resolve_vs_,resolve_ps_,resolve_ms_ps_}) if(module)F().vkDestroyShaderModule(D(),module,nullptr);
  // Every submission uses a completion fence before resource reuse/destruction.
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline->pipeline) F().vkDestroyPipeline(D(), pipeline->pipeline, nullptr);
    if (pipeline->layout) F().vkDestroyPipelineLayout(D(), pipeline->layout, nullptr);
    for (auto set : pipeline->sets) if (set) F().vkDestroyDescriptorSetLayout(D(), set, nullptr);
  }
  pipelines_.clear();
  for (auto& [mask, module] : rectangle_shaders_) F().vkDestroyShaderModule(D(), module, nullptr);
  rectangle_shaders_.clear();
  for (auto& texture : null_textures_) if (texture) { F().vkDestroySampler(D(), texture->sampler, nullptr); DestroyImage(texture->image); texture.reset(); }
  for (auto& [key, texture] : textures_) if (texture) { if (texture->sampler) F().vkDestroySampler(D(), texture->sampler, nullptr); DestroyImage(texture->image); }
  textures_.clear(); texture_plans_.clear();
  for (auto& [key, target] : targets_) { F().vkDestroyFramebuffer(D(), target->framebuffer, nullptr); F().vkDestroyRenderPass(D(), target->pass, nullptr); DestroyImage(target->color); DestroyImage(target->display); }
  targets_.clear();
  for (auto& [key, depth] : depths_) { if (depth->sampled) F().vkDestroyImageView(D(), depth->sampled, nullptr); DestroyImage(depth->image); }
  depths_.clear();
  depth_clears_.clear();
  fast_depth_clears_.clear();
  for (auto& shader : shaders_) if (shader->module) F().vkDestroyShaderModule(D(), shader->module, nullptr);
  shaders_.clear();
  if (present_.pipeline) F().vkDestroyPipeline(D(), present_.pipeline, nullptr);
  if (present_.layout) F().vkDestroyPipelineLayout(D(), present_.layout, nullptr);
  if (present_.sets[0]) F().vkDestroyDescriptorSetLayout(D(), present_.sets[0], nullptr);
  if (present_depth_.pipeline) F().vkDestroyPipeline(D(), present_depth_.pipeline, nullptr);
  if (present_depth_.layout) F().vkDestroyPipelineLayout(D(), present_depth_.layout, nullptr);
  if (present_depth_.sets[0]) F().vkDestroyDescriptorSetLayout(D(), present_depth_.sets[0], nullptr);
  if (present_depth_ps_) F().vkDestroyShaderModule(D(), present_depth_ps_, nullptr);
  if (present_pass_) F().vkDestroyRenderPass(D(), present_pass_, nullptr);
  if (present_vs_) F().vkDestroyShaderModule(D(), present_vs_, nullptr);
  if (present_ps_) F().vkDestroyShaderModule(D(), present_ps_, nullptr);
  if (present_sampler_) F().vkDestroySampler(D(), present_sampler_, nullptr);
  if (mapped_) F().vkUnmapMemory(D(), arena_memory_);
  if (arena_) F().vkDestroyBuffer(D(), arena_, nullptr);
  if (arena_memory_) F().vkFreeMemory(D(), arena_memory_, nullptr);
  if (descriptors_) F().vkDestroyDescriptorPool(D(), descriptors_, nullptr);
  if (coverage_queries_) F().vkDestroyQueryPool(D(), coverage_queries_, nullptr);
  if (fence_) F().vkDestroyFence(D(), fence_, nullptr);
  if (command_pool_) F().vkDestroyCommandPool(D(), command_pool_, nullptr);
  gpu_ = nullptr;
}
}  // namespace bo3::native

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version() { return rex::system::kGpuPluginAbiVersion; }
extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi, const rex::system::GpuCreateInfo* info) {
  if (abi != rex::system::kGpuPluginAbiVersion || !info || info->struct_size != sizeof(*info)) return nullptr;
  if (info->backend && std::strcmp(info->backend, "any") && std::strcmp(info->backend, "vulkan")) return nullptr;
  return new bo3::native::NativeGraphicsSystem();
}
