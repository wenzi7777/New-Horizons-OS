#pragma once

#include "App.h"

namespace nhos {

class Storage;

// The operators a flow graph can use.
//
// Named Flow rather than Rule because that is what it is: a synchronous
// dataflow graph, evaluated start to finish once per event. It does no pattern
// matching, holds no facts and draws no inferences, so calling it a rule
// engine set an expectation nothing here meets.
enum class FlowOp : uint8_t {
  Invalid = 0,
  // --- v1.0.0 ---
  Total,        // sum of every cell
  Peak,         // largest cell
  RegionSum,    // sum over a rectangle
  ActiveCells,  // count of cells at or above `value`
  Threshold,    // bool: input >= value, with hysteresis
  Debounce,     // bool: input held steady for `ms`
  Emit,         // fires a named event on an edge of a bool
  // --- v1.1.0: arithmetic ---
  Const, Add, Sub, Mul, Div, Min, Max, Abs, Clamp,
  // --- v1.1.0: time series ---
  Mean, MaxHold, Delta, Integrate, Counter,
  // --- v1.1.0: richer sweeps ---
  Features, FeatureGet, ArgMax, RowCentroid, ColCentroid,
  // --- v1.1.0: output ---
  Led, EmitValue,
  // --- v1.1.0: conditionals, for self-degradation ---
  Select, Gate, BudgetLoad, GraceLeft,
};

// Fields a single Features sweep produces, in wire order.
enum class FeatureField : uint8_t {
  TotalForce = 0, Peak, Peak01, PeakIndex, ActiveCells, CentroidRow, CentroidCol, InContact,
  Count,
};

// One node of a flow graph. Nodes may only reference nodes declared before
// them, so the array order IS a valid evaluation order and no cycle can be
// expressed in the first place.
struct FlowNode {
  FlowOp op = FlowOp::Invalid;
  int8_t input = -1;   // index of an earlier node, or -1
  int8_t input2 = -1;  // second operand, for the binary operators
  int8_t input3 = -1;  // third operand, for select()
  float value = 0;
  float hysteresis = 0;
  float lo = 0, hi = 0;
  uint16_t ms = 0;
  uint16_t window = 0;     // frames of history, for the time-series operators
  uint16_t windowAt = 0;   // this node's slice of the shared ring pool
  uint8_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
  uint8_t field = 0;       // FeatureField, for FeatureGet
  uint8_t span = 0;        // Gate: how many FOLLOWING nodes it may skip
  uint8_t rgb[3] = {0, 0, 0};
  char event[24] = {0};

  // Evaluation state
  float result = 0;
  bool boolResult = false;
  bool lastBool = false;
  bool skipped = false;
  uint32_t sinceMs = 0;
  uint16_t filled = 0;   // valid samples in the ring
  uint16_t cursor = 0;
};

// Declarative apps: a flow graph loaded from a package, not compiled in.
//
// This covers most of what the device is actually asked to do on-board --
// region thresholds, contact events, feature reduction, ratios -- without an
// interpreter. The reason to prefer it over a scripting VM is that its cost is
// STATICALLY BOUNDED: every operator has a fixed per-frame cost, so the total
// is computed at load time. There is no runtime allocation and nothing to
// sandbox.
//
// Conditionals exist (Select, Gate) and do not break that bound: worst case is
// simply that nothing is skipped. Only loops would, and there are none.
class FlowApp : public App {
 public:
  // v1.3.0 raised this from 12. What the limit protects is not time -- the
  // per-frame budget below does that -- but RAM and reply size: every slot
  // holds kMaxNodes FlowNodes (~76 bytes each) for as long as it exists, and
  // app_list used to carry every node's output. At 12 a two-region gait app
  // (heel strike, toe off, step count, LED) did not fit, at ~250us of a
  // 1500us budget. The App Library derives min_os v1.3.0 for graphs over 12.
  static constexpr uint8_t kMaxNodes = 24;
  // Per-cell cost of a sweep operator, and flat cost of a scalar one.
  //
  // MEASURED on v1.5.F (ESP32-S3 @ 240MHz, 14x14): a flat sweep runs about
  // 86ns per cell, a features sweep about 235ns, and region_sum -- which does
  // two-dimensional index arithmetic per cell -- considerably more. The
  // original 60/120/400 were guesses and under-estimated by 1.4x to 9x, which
  // made the install-time estimate optimistic exactly where it was relied on.
  // These values carry roughly 2x margin over the measurements.
  //
  // A compatibility contract with the App Library's sdk/lib/opset.mjs: if they
  // drift, an app passes there and is refused here, and the author cannot see
  // the other side. sdk/test/firmware-contract.test.mjs pins them together.
  static constexpr uint32_t kCellOpNsPerCell = 300;
  static constexpr uint32_t kFeaturesNsPerCell = 500;
  static constexpr uint32_t kScalarOpNs = 600;
  // 128 frames is ~2s at 60Hz -- long enough to average a gait cycle, and
  // the pool is per slot, so every float here is paid for four times over.
  static constexpr uint16_t kMaxWindow = 128;
  // Shared ring-buffer pool. Sized at load, never allocated at runtime.
  static constexpr uint16_t kWindowPool = 128;
  static constexpr size_t kMaxPackageBytes = 4096;
  static constexpr uint32_t kDefaultBudgetUs = 1500;

