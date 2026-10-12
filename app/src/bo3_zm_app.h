// bo3_zm - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include "crash_reporter.h"
#include "content_packages.h"
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/presenter.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

class Bo3ZmApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  void OnPostInitLogging() override {
    bo3::InstallCrashReporter();
    bo3::StartStackSampler();
  }

  void OnLoadXexImage(std::string& image) override {
    image = "game:\\default_zm.xex";
  }

  void OnPostLoadXexImage() override {
    if (const char* path = std::getenv("BO3_DUMP_IMAGE")) {
      const auto module = runtime()->kernel_state()->GetExecutableModule();
      const auto* xex = module->xex_module();
      std::ofstream file(path, std::ios::binary);
      file.write(reinterpret_cast<const char*>(runtime()->memory()->TranslateVirtual(xex->base_address())),
                 xex->image_size());
      REXLOG_INFO("BO3 diagnostic image dump: {}", path);
    }
    auto* manager = runtime()->kernel_state()->content_manager();
    for (const auto& package : bo3::kContentPackages) {
      rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
      data.device_id = 1;
      data.content_type = rex::system::XContentType::kMarketplaceContent;
      data.xuid = 0;
      data.title_id = 0x4156091D;
      data.set_file_name(package.file_name);
      data.set_display_name(package.display_name);
      if (!manager->ContentExists(0, data)) {
        REXLOG_ERROR("BO3 package directory is missing: {}", package.file_name);
        continue;
      }
      const auto result = manager->WriteContentHeaderFile(0, data, package.license_mask);
      REXLOG_INFO("BO3 content header {}: result {:08X}, original license {:08X}",
                  package.file_name, result, package.license_mask);
    }
    const auto content = manager->ListContent(1, 0,
        rex::system::XContentType::kMarketplaceContent, 0x4156091D);
    REXLOG_INFO("BO3 common marketplace packages available: {}", content.size());
  }

  void LaunchModule() override {
    rex::ReXApp::LaunchModule();
    if (!std::getenv("BO3_CAPTURE_FRAMES")) return;
    capture_thread_ = std::jthread([this](std::stop_token stop) {
      for (int second = 1; second <= 45 && !stop.stop_requested(); ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stop.stop_requested()) return;
        if (second != 10 && second != 30 && second != 45) continue;
        auto* presenter = runtime()->graphics_system()->presenter();
        rex::ui::RawImage image;
        if (!presenter || !presenter->CaptureGuestOutput(image)) {
          REXLOG_INFO("BO3 frame capture at {}s: no guest output", second);
          continue;
        }
        // PPM preserves the guest's RGB pixels without a codec dependency.
        const auto path = cache_root() / ("boot-frame-" + std::to_string(second) + "s.ppm");
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (uint32_t y = 0; y < image.height; ++y)
          for (uint32_t x = 0; x < image.width; ++x)
            file.write(reinterpret_cast<const char*>(image.data.data() + y * image.stride + x * 4), 3);
        REXLOG_INFO("BO3 guest frame captured at {}s: {} ({}x{})", second,
                    path.string(), image.width, image.height);
      }
    });
  }

  void OnShutdown() override {
    capture_thread_.request_stop();
    if (capture_thread_.joinable()) capture_thread_.join();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo3ZmApp>(new Bo3ZmApp(ctx, "bo3_zm",
        PPCImageConfig));
  }

 private:
  std::jthread capture_thread_;
};
