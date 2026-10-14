#pragma once

#include <Arduino.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <vector>

#include "BootModeManager.h"
#include "BatteryGaugeManager.h"
#include "Calibration.h"
#include "AppManager.h"
#include "ConfigRegistry.h"
#include "DeviceConfig.h"
#include "DisplayManager.h"
#include "ExternalLedController.h"
#include "FaultRecorder.h"
#include "FindMeClient.h"
#include "ImuManager.h"
#include "LedController.h"
#include "LifetimeStats.h"
#include "MatrixScanner.h"
#include "MagnetometerManager.h"
#include "OtaManager.h"
#include "PowerManager.h"
#include "PowerGovernor.h"
#include "PowerStateManager.h"
#include "AirtimeArbiter.h"
#include "ProcFs.h"
#include "AppGovernor.h"
#include "AppRegistry.h"
#include "FlowApp.h"
#include "Scheduler.h"
#include "ServiceManager.h"
#include "Storage.h"
#include "ClockService.h"
#include "WifiManager.h"

namespace nhos {

class PowerStateManager;
class EspNowOtaReceiver;

class ControlServer {
 public:
  void begin(
      WifiManager& wifi,
      MatrixScanner& scanner,
      Storage& storage,
      BootModeManager& boot,
      OtaManager& ota,
      FindMeClient& findme,
      PowerManager& power,
      BatteryGaugeManager& batteryGauge,
      PowerStateManager& powerState,
      ImuManager& imu,
      MagnetometerManager& magnetometer,
      LedController& leds,
      DeviceConfig& deviceConfig,
      Calibration& calibration,
      DisplayManager& display,
      ExternalLedController& externalLeds);
  void service();
  void serviceUdpCommand(WiFiUDP& udp);
  // Returns the JSON response so the caller (EspNowPairing) can send it
  // back to the Hub -- see the .cpp for why this differs from
  // serviceUdpCommand()'s ack-then-fire-and-forget shape.
  String serviceEspNowCommand(const uint8_t* data, size_t len);

  // Wired once from newhorizons_os.ino's setup(), only in ESP-NOW/Direct
  // mode. When set, `check_update`/`apply_update` drive the Hub-relayed
  // OTA path (EspNowOtaReceiver) instead of OtaManager's HTTP one, and
  // `status` reports that path's progress as update_state. A Direct-mode
  // device has no WiFi at all, so the HTTP path can only ever fail there
  // (it surfaced in the WebUI as an opaque `manifest_http_-1`).
  void setEspNowOtaReceiver(EspNowOtaReceiver* receiver) { espNowOta_ = receiver; }

  // Wired from setup(). Optional for the same reason the OTA receiver is:
  // begin()'s parameter list is already at its useful limit, and nothing in
  // ControlServer needs the recorder to exist.
  void setFaultRecorder(FaultRecorder* recorder) { faults_ = recorder; }
  void setLifetimeStats(LifetimeStats* stats) { lifetime_ = stats; }
  void setScheduler(Scheduler* scheduler) { scheduler_ = scheduler; }
  void setProcFs(ProcFs* proc) { proc_ = proc; }
  void setArbiter(AirtimeArbiter* arbiter) { arbiter_ = arbiter; }
  void setServiceManager(ServiceManager* services) { services_ = services; }
  void setClock(ClockService* clock) { clock_ = clock; }
  void setPowerGovernor(PowerGovernor* governor) { governor_ = governor; }
  void setAppManager(AppManager* apps, FlowApp* flow, AppRegistry* registry,
                     AppGovernor* governor) {
    apps_ = apps;
    flow_ = flow;
    appRegistry_ = registry;
    appGovernor_ = governor;
  }
  bool maintenanceMode() const;

