// ControlServer command handlers: file_*: the scoped flash filesystem.
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

// file_list
String ControlServer::cmdFileList(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  String scope = extractString(request, "scope");
  if (scope.isEmpty()) {
    scope = "user";
  }
  if (proc_ && ProcFs::isProcScope(scope)) {
    return ok(cmd, "file_list", proc_->list());
  }
  return ok(cmd, "file_list", storage_->listFiles(scope));
}

// file_read_begin
String ControlServer::cmdFileReadBegin(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  return fileSizeJson(cmd, extractString(request, "scope"), extractString(request, "path"));
}

// file_read_chunk
String ControlServer::cmdFileReadChunk(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  String scope = extractString(request, "scope");
  if (scope.isEmpty()) {
    scope = "user";
  }
  // The read buffer is allocated before anything checks the file size,
  // so the length is clamped first: a negative one would wrap to a huge
  // size_t. 32 KB is the most any caller asks for in one go.
  const int length = extractInt(request, "length", 1024);
  const int offset = extractInt(request, "offset", 0);
  return fileChunkJson(
      cmd,
      scope,
      extractString(request, "path"),
      static_cast<size_t>(offset > 0 ? offset : 0),
      static_cast<size_t>(length < 1 ? 1 : (length > kMaxFileReadChunkBytes ? kMaxFileReadChunkBytes : length)));
}

// file_write_begin
String ControlServer::cmdFileWriteBegin(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  writeScope_ = extractString(request, "scope");
  if (ProcFs::isProcScope(writeScope_)) {
    writeScope_ = "";
    return error(cmd, "read_only_scope");
  }
  if (writeScope_.isEmpty()) {
    writeScope_ = "user";
  }
  writePath_ = extractString(request, "path");
  writeExpectedSize_ = static_cast<size_t>(extractInt(request, "size", 0));
  writeWritten_ = 0;
  if (writePath_.isEmpty() || !storage_->validUserPath(writePath_)) {
    return error(cmd, "invalid_path");
  }
  if (storage_->isPathTooLong(writeScope_, writePath_)) {
    return error(cmd, "path_too_long");
  }
  if (writeExpectedSize_ == 0 && !storage_->writeFile(writeScope_, writePath_, nullptr, 0, false)) {
    return error(cmd, "file_write_failed");
  }
  String data = "{\"scope\":\"";
  data += writeScope_;
  data += "\",\"path\":\"";
  data += writePath_;
  data += "\",\"size\":";
  data += String(static_cast<unsigned int>(writeExpectedSize_));
  data += "}";
  return ok(cmd, "file_write_started", data);
}

// file_write_chunk
String ControlServer::cmdFileWriteChunk(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  const String path = extractString(request, "path");
  const size_t offset = static_cast<size_t>(extractInt(request, "offset", 0));
  if (path != writePath_ || offset != writeWritten_) {
    return error(cmd, "file_write_offset_mismatch");
  }
  std::vector<uint8_t> bytes;
  if (!decodeHex(extractString(request, "data"), bytes)) {
    return error(cmd, "invalid_hex_data");
  }
  if (!storage_->writeFile(writeScope_, writePath_, bytes.data(), bytes.size(), offset != 0)) {
    return error(cmd, "file_write_failed");
  }
  writeWritten_ += bytes.size();
  String data = "{\"scope\":\"";
  data += writeScope_;
  data += "\",\"path\":\"";
  data += writePath_;
  data += "\",\"written\":";
  data += String(static_cast<unsigned int>(writeWritten_));
  data += "}";
  return ok(cmd, "file_write_chunk_written", data);
}

// file_write_finish
String ControlServer::cmdFileWriteFinish(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  const String path = extractString(request, "path");
  if (path != writePath_ || writeWritten_ != writeExpectedSize_) {
    return error(cmd, "file_write_incomplete");
  }
  // Optional integrity check. file_write_chunk verifies only that offsets are
  // sequential, so a chunk that is the right LENGTH but wrong content passes
  // silently -- which for a package means installing something nobody wrote.
  const String expectedSha = extractString(request, "sha256");
  String actualSha;
  if (!expectedSha.isEmpty()) {
    std::vector<uint8_t> written;
    if (storage_ == nullptr ||
        !storage_->readFile(writeScope_, writePath_, written, 0, writeWritten_)) {
      return error(cmd, "file_read_failed");
    }
    actualSha = AppPackage::sha256Hex(written.data(), written.size());
    if (!actualSha.equalsIgnoreCase(expectedSha)) {
      // Delete it: a corrupt half-file that looks complete is worse than no
      // file at all, because the next step would happily consume it.
      storage_->deleteFile(writeScope_, writePath_);
      writeScope_ = "";
      writePath_ = "";
      writeExpectedSize_ = 0;
      writeWritten_ = 0;
      return error(cmd, "file_checksum_mismatch");
    }
  }
  String data = "{\"scope\":\"";
  data += writeScope_;
  data += "\",\"path\":\"";
  data += writePath_;
  data += "\",\"size\":";
  data += String(static_cast<unsigned int>(writeWritten_));
  if (!actualSha.isEmpty()) {
    data += ",\"sha256\":\"";
    data += actualSha;
    data += "\"";
  }
  data += "}";
  writeScope_ = "";
  writePath_ = "";
  writeExpectedSize_ = 0;
  writeWritten_ = 0;
  return ok(cmd, "file_write_finished", data);
}

// file_delete
String ControlServer::cmdFileDelete(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  String scope = extractString(request, "scope");
  if (ProcFs::isProcScope(scope)) {
    return error(cmd, "read_only_scope");
  }
  if (scope.isEmpty()) {
    scope = "user";
  }
  String path = extractString(request, "path");
  if (!storage_->deleteFile(scope, path)) {
    return error(cmd, "file_delete_failed");
  }
  String data = "{\"scope\":\"";
  data += scope;
  data += "\",\"path\":\"";
  data += path;
  data += "\"}";
  return ok(cmd, "file_deleted", data);
}

}  // namespace nhos
