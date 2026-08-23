#include "PowerStatusJson.h"

namespace nhos {

std::string formatPowerStatusJson(const PowerStatusJsonSnapshot& s) {
  return std::string("{\"state\":\"") + s.state +
      "\",\"detail\":\"" + s.detail +
      "\",\"charge_state\":\"" + s.state +
      "\",\"charger\":\"" + s.charger +
      "\",\"supported\":" + (s.supported ? "true" : "false") +
      ",\"charger_controller_detected\":" + (s.detected ? "true" : "false") +
      ",\"charger_detected\":" + (s.chargerDetected ? "true" : "false") +
      ",\"soft_off_recommended\":" + ((s.chargerDetected || std::string(s.state) != "not_charging") ? "true" : "false") +
      ",\"configured\":" + (s.configured ? "true" : "false") +
      ",\"profile\":\"" + s.profile +
      "\",\"charge_profile\":\"" + s.profile +
      "\",\"charge_current_ma\":" + std::to_string(s.chargeCurrentMa) +
      ",\"input_limit_ma\":" + std::to_string(s.inputLimitMa) +
      ",\"vbat_reg_mv\":" + std::to_string(s.vbatRegMv) +
      ",\"termination_percent\":" + std::to_string(s.terminationPercent) +
      ",\"precharge_percent\":" + std::to_string(s.prechargePercent) +
      ",\"safety_timer_hours\":" + std::to_string(s.safetyTimerHours) +
      ",\"stat0\":" + std::to_string(s.stat0) +
      ",\"last_stat0\":" + std::to_string(s.stat0) +
      ",\"last_error\":\"" + s.lastError +
      "\",\"config_error\":\"" + s.configError +
      "\",\"temperature_monitoring\":\"bypassed\"}";
}

}  // namespace nhos
