// ControlServer command handlers: calibration_*.
//
// One member per command, registered in ControlCommandTable.cpp. A handler
// never checks maintenance mode itself: the table's kCmdNeedsMaintenance
// flag is enforced once, in processCommand(), before it runs.
#include "ControlServer.h"
#include "EspNowOtaReceiver.h"
#include <esp_timer.h>
#include <utility>
#include <vector>
#include "Config.h"
#include "BatteryStatusJsonMerge.h"
#include "ActionButtonPolicy.h"
#include "EspNowPairing.h"  // kEspNowPairingFailFlagKey, so this stays in sync with EspNowPairing.cpp's own check
#include "EspNowTimeWire.h"
#include "JsonUtils.h"
#include "ReplyPacing.h"
#include "UpdateChannel.h"
#include "VersionCompare.h"

namespace nhos {

// calibration_status
String ControlServer::cmdCalibrationStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  return ok(cmd, "calibration_status", calibration_->statusJson(maintenanceMode()));
}

// calibration_enable
String ControlServer::cmdCalibrationEnable(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  String detail;
  if (!calibration_->setEnabled(true, detail)) {
    return error(cmd, detail.length() ? detail : "calibration_enable_failed");
  }
  return ok(cmd, "calibration_enabled", calibration_->statusJson(maintenanceMode()));
}

// calibration_disable
String ControlServer::cmdCalibrationDisable(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  String detail;
  if (!calibration_->setEnabled(false, detail)) {
    return error(cmd, detail.length() ? detail : "calibration_disable_failed");
  }
  return ok(cmd, "calibration_disabled", calibration_->statusJson(maintenanceMode()));
}

// calibration_session_begin
String ControlServer::cmdCalibrationSessionBegin(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  // A retried begin must not reset a draft that already holds captures.
  const bool alreadyActive = calibration_->sessionActive();
  if (!calibration_->sessionBegin()) {
    return error(cmd, "calibration_session_begin_failed");
  }
  return ok(cmd, alreadyActive ? "calibration_session_already_active" : "calibration_session_started",
            calibration_->statusJson(maintenanceMode()));
}

// calibration_session_abort
String ControlServer::cmdCalibrationSessionAbort(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  calibration_->sessionAbort();
  return ok(cmd, "calibration_session_aborted", calibration_->statusJson(maintenanceMode()));
}

// calibration_session_commit
String ControlServer::cmdCalibrationSessionCommit(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  String detail;
  if (!calibration_->sessionCommit(extractBool(request, "auto_enable", false), detail)) {
    return error(cmd, detail.length() ? detail : "calibration_commit_failed");
  }
  return ok(cmd, "calibration_committed", calibration_->statusJson(maintenanceMode()));
}

// calibration_clear_profile
String ControlServer::cmdCalibrationClearProfile(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_ || !calibration_->clearProfile()) {
    return error(cmd, "calibration_clear_failed");
  }
  return ok(cmd, "calibration_profile_cleared", calibration_->statusJson(maintenanceMode()));
}

// calibration_dump_tare
String ControlServer::cmdCalibrationDumpTare(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  String tareJson;
  if (!calibration_->dumpTareJson(tareJson)) {
    return error(cmd, "calibration_tare_not_found");
  }
  return ok(cmd, "calibration_tare_dump", tareJson);
}

// calibration_dump_level
String ControlServer::cmdCalibrationDumpLevel(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  String levelJson;
  if (!calibration_->dumpLevelJson(extractFloat(request, "level", 0), levelJson)) {
    return error(cmd, "calibration_level_not_found");
  }
  return ok(cmd, "calibration_level_dump", levelJson);
}

// calibration_dump_fit
String ControlServer::cmdCalibrationDumpFit(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  return ok(cmd, "calibration_fit_dump", calibration_->dumpFitJson());
}

// calibration_set_fit
String ControlServer::cmdCalibrationSetFit(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  // Board-level fit inputs (see Calibration::FitSettings); any argument
  // left out keeps its current value. Refits the saved profile.
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  Calibration::FitSettings settings = calibration_->fitSettings();
  settings.minLevel = extractFloat(request, "min_level", settings.minLevel);
  settings.readout[0].c1 = extractFloat(request, "adc1_c1", settings.readout[0].c1);
  settings.readout[0].c2 = extractFloat(request, "adc1_c2", settings.readout[0].c2);
  settings.readout[1].c1 = extractFloat(request, "adc2_c1", settings.readout[1].c1);
  settings.readout[1].c2 = extractFloat(request, "adc2_c2", settings.readout[1].c2);
  settings.clipMv[0] = extractFloat(request, "adc1_clip_mv", settings.clipMv[0]);
  settings.clipMv[1] = extractFloat(request, "adc2_clip_mv", settings.clipMv[1]);
  if (!calibration_->setFitSettings(settings)) {
    return error(cmd, "calibration_fit_settings_invalid");
  }
  return ok(cmd, "calibration_fit_set", calibration_->statusJson(maintenanceMode()));
}

