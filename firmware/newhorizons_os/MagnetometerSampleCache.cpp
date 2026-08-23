#include "MagnetometerSampleCache.h"

namespace nhos {

void MagnetometerSampleCache::store(const float in[3]) {
  if (!in) {
    clear();
    return;
  }
  values_[0] = in[0];
  values_[1] = in[1];
  values_[2] = in[2];
  valid_ = true;
}

void MagnetometerSampleCache::clear() {
  valid_ = false;
}

bool MagnetometerSampleCache::copy(float out[3]) const {
  if (!out || !valid_) return false;
  out[0] = values_[0];
  out[1] = values_[1];
  out[2] = values_[2];
  return true;
}

}  // namespace nhos
