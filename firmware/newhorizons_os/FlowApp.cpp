#include "FlowApp.h"

#include <math.h>

#include <vector>

#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {
namespace {

// Splits a JSON array body into its top-level {...} objects.
void splitObjects(const String& arrayBody, std::vector<String>& out) {
  int depth = 0;
  int start = -1;
  bool inString = false;
  bool escaped = false;
  for (unsigned int i = 0; i < arrayBody.length(); ++i) {
    const char c = arrayBody[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      if (depth == 0) {
        start = static_cast<int>(i);
      }
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && start >= 0) {
        out.push_back(arrayBody.substring(start, i + 1));
        start = -1;
      }
    }
  }
}

uint8_t featureFieldFromName(const String& name) {
  if (name == "total_force") return static_cast<uint8_t>(FeatureField::TotalForce);
  if (name == "peak") return static_cast<uint8_t>(FeatureField::Peak);
  if (name == "peak01") return static_cast<uint8_t>(FeatureField::Peak01);
  if (name == "peak_index") return static_cast<uint8_t>(FeatureField::PeakIndex);
  if (name == "active_cells") return static_cast<uint8_t>(FeatureField::ActiveCells);
  if (name == "centroid_row") return static_cast<uint8_t>(FeatureField::CentroidRow);
  if (name == "centroid_col") return static_cast<uint8_t>(FeatureField::CentroidCol);
  if (name == "in_contact") return static_cast<uint8_t>(FeatureField::InContact);
  return 0xFF;
}

// The scale constants live in MatrixScanner.h and are shared with the scanner
// itself; the extractor this replaces used exactly these. Redefining them here
// would silently change every centroid and peak01 the device reports.
//
// The centroid weights only cells at or above the contact threshold, so it is
// the centre of FORCE and not the centre of the patch's bounding box.

}  // namespace

FlowApp::FlowApp() {
  manifest_.name = nameBuf_;
  manifest_.version = versionBuf_;
  manifest_.summary = summaryBuf_;
  manifest_.capabilities = kAppCapReadMatrix | kAppCapEmitEvent;
  manifest_.frameBudgetUs = kDefaultBudgetUs;
  strncpy(nameBuf_, "flow", sizeof(nameBuf_) - 1);
  strncpy(versionBuf_, "1.0.0", sizeof(versionBuf_) - 1);
  strncpy(summaryBuf_, "Runs a flow graph loaded from a package.", sizeof(summaryBuf_) - 1);
}

void FlowApp::setIdentity(const char* name, uint32_t budgetUs) {
  strncpy(nameBuf_, name != nullptr ? name : "flow", sizeof(nameBuf_) - 1);
  nameBuf_[sizeof(nameBuf_) - 1] = '\0';
  manifest_.frameBudgetUs = budgetUs;
}

void FlowApp::applyPackageManifest(const char* version, const char* summary,
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
  const uint16_t allowed = kAppCapReadMatrix | kAppCapReadImu | kAppCapEmitEvent |
                           kAppCapDriveLed | kAppCapTick | kAppCapBudget;
  manifest_.capabilities = capabilities & allowed;
}

FlowOp FlowApp::opFromName(const String& name) {
  if (name == "total") return FlowOp::Total;
  if (name == "peak") return FlowOp::Peak;
  if (name == "region_sum") return FlowOp::RegionSum;
  if (name == "active_cells") return FlowOp::ActiveCells;
  if (name == "threshold") return FlowOp::Threshold;
  if (name == "debounce") return FlowOp::Debounce;
  if (name == "emit") return FlowOp::Emit;
  if (name == "const") return FlowOp::Const;
  if (name == "add") return FlowOp::Add;
  if (name == "sub") return FlowOp::Sub;
  if (name == "mul") return FlowOp::Mul;
  if (name == "div") return FlowOp::Div;
  if (name == "min") return FlowOp::Min;
  if (name == "max") return FlowOp::Max;
  if (name == "abs") return FlowOp::Abs;
  if (name == "clamp") return FlowOp::Clamp;
  if (name == "mean") return FlowOp::Mean;
  if (name == "max_hold") return FlowOp::MaxHold;
  if (name == "delta") return FlowOp::Delta;
  if (name == "integrate") return FlowOp::Integrate;
  if (name == "counter") return FlowOp::Counter;
  if (name == "features") return FlowOp::Features;
  if (name == "feature_get") return FlowOp::FeatureGet;
  if (name == "arg_max") return FlowOp::ArgMax;
  if (name == "row_centroid") return FlowOp::RowCentroid;
  if (name == "col_centroid") return FlowOp::ColCentroid;
  if (name == "led") return FlowOp::Led;
  if (name == "emit_value") return FlowOp::EmitValue;
  if (name == "select") return FlowOp::Select;
  if (name == "gate") return FlowOp::Gate;
  if (name == "budget_load") return FlowOp::BudgetLoad;
  if (name == "grace_left") return FlowOp::GraceLeft;
  return FlowOp::Invalid;
}

