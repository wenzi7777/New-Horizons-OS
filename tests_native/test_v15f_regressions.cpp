#include <cassert>
#include <iostream>
#include <string>

#include "BatteryChargeSafety.h"
#include "Bmm350BridgePolicy.h"
#include "Bmm350I2cTransport.h"
#include "MagnetometerSamplePolicy.h"
#include "MagnetometerSampleCache.h"
#include "PacketSensorBlocks.h"
#include "PowerStatusJson.h"

namespace {

class FakeBmm350Bus final : public nhos::Bmm350I2cBus {
 public:
  bool failWrite = false;
  bool shortRead = false;
  int reads = 0;
  int writes = 0;
  uint8_t readRegs[3] = {0};

  bool write(uint8_t, uint8_t, const uint8_t*, uint8_t) override {
    ++writes;
    return !failWrite;
  }

  uint8_t read(uint8_t, uint8_t reg, uint8_t* data, uint8_t length) override {
    readRegs[reads++] = reg;
    const uint8_t returned = shortRead ? static_cast<uint8_t>(length - 1) : length;
    for (uint8_t i = 0; i < returned; ++i) data[i] = static_cast<uint8_t>(reg + i);
    return returned;
  }
};

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

void testBmm350TransportChunksAndRejectsShortOrFailedI2c() {
  using namespace nhos;
  FakeBmm350Bus bus;
  uint8_t data[50] = {};
  assert(Bmm350I2cTransport::read(bus, 0x14, 0x10, data, sizeof(data)) ==
         Bmm350TransportResult::Success);
  assert(bus.reads == 3);
  assert(bus.readRegs[0] == 0x10 && bus.readRegs[1] == 0x28 && bus.readRegs[2] == 0x40);
  assert(data[0] == 0x10 && data[24] == 0x28 && data[48] == 0x40);

  bus.reads = 0;
  bus.shortRead = true;
  assert(Bmm350I2cTransport::read(bus, 0x14, 0x10, data, 4) ==
         Bmm350TransportResult::CommunicationFailure);
  assert(bus.reads == 1);

  bus.shortRead = false;
  bus.failWrite = true;
  assert(Bmm350I2cTransport::write(bus, 0x14, 0x10, data, 4) ==
         Bmm350TransportResult::CommunicationFailure);
}

void testMagnetometerReadFailureClearsPublishedSample() {
  using namespace nhos;
  MagnetometerSampleCache sample;
  const float first[3] = {11.0f, -7.0f, 3.5f};
  float out[3] = {};
  sample.store(first);
  assert(sample.copy(out));
  assert(out[0] == 11.0f && out[1] == -7.0f && out[2] == 3.5f);
  sample.clear();  // Models a failed BMM350 compensated read.
  assert(!sample.copy(out));
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
  testBmm350TransportChunksAndRejectsShortOrFailedI2c();
  testMagnetometerReadFailureClearsPublishedSample();
  testPowerStatusJsonClosesExactlyOnce();
  std::cout << "v1.5.F regression tests passed\n";
  return 0;
}
