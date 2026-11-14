#include "AppVm.h"

#include <string.h>

#include <memory>
#include <new>
#include <vector>

#include "JsonUtils.h"
#include <esp_attr.h>
#include "Storage.h"

namespace nhos {

// --- the shared pool -------------------------------------------------------------

namespace {

constexpr uint8_t kMaxBlocks = 8;  // a slot holds two blocks only while replacing its image

struct Block {
  AppVm* owner;
  uint32_t at;
  uint32_t len;
};

alignas(4) uint8_t g_pool[AppVm::kPoolBytes];
Block g_blocks[kMaxBlocks];
uint8_t g_blockCount = 0;  // kept sorted by `at`

uint32_t align4(uint32_t n) { return (n + 3U) & ~3U; }

}  // namespace

// Friend of AppVm, so compaction can move a slot's image and tell it where.
struct AppVmPool {
  static uint8_t* allocate(AppVm* owner, uint32_t len) {
    len = align4(len);
    if (len == 0 || len > AppVm::kPoolBytes || g_blockCount >= kMaxBlocks) return nullptr;
    // First fit between the blocks.
    uint32_t cursor = 0;
    uint8_t insertAt = g_blockCount;
    bool found = false;
    for (uint8_t i = 0; i <= g_blockCount; ++i) {
      const uint32_t end = i < g_blockCount ? g_blocks[i].at : AppVm::kPoolBytes;
      if (end - cursor >= len) {
        insertAt = i;
        found = true;
        break;
      }
      if (i < g_blockCount) cursor = g_blocks[i].at + g_blocks[i].len;
    }
    if (!found) {
      // Fragmented: compact. Only ever called between deliveries (bind
      // happens in a command handler on the loop task), so no handler is
      // mid-run on the bytes being moved -- and a suspended one keeps only
      // offsets, which moving does not change.
      uint32_t used = 0;
      for (uint8_t i = 0; i < g_blockCount; ++i) used += g_blocks[i].len;
      if (AppVm::kPoolBytes - used < len) return nullptr;
      cursor = 0;
      for (uint8_t i = 0; i < g_blockCount; ++i) {
        Block& block = g_blocks[i];
        if (block.at != cursor) {
          memmove(g_pool + cursor, g_pool + block.at, block.len);
          block.at = cursor;
          block.owner->relocate(g_pool + cursor);
        }
        cursor += block.len;
      }
      insertAt = g_blockCount;
    }
    for (uint8_t i = g_blockCount; i > insertAt; --i) g_blocks[i] = g_blocks[i - 1];
    g_blocks[insertAt] = {owner, cursor, len};
    ++g_blockCount;
    return g_pool + cursor;
  }