const char* FlowApp::opName(FlowOp op) {
  switch (op) {
    case FlowOp::Total: return "total";
    case FlowOp::Peak: return "peak";
    case FlowOp::RegionSum: return "region_sum";
    case FlowOp::ActiveCells: return "active_cells";
    case FlowOp::Threshold: return "threshold";
    case FlowOp::Debounce: return "debounce";
    case FlowOp::Emit: return "emit";
    case FlowOp::Const: return "const";
    case FlowOp::Add: return "add";
    case FlowOp::Sub: return "sub";
    case FlowOp::Mul: return "mul";
    case FlowOp::Div: return "div";
    case FlowOp::Min: return "min";
    case FlowOp::Max: return "max";
    case FlowOp::Abs: return "abs";
    case FlowOp::Clamp: return "clamp";
    case FlowOp::Mean: return "mean";
    case FlowOp::MaxHold: return "max_hold";
    case FlowOp::Delta: return "delta";
    case FlowOp::Integrate: return "integrate";
    case FlowOp::Counter: return "counter";
    case FlowOp::Features: return "features";
    case FlowOp::FeatureGet: return "feature_get";
    case FlowOp::ArgMax: return "arg_max";
    case FlowOp::RowCentroid: return "row_centroid";
    case FlowOp::ColCentroid: return "col_centroid";
    case FlowOp::Led: return "led";
    case FlowOp::EmitValue: return "emit_value";
    case FlowOp::Select: return "select";
    case FlowOp::Gate: return "gate";
    case FlowOp::BudgetLoad: return "budget_load";
    case FlowOp::GraceLeft: return "grace_left";
    case FlowOp::Invalid:
    default: return "invalid";
  }
}

bool FlowApp::isSweepOp(FlowOp op) {
  return op == FlowOp::Total || op == FlowOp::Peak || op == FlowOp::RegionSum ||
         op == FlowOp::ActiveCells || op == FlowOp::Features || op == FlowOp::ArgMax ||
         op == FlowOp::RowCentroid || op == FlowOp::ColCentroid;
}

namespace {

// One parse buffer for every slot. Static rather than on the stack: at 24
// nodes it is ~1.8 KB, and graphs are parsed inside a command handler on the
// 8 KB loop task, beside the JSON strings. Every graph load happens on that
// one task (nothing here runs from an ISR or another task), so it is never
// shared between two parses.
FlowNode g_parseScratch[FlowApp::kMaxNodes];

}  // namespace

uint32_t FlowApp::estimateUs(uint16_t cellCount) const {
  return estimateNodesUs(nodes_, nodeCount_, cellCount);
}

uint32_t FlowApp::estimateNodesUs(const FlowNode* nodes, uint8_t count, uint16_t cellCount) {
  uint64_t totalNs = 0;
  for (uint8_t i = 0; i < count; ++i) {
    // Gate is charged as if it never skips: costing the average would
    // understate the bound on exactly the frames that matter.
    if (nodes[i].op == FlowOp::Features) {
      totalNs += static_cast<uint64_t>(cellCount) * kFeaturesNsPerCell;
    } else if (isSweepOp(nodes[i].op)) {
      totalNs += static_cast<uint64_t>(cellCount) * kCellOpNsPerCell;
    } else {
      totalNs += kScalarOpNs;
    }
  }
  return static_cast<uint32_t>((totalNs + 999) / 1000);
}

