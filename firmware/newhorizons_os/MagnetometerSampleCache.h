#pragma once

namespace nhos {

class MagnetometerSampleCache {
 public:
  void store(const float in[3]);
  void clear();
  bool copy(float out[3]) const;
  bool valid() const { return valid_; }

 private:
  bool valid_ = false;
  float values_[3] = {0.0f, 0.0f, 0.0f};
};

}  // namespace nhos
