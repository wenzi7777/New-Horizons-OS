#include "ServiceManager.h"

#include "JsonUtils.h"

namespace nhos {

bool ServiceManager::registerService(const char* name, bool startedOk, StartFn start,
                                     HealthFn healthy, void* ctx) {
  if (count_ >= kMaxServices || name == nullptr || indexOf(name) >= 0) {
    return false;
  }
  Service& service = services_[count_++];
  service.name = name;
  service.start = start;
  service.healthy = healthy;
  service.ctx = ctx;
  service.state = startedOk ? ServiceState::Running : ServiceState::Degraded;
  if (!startedOk) {
    // A module that never came up is retried on the same schedule as one
    // that failed later, rather than being written off for the whole boot.
    service.nextAttemptMs = millis() + kBaseBackoffMs;
  }
  return true;
}

int8_t ServiceManager::indexOf(const char* name) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (strcmp(services_[i].name, name) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

void ServiceManager::scheduleRetry(Service& service, uint32_t nowMs) {
  if (service.start == nullptr) {
    // Not restartable: report the truth and stop pretending a retry is coming.
    service.state = ServiceState::Failed;
    return;
  }
  if (service.attempts >= kMaxAttempts) {
    service.state = ServiceState::Failed;
    return;
  }
  uint32_t backoff = kBaseBackoffMs;
  for (uint8_t i = 0; i < service.attempts && backoff < kMaxBackoffMs; ++i) {
    backoff *= 2;
  }
  if (backoff > kMaxBackoffMs) {
    backoff = kMaxBackoffMs;
  }
  service.state = ServiceState::Degraded;
  service.nextAttemptMs = nowMs + backoff;
}

void ServiceManager::attemptRestart(Service& service, uint32_t nowMs) {
  service.state = ServiceState::Starting;
  ++service.attempts;
  ++service.restarts;
  const bool ok = service.start(service.ctx);
  if (ok) {
    service.state = ServiceState::Running;
    service.attempts = 0;
    return;
  }
  scheduleRetry(service, nowMs);
}

void ServiceManager::service(uint32_t nowMs) {
  if (lastPollMs_ != 0 && nowMs - lastPollMs_ < kHealthPollMs) {
    return;
  }
  lastPollMs_ = nowMs;

  for (uint8_t i = 0; i < count_; ++i) {
    Service& service = services_[i];
    switch (service.state) {
      case ServiceState::Running: {
        if (service.healthy != nullptr && !service.healthy(service.ctx)) {
          ++service.healthFailures;
          scheduleRetry(service, nowMs);
        }
        break;
      }
      case ServiceState::Degraded: {
        if (service.start != nullptr &&
            static_cast<int32_t>(nowMs - service.nextAttemptMs) >= 0) {
          attemptRestart(service, nowMs);
        }
        break;
      }
      case ServiceState::Starting:
      case ServiceState::Stopped:
      case ServiceState::Failed:
      default:
        break;
    }
  }
}

bool ServiceManager::restart(const char* name) {
  const int8_t index = indexOf(name);
  if (index < 0) {
    return false;
  }
  Service& service = services_[index];
  if (service.start == nullptr) {
    return false;
  }
  service.attempts = 0;
  attemptRestart(service, millis());
  return service.state == ServiceState::Running;
}

const char* ServiceManager::stateName(ServiceState state) {
  switch (state) {
    case ServiceState::Stopped: return "stopped";
    case ServiceState::Starting: return "starting";
    case ServiceState::Running: return "running";
    case ServiceState::Degraded: return "degraded";
    case ServiceState::Failed: return "failed";
    default: return "unknown";
  }
}

String ServiceManager::statusJson() const {
  String json = "{\"count\":";
  json += String(count_);
  json += ",\"services\":[";
  for (uint8_t i = 0; i < count_; ++i) {
    const Service& service = services_[i];
    if (i != 0) {
      json += ",";
    }
    json += "{\"name\":\"";
    json += jsonEscape(String(service.name));
    json += "\",\"state\":\"";
    json += stateName(service.state);
    json += "\",\"restartable\":";
    json += service.start != nullptr ? "true" : "false";
    json += ",\"restarts\":";
    json += String(service.restarts);
    json += ",\"health_failures\":";
    json += String(service.healthFailures);
    json += "}";
  }
  json += "]}";
  return json;
}

String ServiceManager::servicesText() const {
  String out = "NAME              STATE      RESTARTABLE  RESTARTS  HEALTH_FAILS\n";
  for (uint8_t i = 0; i < count_; ++i) {
    const Service& service = services_[i];
    String name(service.name);
    while (name.length() < 17) {
      name += ' ';
    }
    out += name;
    String state(stateName(service.state));
    while (state.length() < 11) {
      state += ' ';
    }
    out += state;
    out += service.start != nullptr ? "yes          " : "no           ";
    String restarts(service.restarts);
    while (restarts.length() < 9) {
      restarts = " " + restarts;
    }
    out += restarts;
    String fails(service.healthFailures);
    while (fails.length() < 14) {
      fails = " " + fails;
    }
    out += fails;
    out += '\n';
  }
  return out;
}

}  // namespace nhos