bool FlowApp::parseNodes(const String& json, FlowNode* scratch, uint8_t& count,
                         uint16_t& windowUsed, String& error) {
  count = 0;
  windowUsed = 0;
  String arrayBody;
  if (!jsonExtractArray(json, "nodes", arrayBody)) {
    error = "missing_nodes";
    return false;
  }
  std::vector<String> objects;
  splitObjects(arrayBody, objects);
  if (objects.empty()) {
    error = "empty_graph";
    return false;
  }
  if (objects.size() > kMaxNodes) {
    error = "too_many_nodes";
    return false;
  }

  for (const String& object : objects) {
    FlowNode& node = scratch[count];
    node = FlowNode();
    const String opText = jsonExtractString(object, "op", "");
    node.op = opFromName(opText);
    if (node.op == FlowOp::Invalid) {
      error = String("unknown_op:") + opText;
      return false;
    }

    // "in" is either a single earlier index or a list of them.
    String inputArray;
    if (jsonExtractArray(object, "in", inputArray)) {
      int8_t* targets[3] = {&node.input, &node.input2, &node.input3};
      uint8_t parsed = 0;
      int cursor = 0;
      while (parsed < 3 && cursor < static_cast<int>(inputArray.length())) {
        // The captured value still carries its enclosing brackets, so they
        // have to be skipped like any other separator -- stopping at '['
        // silently left every multi-input operator with no inputs at all.
        while (cursor < static_cast<int>(inputArray.length()) &&
               (inputArray[cursor] == ' ' || inputArray[cursor] == ',' ||
                inputArray[cursor] == '[' || inputArray[cursor] == ']')) {
          ++cursor;
        }
        int start = cursor;
        while (cursor < static_cast<int>(inputArray.length()) &&
               inputArray[cursor] >= '0' && inputArray[cursor] <= '9') {
          ++cursor;
        }
        if (cursor == start) {
          break;
        }
        const long value = inputArray.substring(start, cursor).toInt();
        if (value < 0 || value >= count) {
          error = "input_out_of_order";
          return false;
        }
        *targets[parsed++] = static_cast<int8_t>(value);
      }
    } else {
      long input = -1;
      if (jsonExtractInt(object, "in", input)) {
        // Backward references only: this is what makes a cycle unrepresentable
        // and the array order a valid evaluation order.
        if (input < 0 || input >= count) {
          error = "input_out_of_order";
          return false;
        }
        node.input = static_cast<int8_t>(input);
      }
    }

    float number = 0;
    if (jsonExtractFloat(object, "value", number)) node.value = number;
    if (jsonExtractFloat(object, "hysteresis", number)) node.hysteresis = number;
    if (jsonExtractFloat(object, "lo", number)) node.lo = number;
    if (jsonExtractFloat(object, "hi", number)) node.hi = number;
    long integer = 0;
    if (jsonExtractInt(object, "ms", integer) && integer > 0) {
      node.ms = static_cast<uint16_t>(integer);
    }
    if (jsonExtractInt(object, "span", integer) && integer > 0) {
      node.span = static_cast<uint8_t>(integer);
    }
    long coord = 0;
    if (jsonExtractInt(object, "r0", coord)) node.r0 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "c0", coord)) node.c0 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "r1", coord)) node.r1 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "c1", coord)) node.c1 = static_cast<uint8_t>(coord);
    const String event = jsonExtractString(object, "event", "");
    strncpy(node.event, event.c_str(), sizeof(node.event) - 1);
    node.event[sizeof(node.event) - 1] = '\0';

    const String rgb = jsonExtractString(object, "rgb", "");
    if (rgb.length() > 0) {
      // A symbolic colour keeps the package readable; the palette is tiny on
      // purpose, since an app driving arbitrary colour is not a use case yet.
      if (rgb == "red") { node.rgb[0] = 255; }
      else if (rgb == "green") { node.rgb[1] = 255; }
      else if (rgb == "blue") { node.rgb[2] = 255; }
      else if (rgb == "white") { node.rgb[0] = node.rgb[1] = node.rgb[2] = 255; }
      else if (rgb == "off") { }
      else {
        error = String("unknown_colour:") + rgb;
        return false;
      }
    }

    if (node.op == FlowOp::FeatureGet) {
      const String fieldName = jsonExtractString(object, "field", "");
      node.field = featureFieldFromName(fieldName);
      if (node.field == 0xFF) {
        error = String("unknown_feature_field:") + fieldName;
        return false;
      }
      if (node.input < 0 || scratch[node.input].op != FlowOp::Features) {
        error = "feature_get_input_must_be_features";
        return false;
      }
    }

    // Ring buffers are reserved here, at load time, from a fixed pool. There
    // is no runtime allocation anywhere in this engine.
    if (node.op == FlowOp::Mean || node.op == FlowOp::MaxHold || node.op == FlowOp::Integrate) {
      if (!jsonExtractInt(object, "window", integer) || integer <= 0 || integer > kMaxWindow) {
        error = "invalid_window";
        return false;
      }
      node.window = static_cast<uint16_t>(integer);
      if (windowUsed + node.window > kWindowPool) {
        error = String("window_pool_exhausted:") + String(windowUsed + node.window) + ">" +
                String(kWindowPool);
        return false;
      }
      node.windowAt = windowUsed;
      windowUsed = static_cast<uint16_t>(windowUsed + node.window);
    }

    // Operators that consume a value must actually have one.
    uint8_t needed = 0;
    switch (node.op) {
      case FlowOp::Threshold: case FlowOp::Debounce: case FlowOp::Emit:
      case FlowOp::Abs: case FlowOp::Clamp: case FlowOp::Mean: case FlowOp::MaxHold:
      case FlowOp::Delta: case FlowOp::Integrate: case FlowOp::Counter:
      case FlowOp::FeatureGet: case FlowOp::Led:
        needed = 1; break;
      case FlowOp::Add: case FlowOp::Sub: case FlowOp::Mul: case FlowOp::Div:
      case FlowOp::Min: case FlowOp::Max: case FlowOp::EmitValue: case FlowOp::Gate:
        needed = 2; break;
      case FlowOp::Select:
        needed = 3; break;
      default:
        needed = 0; break;
    }
    if ((needed >= 1 && node.input < 0) || (needed >= 2 && node.input2 < 0) ||
        (needed >= 3 && node.input3 < 0)) {
      error = String("missing_input:") + opName(node.op);
      return false;
    }
    if ((node.op == FlowOp::Emit || node.op == FlowOp::EmitValue) && node.event[0] == '\0') {
      error = "missing_event_name";
      return false;
    }
    ++count;
  }
  return true;
}

