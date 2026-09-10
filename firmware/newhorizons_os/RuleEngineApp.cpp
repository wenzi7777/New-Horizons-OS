#include "RuleEngineApp.h"

#include <vector>

#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {
namespace {

const AppManifest kManifest = {
    "rules",
    "1.0.0",
    kAppCapReadMatrix | kAppCapEmitEvent,
    // Ceiling for any loaded graph. loadFromFile() refuses a graph whose
    // statically estimated cost exceeds this, so the budget is enforced
    // before the graph ever runs -- not after it has cost frames.
    1500,
    "Runs a declarative rule graph loaded from a file.",
};

bool isCellOp(RuleOp op) {
  return op == RuleOp::Total || op == RuleOp::Peak || op == RuleOp::RegionSum ||
         op == RuleOp::ActiveCells;
}

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

}  // namespace

const AppManifest& RuleEngineApp::manifest() const { return kManifest; }

RuleOp RuleEngineApp::opFromName(const String& name) {
  if (name == "total") return RuleOp::Total;
  if (name == "peak") return RuleOp::Peak;
  if (name == "region_sum") return RuleOp::RegionSum;
  if (name == "active_cells") return RuleOp::ActiveCells;
  if (name == "threshold") return RuleOp::Threshold;
  if (name == "debounce") return RuleOp::Debounce;
  if (name == "emit") return RuleOp::Emit;
  return RuleOp::Invalid;
}

const char* RuleEngineApp::opName(RuleOp op) {
  switch (op) {
    case RuleOp::Total: return "total";
    case RuleOp::Peak: return "peak";
    case RuleOp::RegionSum: return "region_sum";
    case RuleOp::ActiveCells: return "active_cells";
    case RuleOp::Threshold: return "threshold";
    case RuleOp::Debounce: return "debounce";
    case RuleOp::Emit: return "emit";
    case RuleOp::Invalid:
    default: return "invalid";
  }
}

uint32_t RuleEngineApp::estimateUs(uint16_t cellCount) const {
  uint64_t totalNs = 0;
  for (uint8_t i = 0; i < nodeCount_; ++i) {
    if (isCellOp(nodes_[i].op)) {
      totalNs += static_cast<uint64_t>(cellCount) * kCellOpNsPerCell;
    } else {
      totalNs += kScalarOpNs;
    }
  }
  return static_cast<uint32_t>((totalNs + 999) / 1000);
}

bool RuleEngineApp::parse(const String& json, uint16_t cellCount, String& error) {
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

  // Parsed into scratch first, so a rejected graph leaves the running one
  // alone -- same transactional shape as a calibration session.
  RuleNode scratch[kMaxNodes];
  uint8_t count = 0;
  for (const String& object : objects) {
    RuleNode& node = scratch[count];
    node = RuleNode();
    const String opText = jsonExtractString(object, "op", "");
    node.op = opFromName(opText);
    if (node.op == RuleOp::Invalid) {
      error = String("unknown_op:") + opText;
      return false;
    }
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
    float value = 0;
    if (jsonExtractFloat(object, "value", value)) {
      node.value = value;
    }
    float hysteresis = 0;
    if (jsonExtractFloat(object, "hysteresis", hysteresis)) {
      node.hysteresis = hysteresis;
    }
    long ms = 0;
    if (jsonExtractInt(object, "ms", ms) && ms > 0) {
      node.ms = static_cast<uint16_t>(ms);
    }
    long coord = 0;
    if (jsonExtractInt(object, "r0", coord)) node.r0 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "c0", coord)) node.c0 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "r1", coord)) node.r1 = static_cast<uint8_t>(coord);
    if (jsonExtractInt(object, "c1", coord)) node.c1 = static_cast<uint8_t>(coord);
    const String event = jsonExtractString(object, "event", "");
    strncpy(node.event, event.c_str(), sizeof(node.event) - 1);
    node.event[sizeof(node.event) - 1] = '\0';

    // Operators that consume a value must actually have one.
    if ((node.op == RuleOp::Threshold || node.op == RuleOp::Debounce ||
         node.op == RuleOp::Emit) &&
        node.input < 0) {
      error = String("missing_input:") + opName(node.op);
      return false;
    }
    if (node.op == RuleOp::Emit && node.event[0] == '\0') {
      error = "missing_event_name";
      return false;
    }
    ++count;
  }

  // Cost check before commit. This is the whole safety argument for the
  // declarative model: an over-budget graph is refused at load time.
  uint8_t previousCount = nodeCount_;
  for (uint8_t i = 0; i < count; ++i) {
    nodes_[i] = scratch[i];
  }
  nodeCount_ = count;
  const uint32_t estimated = estimateUs(cellCount);
  if (estimated > kManifest.frameBudgetUs) {
    nodeCount_ = previousCount;
    error = String("over_budget:") + String(estimated) + "us>" +
            String(kManifest.frameBudgetUs) + "us";
    return false;
  }
  estimatedUs_ = estimated;
  graphName_ = jsonExtractString(json, "name", "unnamed");
  return true;
}