  static void release(const uint8_t* memory) {
    for (uint8_t i = 0; i < g_blockCount; ++i) {
      if (g_pool + g_blocks[i].at != memory) continue;
      for (uint8_t j = i; j + 1 < g_blockCount; ++j) g_blocks[j] = g_blocks[j + 1];
      --g_blockCount;
      return;
    }
  }
};

uint32_t AppVm::poolUsedBytes() {
  uint32_t used = 0;
  for (uint8_t i = 0; i < g_blockCount; ++i) used += g_blocks[i].len;
  return used;
}

uint32_t AppVm::poolLargestFree() {
  // Compaction makes all free space one block, so that is what can be had.
  return kPoolBytes - poolUsedBytes();
}

// --- loading -----------------------------------------------------------------------

namespace {

// The image is the package's "vm" string: base64, which JSON never escapes.
// Located without copying it into a String -- it is the bulk of the package.
bool findImage(const String& json, const char*& text, size_t& length) {
  const char* body = json.c_str();
  const char* key = strstr(body, "\"vm\"");
  while (key != nullptr) {
    const char* p = key + 4;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == ':') {
      ++p;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
      if (*p != '"') return false;
      ++p;
      const char* end = strchr(p, '"');
      if (end == nullptr) return false;
      text = p;
      length = static_cast<size_t>(end - p);
      return true;
    }
    key = strstr(key + 4, "\"vm\"");
  }
  return false;
}

// Decodes and verifies a package's image into a transient buffer. Install and
// bind time only: the buffer is gone before anything runs.
bool decodeAndVerify(const String& json, uint16_t cellCount, uint16_t declaredCaps,
                     std::unique_ptr<uint8_t[]>& image, size_t& length, VmVerified& verified,
                     String& error) {
  const char* text = nullptr;
  size_t textLen = 0;
  if (!findImage(json, text, textLen)) {
    // Every package built before v2.0.0: a flow graph with no image.
    error = "needs_update";
    return false;
  }
  length = vmBase64DecodedLength(text, textLen);
  if (length == 0 || length > AppVm::kMaxPackageBytes) {
    error = "image_size";
    return false;
  }
  image.reset(new (std::nothrow) uint8_t[length]);
  if (!image) {
    error = "out_of_memory";
    return false;
  }
  if (vmBase64Decode(text, textLen, image.get(), length) != static_cast<int32_t>(length)) {
    error = "image_base64";
    return false;
  }
  const char* reason = "";
  if (!vmVerify(image.get(), length, declaredCaps, cellCount, verified, reason)) {
    error = reason;
    return false;
  }
  return true;
}

uint32_t estimateUs(const VmVerified& verified) {
  return verified.loopFree ? (verified.estimatedNs + 999U) / 1000U : 0;
}

bool withinBudget(const VmVerified& verified, uint32_t budgetUs, String& error) {
  // A loop-free image has a static bound and is held to it, as a flow graph
  // always was. One with loops is held to its time slice instead.
  const uint32_t estimated = estimateUs(verified);
  if (verified.loopFree && estimated > budgetUs) {
    error = String("over_budget:") + String(estimated) + "us>" + String(budgetUs) + "us";
    return false;
  }
  return true;
}

// Persisted counters live in one NVS blob per package, keyed by the id's first
// 12 characters (NVS keys stop at 15). The blob repeats the whole id and the
// version, so a prefix collision or a new version restores nothing rather
// than the wrong numbers. Unchanged from FlowApp: node index and op come from
// the image's node table, which lists the graph's nodes in order.
constexpr uint8_t kPersistMagic = 0x50;  // 'P'
constexpr size_t kPersistIdBytes = 16;
constexpr size_t kPersistVersionBytes = 12;
constexpr size_t kPersistHeaderBytes = 2 + kPersistIdBytes + kPersistVersionBytes;
constexpr size_t kPersistItemBytes = 6;  // node index, op, float value

String persistKey(const char* packageId) {
  String key = "pc_";
  key += String(packageId).substring(0, 12);
  return key;
}

constexpr uint16_t kSlotAllowedCaps = kAppCapReadMatrix | kAppCapReadImu | kAppCapEmitEvent |
                                      kAppCapDriveLed | kAppCapTick | kAppCapBudget |
                                      kAppCapButton | kAppCapDisplay | kAppCapExtLed |
                                      kAppCapReadMag | kAppCapPower | kAppCapLink | kAppCapPersist;

uint32_t clockUs() { return micros(); }

}  // namespace

AppVm::AppVm() {
  manifest_.name = nameBuf_;
  manifest_.version = versionBuf_;
  manifest_.summary = summaryBuf_;
  manifest_.capabilities = kAppCapReadMatrix | kAppCapEmitEvent;
  manifest_.frameBudgetUs = kDefaultBudgetUs;
  strncpy(nameBuf_, "flow", sizeof(nameBuf_) - 1);
  strncpy(versionBuf_, "1.0.0", sizeof(versionBuf_) - 1);
  strncpy(summaryBuf_, "Runs an app image loaded from a package.", sizeof(summaryBuf_) - 1);
}

void AppVm::setIdentity(const char* name, uint32_t budgetUs) {
  strncpy(nameBuf_, name != nullptr ? name : "flow", sizeof(nameBuf_) - 1);
  nameBuf_[sizeof(nameBuf_) - 1] = '\0';
  manifest_.frameBudgetUs = budgetUs;
}

