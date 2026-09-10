#include "FeatureExtractorApp.h"

namespace nhos {
namespace {
const AppManifest kManifest = {
    "features",
    "1.0.0",
    kAppCapReadMatrix | kAppCapEmitEvent,
    // One pass over <=225 cells of trivial arithmetic. Measured headroom is
    // wide; the budget is set so a regression shows up as an overrun rather
    // than as quietly missed frames.
    400,
    "Total force, contact area, peak and centre of pressure per frame.",
};
}  // namespace

const AppManifest& FeatureExtractorApp::manifest() const { return kManifest; }

bool FeatureExtractorApp::start() {
  total_ = 0;
  peak_ = 0;
  peakIndex_ = 0;
  activeCells_ = 0;
  centroidRow_ = 0;
  centroidCol_ = 0;
  inContact_ = false;
  peak01_ = 0;
  return true;
}

void FeatureExtractorApp::onFrame(const AppFrameContext& context) {
  const MatrixFrame* frame = context.frame;
  if (frame == nullptr || frame->rows == 0 || frame->cols == 0) {
    return;
  }
  const uint16_t cells = frame->pointCount;
  float total = 0;
  float peak = 0;
  uint16_t peakIndex = 0;
  uint16_t active = 0;
  float weightedRow = 0;
  float weightedCol = 0;

  for (uint16_t i = 0; i < cells; ++i) {
    const float value = frame->values[i];
    total += value;
    if (value > peak) {
      peak = value;
      peakIndex = i;
    }
    if (value >= kPressureActiveThreshold) {
      ++active;
      // Weight by pressure, so the centroid is the centre of *force*, not
      // the centre of the contact patch's bounding box.
      weightedRow += value * static_cast<float>(i / frame->cols);
      weightedCol += value * static_cast<float>(i % frame->cols);
    }
  }

  total_ = total;
  peak_ = peak;
  peak01_ = peak / kPressureFullScale;
  if (peak01_ > 1.0f) {
    peak01_ = 1.0f;
  }
  peakIndex_ = peakIndex;
  activeCells_ = active;
  centroidRow_ = total > 0 ? weightedRow / total : 0;
  centroidCol_ = total > 0 ? weightedCol / total : 0;
  ++frames_;

  const bool contact = active > 0;
  if (contact != inContact_) {
    inContact_ = contact;
    if (contact) {
      ++contacts_;
    }
    if (context.host != nullptr) {
      context.host->emitEvent(kManifest.name, contact ? "contact_begin" : "contact_end",
                              String("cells=") + String(active) + " total=" + String(total, 2));
    }
  }
}

String FeatureExtractorApp::statusJson() const {
  String json = "{\"total_force\":";
  json += String(total_, 3);
  json += ",\"peak\":";
  json += String(peak_, 3);
  json += ",\"peak01\":";
  json += String(peak01_, 4);
  json += ",\"peak_index\":";
  json += String(peakIndex_);
  json += ",\"active_cells\":";
  json += String(activeCells_);
  json += ",\"centroid_row\":";
  json += String(centroidRow_, 2);
  json += ",\"centroid_col\":";
  json += String(centroidCol_, 2);
  json += ",\"in_contact\":";
  json += inContact_ ? "true" : "false";
  json += ",\"contacts\":";
  json += String(contacts_);
  json += ",\"frames\":";
  json += String(frames_);
  json += "}";
  return json;
}

}  // namespace nhos
