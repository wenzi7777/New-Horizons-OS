#include "Watchdog.h"

#include <Arduino.h>

namespace nhos {
namespace {
bool g_armed = false;
}  // namespace

void watchdogArm() {
  if (g_armed) {
    return;
  }
  enableLoopWDT();
  g_armed = true;
}

void watchdogDisarm() {
  if (!g_armed) {
    return;
  }
  disableLoopWDT();
  g_armed = false;
}

bool watchdogArmed() { return g_armed; }

void watchdogFeed() {
  if (!g_armed) {
    return;
  }
  feedLoopWDT();
}

}  // namespace nhos
