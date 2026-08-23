#include "Bmm350BridgePolicy.h"

namespace nhos {

bool bmm350CanPublishCompensatedSample(bool initialized, bool normalMode,
                                       bool compensatedReadSucceeded) {
  return initialized && normalMode && compensatedReadSucceeded;
}

}  // namespace nhos
