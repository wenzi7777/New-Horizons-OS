#pragma once

#include <Arduino.h>

#include "AppContract.h"
#include "AppDisplay.h"
#include "AppExtLed.h"
#include "AppLoad.h"
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
// The bits themselves are generated from the App Library's opset.mjs, in
// AppContract.h:
//   tick         periodic wakeup, independent of scanning
//   button       action button edges
//   power        charge/battery state changes
//   link         gateway/hub connectivity changes
//   budget       budget pressure, delivered to every app that asks for it, so
//                an app can shed work before it is stopped (ignoring it is fine)
//   display      rows on the OLED, shown only while the operator has the OLED
//                on its "app" page: an app never takes the screen over itself
//   drive_ext_led  the external LED strip; unlike the OLED, a running app that
//                may drive it takes it over from the configured preset, and
//                hands it back when it stops
//   read_mag     magnetometer samples, a separate permission from the IMU
//   persist      keep marked values across reboots, in NVS (written outside
//                dispatch)

struct AppManifest {
  const char* name = nullptr;
  const char* version = "0.0.0";
  uint32_t capabilities = kAppCapNone;
  // Per-invocation time allocation. This is the containment mechanism: the
  // runtime has no preemption, so an app cannot be interrupted mid-call --
  // but it can be measured and, after repeated overruns, taken out of the
  // dispatch list before it costs enough frames to matter.
  //
  // Not a constant: AppGovernor divides the apps' share of CPU among whatever
  // is actually running, so this shrinks as more apps are enabled.
  uint32_t frameBudgetUs = 500;
  const char* summary = "";
  // v2.1.0: may borrow time other apps leave unused (AppShare). Never
  // guaranteed, and never past AppShare's cap.
  bool burst = false;
};

enum class AppState : uint8_t {
  Installed = 0,  // present but not running
  Running = 1,
  Killed = 2,     // repeatedly exceeded its own allocation -- the app's fault
  Faulted = 3,    // start() refused
  // Stopped because the system needed the capacity, NOT because the app did
  // anything wrong. Distinct from Killed because it must come back on its own:
  // a suspension that needed an operator to clear it would mean one busy
  // moment takes an app away permanently, with nothing saying why.
  Suspended = 4,
  // Stopped by an operator (app_stop) for this boot only: nothing is
  // persisted, so a reboot brings the app back; unlike Suspended it does not
  // come back by itself.
  Stopped = 5,
};

class App;

// What AppManager keeps about one app while it is in the run list. Part of
// the app object rather than a table in AppManager (v2.1.0): the apps live in
// the AppPool, and how many there are is whatever fits there.
struct AppRun {
  App* next = nullptr;  // the run list, in `order`
  uint16_t order = 0;   // lower first: wins the OLED rows, the LED and the strip
  AppState state = AppState::Installed;
  uint32_t invocations = 0;
  uint32_t lastUs = 0;
  uint32_t maxUs = 0;
  uint32_t overruns = 0;
  uint8_t consecutiveOverruns = 0;
  uint32_t events = 0;
  // The slice a delivery is given: the guaranteed base, minus what is lent,
  // plus what is borrowed (AppShare). 0 while not running.
  uint32_t allocatedUs = 0;
  uint32_t baseUs = 0;
  uint32_t borrowedUs = 0;
  uint32_t lentUs = 0;
  // Deliveries in the current load window, and the app's own pressure
  // counter (suspensions + skipped frames) as of the last settle.
  uint32_t windowDeliveries = 0;
  uint32_t lastPressure = 0;
  bool windowPressured = false;
  AppLoad load;
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
  virtual void logLine(const char* app, const String& line) = 0;
  // app.publish: `topic` (FNV-1a of its name) to every other running app.
  virtual void publish(const char* app, uint32_t topic, float value) {
    (void)app; (void)topic; (void)value;
  }
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
  const float* magSample = nullptr;    // mx, my, mz (uT); nullptr unless kAppCapReadMag and valid
  // Fuel-gauge state of charge in percent, or -1 without kAppCapPower or a
  // valid reading.
  float batteryPercent = -1.0f;
  // Whether the stream transport has somewhere to send; false without
  // kAppCapLink.
  bool linked = false;
  AppHost* host = nullptr;
  // Budget events only: measured cost over allocation, and how many more
  // overruns this app has before it is stopped.
  float load = 0.0f;
  uint8_t graceLeft = 0;
  // The app's time allocation for this delivery (AppManager's share, or the
  // manifest's budget). An app that can stop part-way -- AppVm -- suspends
  // itself before running past it; 0 means use the manifest's.
  uint32_t sliceUs = 0;
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
  // AppManager's bookkeeping for this app; nobody else writes it.
  AppRun run;
  virtual const AppManifest& manifest() const = 0;
  virtual bool start() { return true; }
  virtual void stop() {}
  virtual void onEvent(const AppEvent& event) = 0;
  // A message another app published (AppHost::publish).
  virtual void queueMessage(uint32_t topic, float value) { (void)topic; (void)value; }
  // Housekeeping that must not be charged to the app's allocation, such as a
  // flash write: called outside dispatch, untimed. Must return quickly when
  // there is nothing to do, since it runs every apps tick.
  virtual void service(uint32_t nowMs) { (void)nowMs; }
  // Optional app-specific state for /proc/apps and app_list; must be cheap.
  // `withOutputs` asks for per-node values too, which app_list includes only
  // for the one app a caller names: for every app at once they would not fit
  // an ESP-NOW reply.
  virtual String statusJson(bool withOutputs) const {
    (void)withOutputs;
    return "{}";
  }
  // True while an app has nothing to run (an image that faulted). It is not
  // dispatched and takes no share of the budget.
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
  // `out`, leaving the meter and every pixel an earlier app already set.
  // Pulled by the LED service, like displayLine.
  virtual void extLedFrame(AppExtLedFrame& out) const { (void)out; }
  // The status LED colour this app is holding, if any. Pulled, like
  // displayLine: false means the app has let go and the system pattern shows.
  // Whether the colour reaches the pixel is AppManager's and LedController's
  // call, not the app's.
  virtual bool statusLed(uint8_t rgb[3]) const {
    (void)rgb;
    return false;
  }
  // Times this app ran out of its slice or missed a frame because of it, ever.
  // AppManager turns the change per second into the load's pressure.
  virtual uint32_t pressureCount() const { return 0; }
  // Bytes of the AppPool this app holds (control block, store, image).
  virtual uint32_t memoryBytes() const { return 0; }
};

}  // namespace nhos
