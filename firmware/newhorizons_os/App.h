#pragma once

#include <Arduino.h>

#include "AppDisplay.h"
#include "AppExtLed.h"
#include "MatrixScanner.h"

namespace nhos {

// What an app is allowed to touch, and -- for the event sources -- what it is
// woken for. Declared up front in its manifest, so the permission set is
// inspectable before the app ever runs rather than being whatever its code
// happens to call.
//
// Permission and subscription are the same bit on purpose: an app that may not
// read the matrix has no business being woken when a frame arrives, and having
// two lists that could disagree is a bug waiting to happen.
enum AppCapability : uint16_t {
  kAppCapNone = 0,
  kAppCapReadMatrix = 1 << 0,
  kAppCapReadImu = 1 << 1,
  kAppCapEmitEvent = 1 << 2,
  kAppCapDriveLed = 1 << 3,
  kAppCapWriteFile = 1 << 4,
  kAppCapTick = 1 << 5,      // periodic wakeup, independent of scanning
  kAppCapButton = 1 << 6,    // action button edges
  kAppCapPower = 1 << 7,     // charge/battery state changes
  kAppCapLink = 1 << 8,      // gateway/hub connectivity changes
  // Budget pressure is delivered to every app that asks for it, so an app can
  // shed work before it is stopped. Ignoring it is a valid choice.
  kAppCapBudget = 1 << 9,
  // Rows on the OLED. Shown only while the operator has the OLED on its "app"
  // page: an installed app never takes the screen over by itself.
  kAppCapDisplay = 1 << 10,
  // The external LED strip. Unlike the OLED, a running app that may drive it
  // takes it over from the configured preset, and hands it back when it stops.
  kAppCapExtLed = 1 << 11,
};

struct AppManifest {
  const char* name = nullptr;
  const char* version = "0.0.0";
  uint16_t capabilities = kAppCapNone;
  // Per-invocation time allocation. This is the containment mechanism: the
  // runtime has no preemption, so an app cannot be interrupted mid-call --
  // but it can be measured and, after repeated overruns, taken out of the
  // dispatch list before it costs enough frames to matter.
  //
  // Not a constant: AppGovernor divides the apps' share of CPU among whatever
  // is actually running, so this shrinks as more apps are enabled.
  uint32_t frameBudgetUs = 500;
  const char* summary = "";
};

// Why an app is being called. The scanned frame is one source among several --
// an app that only subscribes to Tick keeps running while scanning is stopped,
// and an app that only subscribes to Frame costs nothing when it is.
enum class AppEventKind : uint8_t {
  Frame = 0,   // a matrix frame was just scanned
  Imu,         // a fresh IMU sample
  Tick,        // periodic, whether or not the scanner is running
  Button,      // action button edge
  Power,       // charge or battery state change
  Link,        // gateway/hub connectivity change
  Budget,      // this app is over its allocation; see load/graceLeft
  Custom,      // an event emitted by another app
};

// Services the runtime lends an app for the duration of one call. An app never
// reaches into modules directly; everything it may do arrives here, and each
// entry point re-checks the manifest's capability bits.
class AppHost {
 public:
  virtual ~AppHost() = default;
  // Named observation, surfaced through /proc/apps and app_events. Kept off
  // the sensor wire format deliberately: adding an event block to
  // NHO/Arduino/1 would mean matching changes in the Gateway, Hub and
  // Backend parsers, which is a protocol decision, not an app-framework one.
  virtual void emitEvent(const char* app, const char* event, const String& detail) = 0;
  // As above, but carrying a number -- the common case for a measurement.
  virtual void emitValue(const char* app, const char* event, float value) = 0;
  virtual void setLed(const char* app, uint8_t r, uint8_t g, uint8_t b) = 0;
  virtual void logLine(const char* app, const String& line) = 0;
};

// One delivery. Read-only, and every pointer is valid only for this call.
struct AppEvent {
  AppEventKind kind = AppEventKind::Tick;
  uint32_t nowMs = 0;
  // Sequence of the frame this event belongs to, so an app's output can be
  // lined up against recorded samples afterwards.
  uint32_t frameSeq = 0;
  const MatrixFrame* frame = nullptr;  // nullptr unless kind == Frame
  const float* imuSample = nullptr;    // nullptr unless kAppCapReadImu and valid
  AppHost* host = nullptr;
  // Budget events only: measured cost over allocation, and how many more
  // overruns this app has before it is stopped.
  float load = 0.0f;
  uint8_t graceLeft = 0;
};

// An application: its own lifecycle, declared permissions, measured cost, and
// the ability to be enabled, disabled, suspended or killed at runtime without
// touching the rest of the firmware.
//
// Apps are hosts for installed packages rather than compiled-in features. The
// thing that makes third-party content safe here is a measured, shrinking time
// allocation -- not a separate address space, which this MCU cannot offer and
// which would not help against the failure that actually matters (missing a
// sampling deadline).
class App {
 public:
  virtual ~App() = default;
  virtual const AppManifest& manifest() const = 0;
  virtual bool start() { return true; }
  virtual void stop() {}
  virtual void onEvent(const AppEvent& event) = 0;
  // Optional app-specific state for /proc/apps and app_list; must be cheap.
  // `withOutputs` asks for per-node values too, which app_list includes only
  // for the one slot a caller names: for every slot at once they would not fit
  // an ESP-NOW reply.
  virtual String statusJson(bool withOutputs) const {
    (void)withOutputs;
    return "{}";
  }
  // True while an enabled app has nothing to run -- a flow slot with no graph
  // bound. It is not dispatched and takes no share of the budget, but keeps
  // its enabled state so binding a package later starts it straight away.
  virtual bool idle() const { return false; }
  // What this app last put on OLED row `row`, if anything. Pulled by the
  // display at its own rate rather than pushed per frame, so drawing costs the
  // app nothing and the panel's I2C traffic stays outside every budget.
  virtual bool displayLine(uint8_t row, AppDisplayLine& out) const {
    (void)row;
    (void)out;
    return false;
  }
  // Adds what this app put on the external LED strip on its last frame to
  // `out`, leaving the meter and every pixel an earlier slot already set.
  // Pulled by the LED service, like displayLine.
  virtual void extLedFrame(AppExtLedFrame& out) const { (void)out; }
};

}  // namespace nhos
