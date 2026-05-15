#include "py/runtime.h"
#include "esp_system.h"
#include "vdboard.h"

static mp_obj_t vdboard_sys_reboot(void) {
    esp_restart();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_sys_reboot_obj, vdboard_sys_reboot);

static mp_obj_t vdboard_sys_info(void) {
    mp_obj_t tuple[4];
    tuple[0] = mp_obj_new_str("vdboard-native", 14);
    tuple[1] = mp_obj_new_int(VDBOARD_PAYLOAD_TYPE_MV_U16);
    tuple[2] = mp_obj_new_int(sizeof(vdboard_frame_header_t));
    tuple[3] = mp_obj_new_int(sizeof(vdboard_scan_state_t));
    return mp_obj_new_tuple(4, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_sys_info_obj, vdboard_sys_info);

static const mp_rom_map_elem_t vdboard_sys_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sys) },
    { MP_ROM_QSTR(MP_QSTR_reboot), MP_ROM_PTR(&vdboard_sys_reboot_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&vdboard_sys_info_obj) },
};

static MP_DEFINE_CONST_DICT(vdboard_sys_globals, vdboard_sys_globals_table);

const mp_obj_module_t vdboard_sys_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vdboard_sys_globals,
};