void AppVm::applyPackageManifest(const char* version, const char* summary,
                                 const char* packageId, uint16_t capabilities) {
  if (version != nullptr) {
    strncpy(versionBuf_, version, sizeof(versionBuf_) - 1);
    versionBuf_[sizeof(versionBuf_) - 1] = '\0';
  }
  if (summary != nullptr) {
    strncpy(summaryBuf_, summary, sizeof(summaryBuf_) - 1);
    summaryBuf_[sizeof(summaryBuf_) - 1] = '\0';
  }
  if (packageId != nullptr) {
    strncpy(packageId_, packageId, sizeof(packageId_) - 1);
    packageId_[sizeof(packageId_) - 1] = '\0';
  }
  // Never widened: a package cannot grant itself a permission the host slot
  // does not already allow.
  manifest_.capabilities = capabilities & kSlotAllowedCaps;
  // The package id and capabilities arrive after the image is bound, and
  // both are needed to find -- and be allowed -- the saved values.
  restorePersisted();
}

bool AppVm::dryRun(const String& json, uint16_t cellCount, uint32_t budgetUs,
                   uint16_t declaredCaps, uint32_t& estimatedUs, String& error) {
  std::unique_ptr<uint8_t[]> image;
  size_t length = 0;
  VmVerified verified;
  if (!decodeAndVerify(json, cellCount, declaredCaps, image, length, verified, error)) {
    return false;
  }
  if (!withinBudget(verified, budgetUs, error)) return false;
  // Pool fit is decided at activation, against what else is running.
  if (verified.layout.poolBytes() > kPoolBytes) {
    error = "app_pool_exhausted";
    return false;
  }
  estimatedUs = estimateUs(verified);
  return true;
}

bool AppVm::loadFromJson(const String& json, const String& sourcePath, uint16_t cellCount,
                         uint16_t declaredCaps, String& error) {
  std::unique_ptr<uint8_t[]> image;
  size_t length = 0;
  VmVerified verified;
  if (!decodeAndVerify(json, cellCount, declaredCaps & kSlotAllowedCaps, image, length,
                       verified, error)) {
    return false;
  }
  if (!withinBudget(verified, manifest_.frameBudgetUs, error)) return false;

  // Transactional: the new block is taken while the old image still runs, so
  // a refusal leaves the slot as it was. Only when the pool cannot hold both
  // is the old one given up first.
  const uint32_t need = verified.layout.poolBytes();
  uint8_t* block = AppVmPool::allocate(this, need);
  if (block == nullptr && memory_ != nullptr) {
    unload();
    block = AppVmPool::allocate(this, need);
  }
  if (block == nullptr) {
    error = "app_pool_exhausted";
    return false;
  }
  if (memory_ != nullptr) unload();
  memcpy(block, image.get(), length);
  memory_ = block;
  verified_ = verified;
  machine_.bind(memory_, verified_.layout);
  estimatedUs_ = estimateUs(verified_);
  sourcePath_ = sourcePath;
  graphName_ = jsonExtractString(json, "name", "unnamed");
  fault_ = 0;
  handler_ = -1;
  degraded_ = false;
  framesSkipped_ = 0;
  pendingPresses_ = 0;
  pressShown_ = false;
  return true;
}

bool AppVm::loadFromFile(const String& path, uint16_t cellCount, String& error) {
  if (storage_ == nullptr) {
    error = "storage_unavailable";
    return false;
  }
  std::vector<uint8_t> bytes;
  const size_t size = storage_->fileSize("user", path);
  if (size == 0) {
    error = "file_not_found";
    return false;
  }
  if (size > kMaxPackageBytes) {
    error = "file_too_large";
    return false;
  }
  if (!storage_->readFile("user", path, bytes, 0, size)) {
    error = "file_read_failed";
    return false;
  }
  String json;
  json.reserve(bytes.size() + 1);
  for (uint8_t byte : bytes) {
    json += static_cast<char>(byte);
  }
  if (!loadFromJson(json, path, cellCount, kSlotAllowedCaps, error)) {
    return false;
  }
  // A raw package belongs to no install, so it has nowhere to persist to --
  // and must not write its counters under the key of whatever package this
  // slot held before.
  packageId_[0] = '\0';
  machine_.clearPersistDirty();
  return true;
}

