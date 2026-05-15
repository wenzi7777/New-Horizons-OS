#ifndef VDBOARD_H
#define VDBOARD_H

#include "py/obj.h"
#include <stdint.h>
#include <stdbool.h>

#define VDBOARD_PAYLOAD_TYPE_MV_U16 (1)

typedef struct _vdboard_frame_header_t {
    uint32_t seq;
    uint32_t timestamp_ms;
    uint16_t rows;
    uint16_t cols;
    uint16_t point_count;
    uint16_t payload_type;
} vdboard_frame_header_t;

typedef struct _vdboard_scan_state_t {
    uint16_t rows;
    uint16_t cols;
    uint16_t point_count;
    uint16_t fps;
    uint16_t settle_us;
    uint16_t buffer_frames;
    int8_t core_id;
    bool started;
    uint32_t produced_frames;
    uint32_t consumed_frames;
    uint32_t dropped_frames;
    uint32_t last_written_seq;
    uint32_t last_read_seq;
} vdboard_scan_state_t;

extern const mp_obj_module_t vdboard_scan_module;
extern const mp_obj_module_t vdboard_sys_module;

#endif
