#pragma once

#include <Arduino.h>

// The command table's types. Every command ControlServer accepts is one row
// of ControlServer::commandTable() (ControlCommandTable.cpp): its name, the
// member that handles it, and the flags below.
//
// Before the table, dispatch was an 87-branch if-chain and everything ABOUT a
// command -- which mode it needs, whether it flashes the LED, whether it is
// expensive -- lived somewhere else: two different maintenance idioms inside
// the bodies, a hardcoded quiet list, and the Desktop's own copies of all of
// it. The flags make those properties data the device itself enforces and
// reports (`commands`), so a client no longer has to mirror them by hand.

namespace nhos {

class ControlServer;

struct CommandContext {
  const String& cmd;      // the name it was invoked as (status vs query)
  const String& request;  // the full request JSON
};

enum CommandFlag : uint16_t {
  // Refused with maintenance_required outside maintenance mode, by the
  // dispatcher, before the handler runs.
  kCmdNeedsMaintenance = 1 << 0,
  // Never flashes the "command received" LED: polled continuously.
  kCmdQuietLed = 1 << 1,
  // Expensive to answer (~14 ms for status). Over UDP it is held until the
  // gap after a scan so it does not delay one (HeavyCommandPacing).
  kCmdHeavy = 1 << 2,
  // Used by the Desktop's own flows (transport switch, WiFi credentials), not
  // offered to an operator directly.
  kCmdInternal = 1 << 3,
  // A status snapshot: its data merges into the client's device state.
  kCmdStatusSnapshot = 1 << 4,
};

using CommandHandler = String (ControlServer::*)(const CommandContext&);

struct CommandSpec {
  const char* name;
  CommandHandler handler;
  uint16_t flags;
  const char* summary;
};

}  // namespace nhos