void AppVm::unload() {
  // Written out before the image goes: deactivating keeps a count, so
  // re-activating the package carries on from it.
  savePersisted();
  if (memory_ != nullptr) AppVmPool::release(memory_);
  memory_ = nullptr;
  machine_.unbind();
  verified_ = VmVerified();
  estimatedUs_ = 0;
  degraded_ = false;
  fault_ = 0;
  handler_ = -1;
  pendingPresses_ = 0;
  pressShown_ = false;
  framesSkipped_ = 0;
  packageId_[0] = '\0';
  sourcePath_ = "";
  graphName_ = "";
  framesSeen_ = false;
  ticks_ = 0;
}

void AppVm::relocate(uint8_t* memory) {
  memory_ = memory;
  machine_.relocate(memory);
}

bool AppVm::start() { return true; }

// --- persistence -------------------------------------------------------------------

bool AppVm::persists() const {
  if (storage_ == nullptr || (manifest_.capabilities & kAppCapPersist) == 0 ||
      packageId_[0] == '\0' || !loaded()) {
    return false;
  }
  for (uint8_t i = 0; i < machine_.nodeCount(); ++i) {
    if ((machine_.node(i).flags & 1) != 0) return true;
  }
  return false;
}

void AppVm::restorePersisted() {
  machine_.clearPersistDirty();
  if (!persists()) {
    return;
  }
  uint8_t blob[kPersistHeaderBytes + kMaxNodes * kPersistItemBytes];
  const String key = persistKey(packageId_);
  const size_t size = storage_->prefs().getBytesLength(key.c_str());
  if (size < kPersistHeaderBytes || size > sizeof(blob)) {
    return;
  }
  if (storage_->prefs().getBytes(key.c_str(), blob, size) != size || blob[0] != kPersistMagic) {
    return;
  }
  char id[kPersistIdBytes + 1] = {0};
  char version[kPersistVersionBytes + 1] = {0};
  memcpy(id, blob + 2, kPersistIdBytes);
  memcpy(version, blob + 2 + kPersistIdBytes, kPersistVersionBytes);
  // A new version may have renumbered its nodes; starting from zero is the
  // honest outcome, and the one the App Library documents.
  if (strcmp(id, packageId_) != 0 || strcmp(version, versionBuf_) != 0) {
    return;
  }
  const uint8_t count = blob[1];
  if (kPersistHeaderBytes + static_cast<size_t>(count) * kPersistItemBytes > size) {
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t* item = blob + kPersistHeaderBytes + i * kPersistItemBytes;
    const uint8_t index = item[0];
    if (index >= machine_.nodeCount()) continue;
    const VmNodeEntry node = machine_.node(index);
    if ((node.flags & 1) == 0 || node.op != item[1]) continue;
    float value = 0;
    memcpy(&value, item + 2, sizeof(value));
    machine_.setSlot(node.slot, value);
  }
}

void AppVm::savePersisted() {
  if (!machine_.persistDirty() || !persists()) {
    return;
  }
  uint8_t blob[kPersistHeaderBytes + kMaxNodes * kPersistItemBytes] = {0};
  blob[0] = kPersistMagic;
  strncpy(reinterpret_cast<char*>(blob + 2), packageId_, kPersistIdBytes);
  strncpy(reinterpret_cast<char*>(blob + 2 + kPersistIdBytes), versionBuf_, kPersistVersionBytes);
  uint8_t count = 0;
  for (uint8_t i = 0; i < machine_.nodeCount(); ++i) {
    const VmNodeEntry node = machine_.node(i);
    if ((node.flags & 1) == 0) continue;
    uint8_t* item = blob + kPersistHeaderBytes + count * kPersistItemBytes;
    item[0] = i;
    item[1] = node.op;
    const float value = machine_.slot(node.slot);
    memcpy(item + 2, &value, sizeof(float));
    ++count;
  }
  blob[1] = count;
  storage_->prefs().putBytes(persistKey(packageId_).c_str(), blob,
                             kPersistHeaderBytes + count * kPersistItemBytes);
  machine_.clearPersistDirty();
}

void AppVm::clearPersisted(Storage& storage, const char* packageId) {
  const String key = persistKey(packageId);
  if (storage.prefs().isKey(key.c_str())) {
    storage.prefs().remove(key.c_str());
  }
}