 private:
  void servicePendingApplyUpdate();
  // Sends a command result: inline when it fits one datagram, otherwise
  // queued as result_chunk datagrams that serviceUdpReplies() paces out one
  // per tick between scans (ReplyPacing.h).
  void sendUdpResult(WiFiUDP& udp, const String& host, uint16_t port, const String& uid, const String& requestId, const String& response);
  void serviceUdpReplies(WiFiUDP& udp);
  String processCommand(const String& request);
  String ok(const String& command, const String& message, const String& data = "{}") const;
  String error(const String& command, const String& message) const;
  String deviceUidString() const;
  String commandName(const String& request) const;
  String scanTimingStatusJson() const;
  String layoutStatusJson() const;
  String indicatorsStatusJson() const;
  String appViewJson() const;
  String actionButtonStatusJson() const;
  String batteryStatusJson() const;
  // check_update/apply_update: the request's manifest_url when it names
  // one, otherwise the device's own (custom URL or its channel's manifest).
  String requestedManifestUrl(const String& request) const;
  String extractString(const String& request, const char* key) const;
  int extractInt(const String& request, const char* key, int fallback) const;
  // {"clock":<ClockService::statusJson>,"clock_config":<DeviceConfig::clockJson>}
  String clockStatusData() const;
  float extractFloat(const String& request, const char* key, float fallback) const;
  bool extractBool(const String& request, const char* key, bool fallback) const;
  size_t extractArray(const String& request, const char* key, uint8_t* out, size_t maxCount) const;
  String extractObject(const String& request, const char* key) const;
  bool requireMaintenance(const String& command) const;
  String fileSizeJson(const String& command, const String& scope, const String& path) const;
  String fileChunkJson(const String& command, const String& scope, const String& path, size_t offset, size_t length) const;
  bool decodeHex(const String& hex, std::vector<uint8_t>& out) const;
  String encodeHex(const std::vector<uint8_t>& data) const;

  WiFiServer server_{kControlPort};
  WifiManager* wifi_ = nullptr;
  MatrixScanner* scanner_ = nullptr;
  Storage* storage_ = nullptr;
  BootModeManager* boot_ = nullptr;
  OtaManager* ota_ = nullptr;
  EspNowOtaReceiver* espNowOta_ = nullptr;
  FaultRecorder* faults_ = nullptr;
  LifetimeStats* lifetime_ = nullptr;
  Scheduler* scheduler_ = nullptr;
  ProcFs* proc_ = nullptr;
  AirtimeArbiter* arbiter_ = nullptr;
  ServiceManager* services_ = nullptr;
  ClockService* clock_ = nullptr;
  PowerGovernor* governor_ = nullptr;
  AppManager* apps_ = nullptr;
  FlowApp* flow_ = nullptr;
  AppRegistry* appRegistry_ = nullptr;
  AppGovernor* appGovernor_ = nullptr;
  FindMeClient* findme_ = nullptr;
  PowerManager* power_ = nullptr;
  BatteryGaugeManager* batteryGauge_ = nullptr;
  PowerStateManager* powerState_ = nullptr;
  ImuManager* imu_ = nullptr;
  MagnetometerManager* magnetometer_ = nullptr;
  LedController* leds_ = nullptr;
  DeviceConfig* deviceConfig_ = nullptr;
  Calibration* calibration_ = nullptr;
  DisplayManager* display_ = nullptr;
  ExternalLedController* externalLeds_ = nullptr;
  bool started_ = false;
  // Large UDP results waiting to go out, oldest first. Two slots: a user
  // command and a background poll can overlap; a third falls back to the
  // old blocking send rather than holding more heap.
  struct PendingUdpReply {
    String host;
    uint16_t port = 0;
    String uid;
    String requestId;
    String response;
    size_t nextChunk = 0;
    size_t totalChunks = 0;
  };
  static constexpr uint8_t kMaxPendingUdpReplies = 2;
  PendingUdpReply udpReplies_[kMaxPendingUdpReplies];
  uint8_t udpReplyCount_ = 0;
  uint32_t udpChunkReadyUs_ = 0;
  void sendUdpChunk(WiFiUDP& udp, const PendingUdpReply& reply, size_t index);
  String writeScope_;
  String writePath_;
  size_t writeExpectedSize_ = 0;
  size_t writeWritten_ = 0;
  bool otaPending_ = false;
  String otaPendingManifestUrl_;
};

}  // namespace nhos
