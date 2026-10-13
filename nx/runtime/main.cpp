#include "bo3_zm_init.h"
#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include "content_packages.h"

class Bo3NxApp final : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path root = "sdmc:/switch/bo3recomp";
    paths.game_data_root = root / "game";
    paths.update_data_root = root / "update";
    paths.user_data_root = root / "user";
    paths.cache_root = root / "cache";
    paths.config_path = root / "bo3_zm_nx.toml";
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    rex::cvar::SetFlagByName("xam_signin_state", "1");
    rex::cvar::SetFlagByName("log_file", "sdmc:/switch/bo3recomp/bo3_zm_nx.log");
    // Begin with predictable file I/O; trace BO3 streaming before tuning caches.
    rex::cvar::SetFlagByName("nfsmw_io_rangos_mb", "0");
  }
  void OnLoadXexImage(std::string& image) override { image = "game:\\default_zm.xex"; }
  void OnPostLoadXexImage() override {
    auto* manager = runtime()->kernel_state()->content_manager();
    for (const auto& package : bo3::kContentPackages) {
      rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
      data.device_id = 1;
      data.content_type = rex::system::XContentType::kMarketplaceContent;
      data.title_id = 0x4156091D;
      data.set_file_name(package.file_name);
      data.set_display_name(package.display_name);
      if (!manager->ContentExists(0, data)) {
        REXLOG_ERROR("BO3 NX package directory missing: {}", package.file_name);
        continue;
      }
      const auto result = manager->WriteContentHeaderFile(0, data, package.license_mask);
      REXLOG_INFO("BO3 NX content header {}: {:08X}", package.file_name, result);
    }
  }
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& context) {
    return std::unique_ptr<Bo3NxApp>(new Bo3NxApp(context, "bo3_zm_nx", PPCImageConfig));
  }
};
REX_DEFINE_APP(bo3_zm_nx, Bo3NxApp::Create)