void AppVm::service(uint32_t nowMs) {
  // At most every kPersistIntervalMs, and only when a count moved: a flash
  // write is milliseconds, so it happens here, untimed, never in a handler.
  if (!machine_.persistDirty() || nowMs - lastPersistMs_ < kPersistIntervalMs) {
    return;
  }
  lastPersistMs_ = nowMs;
  savePersisted();
}

// --- running -----------------------------------------------------------------------

IRAM_ATTR void AppVm::onEvent(const AppEvent& event) {
  if (!loaded() || fault_ != 0) return;
  switch (event.kind) {
    case AppEventKind::Budget:
      // Latched, then read by budget_load/grace_left on the next run.
      machine_.setBudget(event.load, event.graceLeft);
      return;
    case AppEventKind::Button:
      if (pendingPresses_ < kMaxPendingPresses) ++pendingPresses_;
      return;
    case AppEventKind::Tick:
      // A suspended handler resumes on any delivery.
      if (machine_.suspended()) {
        deliver(event, -1);
        return;
      }
      // The scanner is stopped or nothing streams: keep an image that asked
      // for it running at the tick's 10 Hz, with the matrix reads holding.
      // Only while frames are absent, so it never runs twice per frame.
      if ((manifest_.capabilities & kAppCapTick) == 0 || !machine_.hasEntry(VmEvent::IdleTick) ||
          (framesSeen_ && event.nowMs - lastFrameMs_ < kTickFallbackMs)) {
        return;
      }
      ++ticks_;
      deliver(event, static_cast<int8_t>(VmEvent::IdleTick));
      return;
    case AppEventKind::Frame:
      if (event.frame == nullptr) return;
      ++frames_;
      framesSeen_ = true;
      lastFrameMs_ = event.nowMs;
      deliver(event, static_cast<int8_t>(VmEvent::Frame));
      return;
    default:
      return;
  }
}

IRAM_ATTR void AppVm::deliver(const AppEvent& event, int8_t handler) {
#if NHOS_VM_PROFILE
  const uint32_t d0 = vmCycles();
  struct Done {
    VmProfile& p;
    uint32_t t0;
    ~Done() { p.deliverCyc += vmCycles() - t0; ++p.deliveries; }
  } done{machine_.prof, d0};
#endif
  const bool wasSuspended = machine_.suspended();
  if (wasSuspended) {
    // Resuming. A frame that arrives meanwhile is one the frame handler never
    // saw: counted, and reported when the handler ends.
    if (handler == static_cast<int8_t>(VmEvent::Frame) &&
        handler_ == static_cast<int8_t>(VmEvent::Frame)) {
      ++framesSkipped_;
      ++framesSkippedTotal_;
    }
    if (event.nowMs - suspendedSinceMs_ >= kVmStarvedMs) {
      raiseFault(7, event);  // starved
      return;
    }
  } else {
    if (handler < 0 || !machine_.hasEntry(static_cast<VmEvent>(handler))) return;
    bool pressed = false;
    if (pressShown_) {
      pressShown_ = false;
    } else if (pendingPresses_ != 0) {
      --pendingPresses_;
      pressed = true;
      pressShown_ = true;
    }
    handler_ = handler;
    machine_.begin(static_cast<VmEvent>(handler), pressed);
  }

  VmContext ctx;
  ctx.nowMs = event.nowMs;
  ctx.frameSeq = event.frameSeq;
  if (event.frame != nullptr) {
    ctx.values = event.frame->values;
    ctx.rows = event.frame->rows;
    ctx.cols = event.frame->cols != 0 ? event.frame->cols : 1;
    ctx.cells = event.frame->pointCount;
  }
  ctx.imu = event.imuSample;
  ctx.mag = event.magSample;
  ctx.battery = event.batteryPercent;
  ctx.linked = event.linked;

  const uint32_t allocation = event.sliceUs != 0 ? event.sliceUs : manifest_.frameBudgetUs;
  const uint32_t slice = allocation / 100U * kSlicePercent;
  host_ = event.host;
#if NHOS_VM_PROFILE
  const uint32_t r0 = vmCycles();
#endif
  const VmRunResult result = machine_.run(ctx, *this, UINT32_MAX, clockUs, clockUs() + slice);
#if NHOS_VM_PROFILE
  machine_.prof.runCyc += vmCycles() - r0;
#endif
  host_ = nullptr;

  if (result == VmRunResult::Fault) {
    raiseFault(machine_.fault(), event);
    return;
  }
  if (result == VmRunResult::Suspended) {
    if (!wasSuspended) suspendedSinceMs_ = event.nowMs;
    ++suspensions_;
    return;
  }
  handler_ = -1;
  // Degradation is recorded, not silent. On a research instrument a signal
  // that quietly changes fidelity because another app was enabled would put a
  // step change in the data that has nothing to do with the subject.
  if (machine_.runDegraded() != degraded_) {
    degraded_ = machine_.runDegraded();
    if (degraded_) ++degradations_;
    if (event.host != nullptr) {
      event.host->emitEvent(manifest_.name, "degraded", degraded_ ? "rise" : "fall");
    }
  }
  if (framesSkipped_ > 0) {
    if (event.host != nullptr) {
      event.host->emitValue(manifest_.name, "frames_skipped", static_cast<float>(framesSkipped_));
    }
    framesSkipped_ = 0;
  }
}

