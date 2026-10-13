#include "draw_capture.h"
#include <windows.h>
#include <renderdoc_app.h>
#include <rex/logging.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <vector>

namespace bo3 {
void DrawCaptureControl::Initialize() {
  const char* capture_dir = std::getenv("BO3_GPU_CAPTURE_DIR");
  const char* dll_path = std::getenv("BO3_RENDERDOC_DLL");
  if (!capture_dir || !dll_path) return;
  // Opt-in diagnostic launch only. Load before creating any graphics API object.
  HMODULE module = LoadLibraryW(std::filesystem::path(dll_path).c_str());
  auto get_api = module ? reinterpret_cast<pRENDERDOC_GetAPI>(
      GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
  RENDERDOC_API_1_0_0* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api))) {
    REXLOG_ERROR("BO3 draw capture: RenderDoc API unavailable");
    return;
  }
  const std::filesystem::path directory(capture_dir);
  api->SetLogFilePathTemplate((directory / "white-squares").string().c_str());
  api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
  api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacksOnlyDraws, 1);
  REXLOG_INFO("BO3 draw capture ready; request file: {}", (directory / "request.txt").string());
  watcher_ = std::jthread([api, directory](std::stop_token stop) {
    auto status = [&directory](const std::string& message) {
      std::ofstream file(directory / "status.txt", std::ios::trunc);
      file << message << '\n';
      REXLOG_INFO("BO3 draw capture: {}", message);
    };
    status("Ready: write request.txt while facing the square effects");
    uint32_t completed = api->GetNumCaptures();
    bool pending = false;
    while (!stop.stop_requested()) {
      std::error_code error;
      if (!pending && std::filesystem::remove(directory / "request.txt", error)) {
        api->TriggerCapture();
        pending = true;
        status("Requested next presented frame; waiting for capture");
      }
      const uint32_t count = api->GetNumCaptures();
      if (count > completed) {
        uint32_t length = 0;
        api->GetCapture(count - 1, nullptr, &length, nullptr);
        std::vector<char> path(length + 1, '\0');
        if (api->GetCapture(count - 1, path.data(), &length, nullptr)) {
          status(std::string("Saved: ") + path.data());
        }
        completed = count;
        pending = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });
  // Keep the injected module resident through device destruction/process exit.
}
void DrawCaptureControl::Stop() {
  watcher_.request_stop();
  if (watcher_.joinable()) watcher_.join();
}
}