bool FlowApp::dryRun(const String& json, uint16_t cellCount, uint32_t budgetUs,
                     uint32_t& estimatedUs, String& error) {
  uint8_t count = 0;
  uint16_t windowUsed = 0;
  if (!parseNodes(json, g_parseScratch, count, windowUsed, error)) {
    return false;
  }
  estimatedUs = estimateNodesUs(g_parseScratch, count, cellCount);
  if (estimatedUs > budgetUs) {
    error = String("over_budget:") + String(estimatedUs) + "us>" + String(budgetUs) + "us";
    return false;
  }
  return true;
}

bool FlowApp::parse(const String& json, uint16_t cellCount, String& error) {
  // Parsed into scratch first, so a rejected graph leaves the running one
  // alone -- same transactional shape as a calibration session.
  uint8_t count = 0;
  uint16_t windowUsed = 0;
  if (!parseNodes(json, g_parseScratch, count, windowUsed, error)) {
    return false;
  }

  // Cost check before commit. This is the whole safety argument for the
  // declarative model: an over-budget graph is refused at load time. Checked
  // on the scratch copy: committing first, as before v1.3.0, overwrote the
  // running graph's nodes with a graph that was then refused.
  const uint32_t estimated = estimateNodesUs(g_parseScratch, count, cellCount);
  if (estimated > manifest_.frameBudgetUs) {
    error = String("over_budget:") + String(estimated) + "us>" +
            String(manifest_.frameBudgetUs) + "us";
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    nodes_[i] = g_parseScratch[i];
  }
  nodeCount_ = count;
  estimatedUs_ = estimated;
  windowUsed_ = windowUsed;
  for (uint16_t i = 0; i < kWindowPool; ++i) {
    windowPool_[i] = 0;
  }
  graphName_ = jsonExtractString(json, "name", "unnamed");
  return true;
}

bool FlowApp::loadFromFile(const String& path, uint16_t cellCount, String& error) {
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
  return loadFromJson(json, path, cellCount, error);
}

bool FlowApp::loadFromJson(const String& json, const String& sourcePath,
                           uint16_t cellCount, String& error) {
  if (!parse(json, cellCount, error)) {
    return false;
  }
  sourcePath_ = sourcePath;
  return true;
}

