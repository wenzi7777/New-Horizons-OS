#pragma once

#include <Arduino.h>

namespace nhos {

enum class ServiceState : uint8_t {
  Stopped = 0,   // never brought up, or deliberately disabled
  Starting = 1,  // a restart attempt is in flight
  Running = 2,   // up and passing its health probe
  Degraded = 3,  // up but its health probe is failing; a retry is scheduled
  Failed = 4,    // gave up after kMaxAttempts; only an operator restarts it
};

// Supervision for the modules setup() brings up.
//
// Before this, a module that failed to initialise just logged a line and was
// gone for the rest of the boot: a wedged I2C sensor meant `imu.setEnabled(false)`
// forever, with nothing to report and nothing to retry. "Degraded operation"
// was an implicit concept spread across a dozen log strings; this makes it an
// explicit, queryable state with a retry policy behind it.
//
// Note on scope: setup()'s 16 boot stages are NOT reordered into a dependency
// graph here. That ordering already works, is documented stage by stage, and
// rewriting it buys little next to the risk. Services register in the order
// setup() starts them, which is the same declaration order a graph would have
// produced.
class ServiceManager {
 public:
  // Returns true if the (re)start succeeded. nullptr means "cannot be
  // restarted at runtime" -- foundational services register this way and are
  // reported but never retried.
  using StartFn = bool (*)(void* ctx);
  // nullptr means "assumed healthy whenever it is Running".
  using HealthFn = bool (*)(void* ctx);
  // Operator stop. nullptr means the service cannot be stopped at runtime --
  // the default, so stopping is something a service opts into.
  using StopFn = void (*)(void* ctx);

  enum class OpResult : uint8_t {
    Ok = 0,
    UnknownService,
    NotStoppable,
    NotRestartable,
    AlreadyStopped,
    AlreadyRunning,
    StartFailed,
  };

  // Retry backoff: 5s doubling to a 5min ceiling, then give up.
  static constexpr uint32_t kBaseBackoffMs = 5000;
  static constexpr uint32_t kMaxBackoffMs = 300000;
  static constexpr uint8_t kMaxAttempts = 5;
  static constexpr uint32_t kHealthPollMs = 1000;

  bool registerService(const char* name, bool startedOk, StartFn start = nullptr,
                       HealthFn healthy = nullptr, void* ctx = nullptr, StopFn stop = nullptr);

  void service(uint32_t nowMs);

  // Operator-initiated. Clears the backoff and the Failed verdict, so a
  // service that exhausted its automatic retries can still be recovered
  // without rebooting the whole device.
  // Also clears an operator stop: a restart is a start.
  bool restart(const char* name);
  bool exists(const char* name) const { return indexOf(name) >= 0; }

  // Operator stop/start (service_stop / service_start). Runtime only: never
  // persisted, so a reboot brings every service back. A stopped service is
  // left alone by supervision -- no health probe, no automatic retry -- until
  // an operator starts it again.
  OpResult stop(const char* name);
  OpResult start(const char* name);
  // Code that brings a module back on its own (soft-off resume, leaving
  // maintenance, a layout change) asks this first, or it would silently undo
  // the operator's stop.
  bool isOperatorStopped(const char* name) const;
  static const char* opResultCode(OpResult result);

  static const char* stateName(ServiceState state);
  String statusJson() const;
  String servicesText() const;

 private:
  static constexpr uint8_t kMaxServices = 16;

  struct Service {
    const char* name = nullptr;
    StartFn start = nullptr;
    HealthFn healthy = nullptr;
    StopFn stop = nullptr;
    void* ctx = nullptr;
    bool operatorStopped = false;
    ServiceState state = ServiceState::Stopped;
    uint8_t attempts = 0;
    uint32_t nextAttemptMs = 0;
    uint32_t restarts = 0;
    uint32_t healthFailures = 0;
  };

  int8_t indexOf(const char* name) const;
  void scheduleRetry(Service& service, uint32_t nowMs);
  void attemptRestart(Service& service, uint32_t nowMs);

  Service services_[kMaxServices];
  uint8_t count_ = 0;
  uint32_t lastPollMs_ = 0;
};

}  // namespace nhos
