#include <string.h>

#include "py/runtime.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "vdboard.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

typedef enum {
    VDBOARD_PROV_STATUS_IDLE = 0,
    VDBOARD_PROV_STATUS_STARTING,
    VDBOARD_PROV_STATUS_WAITING_CREDENTIALS,
    VDBOARD_PROV_STATUS_CONNECTING_WIFI,
    VDBOARD_PROV_STATUS_CONNECTED,
    VDBOARD_PROV_STATUS_FAILED,
} vdboard_prov_status_code_t;

typedef enum {
    VDBOARD_WIFI_STATE_IDLE = 0,
    VDBOARD_WIFI_STATE_CONNECTING,
    VDBOARD_WIFI_STATE_CONNECTED,
    VDBOARD_WIFI_STATE_DISCONNECTED,
} vdboard_wifi_state_code_t;

typedef struct _vdboard_prov_state_t {
    bool supported;
    bool provisioned;
    bool manager_initialized;
    bool handlers_registered;
    bool provisioning_active;
    vdboard_prov_status_code_t status_code;
    vdboard_wifi_state_code_t wifi_state_code;
    wifi_prov_sta_fail_reason_t last_fail_reason;
    char service_name[32];
    char pop[65];
} vdboard_prov_state_t;

static vdboard_prov_state_t g_prov_state = {
    .supported = true,
    .provisioned = false,
    .manager_initialized = false,
    .handlers_registered = false,
    .provisioning_active = false,
    .status_code = VDBOARD_PROV_STATUS_IDLE,
    .wifi_state_code = VDBOARD_WIFI_STATE_IDLE,
    .last_fail_reason = WIFI_PROV_STA_AUTH_ERROR,
    .service_name = "New Horizons OS",
    .pop = "abcd1234",
};

static const char *vdboard_prov_status_str(vdboard_prov_status_code_t code) {
    switch (code) {
        case VDBOARD_PROV_STATUS_STARTING:
            return "starting";
        case VDBOARD_PROV_STATUS_WAITING_CREDENTIALS:
            return "waiting_credentials";
        case VDBOARD_PROV_STATUS_CONNECTING_WIFI:
            return "connecting_wifi";
        case VDBOARD_PROV_STATUS_CONNECTED:
            return "connected";
        case VDBOARD_PROV_STATUS_FAILED:
            return "failed";
        case VDBOARD_PROV_STATUS_IDLE:
        default:
            return "idle";
    }
}

static const char *vdboard_wifi_state_str(vdboard_wifi_state_code_t code) {
    switch (code) {
        case VDBOARD_WIFI_STATE_CONNECTING:
            return "connecting";
        case VDBOARD_WIFI_STATE_CONNECTED:
            return "connected";
        case VDBOARD_WIFI_STATE_DISCONNECTED:
            return "disconnected";
        case VDBOARD_WIFI_STATE_IDLE:
        default:
            return "idle";
    }
}

static void vdboard_prov_set_status(vdboard_prov_status_code_t status_code) {
    g_prov_state.status_code = status_code;
}

static void vdboard_prov_set_wifi_state(vdboard_wifi_state_code_t wifi_state_code) {
    g_prov_state.wifi_state_code = wifi_state_code;
}