void FlowApp::unload() {
  nodeCount_ = 0;
  estimatedUs_ = 0;
  windowUsed_ = 0;
  degraded_ = false;
  packageId_[0] = '\0';
  sourcePath_ = "";
  graphName_ = "";
}

bool FlowApp::start() { return true; }

float FlowApp::pushWindow(FlowNode& node, float sample, FlowOp op) {
  float* ring = &windowPool_[node.windowAt];
  ring[node.cursor] = sample;
  node.cursor = static_cast<uint16_t>((node.cursor + 1) % node.window);
  if (node.filled < node.window) {
    ++node.filled;
  }
  if (op == FlowOp::MaxHold) {
    float peak = ring[0];
    for (uint16_t i = 1; i < node.filled; ++i) {
      if (ring[i] > peak) peak = ring[i];
    }
    return peak;
  }
  float sum = 0;
  for (uint16_t i = 0; i < node.filled; ++i) {
    sum += ring[i];
  }
  // Integrate reports the accumulated total over the window; mean divides it.
  return op == FlowOp::Integrate ? sum : (node.filled > 0 ? sum / node.filled : 0);
}

void FlowApp::computeFeatures(const MatrixFrame& frame) {
  const uint16_t cells = frame.pointCount;
  float total = 0;
  float peak = 0;
  uint16_t peakIndex = 0;
  uint16_t active = 0;
  float weightedRow = 0;
  float weightedCol = 0;
  for (uint16_t i = 0; i < cells; ++i) {
    const float value = frame.values[i];
    total += value;
    if (value > peak) {
      peak = value;
      peakIndex = i;
    }
    if (value >= kPressureActiveThreshold) {
      ++active;
      // Weight by pressure, so the centroid is the centre of *force*, not the
      // centre of the contact patch's bounding box.
      weightedRow += value * static_cast<float>(i / (frame.cols ? frame.cols : 1));
      weightedCol += value * static_cast<float>(i % (frame.cols ? frame.cols : 1));
    }
  }
  float peak01 = peak / kPressureFullScale;
  if (peak01 > 1.0f) peak01 = 1.0f;
  features_[static_cast<uint8_t>(FeatureField::TotalForce)] = total;
  features_[static_cast<uint8_t>(FeatureField::Peak)] = peak;
  features_[static_cast<uint8_t>(FeatureField::Peak01)] = peak01;
  features_[static_cast<uint8_t>(FeatureField::PeakIndex)] = static_cast<float>(peakIndex);
  features_[static_cast<uint8_t>(FeatureField::ActiveCells)] = static_cast<float>(active);
  features_[static_cast<uint8_t>(FeatureField::CentroidRow)] = total > 0 ? weightedRow / total : 0;
  features_[static_cast<uint8_t>(FeatureField::CentroidCol)] = total > 0 ? weightedCol / total : 0;
  features_[static_cast<uint8_t>(FeatureField::InContact)] = active > 0 ? 1.0f : 0.0f;
}

