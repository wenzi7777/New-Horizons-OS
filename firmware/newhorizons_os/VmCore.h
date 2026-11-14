#pragma once

#include <stddef.h>
#include <stdint.h>

#include "AppContract.h"

namespace nhos {

// NHVM/1, the app virtual machine (NHOS-App-Library docs/vm.md): the image
// layout, the install-time verifier and the interpreter. Plain C++ with no
// Arduino dependency, so the App Library's conformance test compiles this file
// on a desktop and runs it on the same images and inputs as the JavaScript VM
// (sdk/lib/vm.mjs), requiring the same outputs.
//
// The VM never allocates. An image, its arena and its operand stack live in
// memory the caller hands it (AppVm's static pool); the verifier alone takes
// a transient scratch buffer, once, at install or bind time.

// Where an image's tables are, as byte offsets from the image start.
struct VmLayout {
  uint16_t length = 0;      // bytes of the image itself
  uint16_t codeAt = 0;
  uint16_t codeLen = 0;
  uint16_t constsAt = 0;
  uint16_t constCount = 0;
  uint16_t arena = 0;       // slots
  uint16_t stack = 0;       // slots
  uint16_t fnAt = 0;
  uint8_t fnCount = 0;
  uint16_t entryAt = 0;
  uint8_t entryCount = 0;
  uint16_t initAt = 0;
  uint8_t initCount = 0;
  uint16_t nodesAt = 0;
  uint8_t nodeCount = 0;
  uint16_t stringsAt = 0;
  uint8_t stringCount = 0;
  uint16_t stringBytes = 0;
  // Pool bytes for image + arena + stack, each 4-byte aligned.
  uint32_t poolBytes() const;
  uint32_t arenaAt() const { return (static_cast<uint32_t>(length) + 3U) & ~3U; }
  uint32_t stackAt() const { return arenaAt() + static_cast<uint32_t>(arena) * 4U; }
};

struct VmFunctionInfo {
  uint16_t pc;
  uint16_t base;
  uint8_t args;
  uint8_t locals;
  uint8_t rets;
};

struct VmNodeEntry {
  uint8_t op;     // FlowOp number
  uint8_t flags;  // bit 0 persist, bit 1 boolean
  uint16_t slot;  // arena slot holding the node's value
};

struct VmVerified {
  VmLayout layout;
  bool loopFree = false;
  uint32_t estimatedNs = 0;  // a bound when loopFree, else 0
  uint16_t maxStack = 0;
  uint16_t usedCapabilities = 0;
};

// Reads the header and tables and checks their layout (not the code).
// `error` points at a static reason code on failure.
bool vmParseLayout(const uint8_t* image, size_t length, VmLayout& out, const char*& error);

// The install-time checks, docs/vm.md §9. `scratch` must hold at least
// layout.codeLen * 3 bytes (stack depth per byte + a work list); null means
// "allocate it", which only the host-side harness and the install path do.
bool vmVerify(const uint8_t* image, size_t length, uint16_t declaredCaps, uint16_t cellCount,
              VmVerified& out, const char*& error);

// Base64 (standard alphabet, padded) decoding without allocation. Returns the
// decoded length, or -1 on a malformed input or when `capacity` is too small.
int32_t vmBase64Decode(const char* text, size_t textLen, uint8_t* out, size_t capacity);
size_t vmBase64DecodedLength(const char* text, size_t textLen);

// What one run sees. Mirrors AppEvent, minus the Arduino types.
struct VmContext {
  uint32_t nowMs = 0;
  uint32_t frameSeq = 0;
  const float* values = nullptr;  // nullptr: no frame (a tick, or no read_matrix)
  uint16_t rows = 0;
  uint16_t cols = 0;
  uint16_t cells = 0;
  const float* imu = nullptr;     // ax ay az gx gy gz, or nullptr
  const float* mag = nullptr;     // mx my mz, or nullptr
  float battery = -1.0f;
  bool linked = false;
};

class VmHostSink {
 public:
  virtual ~VmHostSink() = default;
  virtual void vmEmitEvent(const char* event, bool rise) = 0;
  virtual void vmEmitValue(const char* event, float value) = 0;
  virtual void vmLog(const char* text, float value) = 0;
  // The status LED changed: held in `rgb` while `on`, released when not.
  virtual void vmLed(bool on, const uint8_t rgb[3], uint16_t stateSlot) = 0;
};

enum class VmRunResult : uint8_t { Done = 0, Suspended = 1, Fault = 2 };

// PROFILE (bench only): cycle counters, reset on read. Build with
// -DNHOS_VM_PROFILE=1 to turn them on; off, they cost nothing.
#ifndef NHOS_VM_PROFILE
#define NHOS_VM_PROFILE 0
#endif
uint32_t vmCycles();
#if NHOS_VM_PROFILE
struct VmProfile {
  uint32_t deliveries = 0;
  uint64_t deliverCyc = 0;  // AppVm::deliver, whole
  uint64_t runCyc = 0;      // VmMachine::run
  uint64_t natCyc = 0;      // native() calls inside run
  uint32_t insns = 0;
  uint32_t nats = 0;
  uint32_t opCyc[kFlowOpCount] = {};
  uint32_t opCalls[kFlowOpCount] = {};
};
#endif

class VmMachine {
 public:
  static constexpr uint16_t kNoPc = 0xFFFF;
#if NHOS_VM_PROFILE
  VmProfile prof;
#endif

