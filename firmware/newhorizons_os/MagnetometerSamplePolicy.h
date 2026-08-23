#pragma once

namespace nhos {

enum class MagnetometerModel : unsigned char { None, Bmm150, Bmm350 };

bool magnetometerCanPublish(MagnetometerModel model, bool initialized,
                            bool calibrationAvailable, bool readSucceeded);

}  // namespace nhos
