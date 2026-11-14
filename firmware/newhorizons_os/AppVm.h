#pragma once

#include "App.h"
#include "AppStore.h"
#include "VmCore.h"

namespace nhos {

class Storage;

// The one kind of on-device app: a package's NHVM/1 image (NHOS-App-Library
// docs/vm.md), run by VmCore's interpreter inside the main loop's `apps` task.
//
// It replaced FlowApp in v2.0.0. A flow graph is still how most apps are
// written, but the App Library now lowers it to bytecode at build time, so the
// device runs one interpreter whatever the source looked like. A package
// without an image -- every package built before v2.0.0 -- is refused as
// `needs_update`, and the Desktop/Studio offer to reinstall it.
//
// Containment is two things, neither of which trusts the image:
//  * the verifier (VmCore vmVerify) at install and bind time: every operand,
//    jump, stack depth, arena index and capability is checked once, so the
//    interpreter needs no checks of its own beyond array indices;
//  * a time slice per delivery: a handler that runs past it is suspended,
//    not killed, and resumed by the next delivery -- the frames it missed are
//    counted and reported as `frames_skipped`, never silently dropped.
//
// Memory is one static pool shared by every slot (kPoolBytes): an image is
// copied in at bind time with its
// arena and stack, first fit, compacted when fragmented. Nothing is allocated
// while running, and an image that does not fit is refused at activation as
// `app_pool_exhausted` rather than taken from the heap.
class AppVm : public App, private VmHostSink {
 public:
  // The App Library's limits; the contract test reads them from here.
  static constexpr uint8_t kMaxNodes = kVmMaxNodes;
  static constexpr size_t kMaxPackageBytes = 8192;
  static constexpr uint32_t kDefaultBudgetUs = 1500;
  // Presses held for handlers not yet started. Small on purpose: presses made
  // while no frames arrive (streaming paused) must not replay as a burst later.
  static constexpr uint8_t kMaxPendingPresses = 3;
  // An image with an idle_tick handler (the `tick` capability) runs it on the
  // 10 Hz tick, but only once no frame has arrived for this long.
  static constexpr uint32_t kTickFallbackMs = 250;
  // Persisted counters are written at most this often, from service(), so a
  // reboot loses at most this much counting and the flash sees few writes.
  static constexpr uint32_t kPersistIntervalMs = 30000;
  // Shared by every slot. 4 KB: the four library apps measured on a 14x14
  // board took 2.3 KB together (the largest 636 B). What the static FlowApp
  // slots used to hold went mostly to IRAM instead: the interpreter's hot
  // path runs from there, since from flash it lost most of its time to
  // cache misses (bench/app-vm, variant D).
  static constexpr uint32_t kPoolBytes = 4096;
  // Share of the slot's allocation a handler may run for before it is
  // suspended. Below 100% so the clock check's granularity (kVmFuelCheck
  // instructions) does not turn a suspension into an overrun.
  static constexpr uint8_t kSlicePercent = 75;

  AppVm();
  // The manifest points into this object's own buffers.
  AppVm(const AppVm&) = delete;
  AppVm& operator=(const AppVm&) = delete;

  // Must be called BEFORE AppManager::install(): install() indexes by name and
  // persists app_en_<name>, so the name has to be final by then.
  void setIdentity(const char* name, uint32_t budgetUs = kDefaultBudgetUs);
  // Applied when a package is bound; reverted by unload().
  void applyPackageManifest(const char* version, const char* summary,
                            const char* packageId, uint32_t capabilities);

  void attach(Storage& storage) {
    storage_ = &storage;
    store_.attach(&storage);
  }
  // A raw package file (app_load_flow): no manifest of record, so it may use
  // whatever a slot allows, and it persists nothing.
  bool loadFromFile(const String& path, uint16_t cellCount, String& error);
  // Decodes, verifies and binds a package's image. On failure the slot keeps
  // what it was running. `declaredCaps` is the manifest's capability set: an
  // image that needs more is refused.
  bool loadFromJson(const String& json, const String& sourcePath, uint16_t cellCount,
                    uint32_t declaredCaps, String& error);
  bool loaded() const { return machine_.bound(); }
  // A faulted image is idle too: not dispatched, no share of the budget,
  // until it is reloaded.
  bool idle() const override { return !loaded() || fault_ != 0; }
  void unload();
  uint32_t estimatedUs() const { return estimatedUs_; }
  const char* packageId() const { return packageId_; }

  const AppManifest& manifest() const override { return manifest_; }
  bool start() override;
  // Runs on("stop") and gives back what the app took: its store is written
  // out, device settings it changed are restored, the UART is closed.
  void stop() override;
  void onEvent(const AppEvent& event) override;
  // app.publish from another app: queued (kVmMessageQueue) for this image's
  // on("message") handlers, run before its next handler.
  void queueMessage(uint32_t topic, float value) override;
  void service(uint32_t nowMs) override;
  String statusJson(bool withOutputs) const override;
  bool displayLine(uint8_t row, AppDisplayLine& out) const override;
  void extLedFrame(AppExtLedFrame& out) const override;
  bool statusLed(uint8_t rgb[3]) const override;