// calibration_set_reference
String ControlServer::cmdCalibrationSetReference(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_ || !calibration_->setReference(extractFloat(request, "level", 0), extractFloat(request, "reference", NAN))) {
    return error(cmd, "calibration_level_not_found");
  }
  return ok(cmd, "calibration_reference_set", calibration_->statusJson(maintenanceMode()));
}

// calibration_delete_level
String ControlServer::cmdCalibrationDeleteLevel(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_ || !calibration_->deleteLevel(extractFloat(request, "level", 0))) {
    return error(cmd, "calibration_level_delete_failed");
  }
  return ok(cmd, "calibration_level_deleted", calibration_->statusJson(maintenanceMode()));
}

// calibration_capture_tare
String ControlServer::cmdCalibrationCaptureTare(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_ || !scanner_) {
    return error(cmd, "calibration_unavailable");
  }
  const ScanHealth health = scanner_->health();
  std::vector<float> values(health.pointCount, 0);
  const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 3000));
  if (!scanner_->captureAllBlockMedians(values.data(), values.size(), durationMs) || !calibration_->captureTare(values.data(), values.size())) {
    return error(cmd, "calibration_capture_failed");
  }
  String tareJson;
  calibration_->dumpTareJson(tareJson);
  return ok(cmd, "calibration_tare_captured", tareJson);
}

// calibration_tare_capture
String ControlServer::cmdCalibrationTareCapture(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  // One-tap zero. Works in normal mode too: the scan pauses for the
  // capture and resumes, so the operator never has to enter maintenance.
  if (!calibration_ || !scanner_) {
    return error(cmd, "calibration_unavailable");
  }
  if (calibration_->sessionActive()) {
    // Inside a session the baseline is the draft's (calibration_capture_tare);
    // writing the saved tare here would be undone by the commit.
    return error(cmd, "calibration_session_active");
  }
  const ScanHealth health = scanner_->health();
  std::vector<float> values(health.pointCount, 0);
  const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 1000));
  const bool pauseScan = !maintenanceMode() && scanner_->active();
  if (pauseScan) scanner_->stop();
  const bool captured = scanner_->captureAllBlockMedians(values.data(), values.size(), durationMs);
  if (pauseScan) scanner_->start();
  if (!captured || !calibration_->applyTareDirect(values.data(), values.size())) {
    return error(cmd, "calibration_tare_capture_failed");
  }
  return ok(cmd, "calibration_tare_captured", calibration_->statusJson(maintenanceMode()));
}

// calibration_tare_clear
String ControlServer::cmdCalibrationTareClear(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!calibration_) {
    return error(cmd, "calibration_unavailable");
  }
  if (calibration_->sessionActive()) {
    return error(cmd, "calibration_session_active");
  }
  if (!calibration_->clearTare()) {
    return error(cmd, "calibration_tare_clear_failed");
  }
  return ok(cmd, "calibration_tare_cleared", calibration_->statusJson(maintenanceMode()));
}

// calibration_capture_cell
String ControlServer::cmdCalibrationCaptureCell(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_ || !scanner_) {
    return error(cmd, "calibration_unavailable");
  }
  const uint16_t sensorIndex = static_cast<uint16_t>(extractInt(request, "sensor_index", -1));
  const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 3000));
  // `level` names the level (nominal); `reference` is the pressure actually
  // applied, which the fit uses -- the same unless the caller measured it.
  const float level = extractFloat(request, "level", 0);
  const float reference = extractFloat(request, "reference", level);
  // Levels are stored relative to the draft baseline, so it comes first.
  if (calibration_->sessionActive() && !calibration_->draftTareCaptured(sensorIndex)) {
    return error(cmd, "calibration_tare_required");
  }
  float value = 0;
  if (!scanner_->captureCellBlockMedian(sensorIndex, durationMs, value) ||
      !calibration_->captureCell(sensorIndex, level, reference, value)) {
    return error(cmd, "calibration_capture_failed");
  }
  String levelJson;
  calibration_->dumpLevelJson(level, levelJson);
  return ok(cmd, "calibration_cell_captured", levelJson);
}

// calibration_capture_all
String ControlServer::cmdCalibrationCaptureAll(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!calibration_ || !scanner_) {
    return error(cmd, "calibration_unavailable");
  }
  const ScanHealth health = scanner_->health();
  std::vector<float> values(health.pointCount, 0);
  const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 3000));
  const float level = extractFloat(request, "level", 0);
  const float reference = extractFloat(request, "reference", level);
  if (calibration_->sessionActive() && !calibration_->draftTareCaptured()) {
    return error(cmd, "calibration_tare_required");
  }
  if (!scanner_->captureAllBlockMedians(values.data(), values.size(), durationMs) ||
      !calibration_->captureAll(level, reference, values.data(), values.size())) {
    return error(cmd, "calibration_capture_failed");
  }
  String levelJson;
  calibration_->dumpLevelJson(level, levelJson);
  return ok(cmd, "calibration_all_captured", levelJson);
}

}  // namespace nhos