static void vdboard_prov_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                vdboard_prov_set_status(VDBOARD_PROV_STATUS_WAITING_CREDENTIALS);
                break;
            case WIFI_PROV_CRED_RECV:
                vdboard_prov_set_status(VDBOARD_PROV_STATUS_CONNECTING_WIFI);
                vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_CONNECTING);
                break;
            case WIFI_PROV_CRED_FAIL:
                g_prov_state.last_fail_reason = *((wifi_prov_sta_fail_reason_t *)event_data);
                vdboard_prov_set_status(VDBOARD_PROV_STATUS_FAILED);
                vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_DISCONNECTED);
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            case WIFI_PROV_CRED_SUCCESS:
                vdboard_prov_set_status(VDBOARD_PROV_STATUS_CONNECTING_WIFI);
                vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_CONNECTING);
                g_prov_state.provisioned = true;
                break;
            case WIFI_PROV_END:
                g_prov_state.provisioning_active = false;
                wifi_prov_mgr_deinit();
                g_prov_state.manager_initialized = false;
                if (g_prov_state.provisioned) {
                    vdboard_prov_set_status(VDBOARD_PROV_STATUS_CONNECTED);
                } else if (g_prov_state.status_code != VDBOARD_PROV_STATUS_FAILED) {
                    vdboard_prov_set_status(VDBOARD_PROV_STATUS_IDLE);
                }
                break;
            default:
                break;
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_CONNECTING);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_DISCONNECTED);
                if (g_prov_state.provisioning_active) {
                    vdboard_prov_set_status(VDBOARD_PROV_STATUS_CONNECTING_WIFI);
                }
                break;
            default:
                break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_prov_state.provisioned = true;
        g_prov_state.provisioning_active = false;
        vdboard_prov_set_status(VDBOARD_PROV_STATUS_CONNECTED);
        vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_CONNECTED);
    }
}

static esp_err_t vdboard_prov_register_handlers(void) {
    if (g_prov_state.handlers_registered) {
        return ESP_OK;
    }

    esp_err_t err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &vdboard_prov_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &vdboard_prov_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &vdboard_prov_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    g_prov_state.handlers_registered = true;
    return ESP_OK;
}

static esp_err_t vdboard_prov_ensure_wifi_stack(void) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    return vdboard_prov_register_handlers();
}

static esp_err_t vdboard_prov_ensure_manager(void) {
    if (g_prov_state.manager_initialized) {
        return ESP_OK;
    }

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .wifi_prov_conn_cfg = {
            .wifi_conn_attempts = 0,
        },
    };

    esp_err_t err = wifi_prov_mgr_init(config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    g_prov_state.manager_initialized = true;
    return ESP_OK;
}

static esp_err_t vdboard_prov_refresh_state(void) {
    esp_err_t err = vdboard_prov_ensure_wifi_stack();
    if (err != ESP_OK) {
        return err;
    }
    err = vdboard_prov_ensure_manager();
    if (err != ESP_OK) {
        return err;
    }

    bool provisioned = false;
    err = wifi_prov_mgr_is_provisioned(&provisioned);
    if (err == ESP_OK) {
        g_prov_state.provisioned = provisioned;
    }
    return err;
}

static mp_obj_t vdboard_prov_is_supported(void) {
    return mp_obj_new_bool(g_prov_state.supported);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_is_supported_obj, vdboard_prov_is_supported);

static mp_obj_t vdboard_prov_is_provisioned(void) {
    if (vdboard_prov_refresh_state() == ESP_OK) {
        return mp_obj_new_bool(g_prov_state.provisioned);
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_is_provisioned_obj, vdboard_prov_is_provisioned);

static mp_obj_t vdboard_prov_start_ble(mp_obj_t pop_obj, mp_obj_t service_name_obj) {
    const char *pop = pop_obj == mp_const_none ? g_prov_state.pop : mp_obj_str_get_str(pop_obj);
    const char *service_name = service_name_obj == mp_const_none ? g_prov_state.service_name : mp_obj_str_get_str(service_name_obj);

    strlcpy(g_prov_state.pop, pop, sizeof(g_prov_state.pop));
    strlcpy(g_prov_state.service_name, service_name, sizeof(g_prov_state.service_name));

    esp_err_t err = vdboard_prov_refresh_state();
    if (err != ESP_OK) {
        vdboard_prov_set_status(VDBOARD_PROV_STATUS_FAILED);
        return mp_const_false;
    }

    if (g_prov_state.provisioned) {
        err = wifi_prov_mgr_reset_provisioning();
        if (err != ESP_OK) {
            vdboard_prov_set_status(VDBOARD_PROV_STATUS_FAILED);
            return mp_const_false;
        }
        g_prov_state.provisioned = false;
    }

    wifi_prov_mgr_disable_auto_stop(1000);
    wifi_prov_mgr_set_app_info("New Horizons OS", "0.1.0", NULL, 0);

    vdboard_prov_set_status(VDBOARD_PROV_STATUS_STARTING);
    vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_IDLE);

    err = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1,
        (const void *)g_prov_state.pop,
        g_prov_state.service_name,
        NULL
    );
    if (err != ESP_OK) {
        vdboard_prov_set_status(VDBOARD_PROV_STATUS_FAILED);
        return mp_const_false;
    }

    g_prov_state.provisioning_active = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vdboard_prov_start_ble_obj, vdboard_prov_start_ble);