bool RuleEngineApp::loadFromFile(const String& path, uint16_t cellCount, String& error) {
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
  if (size > 4096) {
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
  if (!parse(json, cellCount, error)) {
    return false;
  }
  sourcePath_ = path;
  return true;
}

void RuleEngineApp::unload() {
  nodeCount_ = 0;
  estimatedUs_ = 0;
  sourcePath_ = "";
  graphName_ = "";
}

bool RuleEngineApp::start() { return true; }

void RuleEngineApp::evaluate(const AppFrameContext& context) {
  const MatrixFrame* frame = context.frame;
  const uint16_t cells = frame->pointCount;

  for (uint8_t i = 0; i < nodeCount_; ++i) {
    RuleNode& node = nodes_[i];
    switch (node.op) {
      case RuleOp::Total: {
        float sum = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          sum += frame->values[c];
        }
        node.result = sum;
        break;
      }
      case RuleOp::Peak: {
        float peak = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          if (frame->values[c] > peak) {
            peak = frame->values[c];
          }
        }
        node.result = peak;
        break;
      }
      case RuleOp::RegionSum: {
        float sum = 0;
        for (uint16_t r = node.r0; r <= node.r1 && r < frame->rows; ++r) {
          for (uint16_t c = node.c0; c <= node.c1 && c < frame->cols; ++c) {
            const uint16_t index = r * frame->cols + c;
            if (index < cells) {
              sum += frame->values[index];
            }
          }
        }
        node.result = sum;
        break;
      }
      case RuleOp::ActiveCells: {
        uint16_t active = 0;
        for (uint16_t c = 0; c < cells; ++c) {
          if (frame->values[c] >= node.value) {
            ++active;
          }
        }
        node.result = static_cast<float>(active);
        break;
      }
      case RuleOp::Threshold: {
        const float input = nodes_[node.input].result;
        // Hysteresis: once latched, hold until the input falls below
        // value - hysteresis. Without it a signal sitting on the threshold
        // emits an event every single frame.
        const float releaseAt = node.value - node.hysteresis;
        node.boolResult = node.boolResult ? (input > releaseAt) : (input >= node.value);
        node.result = node.boolResult ? 1.0f : 0.0f;
        break;
      }
      case RuleOp::Debounce: {
        const bool raw = nodes_[node.input].boolResult;
        if (raw != node.lastBool) {
          node.lastBool = raw;
          node.sinceMs = context.nowMs;
        } else if (raw != node.boolResult && context.nowMs - node.sinceMs >= node.ms) {
          node.boolResult = raw;
        }
        node.result = node.boolResult ? 1.0f : 0.0f;
        break;
      }
      case RuleOp::Emit: {
        const bool current = nodes_[node.input].boolResult;
        if (current != node.lastBool) {
          node.lastBool = current;
          node.boolResult = current;
          if (context.host != nullptr) {
            context.host->emitEvent(kManifest.name, node.event, current ? "rise" : "fall");
            ++emissions_;
          }
        }
        break;
      }
      case RuleOp::Invalid:
      default:
        break;
    }
  }
}

void RuleEngineApp::onFrame(const AppFrameContext& context) {
  if (nodeCount_ == 0 || context.frame == nullptr) {
    return;
  }
  ++frames_;
  evaluate(context);
}

String RuleEngineApp::statusJson() const {
  String json = "{\"graph\":\"";
  json += jsonEscape(graphName_);
  json += "\",\"source\":\"";
  json += jsonEscape(sourcePath_);
  json += "\",\"nodes\":";
  json += String(nodeCount_);
  json += ",\"estimated_us\":";
  json += String(estimatedUs_);
  json += ",\"budget_us\":";
  json += String(kManifest.frameBudgetUs);
  json += ",\"emissions\":";
  json += String(emissions_);
  json += ",\"frames\":";
  json += String(frames_);
  json += ",\"outputs\":[";
  for (uint8_t i = 0; i < nodeCount_; ++i) {
    if (i != 0) {
      json += ",";
    }
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
