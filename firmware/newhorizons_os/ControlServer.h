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
#include "CommandRegistry.h"
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

  // The command table (ControlCommandTable.cpp). Public so the transports'
  // LED policy and tests can read flags without a ControlServer instance.
  static const CommandSpec* findCommand(const String& name);
  static uint32_t commandSchemaHash();
  // Whether the scanner will actually scan soon: the sketch's stream gate
  // (streamingGateOk). Heavy UDP commands wait for the gap after a scan only
  // while this holds -- with the gate closed no scan comes, and waiting for
  // one would only add the full deferral.
  void setScanGate(bool (*gate)()) { scanGate_ = gate; }

 private:
  static const CommandSpec* commandTable(size_t& count);
  // {"hash","count","commands":[{name, maintenance, heavy, quiet, internal,
  // status_snapshot, summary}]} -- what `commands` answers.
  static String commandsJson();
  static constexpr int kMaxFileReadChunkBytes = 32768;

  // One handler per command (ControlCommands*.cpp), registered in the table.
  String cmdStatus(const CommandContext& ctx);
  String cmdSensorSample(const CommandContext& ctx);
  String cmdMemoryStatus(const CommandContext& ctx);
  String cmdTaskList(const CommandContext& ctx);
  String cmdAppList(const CommandContext& ctx);
  String cmdAppView(const CommandContext& ctx);
  String cmdAppEnable(const CommandContext& ctx);
  String cmdAppRevive(const CommandContext& ctx);
  String cmdAppEvents(const CommandContext& ctx);
  String cmdAppLoadFlow(const CommandContext& ctx);
  String cmdAppUnloadFlow(const CommandContext& ctx);
  String cmdAppListPackages(const CommandContext& ctx);
  String cmdAppInstall(const CommandContext& ctx);
  String cmdAppUninstall(const CommandContext& ctx);
  String cmdAppActivate(const CommandContext& ctx);
  String cmdAppDeactivate(const CommandContext& ctx);
  String cmdAppVerify(const CommandContext& ctx);
  String cmdAppReindex(const CommandContext& ctx);
  String cmdConfigSchema(const CommandContext& ctx);
  String cmdConfigGet(const CommandContext& ctx);
  String cmdConfigSet(const CommandContext& ctx);
  String cmdCapabilities(const CommandContext& ctx);
  String cmdSetPowerProfile(const CommandContext& ctx);
  String cmdSetTime(const CommandContext& ctx);
  String cmdClockSync(const CommandContext& ctx);
  String cmdClockStatus(const CommandContext& ctx);
  String cmdSetClockConfig(const CommandContext& ctx);
  String cmdServiceList(const CommandContext& ctx);
  String cmdServiceRestart(const CommandContext& ctx);
  String cmdScanHealth(const CommandContext& ctx);
  String cmdStorageStatus(const CommandContext& ctx);
  String cmdEnterMaintenance(const CommandContext& ctx);
  String cmdExitMaintenance(const CommandContext& ctx);
  String cmdSetScanTiming(const CommandContext& ctx);
  String cmdSetTransport(const CommandContext& ctx);
  String cmdSetStreamBuffer(const CommandContext& ctx);
  String cmdSetMatrixLayout(const CommandContext& ctx);
  String cmdSetWifi(const CommandContext& ctx);
  String cmdFindmeDiscover(const CommandContext& ctx);
  String cmdFindmeSwitchGateway(const CommandContext& ctx);
  String cmdSetChargeProfile(const CommandContext& ctx);
  String cmdPowerSetState(const CommandContext& ctx);
  String cmdSetActionButton(const CommandContext& ctx);
  String cmdSetFilter(const CommandContext& ctx);
  String cmdSetRawAdc(const CommandContext& ctx);
  String cmdSetBatteryProfile(const CommandContext& ctx);
  String cmdBatteryStatus(const CommandContext& ctx);
  String cmdResyncBatteryGauge(const CommandContext& ctx);
  String cmdSetImu(const CommandContext& ctx);
  String cmdSetLog(const CommandContext& ctx);
  String cmdSetIndicators(const CommandContext& ctx);
  String cmdCalibrationStatus(const CommandContext& ctx);
  String cmdCalibrationEnable(const CommandContext& ctx);
  String cmdCalibrationDisable(const CommandContext& ctx);
  String cmdCalibrationSessionBegin(const CommandContext& ctx);
  String cmdCalibrationSessionAbort(const CommandContext& ctx);
  String cmdCalibrationSessionCommit(const CommandContext& ctx);
  String cmdCalibrationClearProfile(const CommandContext& ctx);
  String cmdCalibrationDumpTare(const CommandContext& ctx);
  String cmdCalibrationDumpLevel(const CommandContext& ctx);
  String cmdCalibrationDumpFit(const CommandContext& ctx);
  String cmdCalibrationSetFit(const CommandContext& ctx);
  String cmdCalibrationSetReference(const CommandContext& ctx);
  String cmdCalibrationDeleteLevel(const CommandContext& ctx);
  String cmdCalibrationCaptureTare(const CommandContext& ctx);
  String cmdCalibrationTareCapture(const CommandContext& ctx);
  String cmdCalibrationTareClear(const CommandContext& ctx);
  String cmdCalibrationCaptureCell(const CommandContext& ctx);
  String cmdCalibrationCaptureAll(const CommandContext& ctx);
  String cmdFileList(const CommandContext& ctx);
  String cmdFileReadBegin(const CommandContext& ctx);
  String cmdFileReadChunk(const CommandContext& ctx);
  String cmdFileWriteBegin(const CommandContext& ctx);
  String cmdFileWriteChunk(const CommandContext& ctx);
  String cmdFileWriteFinish(const CommandContext& ctx);
  String cmdFileDelete(const CommandContext& ctx);
  String cmdLogTail(const CommandContext& ctx);
  String cmdDmesg(const CommandContext& ctx);
  String cmdLogClear(const CommandContext& ctx);
  String cmdCrashLog(const CommandContext& ctx);
  String cmdHealth(const CommandContext& ctx);
  String cmdCrashClear(const CommandContext& ctx);
  String cmdSetOtaConfig(const CommandContext& ctx);
  String cmdCheckUpdate(const CommandContext& ctx);
  String cmdApplyUpdate(const CommandContext& ctx);
  String cmdReboot(const CommandContext& ctx);
  String cmdRebootWifiSetup(const CommandContext& ctx);
  String cmdCommands(const CommandContext& ctx);

  void servicePendingApplyUpdate();
  // Sends a command result: inline when it fits one datagram, otherwise
  // queued as result_chunk datagrams that serviceUdpReplies() paces out one
  // per tick between scans (ReplyPacing.h).
  // Takes the response by value: the caller moves it in, and a chunked
  // reply keeps it without copying (~1 ms for a status).
  void sendUdpResult(WiFiUDP& udp, const String& host, uint16_t port, const String& uid, const String& requestId, String response);
  void serviceUdpReplies(WiFiUDP& udp);
  // A UDP command whose ack has gone out, waiting to execute.
  struct PendingUdpCommand {
    String payload;
    String cmd;
    String requestId;
    String sender;
    uint16_t port = 0;
    bool ackLed = false;
    uint32_t sinceUs = 0;
  };
  void executeUdpCommand(WiFiUDP& udp, const PendingUdpCommand& command);
  // Runs a deferred heavy command once HeavyCommandPacing allows it.
  void serviceDeferredUdpCommand(WiFiUDP& udp);
  bool heavyCommandMayRun(uint32_t deferredSinceUs) const;
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
  // One deferred heavy command (ReplyPacing.h, HeavyCommandPacing). A second
  // heavy command arriving while one waits simply runs at once.
  PendingUdpCommand deferredUdp_;
  bool hasDeferredUdp_ = false;
  bool (*scanGate_)() = nullptr;
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
