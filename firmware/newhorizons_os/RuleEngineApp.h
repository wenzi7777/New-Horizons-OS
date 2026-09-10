#pragma once

#include "App.h"

namespace nhos {

class Storage;

enum class RuleOp : uint8_t {
  Invalid = 0,
  Total,        // sum of every cell
  Peak,         // largest cell
  RegionSum,    // sum over a rectangle
  ActiveCells,  // count of cells at or above `threshold`
  Threshold,    // bool: input >= value, with hysteresis
  Debounce,     // bool: input held steady for `ms`
  Emit,         // fires a named event on a rising/falling edge of a bool
};

// One node of a rule graph. Nodes may only reference nodes declared before
// them, so the array order IS a valid evaluation order and no cycle can be
// expressed in the first place.
struct RuleNode {
  RuleOp op = RuleOp::Invalid;
  int8_t input = -1;  // index of an earlier node, or -1
  float value = 0;
  float hysteresis = 0;
  uint16_t ms = 0;
  uint8_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;
  char event[24] = {0};

  // Evaluation state
  float result = 0;
  bool boolResult = false;
  bool lastBool = false;
  uint32_t sinceMs = 0;
};

// Declarative apps: a rule graph loaded from a file, not compiled in.
//
// This is the model that covers most of what the device is actually asked to
// do on-board -- region thresholds, contact events, feature reduction --
// without an interpreter. The reason to prefer it over a scripting VM here is
// that its cost is STATICALLY KNOWN: every operator has a fixed per-frame
// cost, so the total is computed at load time and a graph that would not fit
// the frame budget is rejected then, rather than discovered as dropped frames
// later. There is no runtime allocation and nothing to sandbox.
class RuleEngineApp : public App {
 public:
  static constexpr uint8_t kMaxNodes = 12;
  // Per-cell cost of a sweep operator, and flat cost of a scalar one.
  // Deliberate over-estimates -- the point is a bound, not a prediction.
  static constexpr uint32_t kCellOpNsPerCell = 60;
  static constexpr uint32_t kScalarOpNs = 400;

  void attach(Storage& storage) { storage_ = &storage; }
  // Loads a graph from the given user-scope path. Returns false and leaves
  // any previously loaded graph untouched if the file is missing, malformed,
  // or too expensive for the budget.
  bool loadFromFile(const String& path, uint16_t cellCount, String& error);
  bool loaded() const { return nodeCount_ > 0; }
  void unload();
  uint32_t estimatedUs() const { return estimatedUs_; }

  const AppManifest& manifest() const override;
  bool start() override;
  void onFrame(const AppFrameContext& context) override;
  String statusJson() const override;

  static RuleOp opFromName(const String& name);
  static const char* opName(RuleOp op);

 private:
  bool parse(const String& json, uint16_t cellCount, String& error);
  uint32_t estimateUs(uint16_t cellCount) const;
  void evaluate(const AppFrameContext& context);

  Storage* storage_ = nullptr;
  RuleNode nodes_[kMaxNodes];
  uint8_t nodeCount_ = 0;
  uint32_t estimatedUs_ = 0;
  uint32_t emissions_ = 0;
  uint32_t frames_ = 0;
  String sourcePath_;
  String graphName_;
};

}  // namespace nhos
