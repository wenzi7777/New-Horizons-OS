#pragma once

#include <Arduino.h>

#include "MatrixScanner.h"

namespace nhos {

// What an app is allowed to touch. Declared up front in its manifest, so the
// permission set is inspectable before the app ever runs rather than being
// whatever its code happens to call.
enum AppCapability : uint16_t {
  kAppCapNone = 0,
  kAppCapReadMatrix = 1 << 0,
  kAppCapReadImu = 1 << 1,
  kAppCapEmitEvent = 1 << 2,
  kAppCapDriveLed = 1 << 3,
  kAppCapWriteFile = 1 << 4,
};

struct AppManifest {
  const char* name = nullptr;
  const char* version = "0.0.0";
  uint16_t capabilities = kAppCapNone;
  // Per-invocation time budget. This is the containment mechanism: the
  // runtime has no preemption, so an app cannot be interrupted mid-frame --
  // but it can be measured and, after repeated overruns, taken out of the
  // dispatch list before it costs enough frames to matter.
  uint32_t frameBudgetUs = 500;
  const char* summary = "";
};

// Services the runtime lends an app for the duration of one frame. An app
// never reaches into modules directly; everything it may do arrives here, and
// each entry point re-checks the manifest's capability bits.
class AppHost {
 public:
  virtual ~AppHost() = default;
  // Named observation, surfaced through /proc/apps and app_status. Kept off
  // the sensor wire format deliberately: adding an event block to
  // NHO/Arduino/1 would mean matching changes in the Gateway, Hub and
  // Backend parsers, which is a protocol decision, not an app-framework one.
  virtual void emitEvent(const char* app, const char* event, const String& detail) = 0;
  virtual void logLine(const char* app, const String& line) = 0;
};

// Read-only view of one scanned frame plus whatever else the app is cleared
// for. Points at the scanner's own buffer -- valid only for this call.
struct AppFrameContext {
  const MatrixFrame* frame = nullptr;
  const float* imuSample = nullptr;  // nullptr unless kAppCapReadImu and valid
  uint32_t nowMs = 0;
  AppHost* host = nullptr;
};

// An application: compiled in, but with its own lifecycle, declared
// permissions, measured cost, and the ability to be enabled, disabled or
// killed at runtime without touching the rest of the firmware.
//
// Deliberately static rather than dynamically loaded. On a device with no
// preemption and a hard 60-120Hz sampling obligation, the thing that makes
// third-party code safe is a bounded time budget, not a separate address
// space -- and a budget works just as well for compiled-in code, at none of
// the cost of an interpreter or a WASM runtime.
class App {
 public:
  virtual ~App() = default;
  virtual const AppManifest& manifest() const = 0;
  virtual bool start() { return true; }
  virtual void stop() {}
  virtual void onFrame(const AppFrameContext& context) = 0;
  // Optional app-specific state for /proc/apps; must be cheap.
  virtual String statusJson() const { return "{}"; }
};

}  // namespace nhos
