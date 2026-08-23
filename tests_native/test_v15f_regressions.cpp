#include <cassert>
#include <iostream>
#include <string>

#include "BatteryChargeSafety.h"
#include "Bmm350BridgePolicy.h"
#include "MagnetometerSamplePolicy.h"
#include "PacketSensorBlocks.h"
#include "PowerStatusJson.h"

namespace {

void testMagIsAnExplicitOptionalBlock() {
  using namespace nhos;
  const PacketSensorBlocks imuOnly{true, false, false};
  assert(packetSensorPayloadBytes(imuOnly) == 7 * sizeof(float));
  assert((packetSensorFlags(imuOnly) & kPacketSensorFlagMag) == 0);

  const PacketSensorBlocks legacyBmm150{true, true, false};
  assert(packetSensorPayloadBytes(legacyBmm150) == 10 * sizeof(float));
  assert((packetSensorFlags(legacyBmm150) & kPacketSensorFlagMag) != 0);

  // A v1.5.F seven-float IMU sample cannot gain a MAG block unless the
  // caller separately supplies a three-float magnetometer sample.
  assert(packetSensorPayloadBytes(imuOnly) != packetSensorPayloadBytes(legacyBmm150));
}

void testBatterySafetyLimitIsReappliedOnProfileTransition() {
  using namespace nhos;
  assert(shouldApplyBatteryChargeLimit(false, 0, 200));
  assert(!shouldApplyBatteryChargeLimit(true, 200, 200));
  assert(shouldApplyBatteryChargeLimit(true, 200, 100));
  assert(!shouldApplyBatteryChargeLimit(true, 100, 100));
}

void testUncalibratedBmm350CannotPublishFakeMagneticField() {
  using namespace nhos;
  assert(magnetometerCanPublish(MagnetometerModel::Bmm150, true, true, true));
  assert(!magnetometerCanPublish(MagnetometerModel::Bmm150, false, true, true));
  assert(!magnetometerCanPublish(MagnetometerModel::Bmm350, true, false, true));
  assert(magnetometerCanPublish(MagnetometerModel::Bmm350, true, true, true));
}

void testBmm350RequiresTheOfficialCompensatedReadPath() {
  using namespace nhos;
  assert(!bmm350CanPublishCompensatedSample(false, true, true));
  assert(!bmm350CanPublishCompensatedSample(true, false, true));
  assert(!bmm350CanPublishCompensatedSample(true, true, false));
  assert(bmm350CanPublishCompensatedSample(true, true, true));
}

void testPowerStatusJsonClosesExactlyOnce() {
  using namespace nhos;
  const PowerStatusJsonSnapshot snapshot{
      "not_charging", "not_charging", "bq25180", true, false, false, true,
      "ultra_slow", 100, 500, 4200, 10, 20, 6, 0, "", ""};
  const std::string json = formatPowerStatusJson(snapshot);
  assert(!json.empty());
  assert(json.back() == '}');
  assert(json.find("\"temperature_monitoring\":\"bypassed\"") != std::string::npos);
  assert(json.find("}\"") == std::string::npos);
}

}  // namespace

int main() {
  testMagIsAnExplicitOptionalBlock();
  testBatterySafetyLimitIsReappliedOnProfileTransition();
  testUncalibratedBmm350CannotPublishFakeMagneticField();
  testBmm350RequiresTheOfficialCompensatedReadPath();
  testPowerStatusJsonClosesExactlyOnce();
  std::cout << "v1.5.F regression tests passed\n";
  return 0;
}