  // Everything binding would check -- the image is present and well formed,
  // passes the verifier, needs no undeclared capability, fits the budget --
  // without touching any slot. The registry refuses a package with it at
  // install time.
  static bool dryRun(const String& json, uint16_t cellCount, uint32_t budgetUs,
                     uint32_t declaredCaps, uint32_t& estimatedUs, String& error);
  // Forgets a package's persisted values. The registry calls it on uninstall;
  // deactivating keeps them, so re-activating carries on counting.
  static void clearPersisted(Storage& storage, const char* packageId);

  // Pool accounting, for /proc/apps.
  static uint32_t poolUsedBytes();
  static uint32_t poolLargestFree();

 private:
  // VmHostSink: what a running handler asks of the host.
  void vmEmitEvent(const char* event, bool rise) override;
  void vmEmitValue(const char* event, float value) override;
  void vmLog(const char* text, float value) override;
  void vmLed(bool on, const uint8_t rgb[3], uint16_t stateSlot) override;
  float vmStoreGet(const char* key) override;
  void vmStoreSet(const char* key, float value) override;
  bool vmStoreAppend(const char* file, uint32_t nowMs, const float values[4]) override;
  void vmStoreClear(const char* file) override;
  void vmPublish(const char* topic, float value) override;
  float vmConfigGet(const char* key) override;
  bool vmConfigSet(const char* key, float value, uint32_t nowMs) override;
  void vmStream(bool on) override;
  bool vmI2cRead(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t n) override;
  bool vmI2cWrite(uint8_t addr, const uint8_t* bytes, uint8_t n) override;
  bool vmUartOpen(uint32_t baud) override;
  void vmUartClose() override;
  int32_t vmUartWrite(const uint8_t* bytes, uint8_t n) override;
  int32_t vmUartAvailable() override;
  int32_t vmUartRead() override;

  // Runs `handler` (a VmEvent, or -1 to resume a suspended one). As the App
  // Library's simulator does: on("start") first if it has not run, then
  // waiting messages, each as its own run, then the handler itself.
  void deliver(const AppEvent& event, int8_t handler);
  void setDeadline(const AppEvent& event);
  // What a stop, fault or unload gives back.
  void releaseResources();
  void raiseFault(uint8_t fault, const AppEvent& event);
  bool persists() const;
  void restorePersisted();
  void savePersisted();
  void relocate(uint8_t* memory);
  friend struct AppVmPool;

  AppManifest manifest_;
  char nameBuf_[12] = {0};
  char versionBuf_[12] = {0};
  char summaryBuf_[64] = {0};
  char packageId_[16] = {0};

  Storage* storage_ = nullptr;
  uint8_t* memory_ = nullptr;  // this slot's block of the pool
  VmMachine machine_;
  VmVerified verified_;
  uint32_t estimatedUs_ = 0;
  AppHost* host_ = nullptr;  // valid only during deliver()
  uint32_t emissions_ = 0;
  uint32_t frames_ = 0;
  uint32_t ticks_ = 0;
  uint32_t degradations_ = 0;
  uint32_t suspensions_ = 0;    // handlers that ran out of slice
  uint32_t framesSkipped_ = 0;  // since the last report
  uint32_t framesSkippedTotal_ = 0;
  uint32_t lastFrameMs_ = 0;
  uint32_t suspendedSinceMs_ = 0;
  uint32_t lastPersistMs_ = 0;
  int8_t handler_ = -1;  // the VmEvent being run, or -1
  uint8_t fault_ = 0;
  bool framesSeen_ = false;
  bool degraded_ = false;
  // Short presses not yet shown to a handler. Each one is true for exactly
  // one run, with a false run after it, so two presses are always two rising
  // edges.
  uint8_t pendingPresses_ = 0;
  bool pressShown_ = false;
  // v2.1.0: lifecycle, messages and what the app holds on the device.
  bool started_ = false;  // on("start") has run
  bool stopped_ = false;  // on("stop") has run
  struct Message {
    uint32_t topic;
    float value;
  };
  Message inbox_[kVmMessageQueue] = {};
  uint8_t inboxCount_ = 0;
  Message nextMessage_ = {};
  bool runMessage_ = false;  // the handler being run is a message's
  Message message_ = {};
  uint32_t messagesDropped_ = 0;
  uint32_t deadlineUs_ = 0;
  uint32_t lastConfigSetMs_ = 0;
  bool configSet_ = false;
  uint32_t lastFrameSeq_ = 0;
  AppHost* lastHost_ = nullptr;  // AppManager, for on("stop") outside dispatch
  AppStore store_;
  String sourcePath_;
  String graphName_;
};

}  // namespace nhos
