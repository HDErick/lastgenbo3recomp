// SPDX-License-Identifier: GPL-3.0-only
// BO3 PC native Vulkan prototype. The MW2-derived translator has GPL-3.0-only
// provenance; SDK command parsing and host presentation retain their BSD license.
#include "shader_translator.h"
#include "present_spirv.h"
#include "memexport_copy.h"
#include "bc_decode.h"
#include "physical_access.h"
#include "sha256.h"
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/presenter.h>
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
  uint32_t width = 0, height = 0;
  uint64_t allocation = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Target { Image color, depth; VkRenderPass pass{}; VkFramebuffer framebuffer{}; };
struct Pipeline {
  VkDescriptorSetLayout sets[3]{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  std::vector<VkDescriptorSetLayoutBinding> textures;
};
struct Texture { Image image; VkSampler sampler{}; uint64_t content = 0; };

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
  VkCommandPool command_pool_{};
  VkCommandBuffer command_{};
  VkFence fence_{};
  VkDescriptorPool descriptors_{};
  VkBuffer arena_{};
  VkDeviceMemory arena_memory_{};
  uint8_t* mapped_ = nullptr;
  uint32_t cursor_ = 16384;
  std::vector<std::unique_ptr<NativeShader>> shaders_;
  std::map<std::array<uint32_t, 6>, std::unique_ptr<Target>> targets_;
  std::map<std::vector<uint64_t>, std::unique_ptr<Pipeline>> pipelines_;
  std::map<std::array<uint32_t, 6>, std::unique_ptr<Texture>> textures_;
  std::map<uint32_t, Target*> resolved_;
  Target* latest_ = nullptr;
  std::map<std::string, uint64_t> unsupported_;
  uint64_t draws_ = 0, submitted_ = 0, swaps_ = 0, copies_ = 0;
  uint64_t image_bytes_ = 0;
  bool failed_ = false;
  bool no_cull_diagnostic_ = false;
  bool readback_diagnostic_ = false, recording_ = false;
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
  VkShaderModule present_vs_{}, present_ps_{};
  VkSampler present_sampler_{};
  const VulkanDevice::Functions& F() const { return gpu_->functions(); }
  VkDevice D() const { return gpu_->device(); }
  uint32_t MemoryType(uint32_t allowed, uint32_t preferred);
  void Begin();
  void Submit();
  void Transition(Image& image, VkImageLayout layout, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
  Image CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkComponentMapping mapping = {});
  void DestroyImage(Image& image);
  uint32_t Upload(const void* source, uint32_t bytes, uint32_t align = 16);
  const uint8_t* Guest(uint32_t address, uint32_t size);
  bool Reject(const std::string& reason);
  Target* GetTarget();
  Texture* GetTexture(uint32_t slot);
  Pipeline* GetPipeline(NativeShader& vs, NativeShader& ps, Target& target, VkPrimitiveTopology topology);
};
class NativeGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "BO3 native Vulkan prototype"; }
 protected:
  void CreateProvider(bool presentation) override { provider_ = VulkanProvider::Create(false, presentation); }
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
    readback_diagnostic_ = std::getenv("BO3_NATIVE_READBACK") != nullptr;
    if (no_cull_diagnostic_) REXGPU_WARN("BO3 NATIVE diagnostic: culling disabled for comparison");
    clear_depth_ = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(gpu_->vulkan_instance()->functions().vkGetDeviceProcAddr(gpu_->device(), "vkCmdClearDepthStencilImage"));
    if (!clear_depth_) throw std::runtime_error("Missing core Vulkan depth-clear function");
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu_->queue_family_graphics_compute();
    Check(F().vkCreateCommandPool(D(), &pool, nullptr, &command_pool_), "command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = command_pool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    Check(F().vkAllocateCommandBuffers(D(), &allocation, &command_), "command buffer");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(F().vkCreateFence(D(), &fence, nullptr, &fence_), "fence");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5 * kBatchDraws},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBatchDraws}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 * kBatchDraws}};
    VkDescriptorPoolCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    desc.maxSets = 3 * kBatchDraws; desc.poolSizeCount = 3; desc.pPoolSizes = sizes;
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
    REXGPU_INFO("BO3 NATIVE backend active: own Vulkan targets/pipelines/submissions, 16-MiB upload arena; no Xenos draw backend. Device={}", gpu_->properties().deviceName);
    return true;
  } catch (const std::exception& error) { REXGPU_ERROR("BO3 native setup failed: {}", error.what()); return false; }
}
void NativeCommandProcessor::Begin() {
  if (recording_) throw std::runtime_error("Native command buffer already recording");
  // The previous Submit retired its fence. Reset uploads/descriptors only at
  // batch boundaries, never while recorded draws still reference them.
  cursor_ = 16384; batch_draws_ = 0;
  Check(F().vkResetDescriptorPool(D(), descriptors_, 0), "reset batch descriptors");
  Check(F().vkResetCommandPool(D(), command_pool_, 0), "reset command pool");
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(F().vkBeginCommandBuffer(command_, &info), "begin command");
  recording_ = true;
}
void NativeCommandProcessor::Submit() {
  TimedScope time(timings_.wait);
  if (!recording_) throw std::runtime_error("Native command buffer is not recording");
  Check(F().vkEndCommandBuffer(command_), "end command");
  recording_ = false;
  Check(F().vkResetFences(D(), 1, &fence_), "reset fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
  { auto queue = gpu_->AcquireQueue(gpu_->queue_family_graphics_compute(), 0);
    Check(F().vkQueueSubmit(queue.queue(), 1, &submit, fence_), "queue submit"); }
  Check(F().vkWaitForFences(D(), 1, &fence_, VK_TRUE, UINT64_MAX), "native completion fence");
  ++timings_.submissions;
}
Image NativeCommandProcessor::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                         VkImageAspectFlags aspect, VkComponentMapping mapping) {
  Image result; result.width = width; result.height = height; result.format = format;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
  info.extent = {width, height, 1}; info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
  VkFormatProperties properties{};
  gpu_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(gpu_->physical_device(), format, &properties);
  VkFormatFeatureFlags required = 0;
  if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if ((properties.optimalTilingFeatures & required) != required) throw Unsupported("host_image_format");
  Check(F().vkCreateImage(D(), &info, nullptr, &result.image), "image");
  try {
  VkMemoryRequirements requirements{}; F().vkGetImageMemoryRequirements(D(), result.image, &requirements);
  VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.allocationSize = requirements.size;
  if (image_bytes_ + requirements.size > 512ull * 1024 * 1024) throw Unsupported("image_memory_budget");
  memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, gpu_->memory_types().device_local);
  Check(F().vkAllocateMemory(D(), &memory, nullptr, &result.memory), "image memory");
  result.allocation = requirements.size; image_bytes_ += result.allocation;
  Check(F().vkBindImageMemory(D(), result.image, result.memory, 0), "bind image");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format; view.components = mapping; view.subresourceRange = {aspect, 0, 1, 0, 1};
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
  barrier.image = image.image; barrier.subresourceRange = {aspect, 0, 1, 0, 1};
  F().vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
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

Target* NativeCommandProcessor::GetTarget() {
  const auto& r = *register_file_;
  const auto surface = r.Get<reg::RB_SURFACE_INFO>();
  const auto color = r.Get<reg::RB_COLOR_INFO>();
  const auto depth = r.Get<reg::RB_DEPTH_INFO>();
  if (uint32_t(surface.msaa_samples)) throw Unsupported("multisample_target");
  uint32_t width = surface.surface_pitch;
  float yscale = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  float yoffset = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  uint32_t height = 0;
  if (std::isfinite(yscale) && std::isfinite(yoffset) && std::abs(yscale) > 0)
    height = uint32_t(std::ceil(std::max(yoffset + std::abs(yscale), 0.0f)));
  if (!height || height > 2048) height = (r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] >> 16) & 0x3fff;
  if (!width || width > 4096 || !height || height > 2048) throw Unsupported("target_extent");
  std::array<uint32_t, 6> key{color.color_base, uint32_t(color.color_format), width, height, depth.depth_base, uint32_t(depth.depth_format)};
  auto found = targets_.find(key);
  if (found != targets_.end()) return found->second.get();
  if (targets_.size() >= 64) throw Unsupported("target_cache_limit");
  auto result = std::make_unique<Target>();
  try {
    result->color = CreateImage(width, height, ColorFormat(uint32_t(color.color_format)),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    // Float24 has no Vulkan image format. Preserve its floating depth semantics
    // in D32; exact Xenos quantization and packed clear/readback remain pending.
    const auto depth_format = uint32_t(depth.depth_format) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    result->depth = CreateImage(width, height, depth_format,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = result->color.format; attachments[1].format = result->depth.format;
    for (auto& a : attachments) { a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE; a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE; }
    attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &cr; sub.pDepthStencilAttachment = &dr;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.attachmentCount = 2; pass.pAttachments = attachments; pass.subpassCount = 1; pass.pSubpasses = &sub;
    Check(F().vkCreateRenderPass(D(), &pass, nullptr, &result->pass), "target pass");
    VkImageView views[]{result->color.view, result->depth.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass = result->pass; fb.attachmentCount = 2; fb.pAttachments = views; fb.width = width; fb.height = height; fb.layers = 1;
    Check(F().vkCreateFramebuffer(D(), &fb, nullptr, &result->framebuffer), "target framebuffer");
    if (recording_) Submit();
    Begin();
    Transition(result->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero{}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    F().vkCmdClearColorImage(command_, result->color.image, result->color.layout, &zero, 1, &range);
    Transition(result->depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkClearDepthStencilValue one{1, 0}; clear_depth_(command_, result->depth.image, result->depth.layout, &one, 1, &range);
    Submit();
  } catch (...) {
    if (result->framebuffer) F().vkDestroyFramebuffer(D(), result->framebuffer, nullptr);
    if (result->pass) F().vkDestroyRenderPass(D(), result->pass, nullptr);
    DestroyImage(result->color); DestroyImage(result->depth); throw;
  }
  auto* pointer = result.get(); targets_.emplace(key, std::move(result));
  REXGPU_INFO("BO3 native target {}x{} format {} color base {} depth base {}", width, height, int(pointer->color.format), color.color_base, depth.depth_base);
  if (uint32_t(depth.depth_format)) REXGPU_INFO("BO3 native float24 depth uses D32 storage; packed quantization/clear parity unverified");
  return pointer;
}

Texture* NativeCommandProcessor::GetTexture(uint32_t slot) {
  TimedScope time(timings_.texture);
  const auto fetch = register_file_->GetTextureFetch(slot);
  if (uint32_t(fetch.type) != 2 || fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked) throw Unsupported("texture_dimension_or_type");
  if (uint32_t(fetch.sign_x) || uint32_t(fetch.sign_y) || uint32_t(fetch.sign_z) || uint32_t(fetch.sign_w) || fetch.num_format) throw Unsupported("texture_numeric_conversion");
  if (fetch.mip_min_level || fetch.mip_max_level) throw Unsupported("texture_mips");
  uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
  if (width > 4096 || height > 4096) throw Unsupported("texture_extent");
  uint32_t bytes = 0, bc = 0; VkFormat format{};
  switch (uint32_t(fetch.format)) {
    case 2: case 8: case 9: bytes = 1; format = VK_FORMAT_R8_UNORM; break;
    case 10: bytes = 2; format = VK_FORMAT_R8G8_UNORM; break;
    case 6: case 14: bytes = 4; format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case 30: bytes = 2; format = VK_FORMAT_R16_SFLOAT; break;
    case 31: bytes = 4; format = VK_FORMAT_R16G16_SFLOAT; break;
    case 32: bytes = 8; format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    case 18: bytes = 8; bc = 1; format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case 19: bytes = 16; bc = 2; format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case 20: bytes = 16; bc = 3; format = VK_FORMAT_R8G8B8A8_UNORM; break;
    default: throw Unsupported(fmt::format("texture_format_{}", uint32_t(fetch.format)));
  }
  uint32_t xoff = 0, yoff = 0, zoff = 0;
  if (fetch.packed_mips) texture_util::GetPackedMipOffset(width, height, 1, fetch.format, 0, xoff, yoff, zoff);
  const auto layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, 1, fetch.tiled, fetch.format, fetch.packed_mips, true, 0);
  const uint32_t source_size = layout.base.array_slice_data_extent_bytes;
  if (!source_size || source_size > kArenaBytes) throw Unsupported("texture_upload_limit");
  const auto* source = Guest(fetch.base_address << 12, source_size);
  // Never sample a stale CPU shadow for native-produced guest data.
  if (resolved_.contains(fetch.base_address << 12)) throw Unsupported("resolved_texture_view");
  uint64_t hash = XXH3_64bits(source, source_size);
  std::array<uint32_t, 6> key{}; std::memcpy(key.data(), &fetch, sizeof(fetch));
  auto& cached = textures_[key];
  if (cached && cached->content == hash) return cached.get();
  if (!cached && textures_.size() > 256) { textures_.erase(key); throw Unsupported("texture_cache_limit"); }
  std::vector<uint8_t> pixels(uint64_t(width) * height * (bc ? 4 : bytes));
  auto swap_byte = [&](uint32_t offset) {
    switch (uint32_t(fetch.endianness)) { case 1: return offset ^ 1u; case 2: return offset ^ 3u; case 3: return offset ^ 2u; default: return offset; }
  };
  uint32_t pitch = fetch.pitch * 32;
  const uint32_t row_bytes = width * bytes;
  if (bc) {
    for (uint32_t y = 0; y < (height + 3) / 4; ++y) for (uint32_t x = 0; x < (width + 3) / 4; ++x) {
      uint32_t offset = fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(x + xoff, y + yoff, pitch / 4, std::countr_zero(bytes))) : (y + yoff) * layout.base.row_pitch_bytes + (x + xoff) * bytes;
      uint8_t block[16]{}, rgba[64];
      for (uint32_t b = 0; b < bytes; ++b) { uint32_t at = swap_byte(offset + b); if (at >= source_size) throw Unsupported("bc_source_bounds"); block[b] = source[at]; }
      DecodeBC(block, bc, rgba);
      for (uint32_t row = 0; row < 4 && y * 4 + row < height; ++row) {
        uint32_t columns = std::min(4u, width - x * 4);
        std::memcpy(pixels.data() + (uint64_t(y * 4 + row) * width + x * 4) * 4, rgba + row * 16, columns * 4);
      }
    }
    timings_.tiled_bytes += pixels.size();
  } else if (!fetch.tiled && !(xoff * bytes & 3) && !(row_bytes & 3)) {
    // Common video planes are linear, with complete dwords per row. Preserve
    // the same endian transform while avoiding per-pixel address calculation.
    for (uint32_t y = 0; y < height; ++y) {
      uint64_t begin = uint64_t(y + yoff) * layout.base.row_pitch_bytes + xoff * bytes;
      if (begin + row_bytes > source_size) throw Unsupported("texture_source_bounds");
      uint8_t* row = pixels.data() + uint64_t(y) * row_bytes;
      if (!uint32_t(fetch.endianness)) std::memcpy(row, source + begin, row_bytes);
      else for (uint32_t at = 0; at < row_bytes; at += 4) {
        uint32_t word; std::memcpy(&word, source + begin + at, 4);
        word = EndianWord(word, uint32_t(fetch.endianness)); std::memcpy(row + at, &word, 4);
      }
    }
    timings_.linear_bytes += pixels.size();
  } else {
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const uint32_t offset = fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(x + xoff, y + yoff, pitch, std::countr_zero(bytes))) : (y + yoff) * layout.base.row_pitch_bytes + (x + xoff) * bytes;
    for (uint32_t b = 0; b < bytes; ++b) { uint32_t at = swap_byte(offset + b); if (at >= source_size) throw Unsupported("texture_source_bounds"); pixels[(uint64_t(y) * width + x) * bytes + b] = source[at]; }
  }
  timings_.tiled_bytes += pixels.size();
  }
  if (!cached) {
    auto result = std::make_unique<Texture>();
    VkComponentMapping mapping{};
    VkComponentSwizzle* channels[]{&mapping.r, &mapping.g, &mapping.b, &mapping.a};
    const VkComponentSwizzle swizzles[]{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    for (uint32_t i = 0; i < 4; ++i) { auto s = (fetch.swizzle >> (3 * i)) & 7; if (s >= 6) throw Unsupported("texture_swizzle"); *channels[i] = swizzles[s]; }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (uint32_t(fetch.mag_filter) > 1 || uint32_t(fetch.min_filter) > 1) throw Unsupported("texture_filter");
    sampler.magFilter = fetch.mag_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.minFilter = fetch.min_filter == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    auto clamp = [](uint32_t c) { switch (c) { case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; default: throw Unsupported("texture_clamp"); } };
    sampler.addressModeU = clamp(uint32_t(fetch.clamp_x)); sampler.addressModeV = clamp(uint32_t(fetch.clamp_y)); sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0;
    Check(F().vkCreateSampler(D(), &sampler, nullptr, &result->sampler), "texture sampler");
    try { result->image = CreateImage(width, height, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mapping); }
    catch (...) { F().vkDestroySampler(D(), result->sampler, nullptr); throw; }
    cached = std::move(result);
  }
  uint32_t saved = cursor_; uint32_t offset = Upload(pixels.data(), uint32_t(pixels.size()), 16);
  const bool standalone = !recording_;
  if (standalone) Begin();
  Transition(cached->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy copy{}; copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
  F().vkCmdCopyBufferToImage(command_, arena_, cached->image.image, cached->image.layout, 1, &copy);
  Transition(cached->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // During a draw, retain all upload ranges until its one completion fence.
  if (standalone) { Submit(); cursor_ = saved; }
  timings_.upload_bytes += pixels.size();
  cached->content = hash; return cached.get();
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

Pipeline* NativeCommandProcessor::GetPipeline(NativeShader& vs, NativeShader& ps, Target& target, VkPrimitiveTopology topology) {
  const auto& r = *register_file_;
  const auto depth = r.Get<reg::RB_DEPTHCONTROL>();
  const auto raster = r.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (depth.stencil_enable) throw Unsupported("stencil_state");
  if (raster.poly_offset_front_enable || raster.poly_offset_back_enable || raster.poly_offset_para_enable || uint32_t(raster.poly_mode) == 1) throw Unsupported("polygon_mode_or_bias");
  if (ps.native.colourMask & ~1u) throw Unsupported("shader_mrt");
  std::vector<uint64_t> key{uint64_t(&vs), uint64_t(&ps), uint64_t(target.color.format), uint64_t(target.depth.format), uint64_t(topology), depth.value, raster.value, r[XE_GPU_REG_RB_BLENDCONTROL0], r[XE_GPU_REG_RB_COLOR_MASK] & 15};
  auto found = pipelines_.find(key); if (found != pipelines_.end()) return found->second.get();
  if (pipelines_.size() >= 1024) throw Unsupported("pipeline_cache_limit");
  TimedScope time(timings_.pipeline);
  auto result = std::make_unique<Pipeline>();
  try {
    VkDescriptorSetLayoutBinding uniforms[5]{};
    for (uint32_t i = 0; i < 5; ++i) uniforms[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = 5; layout.pBindings = uniforms;
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[0]), "uniform layout");
    for (uint32_t slot = 0; slot < 32; ++slot) {
      uint32_t mask = vs.native.textureViewMasks[slot] | ps.native.textureViewMasks[slot];
      if (mask & ~1u) throw Unsupported("typed_cube_or_volume_texture");
      if (mask) result->textures.push_back({slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    layout.bindingCount = uint32_t(result->textures.size()); layout.pBindings = result->textures.data();
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[1]), "texture layout");
    VkDescriptorSetLayoutBinding storage{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    layout.bindingCount = 1; layout.pBindings = &storage;
    Check(F().vkCreateDescriptorSetLayout(D(), &layout, nullptr, &result->sets[2]), "arena layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 3; pl.pSetLayouts = result->sets; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    Check(F().vkCreatePipelineLayout(D(), &pl, nullptr, &result->layout), "pipeline layout");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs.module; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps.module;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; input.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
    rs.cullMode = (raster.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (raster.cull_back ? VK_CULL_MODE_BACK_BIT : 0); rs.frontFace = raster.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (no_cull_diagnostic_) rs.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = depth.z_enable; ds.depthWriteEnable = depth.z_write_enable; ds.depthCompareOp = VkCompareOp(depth.zfunc);
    const auto blend = r.Get<reg::RB_BLENDCONTROL>();
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = r[XE_GPU_REG_RB_COLOR_MASK] & 15;
    ba.blendEnable = true; ba.srcColorBlendFactor = BlendFactor(uint32_t(blend.color_srcblend)); ba.dstColorBlendFactor = BlendFactor(uint32_t(blend.color_destblend)); ba.colorBlendOp = BlendOp(uint32_t(blend.color_comb_fcn));
    ba.srcAlphaBlendFactor = BlendFactor(uint32_t(blend.alpha_srcblend)); ba.dstAlphaBlendFactor = BlendFactor(uint32_t(blend.alpha_destblend)); ba.alphaBlendOp = BlendOp(uint32_t(blend.alpha_comb_fcn));
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; bs.attachmentCount = 1; bs.pAttachments = &ba;
    VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 3; dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &vertex; info.pInputAssemblyState = &input; info.pViewportState = &viewport; info.pRasterizationState = &rs; info.pMultisampleState = &ms; info.pDepthStencilState = &ds; info.pColorBlendState = &bs; info.pDynamicState = &dynamic; info.layout = result->layout; info.renderPass = target.pass;
    Check(F().vkCreateGraphicsPipelines(D(), VK_NULL_HANDLE, 1, &info, nullptr, &result->pipeline), "native pipeline");
  } catch (...) {
    if (result->pipeline) F().vkDestroyPipeline(D(), result->pipeline, nullptr);
    if (result->layout) F().vkDestroyPipelineLayout(D(), result->layout, nullptr);
    for (auto s : result->sets) if (s) F().vkDestroyDescriptorSetLayout(D(), s, nullptr);
    throw;
  }
  auto* pointer = result.get(); pipelines_.emplace(std::move(key), std::move(result)); return pointer;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType primitive, uint32_t count, IndexBufferInfo* indices, bool major) {
  ++draws_;
  if (failed_) return Reject("backend_disabled_after_failure");
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) return IssueCopy();
  bool touched_batch = false;
  try {
    if (!count) return true;
    if (count > 1000000) throw Unsupported("draw_count");
    if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) throw Unsupported("edram_draw_mode");
    auto* vs = static_cast<NativeShader*>(active_vertex_shader_);
    auto* ps = static_cast<NativeShader*>(active_pixel_shader_);
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
        for (auto range : plan.ranges) { std::memcpy(destination + range.destination, export_snapshot_.data() + range.source, range.bytes); bytes += range.bytes; }
      }
      if (++export_copies_ <= 5) REXGPU_INFO("BO3 native verified memexport copy {}: sha={} invocations={} bytes={} ranges={}", export_copies_, vs->canonical_sha256, count, bytes, plan.ranges.size());
      return true;
    }
    if (!vs || !ps || !vs->module || !ps->module) throw Unsupported("missing_or_unsupported_shader");
    VkPrimitiveTopology topology{};
    switch (primitive) {
      case xenos::PrimitiveType::kTriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
      case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
      case xenos::PrimitiveType::kTriangleFan: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
      default: throw Unsupported(fmt::format("primitive_{}", uint32_t(primitive)));
    }
    const auto& r = *register_file_;
    if (r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) throw Unsupported("primitive_restart");
    if (uint32_t(r.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode) == 7) throw Unsupported("memory_export_draw");
    if (r[XE_GPU_REG_RB_COLOR_MASK] & 0xfff0) throw Unsupported("mrt_write_mask");
    auto* target = GetTarget();
    auto* pipeline = GetPipeline(*vs, *ps, *target, topology);
    uint32_t base = r[XE_GPU_REG_VGT_INDX_OFFSET] & 0xffffff;
    uint32_t highest = count - 1;
    std::vector<uint32_t> host_indices;
    uint32_t index_offset = 0;
    if (indices) {
      uint32_t size = indices->format == xenos::IndexFormat::kInt16 ? 2 : 4;
      if (uint64_t(count) * size > indices->length) throw Unsupported("index_buffer_length");
      const uint32_t bytes = (count * size + 3) & ~3u;
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
    // Conservative reservation: all textures may change, and eight bytes per
    // texel covers every supported format (BC textures decode to four). A
    // single draw is still checked by Upload; this only flushes earlier draws
    // before their storage would prevent an otherwise supported draw fitting.
    uint64_t budget = (indices ? uint64_t(count) * 4 + 4 : 0) + 9888 + uint64_t(uniform_align) * 5;
    for (uint32_t slot = 0; slot < 96; ++slot) if (vs->native.vertexFetchSubs[slot % 3] & (1u << (slot / 3)))
      budget += (uint64_t(highest) * vs->native.vertexFetchStride[slot] + vs->native.vertexFetchTail[slot]) * 4 + 4;
    for (const auto& binding : pipeline->textures) {
      const auto tf = r.GetTextureFetch(binding.binding);
      budget += uint64_t(tf.size_2d.width + 1) * (tf.size_2d.height + 1) * 8 + 16;
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
      auto* texture = GetTexture(binding.binding);
      VkDescriptorImageInfo image{texture->sampler, texture->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = sets[1]; write.dstBinding = binding.binding; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
      F().vkUpdateDescriptorSets(D(), 1, &write, 0, nullptr);
    }
    float sx = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE), ox = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
    float sy = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE), oy = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    float sz = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE), oz = r.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
    const auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
    VkViewport viewport{ox - sx, oy - sy, sx * 2, sy * 2, std::clamp(oz, 0.f, 1.f), std::clamp(oz + sz, 0.f, 1.f)};
    struct Push { uint32_t alpha; float ref, scale; uint32_t gamma; float ndc[4]; } push{7, 0, 1, 0, {1, 1, 0, 0}};
    const auto cc = r.Get<reg::RB_COLORCONTROL>(); push.alpha = cc.alpha_test_enable ? uint32_t(cc.alpha_func) : 7; push.ref = r.Get<float>(XE_GPU_REG_RB_ALPHA_REF); push.scale = std::ldexp(1.f, r.Get<reg::RB_COLOR_INFO>().color_exp_bias);
    if (!vte.vport_x_scale_ena && !vte.vport_y_scale_ena) { viewport = {0, 0, float(target->color.width), float(target->color.height), 0, 1}; push.ndc[0] = 2.f / target->color.width; push.ndc[1] = 2.f / target->color.height; push.ndc[2] = push.ndc[3] = -1; }
    else if (!(vte.vport_x_scale_ena && vte.vport_y_scale_ena && vte.vport_x_offset_ena && vte.vport_y_offset_ena)) throw Unsupported("partial_viewport_transform");
    if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 || std::abs(viewport.height) <= 0 || viewport.width > 4096 || std::abs(viewport.height) > 2048) throw Unsupported("viewport_range");
    auto tl = r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], br = r[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t left = tl & 0x3fff, top = (tl >> 16) & 0x3fff, right = br & 0x3fff, bottom = (br >> 16) & 0x3fff;
    if (!(tl & 0x80000000) || r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      uint32_t wo = r[XE_GPU_REG_PA_SC_WINDOW_OFFSET]; int32_t wx = int32_t(wo << 17) >> 17, wy = int32_t(wo << 1) >> 17;
      if (!(tl & 0x80000000)) { left += wx; right += wx; top += wy; bottom += wy; }
      if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) { viewport.x += wx; viewport.y += wy; }
    }
    left = std::clamp(left, 0, int32_t(target->color.width)); right = std::clamp(right, left, int32_t(target->color.width)); top = std::clamp(top, 0, int32_t(target->color.height)); bottom = std::clamp(bottom, top, int32_t(target->color.height));
    if (left == right || top == bottom) { Submit(); return true; }
    VkRect2D scissor{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
    float blend[4]; for (uint32_t i = 0; i < 4; ++i) blend[i] = r.Get<float>(XE_GPU_REG_RB_BLEND_RED + i);
    Transition(target->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); Transition(target->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
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
    F().vkCmdPushConstants(command_, pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, &push);
    F().vkCmdSetViewport(command_, 0, 1, &viewport); F().vkCmdSetScissor(command_, 0, 1, &scissor); F().vkCmdSetBlendConstants(command_, blend);
    if (indices) { F().vkCmdBindIndexBuffer(command_, arena_, index_offset, VK_INDEX_TYPE_UINT32); F().vkCmdDrawIndexed(command_, count, 1, 0, 0, 0); }
    else F().vkCmdDraw(command_, count, 1, base, 0);
    F().vkCmdEndRenderPass(command_); ++batch_draws_; ++submitted_; latest_ = target;
    if (batch_draws_ >= kBatchDraws) Submit();
    if (submitted_ <= 5) REXGPU_INFO("BO3 NATIVE submitted draw {}: VS {:016X} PS {:016X} {} vertices {}x{}", submitted_, vs->ucode_data_hash(), ps->ucode_data_hash(), count, target->color.width, target->color.height);
    if (readback_diagnostic_ && submitted_ <= 5 && target->color.format == VK_FORMAT_R8G8B8A8_UNORM) {
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

bool NativeCommandProcessor::IssueCopy() {
  if (recording_) Submit();
  ++copies_;
  // Native ownership and format conversion are required before any
  // guest-visible resolve is accepted. Never claim a CPU shadow is fresh.
  const auto dest = register_file_->Get<reg::RB_COPY_DEST_INFO>();
  if (dest.copy_dest_exp_bias) return Reject(fmt::format("resolve_exponent_{}", dest.copy_dest_exp_bias));
  return Reject("native_resolve_not_implemented");
}

void NativeCommandProcessor::IssueSwap(uint32_t address, uint32_t width, uint32_t height) {
  if (recording_) Submit();
  ++swaps_;
  if (swaps_ % 30 == 0) {
    double wall = Milliseconds(last_perf_);
    const double thread_ms = ThreadMilliseconds();
    REXGPU_INFO("BO3 NATIVE timing: {:.2f} swaps/s over {:.0f}ms; queue/fence={:.1f}ms textureWall={:.1f}ms exportWall={:.1f}ms shader={:.1f}ms pipeline={:.1f}ms submissions={} uploadMiB={:.2f} linearMiB={:.2f} tiledMiB={:.2f} exportSnapshotMiB={:.2f}", (swaps_ - last_perf_swaps_) * 1000.0 / std::max(wall, .001), wall, timings_.wait, timings_.texture, timings_.exports, timings_.shader, timings_.pipeline, timings_.submissions, timings_.upload_bytes / 1048576.0, timings_.linear_bytes / 1048576.0, timings_.tiled_bytes / 1048576.0, timings_.export_snapshot_bytes / 1048576.0);
    REXGPU_INFO("BO3 NATIVE export phases: plan={:.1f}ms guards={:.1f}ms snapshot={:.1f}ms writes={:.1f}ms exportThreadCPU={:.1f}ms commandThreadCPU={:.1f}ms", timings_.export_plan, timings_.export_guards, timings_.export_snapshot, timings_.export_write, timings_.export_thread, thread_ms >= 0 && last_thread_ms_ >= 0 ? thread_ms - last_thread_ms_ : -1.0);
    last_thread_ms_ = thread_ms;
    timings_ = {}; last_perf_ = PerfClock::now(); last_perf_swaps_ = swaps_;
  }
  if (swaps_ == 1 || swaps_ % 120 == 0) {
    REXGPU_INFO("BO3 NATIVE progress: swaps={} packets={} submitted={} copies={} exports={} targets={} textures={} pipelines={} imageMiB={} frontbuffer={:08X}", swaps_, draws_, submitted_, copies_, export_copies_, targets_.size(), textures_.size(), pipelines_.size(), image_bytes_ / (1024 * 1024), address);
    for (const auto& [reason, count] : unsupported_) REXGPU_INFO("BO3 NATIVE skipped {}: {}", reason, count);
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
        VkDescriptorSet set{};
        if (latest_) {
          VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptors_; allocation.descriptorSetCount = 1; allocation.pSetLayouts = present_.sets;
          Check(F().vkAllocateDescriptorSets(D(), &allocation, &set), "present descriptors");
          VkDescriptorImageInfo source{present_sampler_, latest_->color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &source;
          F().vkUpdateDescriptorSets(D(), 1, &write, 0, nullptr);
        }
        Image output; output.image = context.image(); output.layout = context.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        Transition(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (latest_) Transition(latest_->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearValue clear{}; clear.color.float32[2] = .04f; clear.color.float32[3] = 1;
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = present_pass_; render.framebuffer = framebuffer; render.renderArea.extent = {width, height}; render.clearValueCount = 1; render.pClearValues = &clear;
        F().vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
        if (latest_) {
          VkViewport viewport{0, 0, float(width), float(height), 0, 1}; VkRect2D scissor{{0, 0}, {width, height}};
          F().vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_.pipeline); F().vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_.layout, 0, 1, &set, 0, nullptr);
          F().vkCmdSetViewport(command_, 0, 1, &viewport); F().vkCmdSetScissor(command_, 0, 1, &scissor); F().vkCmdDraw(command_, 3, 1, 0, 0);
        }
        F().vkCmdEndRenderPass(command_); Transition(output, VulkanPresenter::kGuestOutputInternalLayout); Submit();
      } catch (...) { F().vkDestroyFramebuffer(D(), framebuffer, nullptr); throw; }
      F().vkDestroyFramebuffer(D(), framebuffer, nullptr); return true;
    });
  } catch (const std::exception& error) { failed_ = true; REXGPU_ERROR("BO3 native presentation stopped: {}", error.what()); }
}

void NativeCommandProcessor::ShutdownContext() {
  if (!gpu_) return;
  if (recording_) Submit();
  REXGPU_INFO("BO3 NATIVE final: packets={} submitted={} swaps={} copies={}", draws_, submitted_, swaps_, copies_);
  for (const auto& [reason, count] : unsupported_) REXGPU_INFO("BO3 NATIVE final skipped {}: {}", reason, count);
  // Every submission uses a completion fence before resource reuse/destruction.
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline->pipeline) F().vkDestroyPipeline(D(), pipeline->pipeline, nullptr);
    if (pipeline->layout) F().vkDestroyPipelineLayout(D(), pipeline->layout, nullptr);
    for (auto set : pipeline->sets) if (set) F().vkDestroyDescriptorSetLayout(D(), set, nullptr);
  }
  pipelines_.clear();
  for (auto& [key, texture] : textures_) if (texture) { if (texture->sampler) F().vkDestroySampler(D(), texture->sampler, nullptr); DestroyImage(texture->image); }
  textures_.clear();
  for (auto& [key, target] : targets_) { F().vkDestroyFramebuffer(D(), target->framebuffer, nullptr); F().vkDestroyRenderPass(D(), target->pass, nullptr); DestroyImage(target->color); DestroyImage(target->depth); }
  targets_.clear();
  for (auto& shader : shaders_) if (shader->module) F().vkDestroyShaderModule(D(), shader->module, nullptr);
  shaders_.clear();
  if (present_.pipeline) F().vkDestroyPipeline(D(), present_.pipeline, nullptr);
  if (present_.layout) F().vkDestroyPipelineLayout(D(), present_.layout, nullptr);
  if (present_.sets[0]) F().vkDestroyDescriptorSetLayout(D(), present_.sets[0], nullptr);
  if (present_pass_) F().vkDestroyRenderPass(D(), present_pass_, nullptr);
  if (present_vs_) F().vkDestroyShaderModule(D(), present_vs_, nullptr);
  if (present_ps_) F().vkDestroyShaderModule(D(), present_ps_, nullptr);
  if (present_sampler_) F().vkDestroySampler(D(), present_sampler_, nullptr);
  if (mapped_) F().vkUnmapMemory(D(), arena_memory_);
  if (arena_) F().vkDestroyBuffer(D(), arena_, nullptr);
  if (arena_memory_) F().vkFreeMemory(D(), arena_memory_, nullptr);
  if (descriptors_) F().vkDestroyDescriptorPool(D(), descriptors_, nullptr);
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