static mp_obj_t vdboard_prov_stop(void) {
    if (g_prov_state.manager_initialized) {
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        g_prov_state.manager_initialized = false;
    }
    g_prov_state.provisioning_active = false;
    vdboard_prov_set_status(VDBOARD_PROV_STATUS_IDLE);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_stop_obj, vdboard_prov_stop);

static mp_obj_t vdboard_prov_status(void) {
    const char *status = vdboard_prov_status_str(g_prov_state.status_code);
    return mp_obj_new_str(status, strlen(status));
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_status_obj, vdboard_prov_status);

static mp_obj_t vdboard_prov_reset_credentials(void) {
    esp_err_t err = vdboard_prov_refresh_state();
    if (err != ESP_OK) {
        return mp_const_false;
    }

    err = wifi_prov_mgr_reset_provisioning();
    if (err != ESP_OK) {
        vdboard_prov_set_status(VDBOARD_PROV_STATUS_FAILED);
        return mp_const_false;
    }

    g_prov_state.provisioned = false;
    g_prov_state.provisioning_active = false;
    vdboard_prov_set_status(VDBOARD_PROV_STATUS_IDLE);
    vdboard_prov_set_wifi_state(VDBOARD_WIFI_STATE_IDLE);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_reset_credentials_obj, vdboard_prov_reset_credentials);

static mp_obj_t vdboard_prov_wifi_state(void) {
    const char *status = vdboard_wifi_state_str(g_prov_state.wifi_state_code);
    return mp_obj_new_str(status, strlen(status));
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_wifi_state_obj, vdboard_prov_wifi_state);

#else

static mp_obj_t vdboard_prov_is_supported(void) {
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_is_supported_obj, vdboard_prov_is_supported);

static mp_obj_t vdboard_prov_is_provisioned(void) {
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_is_provisioned_obj, vdboard_prov_is_provisioned);

static mp_obj_t vdboard_prov_start_ble(mp_obj_t pop, mp_obj_t service_name) {
    (void)pop;
    (void)service_name;
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vdboard_prov_start_ble_obj, vdboard_prov_start_ble);

static mp_obj_t vdboard_prov_stop(void) {
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_stop_obj, vdboard_prov_stop);

static mp_obj_t vdboard_prov_status(void) {
    return mp_obj_new_str("unsupported", 11);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_status_obj, vdboard_prov_status);

static mp_obj_t vdboard_prov_reset_credentials(void) {
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_reset_credentials_obj, vdboard_prov_reset_credentials);

static mp_obj_t vdboard_prov_wifi_state(void) {
    return mp_obj_new_str("unsupported", 11);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_prov_wifi_state_obj, vdboard_prov_wifi_state);

#endif

static const mp_rom_map_elem_t vdboard_prov_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_prov) },
    { MP_ROM_QSTR(MP_QSTR_is_supported), MP_ROM_PTR(&vdboard_prov_is_supported_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_provisioned), MP_ROM_PTR(&vdboard_prov_is_provisioned_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_ble), MP_ROM_PTR(&vdboard_prov_start_ble_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&vdboard_prov_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&vdboard_prov_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_credentials), MP_ROM_PTR(&vdboard_prov_reset_credentials_obj) },
    { MP_ROM_QSTR(MP_QSTR_wifi_state), MP_ROM_PTR(&vdboard_prov_wifi_state_obj) },
};

static MP_DEFINE_CONST_DICT(vdboard_prov_globals, vdboard_prov_globals_table);

const mp_obj_module_t vdboard_prov_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vdboard_prov_globals,
};