  FlowApp();
  // The manifest points into this object's own buffers, so a copy's manifest
  // would still aim at the original. Forbid it rather than document it.
  FlowApp(const FlowApp&) = delete;
  FlowApp& operator=(const FlowApp&) = delete;

  // Must be called BEFORE AppManager::install(): install() indexes by name and
  // persists app_en_<name>, so the name has to be final by then.
  void setIdentity(const char* name, uint32_t budgetUs = kDefaultBudgetUs);
  // Applied when a package is bound; reverted by unload().
  void applyPackageManifest(const char* version, const char* summary,
                            const char* packageId, uint16_t capabilities);

  void attach(Storage& storage) { storage_ = &storage; }
  bool loadFromFile(const String& path, uint16_t cellCount, String& error);
  // Loads from an already-read body, so the registry can hash and parse one
  // buffer instead of reading the same file twice.
  bool loadFromJson(const String& json, const String& sourcePath,
                    uint16_t cellCount, String& error);
  bool loaded() const { return nodeCount_ > 0; }
  bool idle() const override { return !loaded(); }
  void unload();
  uint32_t estimatedUs() const { return estimatedUs_; }
  const char* packageId() const { return packageId_; }

  const AppManifest& manifest() const override { return manifest_; }
  bool start() override;
  void onEvent(const AppEvent& event) override;
  String statusJson(bool withOutputs) const override;

  // Everything loading would check -- parse, references, windows, budget --
  // without touching any slot. The registry uses it to refuse a package at
  // install time; it needs no FlowApp of its own to do so.
  static bool dryRun(const String& json, uint16_t cellCount, uint32_t budgetUs,
                     uint32_t& estimatedUs, String& error);

  static FlowOp opFromName(const String& name);
  static const char* opName(FlowOp op);
  static bool isSweepOp(FlowOp op);

 private:
  bool parse(const String& json, uint16_t cellCount, String& error);
  static bool parseNodes(const String& json, FlowNode* scratch, uint8_t& count,
                         uint16_t& windowUsed, String& error);
  uint32_t estimateUs(uint16_t cellCount) const;
  static uint32_t estimateNodesUs(const FlowNode* nodes, uint8_t count, uint16_t cellCount);
  void evaluate(const AppEvent& event);
  void computeFeatures(const MatrixFrame& frame);
  float pushWindow(FlowNode& node, float sample, FlowOp op);

  AppManifest manifest_;
  char nameBuf_[12] = {0};
  char versionBuf_[12] = {0};
  char summaryBuf_[64] = {0};
  char packageId_[16] = {0};

  Storage* storage_ = nullptr;
  FlowNode nodes_[kMaxNodes];
  float windowPool_[kWindowPool] = {0};
  uint16_t windowUsed_ = 0;
  float features_[static_cast<uint8_t>(FeatureField::Count)] = {0};
  uint8_t nodeCount_ = 0;
  uint32_t estimatedUs_ = 0;
  uint32_t emissions_ = 0;
  uint32_t frames_ = 0;
  uint32_t degradations_ = 0;
  bool degraded_ = false;
  // Budget pressure, updated by Budget events and read by BudgetLoad/GraceLeft.
  float budgetLoad_ = 0;
  uint8_t graceLeft_ = 0;
  String sourcePath_;
  String graphName_;
};

}  // namespace nhos