void FlowApp::evaluate(const AppEvent& event) {
  const MatrixFrame* frame = event.frame;
  const uint16_t cells = frame != nullptr ? frame->pointCount : 0;
  uint8_t skipUntil = 0;
  bool skippedAny = false;

  for (uint8_t i = 0; i < nodeCount_; ++i) {
    FlowNode& node = nodes_[i];
    if (i < skipUntil) {
      // Held at its previous value rather than zeroed, so a gated branch
      // resumes from where it was instead of glitching through zero.
      node.skipped = true;
      skippedAny = true;
      continue;
    }
    node.skipped = false;

    switch (node.op) {
      case FlowOp::Total: {
        float sum = 0;
        for (uint16_t c = 0; c < cells; ++c) sum += frame->values[c];
        node.result = sum;
        break;
      }
      case FlowOp::Peak: {
        float peak = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          if (frame->values[c] > peak) peak = frame->values[c];
        }
        node.result = peak;
        break;
      }
      case FlowOp::RegionSum: {
        float sum = 0;
        for (uint16_t r = node.r0; r <= node.r1 && frame != nullptr && r < frame->rows; ++r) {
          for (uint16_t c = node.c0; c <= node.c1 && c < frame->cols; ++c) {
            const uint16_t index = r * frame->cols + c;
            if (index < cells) sum += frame->values[index];
          }
        }
        node.result = sum;
        break;
      }
      case FlowOp::ActiveCells: {
        uint16_t active = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          if (frame->values[c] >= node.value) ++active;
        }
        node.result = static_cast<float>(active);
        break;
      }
      case FlowOp::ArgMax: {
        float peak = 0;
        uint16_t index = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          if (frame->values[c] > peak) { peak = frame->values[c]; index = c; }
        }
        node.result = static_cast<float>(index);
        break;
      }
      case FlowOp::RowCentroid:
      case FlowOp::ColCentroid: {
        float total = 0;
        float weighted = 0;
        const uint16_t cols = (frame != nullptr && frame->cols) ? frame->cols : 1;
        for (uint16_t c = 0; c < cells; ++c) {
          const float value = frame->values[c];
          if (value < kPressureActiveThreshold) continue;
          total += value;
          weighted += value * static_cast<float>(node.op == FlowOp::RowCentroid ? c / cols
                                                                                : c % cols);
        }
        node.result = total > 0 ? weighted / total : 0;
        break;
      }
      case FlowOp::Features: {
        if (frame != nullptr) computeFeatures(*frame);
        node.result = features_[static_cast<uint8_t>(FeatureField::TotalForce)];
        break;
      }
      case FlowOp::FeatureGet: {
        node.result = features_[node.field];
        node.boolResult = node.result != 0;
        break;
      }
      case FlowOp::Const: {
        node.result = node.value;
        break;
      }
      case FlowOp::Add: node.result = nodes_[node.input].result + nodes_[node.input2].result; break;
      case FlowOp::Sub: node.result = nodes_[node.input].result - nodes_[node.input2].result; break;
      case FlowOp::Mul: node.result = nodes_[node.input].result * nodes_[node.input2].result; break;
      case FlowOp::Div: {
        const float divisor = nodes_[node.input2].result;
        // Zero rather than NaN: one bad frame must not poison every downstream
        // node for the rest of the session.
        node.result = divisor != 0 ? nodes_[node.input].result / divisor : 0;
        break;
      }
      case FlowOp::Min: {
        const float a = nodes_[node.input].result;
        const float b = nodes_[node.input2].result;
        node.result = a < b ? a : b;
        break;
      }
      case FlowOp::Max: {
        const float a = nodes_[node.input].result;
        const float b = nodes_[node.input2].result;
        node.result = a > b ? a : b;
        break;
      }
      case FlowOp::Abs: node.result = fabsf(nodes_[node.input].result); break;
      case FlowOp::Clamp: {
        float value = nodes_[node.input].result;
        if (value < node.lo) value = node.lo;
        if (value > node.hi) value = node.hi;
        node.result = value;
        break;
      }
      case FlowOp::Mean:
      case FlowOp::MaxHold:
      case FlowOp::Integrate:
        node.result = pushWindow(node, nodes_[node.input].result, node.op);
        break;
      case FlowOp::Delta: {
        const float current = nodes_[node.input].result;
        node.result = current - node.value;
        node.value = current;  // `value` doubles as the previous sample here
        break;
      }
      case FlowOp::Counter: {
        const bool current = nodes_[node.input].boolResult;
        if (current && !node.lastBool) node.result += 1.0f;
        node.lastBool = current;
        break;
      }
      case FlowOp::Threshold: {
        const float input = nodes_[node.input].result;
        // Hysteresis: once latched, hold until the input falls below
        // value - hysteresis. Without it a signal sitting on the threshold
        // emits an event every single frame.
        const float releaseAt = node.value - node.hysteresis;
        node.boolResult = node.boolResult ? (input > releaseAt) : (input >= node.value);
        node.result = node.boolResult ? 1.0f : 0.0f;
        break;
      }
      case FlowOp::Debounce: {
        const bool raw = nodes_[node.input].boolResult;
        if (raw != node.lastBool) {
          node.lastBool = raw;
          node.sinceMs = event.nowMs;
        } else if (raw != node.boolResult && event.nowMs - node.sinceMs >= node.ms) {
          node.boolResult = raw;
        }
        node.result = node.boolResult ? 1.0f : 0.0f;
        break;
      }
      case FlowOp::Emit: {
        const bool current = nodes_[node.input].boolResult;
        if (current != node.lastBool) {
          node.lastBool = current;
          node.boolResult = current;
          if (event.host != nullptr) {
            event.host->emitEvent(manifest_.name, node.event, current ? "rise" : "fall");
            ++emissions_;
          }
        }
        break;
      }
      case FlowOp::EmitValue: {
        const bool current = nodes_[node.input].boolResult;
        if (current && !node.lastBool && event.host != nullptr) {
          event.host->emitValue(manifest_.name, node.event, nodes_[node.input2].result);
          ++emissions_;
        }
        node.lastBool = current;
        break;
      }
      case FlowOp::Led: {
        const bool current = nodes_[node.input].boolResult;
        if (current != node.lastBool) {
          node.lastBool = current;
          if (event.host != nullptr) {
            if (current) {
              event.host->setLed(manifest_.name, node.rgb[0], node.rgb[1], node.rgb[2]);
            } else {
              event.host->setLed(manifest_.name, 0, 0, 0);
            }
          }
        }
        break;
      }
      case FlowOp::Select: {
        node.result = nodes_[node.input].boolResult ? nodes_[node.input2].result
                                                    : nodes_[node.input3].result;
        break;
      }
      case FlowOp::Gate: {
        // Skips FOLLOWING nodes, not earlier ones: with backward-only data
        // references a subtree has already run by the time we reach here, so
        // the only work a gate can actually avoid is what comes after it.
        node.boolResult = nodes_[node.input].boolResult;
        node.result = node.boolResult ? 1.0f : 0.0f;
        if (!node.boolResult && node.span > 0) {
          skipUntil = static_cast<uint8_t>(i + 1 + node.span);
          if (skipUntil > nodeCount_) skipUntil = nodeCount_;
        }
        break;
      }
      case FlowOp::BudgetLoad: node.result = budgetLoad_; break;
      case FlowOp::GraceLeft: node.result = static_cast<float>(graceLeft_); break;
      case FlowOp::Invalid:
      default:
        break;
    }
  }

  // Degradation is recorded, not silent. On a research instrument a signal
  // that quietly changes fidelity because another app was enabled would put a
  // step change in the data that has nothing to do with the subject.
  if (skippedAny != degraded_) {
    degraded_ = skippedAny;
    if (skippedAny) ++degradations_;
    if (event.host != nullptr) {
      event.host->emitEvent(manifest_.name, "degraded", skippedAny ? "rise" : "fall");
    }
  }
}

