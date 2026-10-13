// Validate the Switch SDK's Vulkan Xenos backend on Windows before native work.
#include "bo3_zm_init.h"
#include "bo3_zm_app.h"
#include "draw_capture.h"
#include "d3d_trace.h"

class Bo3VulkanApp final : public Bo3ZmApp {
 public:
  using Bo3ZmApp::Bo3ZmApp;
  void OnPostInitLogging() override {
    Bo3ZmApp::OnPostInitLogging();
    draw_capture_.Initialize();
    bo3::InitializeD3DTrace();
  }
  void OnShutdown() override {
    draw_capture_.Stop();
    bo3::FinishD3DTrace();
    Bo3ZmApp::OnShutdown();
  }
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& context) {
    return std::unique_ptr<Bo3VulkanApp>(new Bo3VulkanApp(context, "bo3_zm_vulkan", PPCImageConfig));
  }
 private:
  bo3::DrawCaptureControl draw_capture_;
};
REX_DEFINE_APP(bo3_zm_vulkan, Bo3VulkanApp::Create)
