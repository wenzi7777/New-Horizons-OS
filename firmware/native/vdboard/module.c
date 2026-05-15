#include "py/runtime.h"
#include "vdboard.h"

static const mp_rom_map_elem_t vdboard_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_vdboard) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&vdboard_scan_module) },
    { MP_ROM_QSTR(MP_QSTR_sys), MP_ROM_PTR(&vdboard_sys_module) },
};

static MP_DEFINE_CONST_DICT(vdboard_globals, vdboard_globals_table);

const mp_obj_module_t vdboard_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vdboard_globals,
};

MP_REGISTER_MODULE(MP_QSTR_vdboard, vdboard_user_cmodule);