void FlowApp::onEvent(const AppEvent& event) {
  if (event.kind == AppEventKind::Budget) {
    // Latched, then read by BudgetLoad/GraceLeft on the next evaluation: the
    // push and the pull are two views of one piece of state.
    budgetLoad_ = event.load;
    graceLeft_ = event.graceLeft;
    return;
  }
  if (event.kind != AppEventKind::Frame || event.frame == nullptr || nodeCount_ == 0) {
    return;
  }
  ++frames_;
  evaluate(event);
}

String FlowApp::statusJson(bool withOutputs) const {
  String json = "{\"graph\":\"";
  json += jsonEscape(graphName_);
  json += "\",\"package\":\"";
  json += jsonEscape(String(packageId_));
  json += "\",\"source\":\"";
  json += jsonEscape(sourcePath_);
  json += "\",\"nodes\":";
  json += String(nodeCount_);
  json += ",\"estimated_us\":";
  json += String(estimatedUs_);
  json += ",\"budget_us\":";
  json += String(manifest_.frameBudgetUs);
  json += ",\"window_floats\":";
  json += String(windowUsed_);
  json += ",\"emissions\":";
  json += String(emissions_);
  json += ",\"frames\":";
  json += String(frames_);
  json += ",\"degraded\":";
  json += degraded_ ? "true" : "false";
  json += ",\"degradations\":";
  json += String(degradations_);
  if (!withOutputs) {
    json += "}";
    return json;
  }
  json += ",\"outputs\":[";
  for (uint8_t i = 0; i < nodeCount_; ++i) {
    if (i != 0) json += ",";
    json += "{\"op\":\"";
    json += opName(nodes_[i].op);
    json += "\",\"result\":";
    json += String(nodes_[i].result, 3);
    json += ",\"bool\":";
    json += nodes_[i].boolResult ? "true" : "false";
    json += "}";
  }
  json += "]}";
  return json;
}

}  // namespace nhos