  // `memory` holds the image (already copied in), then the arena and stack
  // at the offsets `layout` gives; it must stay valid until unbind() or
  // relocate(). The image must have passed vmVerify().
  void bind(uint8_t* memory, const VmLayout& layout);
  void unbind();
  bool bound() const { return mem_ != nullptr; }
  // The pool moved the image: same bytes, new address.
  void relocate(uint8_t* memory) { mem_ = memory; }
  const VmLayout& layout() const { return layout_; }

  // Clears every value and state, then applies the image's initialisers.
  void reset();

  bool hasEntry(VmEvent event) const;
  // Starts a handler: pc at its function, empty stack, per-run outputs cleared.
  void begin(VmEvent event, bool pressed);
  // Runs until the handler ends, faults, spends `fuelLimit`, or -- when
  // `clock` is given -- the clock passes `deadlineUs` (checked every
  // kVmFuelCheck units of fuel).
  VmRunResult run(const VmContext& ctx, VmHostSink& host, uint32_t fuelLimit,
                  uint32_t (*clock)() = nullptr, uint32_t deadlineUs = 0);

  bool suspended() const { return suspended_; }
  uint8_t fault() const { return fault_; }
  bool runDegraded() const { return runDegraded_; }
  bool persistDirty() const { return persistDirty_; }
  void clearPersistDirty() { persistDirty_ = false; }

  void setBudget(float load, uint8_t graceLeft) {
    budgetLoad_ = load;
    graceLeft_ = graceLeft;
  }

  // Output bookkeeping, pulled by the display and the LED service.
  uint16_t displayPc(uint8_t row) const { return row < 4 ? displayPc_[row] : kNoPc; }
  uint16_t pixelPc(uint8_t pixel) const { return pixel < kPixels ? pixelPc_[pixel] : kNoPc; }
  uint16_t meterPc() const { return meterPc_; }
  bool ledHeld() const { return ledHeld_; }
  const uint8_t* ledRgb() const { return ledRgb_; }

  // Reads the NAT instruction at `pc`: op, state slot and parameter record.
  bool natAt(uint16_t pc, uint8_t& op, uint16_t& state, uint16_t& param) const;
  float slot(uint16_t index) const;
  void setSlot(uint16_t index, float value);
  float constant(uint16_t index) const;
  // String `index`, NUL-terminated, or "" when out of range.
  const char* string(uint16_t index) const;
  VmNodeEntry node(uint8_t index) const;
  uint8_t nodeCount() const { return layout_.nodeCount; }

 private:
  static constexpr uint8_t kPixels = 9;
  struct Frame {
    uint16_t pc;
    uint8_t fn;
  };

  VmFunctionInfo function(uint8_t index) const;
  void native(uint8_t op, uint16_t s, uint16_t p, const float* in, const VmContext& ctx,
              VmHostSink& host, uint16_t pc);
  bool syscall(uint16_t id, float* args, float& result, const VmContext& ctx, VmHostSink& host);
  float* arena() const { return reinterpret_cast<float*>(mem_ + layout_.arenaAt()); }
  float* stack() const { return reinterpret_cast<float*>(mem_ + layout_.stackAt()); }

  uint8_t* mem_ = nullptr;
  VmLayout layout_;
  uint16_t pc_ = 0;
  uint8_t sp_ = 0;
  uint8_t fn_ = 0;
  uint8_t callDepth_ = 0;
  Frame calls_[kVmMaxCallDepth] = {};
  bool suspended_ = false;
  uint8_t fault_ = 0;
  bool runDegraded_ = false;
  bool pressed_ = false;
  bool persistDirty_ = false;
  float budgetLoad_ = 0;
  uint8_t graceLeft_ = 0;
  uint16_t displayPc_[4] = {kNoPc, kNoPc, kNoPc, kNoPc};
  uint16_t pixelPc_[kPixels] = {kNoPc, kNoPc, kNoPc, kNoPc, kNoPc, kNoPc, kNoPc, kNoPc, kNoPc};
  uint16_t meterPc_ = kNoPc;
  bool ledHeld_ = false;
  uint8_t ledRgb_[3] = {0, 0, 0};
};

}  // namespace nhos
