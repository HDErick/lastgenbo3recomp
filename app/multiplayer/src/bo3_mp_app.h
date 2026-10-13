#pragma once

// Reuse the content installation and diagnostics of the BO3 host.
#include "bo3_zm_app.h"

class Bo3MpApp : public Bo3ZmApp {
 public:
  using Bo3ZmApp::Bo3ZmApp;

  void OnLoadXexImage(std::string& image) override {
    image = "game:\\default.xex";
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo3MpApp>(new Bo3MpApp(ctx, "bo3_mp", PPCImageConfig));
  }
};