void AppVm::raiseFault(uint8_t fault, const AppEvent& event) {
  fault_ = fault != 0 ? fault : 6;  // native_fault when unspecified
  handler_ = -1;
  suspendedSinceMs_ = 0;
  if (event.host != nullptr) {
    const uint8_t count = sizeof(kVmFaultNames) / sizeof(kVmFaultNames[0]);
    event.host->emitEvent(manifest_.name, "fault", fault_ < count ? kVmFaultNames[fault_] : "fault");
  }
}

void AppVm::vmEmitEvent(const char* event, bool rise) {
  if (host_ == nullptr) return;
  host_->emitEvent(manifest_.name, event, rise ? "rise" : "fall");
  ++emissions_;
}

void AppVm::vmEmitValue(const char* event, float value) {
  if (host_ == nullptr) return;
  host_->emitValue(manifest_.name, event, value);
  ++emissions_;
}

void AppVm::vmLog(const char* text, float value) {
  if (host_ == nullptr) return;
  host_->logLine(manifest_.name, String(text) + " " + String(value, 3));
}

void AppVm::vmLed(bool on, const uint8_t rgb[3], uint16_t stateSlot) {
  // Pulled by statusLed(); nothing to push.
  (void)on;
  (void)rgb;
  (void)stateSlot;
}

// --- outputs, pulled ----------------------------------------------------------------

bool AppVm::displayLine(uint8_t row, AppDisplayLine& out) const {
  uint8_t op = 0;
  uint16_t state = 0, param = 0;
  if (!loaded() || row >= kOledRows || !machine_.natAt(machine_.displayPc(row), op, state, param)) {
    return false;
  }
  const bool bar = op == static_cast<uint8_t>(FlowOp::OledBar);
  out.kind = bar ? AppDisplayKind::Bar : AppDisplayKind::Text;
  out.label = machine_.string(static_cast<uint16_t>(machine_.constant(param + 1)));
  out.value = machine_.slot(state);
  out.digits = bar ? 0 : static_cast<uint8_t>(machine_.constant(param + 2));
  out.lo = bar ? machine_.constant(param + 2) : 0;
  out.hi = bar ? machine_.constant(param + 3) : 0;
  return true;
}

bool AppVm::statusLed(uint8_t rgb[3]) const {
  if (!loaded() || !machine_.ledHeld()) {
    return false;
  }
  const uint8_t* held = machine_.ledRgb();
  rgb[0] = held[0];
  rgb[1] = held[1];
  rgb[2] = held[2];
  return true;
}

