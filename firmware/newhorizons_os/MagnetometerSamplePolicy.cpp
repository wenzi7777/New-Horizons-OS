#include "MagnetometerSamplePolicy.h"

namespace nhos {

bool magnetometerCanPublish(MagnetometerModel model, bool initialized,
                            bool calibrationAvailable, bool readSucceeded) {
  if (model == MagnetometerModel::None) {
    return false;
  }
  return initialized && calibrationAvailable && readSucceeded;
}

}  // namespace nhos
