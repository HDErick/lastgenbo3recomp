#pragma once
#include <thread>
#include <string>

namespace bo3 {
class DrawCaptureControl {
 public:
  void Initialize();
  void Stop();
 private:
  std::jthread watcher_;
};
}
