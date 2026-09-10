#pragma once

#include "App.h"

namespace nhos {

// Turns a full pressure map into the handful of numbers that actually get
// analysed: total force, contact area, peak, and centre of pressure.
//
// This is the data-reduction case the app model exists for. A v1.5.F frame is
// 196 floats; these five scalars are what the research question is usually
// asked in, and on a Direct-mode device they are ~100x cheaper to ship over a
// link that already cannot keep up with raw frames.
class FeatureExtractorApp : public App {
 public:
  const AppManifest& manifest() const override;
  bool start() override;
  void onFrame(const AppFrameContext& context) override;
  String statusJson() const override;

 private:
  float total_ = 0;
  float peak_ = 0;
  uint16_t peakIndex_ = 0;
  uint16_t activeCells_ = 0;
  float centroidRow_ = 0;
  float centroidCol_ = 0;
  float peak01_ = 0;
  bool inContact_ = false;
  uint32_t contacts_ = 0;
  uint32_t frames_ = 0;
};

}  // namespace nhos