void AppVm::extLedFrame(AppExtLedFrame& out) const {
  if (!loaded()) {
    return;
  }
  uint8_t op = 0;
  uint16_t state = 0, param = 0;
  if (!out.hasMeter && machine_.natAt(machine_.meterPc(), op, state, param)) {
    out.hasMeter = true;
    out.meterValue = machine_.slot(state);
    out.meterLo = machine_.constant(param);
    out.meterHi = machine_.constant(param + 1);
  }
  for (uint8_t pixel = 0; pixel < kMaxAppExtLeds; ++pixel) {
    const uint16_t bit = static_cast<uint16_t>(1U << pixel);
    if ((out.pixelMask & bit) != 0 || !machine_.natAt(machine_.pixelPc(pixel), op, state, param)) {
      continue;
    }
    out.pixelMask |= bit;
    out.pixelRgb[pixel][0] = static_cast<uint8_t>(machine_.constant(param + 1));
    out.pixelRgb[pixel][1] = static_cast<uint8_t>(machine_.constant(param + 2));
    out.pixelRgb[pixel][2] = static_cast<uint8_t>(machine_.constant(param + 3));
  }
}

String AppVm::statusJson(bool withOutputs) const {
  const VmLayout& layout = verified_.layout;
  String json = "{\"graph\":\"";
  json += jsonEscape(graphName_);
  json += "\",\"package\":\"";
  json += jsonEscape(String(packageId_));
  json += "\",\"source\":\"";
  json += jsonEscape(sourcePath_);
  json += "\",\"nodes\":";
  json += String(loaded() ? machine_.nodeCount() : 0);
  json += ",\"estimated_us\":";
  json += String(estimatedUs_);
  json += ",\"budget_us\":";
  json += String(manifest_.frameBudgetUs);
  json += ",\"emissions\":";
  json += String(emissions_);
  json += ",\"frames\":";
  json += String(frames_);
  json += ",\"degraded\":";
  json += degraded_ ? "true" : "false";
  json += ",\"degradations\":";
  json += String(degradations_);
  if (ticks_ != 0) {
    json += ",\"ticks\":";
    json += String(ticks_);
  }
  json += ",\"vm\":{\"pool_bytes\":";
  json += String(loaded() ? layout.poolBytes() : 0);
  json += ",\"code\":";
  json += String(layout.codeLen);
  json += ",\"arena\":";
  json += String(layout.arena);
  json += ",\"loop_free\":";
  json += verified_.loopFree ? "true" : "false";
  json += ",\"suspensions\":";
  json += String(suspensions_);
  json += ",\"frames_skipped\":";
  json += String(framesSkippedTotal_);
#if NHOS_VM_PROFILE
  {
    // PROFILE: averages in cycles per delivery since the last read, then reset.
    VmProfile& p = const_cast<VmMachine&>(machine_).prof;
    const uint32_t n = p.deliveries ? p.deliveries : 1;
    json += ",\"prof\":{\"n\":" + String(p.deliveries);
    json += ",\"deliver\":" + String((uint32_t)(p.deliverCyc / n));
    json += ",\"run\":" + String((uint32_t)(p.runCyc / n));
    json += ",\"nat\":" + String((uint32_t)(p.natCyc / n));
    json += ",\"insns\":" + String(p.insns / n);
    json += ",\"nats\":" + String(p.nats / n);
    json += ",\"ops\":{";
    bool first = true;
    for (uint8_t i = 0; i < kFlowOpCount; ++i) {
      if (p.opCalls[i] == 0) continue;
      if (!first) json += ",";
      first = false;
      json += "\"" + String(i) + "\":[" + String(p.opCalls[i]) + "," + String(p.opCyc[i] / p.opCalls[i]) + "]";
    }
    json += "}}";
    p = VmProfile();
  }
#endif
  json += ",\"fault\":\"";
  const uint8_t faults = sizeof(kVmFaultNames) / sizeof(kVmFaultNames[0]);
  json += fault_ < faults ? kVmFaultNames[fault_] : "fault";
  json += "\"}";
  if (!withOutputs) {
    json += "}";
    return json;
  }
  json += ",\"outputs\":[";
  for (uint8_t i = 0; loaded() && i < machine_.nodeCount(); ++i) {
    const VmNodeEntry node = machine_.node(i);
    const float value = machine_.slot(node.slot);
    if (i != 0) json += ",";
    json += "{\"op\":\"";
    json += node.op < kFlowOpCount ? kFlowOps[node.op].name : "invalid";
    json += "\",\"result\":";
    json += String(value, 3);
    json += ",\"bool\":";
    json += (node.flags & 2) != 0 && value != 0 ? "true" : "false";
    json += "}";
  }
  json += "]}";
  return json;
}

}  // namespace nhos
