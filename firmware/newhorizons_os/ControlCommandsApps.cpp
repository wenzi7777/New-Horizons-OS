// ControlServer command handlers: app_*: the app runtime and the package registry.
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

// app_list
String ControlServer::cmdAppList(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!apps_) {
    return error(cmd, "app_manager_unavailable");
  }
  // {"command":"app_list","outputs":"flow1"} adds that slot's per-node values.
  return ok(cmd, "app_list", apps_->statusJson(extractString(request, "outputs")));
}

// app_view
String ControlServer::cmdAppView(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!apps_) {
    return error(cmd, "app_manager_unavailable");
  }
  return ok(cmd, "app_view", appViewJson());
}

// app_enable, app_disable
String ControlServer::cmdAppEnable(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!apps_) {
    return error(cmd, "app_manager_unavailable");
  }
  const String name = extractString(request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  const bool enable = cmd == "app_enable";
  if (!apps_->setEnabled(name, enable)) {
    // setEnabled() refuses to silently un-kill an app that blew its budget;
    // app_revive is the deliberate acknowledgement.
    return error(cmd, "app_state_change_rejected");
  }
  return ok(cmd, enable ? "app_enabled" : "app_disabled", apps_->statusJson());
}

// app_revive
String ControlServer::cmdAppRevive(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!apps_) {
    return error(cmd, "app_manager_unavailable");
  }
  const String name = extractString(request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  if (!apps_->revive(name)) {
    return error(cmd, "app_revive_failed");
  }
  return ok(cmd, "app_revived", apps_->statusJson());
}

// app_events
String ControlServer::cmdAppEvents(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!apps_) {
    return error(cmd, "app_manager_unavailable");
  }
  const int since = extractInt(request, "since_seq", 0);
  const int limit = extractInt(request, "limit", 0);
  return ok(cmd, "app_events", apps_->eventsJson(static_cast<uint32_t>(since < 0 ? 0 : since),
                                                 static_cast<uint8_t>(limit < 0 ? 0 : limit)));
}

// app_load_flow
String ControlServer::cmdAppLoadFlow(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!flow_ || !scanner_) {
    return error(cmd, "flow_engine_unavailable");
  }
  String path = extractString(request, "path");
  if (path.isEmpty()) {
    path = "apps/flow.json";
  }
  String loadError;
  // Cell count drives the load-time cost estimate, so the same graph is
  // judged against the board it will actually run on.
  if (!flow_->loadFromFile(path, scanner_->health().pointCount, loadError)) {
    return error(cmd, String("flow_load_failed:") + loadError);
  }
  return ok(cmd, "flow_graph_loaded", flow_->statusJson(false));
}

// app_unload_flow
String ControlServer::cmdAppUnloadFlow(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!flow_) {
    return error(cmd, "flow_engine_unavailable");
  }
  flow_->unload();
  return ok(cmd, "flow_graph_unloaded", flow_->statusJson(false));
}

// app_list_packages
String ControlServer::cmdAppListPackages(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  return ok(cmd, "app_list_packages", appRegistry_->statusJson());
}

// app_install
String ControlServer::cmdAppInstall(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  // Consumes a file written by the already maintenance-only file_write_*
  // sequence; gating it the same way stops a half-written package being
  // installed mid-scan.
  const String path = extractString(request, "path");
  if (path.isEmpty()) {
    return error(cmd, "path_required");
  }
  String installedId;
  String installError;
  if (!appRegistry_->install(path, extractString(request, "sha256"),
                             extractBool(request, "replace", false), installedId, installError)) {
    return error(cmd, installError);
  }
  String data = "{\"id\":\"";
  data += jsonEscape(installedId);
  data += "\",\"registry\":";
  data += appRegistry_->statusJson();
  data += "}";
  return ok(cmd, "app_installed", data);
}

// app_uninstall
String ControlServer::cmdAppUninstall(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  const String id = extractString(request, "id");
  if (id.isEmpty()) {
    return error(cmd, "id_required");
  }
  String uninstallError;
  if (!appRegistry_->uninstall(id, extractBool(request, "keep_file", false), uninstallError)) {
    return error(cmd, uninstallError);
  }
  return ok(cmd, "app_uninstalled", appRegistry_->statusJson());
}

// app_activate
String ControlServer::cmdAppActivate(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  const String id = extractString(request, "id");
  if (id.isEmpty()) {
    return error(cmd, "id_required");
  }
  const int slot = extractInt(request, "slot", -1);
  String activateError;
  if (!appRegistry_->activate(id, static_cast<int8_t>(slot), activateError)) {
    return error(cmd, activateError);
  }
  return ok(cmd, "app_activated", appRegistry_->statusJson());
}

// app_deactivate
String ControlServer::cmdAppDeactivate(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  const String id = extractString(request, "id");
  const int slot = extractInt(request, "slot", -1);
  if (id.isEmpty() && slot < 0) {
    return error(cmd, "id_or_slot_required");
  }
  String deactivateError;
  if (!appRegistry_->deactivate(id, static_cast<int8_t>(slot), deactivateError)) {
    return error(cmd, deactivateError);
  }
  return ok(cmd, "app_deactivated", appRegistry_->statusJson());
}

// app_verify
String ControlServer::cmdAppVerify(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  const String id = extractString(request, "id");
  if (id.isEmpty()) {
    return error(cmd, "id_required");
  }
  String computed;
  bool match = false;
  String verifyError;
  if (!appRegistry_->verify(id, computed, match, verifyError)) {
    return error(cmd, verifyError);
  }
  String data = "{\"id\":\"";
  data += jsonEscape(id);
  data += "\",\"sha256\":\"";
  data += jsonEscape(computed);
  data += "\",\"match\":";
  data += match ? "true" : "false";
  data += "}";
  return ok(cmd, "app_verified", data);
}

// app_reindex
String ControlServer::cmdAppReindex(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!appRegistry_) {
    return error(cmd, "app_registry_unavailable");
  }
  uint16_t recovered = 0;
  uint16_t dropped = 0;
  String reindexError;
  if (!appRegistry_->reindex(recovered, dropped, reindexError)) {
    return error(cmd, reindexError);
  }
  String data = "{\"recovered\":";
  data += String(recovered);
  data += ",\"dropped\":";
  data += String(dropped);
  data += ",\"registry\":";
  data += appRegistry_->statusJson();
  data += "}";
  return ok(cmd, "app_reindexed", data);
}

}  // namespace nhos
