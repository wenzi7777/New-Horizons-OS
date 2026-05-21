#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "adc.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "vdboard.h"

#define VDBOARD_SCAN_MAX_ROWS (10)
#define VDBOARD_SCAN_MAX_COLS (21)
#define VDBOARD_SCAN_MAX_POINTS (VDBOARD_SCAN_MAX_ROWS * VDBOARD_SCAN_MAX_COLS)
#define VDBOARD_SCAN_MAX_RING_FRAMES (8)
#define VDBOARD_SCAN_DEFAULT_FPS (60)
#define VDBOARD_SCAN_DEFAULT_SETTLE_US (20)
#define VDBOARD_SCAN_DEFAULT_BUFFER_FRAMES (2)
#define VDBOARD_SCAN_DEFAULT_CORE (1)
#define VDBOARD_SCAN_ACTIVE_LEVEL (0)
#define VDBOARD_SCAN_IDLE_LEVEL (1)
#define VDBOARD_PACKET_MAGIC (0xA55A)
#define VDBOARD_PACKET_VERSION (2)
#define VDBOARD_PACKET_HEADER_LEN (20)
#define VDBOARD_PACKET_FLAG_IMU (0x01)
#define VDBOARD_PACKET_FLAG_BATTERY (0x02)
#define VDBOARD_PACKET_FLAG_HMAC (0x80)
#define VDBOARD_STREAM_HMAC_MAX_LEN (32)
#define VDBOARD_STREAM_HMAC_KEY_MAX_LEN (64)
#define VDBOARD_STREAM_MAX_CAL_POINTS (16)
#define VDBOARD_STREAM_MEDIAN_MAX (5)
#define VDBOARD_SCAN_MAX_FRAME_SIZE (sizeof(vdboard_frame_header_t) + (VDBOARD_SCAN_MAX_POINTS * sizeof(uint16_t)))
#define VDBOARD_STREAM_MAX_PACKET_SIZE (VDBOARD_PACKET_HEADER_LEN + (VDBOARD_SCAN_MAX_POINTS * sizeof(float)) + (7 * sizeof(float)) + 4 + VDBOARD_STREAM_HMAC_MAX_LEN)

typedef struct _vdboard_stream_cal_point_t {
    float sample_mv;
    float level;
} vdboard_stream_cal_point_t;

typedef struct _vdboard_stream_filter_state_t {
    bool initialized;
    float lowpass;
    uint8_t median_count;
    uint8_t median_index;
    float median_values[VDBOARD_STREAM_MEDIAN_MAX];
} vdboard_stream_filter_state_t;

typedef struct _vdboard_stream_context_t {
    uint8_t *frame_scratch;
    uint8_t *packet_scratch;
    uint32_t packet_scratch_size;
    vdboard_stream_filter_state_t *filter_states;
    vdboard_stream_cal_point_t *calibration_points;
    uint8_t *calibration_counts;
    uint16_t *calibration_offsets;
    uint16_t calibration_point_capacity;
    uint16_t capacity_points;
    bool filter_enabled;
    uint8_t filter_median;
    float filter_alpha;
    uint8_t device_uid[6];
    bool use_hmac;
    uint8_t hmac_len;
    uint8_t hmac_key_len;
    uint8_t hmac_key[VDBOARD_STREAM_HMAC_KEY_MAX_LEN];
    bool imu_valid;
    float imu_values[7];
    bool battery_valid;
    uint8_t battery_status;
    uint8_t battery_fault;
    uint16_t battery_mv;
    uint32_t packet_frames;
} vdboard_stream_context_t;

static uint16_t g_scan_payload_mv_storage[VDBOARD_SCAN_MAX_POINTS];
static uint8_t g_scan_frame_storage[VDBOARD_SCAN_MAX_RING_FRAMES * VDBOARD_SCAN_MAX_FRAME_SIZE];
static uint8_t g_stream_frame_scratch[VDBOARD_SCAN_MAX_FRAME_SIZE];
static uint8_t g_stream_packet_scratch[VDBOARD_STREAM_MAX_PACKET_SIZE];
static vdboard_stream_filter_state_t g_stream_filter_states[VDBOARD_SCAN_MAX_POINTS];
static uint8_t g_stream_calibration_counts[VDBOARD_SCAN_MAX_POINTS];
static uint16_t g_stream_calibration_offsets[VDBOARD_SCAN_MAX_POINTS];

typedef struct _vdboard_scan_context_t {
    uint16_t row_pins[VDBOARD_SCAN_MAX_ROWS];
    uint16_t col_pins[VDBOARD_SCAN_MAX_COLS];
    const machine_adc_obj_t *row_adc[VDBOARD_SCAN_MAX_ROWS];
    uint8_t row_pin_count;
    uint8_t col_pin_count;
    uint32_t frame_size;
    uint16_t *payload_mv;
    uint32_t payload_bytes;
    uint8_t *frame_storage;
    SemaphoreHandle_t hw_mutex;
    portMUX_TYPE ring_lock;
    uint16_t read_index;
    uint16_t write_index;
    uint16_t count;
    bool stop_requested;
    uint64_t next_scan_due_us;
    uint32_t scan_interval_us;
} vdboard_scan_context_t;

static vdboard_scan_state_t g_scan_state = {
    .rows = 0,
    .cols = 0,
    .point_count = 0,
    .fps = 0,
    .settle_us = 0,
    .buffer_frames = VDBOARD_SCAN_DEFAULT_BUFFER_FRAMES,
    .core_id = VDBOARD_SCAN_DEFAULT_CORE,
    .started = false,
    .produced_frames = 0,
    .consumed_frames = 0,
    .dropped_frames = 0,
    .last_written_seq = 0,
    .last_read_seq = 0,
};

static vdboard_scan_context_t g_scan_ctx = {
    .payload_mv = NULL,
    .payload_bytes = 0,
    .frame_storage = NULL,
    .hw_mutex = NULL,
    .ring_lock = portMUX_INITIALIZER_UNLOCKED,
    .read_index = 0,
    .write_index = 0,
    .count = 0,
    .stop_requested = false,
    .next_scan_due_us = 0,
    .scan_interval_us = 1000000UL / VDBOARD_SCAN_DEFAULT_FPS,
};

static vdboard_stream_context_t g_stream_ctx = {
    .frame_scratch = NULL,
    .packet_scratch = NULL,
    .packet_scratch_size = 0,
    .filter_states = NULL,
    .calibration_points = NULL,
    .calibration_counts = NULL,
    .calibration_offsets = NULL,
    .calibration_point_capacity = 0,
    .capacity_points = 0,
    .filter_enabled = false,
    .filter_median = 3,
    .filter_alpha = 0.25f,
    .device_uid = {0xA5, 0x5A, 0x00, 0x00, 0x00, 0x01},
    .use_hmac = false,
    .hmac_len = 0,
    .hmac_key_len = 0,
    .imu_valid = false,
    .battery_valid = false,
    .battery_status = 0,
    .battery_fault = 0,
    .battery_mv = 0,
    .packet_frames = 0,
};

static void vdboard_scan_release_storage(void);

static inline void vdboard_put_u16_le(uint8_t *dest, uint16_t value) {
    dest[0] = (uint8_t)(value & 0xff);
    dest[1] = (uint8_t)((value >> 8) & 0xff);
}

static inline void vdboard_put_u32_le(uint8_t *dest, uint32_t value) {
    dest[0] = (uint8_t)(value & 0xff);
    dest[1] = (uint8_t)((value >> 8) & 0xff);
    dest[2] = (uint8_t)((value >> 16) & 0xff);
    dest[3] = (uint8_t)((value >> 24) & 0xff);
}

static inline void vdboard_put_f32_le(uint8_t *dest, float value) {
    memcpy(dest, &value, sizeof(float));
}

static void vdboard_stream_release_buffers(void) {
    free(g_stream_ctx.calibration_points);
    g_stream_ctx.frame_scratch = NULL;
    g_stream_ctx.packet_scratch = NULL;
    g_stream_ctx.packet_scratch_size = 0;
    g_stream_ctx.filter_states = NULL;
    g_stream_ctx.calibration_points = NULL;
    g_stream_ctx.calibration_counts = NULL;
    g_stream_ctx.calibration_offsets = NULL;
    g_stream_ctx.calibration_point_capacity = 0;
    g_stream_ctx.capacity_points = 0;
    g_stream_ctx.packet_frames = 0;
}

static void vdboard_stream_release_filter_states(void) {
    g_stream_ctx.filter_states = NULL;
}

static void vdboard_stream_release_calibration(void) {
    free(g_stream_ctx.calibration_points);
    g_stream_ctx.calibration_points = NULL;
    g_stream_ctx.calibration_counts = NULL;
    g_stream_ctx.calibration_offsets = NULL;
    g_stream_ctx.calibration_point_capacity = 0;
}

static void vdboard_stream_reset_filter_states(void) {
    if (g_stream_ctx.filter_states != NULL && g_stream_ctx.capacity_points > 0) {
        memset(g_stream_ctx.filter_states, 0, g_stream_ctx.capacity_points * sizeof(vdboard_stream_filter_state_t));
    }
}

static void vdboard_stream_prepare_buffers(void) {
    vdboard_stream_release_buffers();
    if (g_scan_state.point_count == 0 || g_scan_ctx.frame_size == 0) {
        return;
    }
    g_stream_ctx.packet_scratch_size = VDBOARD_PACKET_HEADER_LEN
        + ((uint32_t)g_scan_state.point_count * sizeof(float))
        + (7 * sizeof(float))
        + 4
        + VDBOARD_STREAM_HMAC_MAX_LEN;
    if (g_scan_ctx.frame_size > VDBOARD_SCAN_MAX_FRAME_SIZE
            || g_stream_ctx.packet_scratch_size > VDBOARD_STREAM_MAX_PACKET_SIZE) {
        vdboard_scan_release_storage();
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("fixed stream buffer too small"));
    }
    g_stream_ctx.frame_scratch = g_stream_frame_scratch;
    g_stream_ctx.packet_scratch = g_stream_packet_scratch;
    g_stream_ctx.capacity_points = g_scan_state.point_count;
}

static void vdboard_stream_sort_floats(float *values, uint8_t count) {
    for (uint8_t i = 1; i < count; ++i) {
        float value = values[i];
        int j = (int)i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = value;
    }
}

static void vdboard_stream_sort_cal_points(vdboard_stream_cal_point_t *points, uint8_t count) {
    for (uint8_t i = 1; i < count; ++i) {
        vdboard_stream_cal_point_t value = points[i];
        int j = (int)i - 1;
        while (j >= 0 && points[j].sample_mv > value.sample_mv) {
            points[j + 1] = points[j];
            --j;
        }
        points[j + 1] = value;
    }
}

static float vdboard_stream_apply_filter(uint16_t sensor_index, float value) {
    if (!g_stream_ctx.filter_enabled || g_stream_ctx.filter_states == NULL || sensor_index >= g_stream_ctx.capacity_points) {
        return value;
    }

    vdboard_stream_filter_state_t *state = &g_stream_ctx.filter_states[sensor_index];
    uint8_t median = g_stream_ctx.filter_median;
    if (median > 1) {
        if (state->median_count < median) {
            state->median_count += 1;
        }
        state->median_values[state->median_index] = value;
        state->median_index = (state->median_index + 1) % median;
        float sorted[VDBOARD_STREAM_MEDIAN_MAX];
        for (uint8_t i = 0; i < state->median_count; ++i) {
            sorted[i] = state->median_values[i];
        }
        vdboard_stream_sort_floats(sorted, state->median_count);
        value = sorted[state->median_count / 2];
    }

    if (!state->initialized) {
        state->initialized = true;
        state->lowpass = value;
        return value;
    }

    state->lowpass = (g_stream_ctx.filter_alpha * value) + ((1.0f - g_stream_ctx.filter_alpha) * state->lowpass);
    return state->lowpass;
}

static float vdboard_stream_apply_calibration(uint16_t sensor_index, float raw_mv) {
    if (g_stream_ctx.calibration_counts == NULL
            || g_stream_ctx.calibration_offsets == NULL
            || g_stream_ctx.calibration_points == NULL
            || sensor_index >= g_stream_ctx.capacity_points) {
        return raw_mv;
    }
    uint8_t count = g_stream_ctx.calibration_counts[sensor_index];
    if (count < 2) {
        return raw_mv;
    }

    uint16_t offset = g_stream_ctx.calibration_offsets[sensor_index];
    if ((uint32_t)offset + count > g_stream_ctx.calibration_point_capacity) {
        return raw_mv;
    }
    vdboard_stream_cal_point_t *points = &g_stream_ctx.calibration_points[offset];
    if (raw_mv <= points[0].sample_mv) {
        return points[0].level;
    }
    if (raw_mv >= points[count - 1].sample_mv) {
        return points[count - 1].level;
    }
    for (uint8_t idx = 0; idx + 1 < count; ++idx) {
        float mv0 = points[idx].sample_mv;
        float mv1 = points[idx + 1].sample_mv;
        if (raw_mv >= mv0 && raw_mv <= mv1) {
            float level0 = points[idx].level;
            float level1 = points[idx + 1].level;
            if (mv1 == mv0) {
                return level1;
            }
            return level0 + (((raw_mv - mv0) / (mv1 - mv0)) * (level1 - level0));
        }
    }
    return raw_mv;
}

static void vdboard_stream_write_hmac(uint8_t *packet, size_t body_len, uint8_t *tag_dest) {
    if (!g_stream_ctx.use_hmac || g_stream_ctx.hmac_len == 0) {
        return;
    }
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("sha256 unavailable"));
    }
    uint8_t digest[VDBOARD_STREAM_HMAC_MAX_LEN];
    int result = mbedtls_md_hmac(
        info,
        g_stream_ctx.hmac_key,
        g_stream_ctx.hmac_key_len,
        packet,
        body_len,
        digest
    );
    if (result != 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("hmac failed"));
    }
    memcpy(tag_dest, digest, g_stream_ctx.hmac_len);
}

static inline mp_obj_t vdboard_dict_get(mp_obj_t dict_obj, qstr key) {
    if (dict_obj == mp_const_none) {
        return mp_const_none;
    }
    if (!mp_obj_is_dict_or_ordereddict(dict_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("layout must be dict"));
    }
    mp_map_t *map = mp_obj_dict_get_map(MP_OBJ_TO_PTR(dict_obj));
    mp_map_elem_t *elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return elem == NULL ? mp_const_none : elem->value;
}

static uint16_t vdboard_parse_pin_list(mp_obj_t obj, uint16_t *dest, uint16_t max_len, mp_rom_error_text_t label) {
    if (obj == mp_const_none) {
        return 0;
    }
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(obj, &len, &items);
    if (len == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin list empty"));
    }
    if (len > max_len) {
        mp_raise_ValueError(label);
    }
    for (size_t i = 0; i < len; ++i) {
        mp_int_t pin = mp_obj_get_int(items[i]);
        if (!GPIO_IS_VALID_GPIO(pin)) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid gpio"));
        }
        dest[i] = (uint16_t)pin;
    }
    return (uint16_t)len;
}

static void vdboard_scan_idle_all_columns(void) {
    for (uint16_t col = 0; col < g_scan_ctx.col_pin_count; ++col) {
        gpio_set_level((gpio_num_t)g_scan_ctx.col_pins[col], VDBOARD_SCAN_IDLE_LEVEL);
    }
}

static void vdboard_scan_activate_column(uint16_t col_index) {
    gpio_set_level((gpio_num_t)g_scan_ctx.col_pins[col_index], VDBOARD_SCAN_ACTIVE_LEVEL);
}

static uint16_t vdboard_read_row_mv(uint16_t row_index) {
    const machine_adc_obj_t *adc = g_scan_ctx.row_adc[row_index];
    mp_int_t uv = madcblock_read_uv_helper(adc->block, adc->channel_id, ADC_ATTEN_DB_11);
    if (uv < 0) {
        return 0;
    }
    return (uint16_t)((uv + 500) / 1000);
}

static void vdboard_capture_payload(uint16_t *payload_mv) {
    vdboard_scan_idle_all_columns();
    for (uint16_t col = 0; col < g_scan_ctx.col_pin_count; ++col) {
        vdboard_scan_activate_column(col);
        if (g_scan_state.settle_us > 0) {
            esp_rom_delay_us(g_scan_state.settle_us);
        }
        for (uint16_t row = 0; row < g_scan_ctx.row_pin_count; ++row) {
            payload_mv[row * g_scan_ctx.col_pin_count + col] = vdboard_read_row_mv(row);
        }
        gpio_set_level((gpio_num_t)g_scan_ctx.col_pins[col], VDBOARD_SCAN_IDLE_LEVEL);
    }
}

static void vdboard_commit_frame(const uint16_t *payload_mv, uint32_t timestamp_ms) {
    portENTER_CRITICAL(&g_scan_ctx.ring_lock);
    if (g_scan_ctx.count == g_scan_state.buffer_frames) {
        g_scan_ctx.read_index = (g_scan_ctx.read_index + 1) % g_scan_state.buffer_frames;
        g_scan_ctx.count -= 1;
        g_scan_state.dropped_frames += 1;
    }

    uint8_t *slot = g_scan_ctx.frame_storage + (g_scan_ctx.write_index * g_scan_ctx.frame_size);
    vdboard_frame_header_t header = {
        .seq = g_scan_state.last_written_seq + 1,
        .timestamp_ms = timestamp_ms,
        .rows = g_scan_state.rows,
        .cols = g_scan_state.cols,
        .point_count = g_scan_state.point_count,
        .payload_type = VDBOARD_PAYLOAD_TYPE_MV_U16,
    };
    memcpy(slot, &header, sizeof(header));
    memcpy(slot + sizeof(header), payload_mv, g_scan_state.point_count * sizeof(uint16_t));

    g_scan_ctx.write_index = (g_scan_ctx.write_index + 1) % g_scan_state.buffer_frames;
    g_scan_ctx.count += 1;
    g_scan_state.produced_frames += 1;
    g_scan_state.last_written_seq = header.seq;
    portEXIT_CRITICAL(&g_scan_ctx.ring_lock);
}

static void vdboard_scan_stop_task(void) {
    vdboard_scan_idle_all_columns();
    g_scan_state.started = false;
    g_scan_ctx.stop_requested = false;
    g_scan_ctx.next_scan_due_us = 0;
}

static void vdboard_scan_release_storage(void) {
    g_scan_ctx.payload_mv = NULL;
    g_scan_ctx.payload_bytes = 0;
    g_scan_ctx.frame_storage = NULL;
    vdboard_stream_release_buffers();
    g_scan_ctx.read_index = 0;
    g_scan_ctx.write_index = 0;
    g_scan_ctx.count = 0;
}

static void vdboard_scan_reset_stats(void) {
    g_scan_state.produced_frames = 0;
    g_scan_state.consumed_frames = 0;
    g_scan_state.dropped_frames = 0;
    g_scan_state.last_written_seq = 0;
    g_scan_state.last_read_seq = 0;
}

static void vdboard_scan_prepare_adc_rows(void) {
    mp_map_t empty_kwargs;
    mp_map_init_fixed_table(&empty_kwargs, 0, NULL);

    for (uint16_t row = 0; row < g_scan_ctx.row_pin_count; ++row) {
        const machine_adc_obj_t *adc = madc_search_helper(NULL, -1, (gpio_num_t)g_scan_ctx.row_pins[row]);
        if (adc == NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("row pin is not ADC"));
        }
        madc_init_helper(adc, 0, NULL, &empty_kwargs);
        mp_machine_adc_atten_set_helper(adc, ADC_ATTEN_DB_11);
        apply_self_adc_channel_atten(adc, ADC_ATTEN_DB_11);
        g_scan_ctx.row_adc[row] = adc;
        gpio_set_direction((gpio_num_t)g_scan_ctx.row_pins[row], GPIO_MODE_INPUT);
    }
}

static void vdboard_scan_prepare_columns(void) {
    for (uint16_t col = 0; col < g_scan_ctx.col_pin_count; ++col) {
        gpio_set_direction((gpio_num_t)g_scan_ctx.col_pins[col], GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_level((gpio_num_t)g_scan_ctx.col_pins[col], VDBOARD_SCAN_IDLE_LEVEL);
    }
}

static void vdboard_scan_configure(mp_obj_t layout_obj, mp_obj_t rows_obj, mp_obj_t cols_obj, mp_obj_t row_pins_obj, mp_obj_t col_pins_obj, mp_obj_t fps_obj, mp_obj_t settle_us_obj, mp_obj_t buffer_frames_obj, mp_obj_t core_id_obj) {
    rows_obj = rows_obj == mp_const_none ? vdboard_dict_get(layout_obj, MP_QSTR_rows) : rows_obj;
    cols_obj = cols_obj == mp_const_none ? vdboard_dict_get(layout_obj, MP_QSTR_cols) : cols_obj;
    row_pins_obj = row_pins_obj == mp_const_none ? vdboard_dict_get(layout_obj, MP_QSTR_row_pins) : row_pins_obj;
    col_pins_obj = col_pins_obj == mp_const_none ? vdboard_dict_get(layout_obj, MP_QSTR_col_pins) : col_pins_obj;

    if (row_pins_obj == mp_const_none || col_pins_obj == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("row_pins/col_pins required"));
    }

    vdboard_scan_stop_task();
    vdboard_scan_release_storage();

    memset(g_scan_ctx.row_pins, 0, sizeof(g_scan_ctx.row_pins));
    memset(g_scan_ctx.col_pins, 0, sizeof(g_scan_ctx.col_pins));
    memset(g_scan_ctx.row_adc, 0, sizeof(g_scan_ctx.row_adc));

    g_scan_ctx.row_pin_count = vdboard_parse_pin_list(row_pins_obj, g_scan_ctx.row_pins, VDBOARD_SCAN_MAX_ROWS, MP_ERROR_TEXT("too many row pins"));
    g_scan_ctx.col_pin_count = vdboard_parse_pin_list(col_pins_obj, g_scan_ctx.col_pins, VDBOARD_SCAN_MAX_COLS, MP_ERROR_TEXT("too many col pins"));

    g_scan_state.rows = rows_obj == mp_const_none ? g_scan_ctx.row_pin_count : (uint16_t)mp_obj_get_int(rows_obj);
    g_scan_state.cols = cols_obj == mp_const_none ? g_scan_ctx.col_pin_count : (uint16_t)mp_obj_get_int(cols_obj);
    if (g_scan_state.rows != g_scan_ctx.row_pin_count) {
        g_scan_state.rows = g_scan_ctx.row_pin_count;
    }
    if (g_scan_state.cols != g_scan_ctx.col_pin_count) {
        g_scan_state.cols = g_scan_ctx.col_pin_count;
    }

    g_scan_state.point_count = g_scan_state.rows * g_scan_state.cols;
    g_scan_state.fps = fps_obj == mp_const_none ? VDBOARD_SCAN_DEFAULT_FPS : (uint16_t)mp_obj_get_int(fps_obj);
    g_scan_state.settle_us = settle_us_obj == mp_const_none ? VDBOARD_SCAN_DEFAULT_SETTLE_US : (uint16_t)mp_obj_get_int(settle_us_obj);
    g_scan_state.buffer_frames = buffer_frames_obj == mp_const_none ? VDBOARD_SCAN_DEFAULT_BUFFER_FRAMES : (uint16_t)mp_obj_get_int(buffer_frames_obj);
    g_scan_state.core_id = core_id_obj == mp_const_none ? VDBOARD_SCAN_DEFAULT_CORE : (int8_t)mp_obj_get_int(core_id_obj);
    if (g_scan_state.buffer_frames == 0) {
        g_scan_state.buffer_frames = VDBOARD_SCAN_DEFAULT_BUFFER_FRAMES;
    }
    if (g_scan_state.buffer_frames > VDBOARD_SCAN_MAX_RING_FRAMES) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer_frames max 8"));
    }
    if (g_scan_state.point_count == 0 || g_scan_state.point_count > VDBOARD_SCAN_MAX_POINTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("matrix max 10x21"));
    }
    if (g_scan_state.fps == 0) {
        g_scan_state.fps = VDBOARD_SCAN_DEFAULT_FPS;
    }
    g_scan_ctx.scan_interval_us = 1000000UL / g_scan_state.fps;
    if (g_scan_ctx.scan_interval_us == 0) {
        g_scan_ctx.scan_interval_us = 1;
    }

    g_scan_ctx.frame_size = sizeof(vdboard_frame_header_t) + (g_scan_state.point_count * sizeof(uint16_t));
    g_scan_ctx.payload_bytes = g_scan_state.point_count * sizeof(uint16_t);
    g_scan_ctx.payload_mv = g_scan_payload_mv_storage;
    g_scan_ctx.frame_storage = g_scan_frame_storage;
    memset(g_scan_ctx.payload_mv, 0, g_scan_ctx.payload_bytes);
    memset(g_scan_ctx.frame_storage, 0, g_scan_state.buffer_frames * g_scan_ctx.frame_size);
    vdboard_stream_prepare_buffers();
    if (g_scan_ctx.hw_mutex == NULL) {
        g_scan_ctx.hw_mutex = xSemaphoreCreateMutex();
        if (g_scan_ctx.hw_mutex == NULL) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("scan mutex alloc failed"));
        }
    }

    vdboard_scan_prepare_adc_rows();
    vdboard_scan_prepare_columns();
    vdboard_scan_reset_stats();
    g_scan_state.started = false;
}

static mp_obj_t vdboard_scan_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_obj_t layout_obj = mp_const_none;
    mp_obj_t rows_obj = mp_const_none;
    mp_obj_t cols_obj = mp_const_none;
    mp_obj_t row_pins_obj = mp_const_none;
    mp_obj_t col_pins_obj = mp_const_none;
    mp_obj_t fps_obj = mp_const_none;
    mp_obj_t settle_us_obj = mp_const_none;
    mp_obj_t buffer_frames_obj = mp_const_none;
    mp_obj_t core_id_obj = mp_const_none;

    if (n_args > 0) {
        rows_obj = args[0];
    }
    if (n_args > 1) {
        cols_obj = args[1];
    }
    if (n_args > 2) {
        row_pins_obj = args[2];
    }
    if (n_args > 3) {
        col_pins_obj = args[3];
    }

    if (kw_args != NULL && kw_args->used > 0) {
        mp_map_elem_t *elem = NULL;
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_layout), MP_MAP_LOOKUP);
        if (elem != NULL) {
            layout_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_rows), MP_MAP_LOOKUP);
        if (elem != NULL) {
            rows_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_cols), MP_MAP_LOOKUP);
        if (elem != NULL) {
            cols_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_row_pins), MP_MAP_LOOKUP);
        if (elem != NULL) {
            row_pins_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_col_pins), MP_MAP_LOOKUP);
        if (elem != NULL) {
            col_pins_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_fps), MP_MAP_LOOKUP);
        if (elem != NULL) {
            fps_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_settle_us), MP_MAP_LOOKUP);
        if (elem != NULL) {
            settle_us_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_buffer_frames), MP_MAP_LOOKUP);
        if (elem != NULL) {
            buffer_frames_obj = elem->value;
        }
        elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_core_id), MP_MAP_LOOKUP);
        if (elem != NULL) {
            core_id_obj = elem->value;
        }
    }

    vdboard_scan_configure(layout_obj, rows_obj, cols_obj, row_pins_obj, col_pins_obj, fps_obj, settle_us_obj, buffer_frames_obj, core_id_obj);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(vdboard_scan_init_obj, 0, vdboard_scan_init);

static mp_obj_t vdboard_scan_start(void) {
    if (g_scan_ctx.frame_storage == NULL || g_scan_state.point_count == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("scan not initialized"));
    }
    g_scan_ctx.stop_requested = false;
    g_scan_ctx.next_scan_due_us = esp_timer_get_time();
    g_scan_state.started = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_start_obj, vdboard_scan_start);

static mp_obj_t vdboard_scan_stop(void) {
    vdboard_scan_stop_task();
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_stop_obj, vdboard_scan_stop);

static mp_obj_t vdboard_scan_service(void) {
    if (!g_scan_state.started) {
        return mp_const_false;
    }
    if (g_scan_ctx.payload_mv == NULL || g_scan_ctx.hw_mutex == NULL) {
        g_scan_state.started = false;
        return mp_const_false;
    }
    uint64_t now_us = esp_timer_get_time();
    if (g_scan_ctx.next_scan_due_us != 0 && now_us < g_scan_ctx.next_scan_due_us) {
        return mp_const_true;
    }
    if (xSemaphoreTake(g_scan_ctx.hw_mutex, 0) != pdTRUE) {
        return mp_const_true;
    }
    vdboard_capture_payload(g_scan_ctx.payload_mv);
    xSemaphoreGive(g_scan_ctx.hw_mutex);
    vdboard_commit_frame(g_scan_ctx.payload_mv, (uint32_t)(now_us / 1000ULL));
    uint64_t next_due = g_scan_ctx.next_scan_due_us + g_scan_ctx.scan_interval_us;
    if (next_due <= now_us) {
        next_due = now_us + g_scan_ctx.scan_interval_us;
    }
    g_scan_ctx.next_scan_due_us = next_due;
    return mp_obj_new_bool(g_scan_state.started);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_service_obj, vdboard_scan_service);

static mp_obj_t vdboard_scan_stats(void) {
    mp_obj_t tuple[10];
    tuple[0] = mp_obj_new_int_from_uint(g_scan_state.produced_frames);
    tuple[1] = mp_obj_new_int_from_uint(g_scan_state.consumed_frames);
    tuple[2] = mp_obj_new_int_from_uint(g_scan_state.dropped_frames);
    tuple[3] = mp_obj_new_int_from_uint(g_scan_state.last_written_seq);
    tuple[4] = mp_obj_new_int_from_uint(g_scan_state.last_read_seq);
    tuple[5] = mp_obj_new_int(g_scan_state.buffer_frames);
    tuple[6] = mp_obj_new_int(g_scan_state.core_id);
    tuple[7] = mp_obj_new_int(g_scan_state.started ? 1 : 0);
    tuple[8] = mp_obj_new_int(g_scan_state.rows);
    tuple[9] = mp_obj_new_int(g_scan_state.cols);
    return mp_obj_new_tuple(10, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_stats_obj, vdboard_scan_stats);

static mp_obj_t vdboard_scan_pop_frame_mv(void) {
    if (g_scan_ctx.frame_storage == NULL || g_scan_ctx.count == 0) {
        return mp_const_none;
    }
    uint8_t tmp[g_scan_ctx.frame_size];

    portENTER_CRITICAL(&g_scan_ctx.ring_lock);
    if (g_scan_ctx.count == 0) {
        portEXIT_CRITICAL(&g_scan_ctx.ring_lock);
        return mp_const_none;
    }
    uint8_t *slot = g_scan_ctx.frame_storage + (g_scan_ctx.read_index * g_scan_ctx.frame_size);
    memcpy(tmp, slot, g_scan_ctx.frame_size);
    g_scan_ctx.read_index = (g_scan_ctx.read_index + 1) % g_scan_state.buffer_frames;
    g_scan_ctx.count -= 1;
    g_scan_state.consumed_frames += 1;
    g_scan_state.last_read_seq = ((vdboard_frame_header_t *)tmp)->seq;
    portEXIT_CRITICAL(&g_scan_ctx.ring_lock);

    return mp_obj_new_bytes(tmp, g_scan_ctx.frame_size);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_pop_frame_mv_obj, vdboard_scan_pop_frame_mv);

static mp_obj_t vdboard_scan_peek_latest_mv(void) {
    if (g_scan_ctx.frame_storage == NULL || g_scan_ctx.count == 0) {
        return mp_const_none;
    }
    uint8_t tmp[g_scan_ctx.frame_size];

    portENTER_CRITICAL(&g_scan_ctx.ring_lock);
    if (g_scan_ctx.count == 0) {
        portEXIT_CRITICAL(&g_scan_ctx.ring_lock);
        return mp_const_none;
    }
    uint16_t latest_index = (g_scan_ctx.write_index + g_scan_state.buffer_frames - 1) % g_scan_state.buffer_frames;
    uint8_t *slot = g_scan_ctx.frame_storage + (latest_index * g_scan_ctx.frame_size);
    memcpy(tmp, slot, g_scan_ctx.frame_size);
    portEXIT_CRITICAL(&g_scan_ctx.ring_lock);

    return mp_obj_new_bytes(tmp, g_scan_ctx.frame_size);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_scan_peek_latest_mv_obj, vdboard_scan_peek_latest_mv);

static mp_obj_t vdboard_scan_sample_cell_mv(mp_obj_t analog_pin_obj, mp_obj_t select_pin_obj, mp_obj_t duration_ms_obj) {
    mp_int_t analog_pin = mp_obj_get_int(analog_pin_obj);
    mp_int_t select_pin = mp_obj_get_int(select_pin_obj);
    mp_int_t duration_ms = mp_obj_get_int(duration_ms_obj);

    if (!GPIO_IS_VALID_GPIO(analog_pin) || !GPIO_IS_VALID_GPIO(select_pin)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid gpio"));
    }

    mp_map_t empty_kwargs;
    mp_map_init_fixed_table(&empty_kwargs, 0, NULL);
    const machine_adc_obj_t *adc = madc_search_helper(NULL, -1, (gpio_num_t)analog_pin);
    if (adc == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("analog pin is not ADC"));
    }
    madc_init_helper(adc, 0, NULL, &empty_kwargs);
    mp_machine_adc_atten_set_helper(adc, ADC_ATTEN_DB_11);
    apply_self_adc_channel_atten(adc, ADC_ATTEN_DB_11);

    gpio_set_direction((gpio_num_t)select_pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level((gpio_num_t)select_pin, VDBOARD_SCAN_IDLE_LEVEL);

    uint64_t deadline_us = esp_timer_get_time() + ((duration_ms <= 0 ? 1 : duration_ms) * 1000ULL);
    uint64_t sum_mv = 0;
    uint32_t samples = 0;

    if (xSemaphoreTake(g_scan_ctx.hw_mutex, portMAX_DELAY) == pdTRUE) {
        while (samples == 0 || esp_timer_get_time() < deadline_us) {
            gpio_set_level((gpio_num_t)select_pin, VDBOARD_SCAN_ACTIVE_LEVEL);
            if (g_scan_state.settle_us > 0) {
                esp_rom_delay_us(g_scan_state.settle_us);
            }
            mp_int_t uv = madcblock_read_uv_helper(adc->block, adc->channel_id, ADC_ATTEN_DB_11);
            gpio_set_level((gpio_num_t)select_pin, VDBOARD_SCAN_IDLE_LEVEL);
            if (uv >= 0) {
                sum_mv += (uint64_t)((uv + 500) / 1000);
                samples += 1;
            }
        }
        xSemaphoreGive(g_scan_ctx.hw_mutex);
    }

    if (samples == 0) {
        return mp_const_none;
    }
    return mp_obj_new_float((mp_float_t)sum_mv / (mp_float_t)samples);
}
static MP_DEFINE_CONST_FUN_OBJ_3(vdboard_scan_sample_cell_mv_obj, vdboard_scan_sample_cell_mv);

static mp_obj_t vdboard_scan_set_layout(size_t n_args, const mp_obj_t *args) {
    if (n_args != 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("set_layout expects rows, cols, row_pins, col_pins"));
    }
    vdboard_scan_configure(mp_const_none, args[0], args[1], args[2], args[3], mp_const_none, mp_const_none, mp_const_none, mp_const_none);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vdboard_scan_set_layout_obj, 4, 4, vdboard_scan_set_layout);

static int vdboard_hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static void vdboard_stream_set_device_uid(mp_obj_t obj) {
    mp_buffer_info_t bufinfo;
    if (mp_get_buffer(obj, &bufinfo, MP_BUFFER_READ)) {
        const char *data = (const char *)bufinfo.buf;
        if (bufinfo.len == 6) {
            memcpy(g_stream_ctx.device_uid, data, 6);
            return;
        }
        if (bufinfo.len == 12) {
            for (size_t idx = 0; idx < 6; ++idx) {
                int high = vdboard_hex_nibble(data[idx * 2]);
                int low = vdboard_hex_nibble(data[(idx * 2) + 1]);
                if (high < 0 || low < 0) {
                    mp_raise_ValueError(MP_ERROR_TEXT("device_uid must be 6 bytes or 12 hex chars"));
                }
                g_stream_ctx.device_uid[idx] = (uint8_t)((high << 4) | low);
            }
            return;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("device_uid must be 6 bytes or 12 hex chars"));
}

static mp_obj_t vdboard_stream_set_packet_options(size_t n_args, const mp_obj_t *args) {
    if (n_args < 4) {
        mp_raise_TypeError(MP_ERROR_TEXT("set_packet_options expects device_uid, use_hmac, hmac_len, hmac_key"));
    }
    vdboard_stream_set_device_uid(args[0]);
    g_stream_ctx.use_hmac = mp_obj_is_true(args[1]);
    mp_int_t hmac_len = mp_obj_get_int(args[2]);
    if (hmac_len < 0 || hmac_len > VDBOARD_STREAM_HMAC_MAX_LEN) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid hmac_len"));
    }
    g_stream_ctx.hmac_len = (uint8_t)hmac_len;

    size_t key_len = 0;
    const char *key = mp_obj_str_get_data(args[3], &key_len);
    if (key_len > VDBOARD_STREAM_HMAC_KEY_MAX_LEN) {
        mp_raise_ValueError(MP_ERROR_TEXT("hmac key too long"));
    }
    memcpy(g_stream_ctx.hmac_key, key, key_len);
    g_stream_ctx.hmac_key_len = (uint8_t)key_len;
    if (g_stream_ctx.use_hmac && (g_stream_ctx.hmac_len == 0 || g_stream_ctx.hmac_key_len == 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("hmac key required"));
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vdboard_stream_set_packet_options_obj, 4, 4, vdboard_stream_set_packet_options);

static mp_obj_t vdboard_stream_configure_filter(mp_obj_t enabled_obj, mp_obj_t median_obj, mp_obj_t alpha_obj) {
    mp_int_t median = mp_obj_get_int(median_obj);
    if (!(median == 1 || median == 3 || median == 5)) {
        mp_raise_ValueError(MP_ERROR_TEXT("median must be 1, 3, or 5"));
    }
    mp_float_t alpha = mp_obj_get_float(alpha_obj);
    if (alpha < 0.05 || alpha > 0.6) {
        mp_raise_ValueError(MP_ERROR_TEXT("alpha out of range"));
    }
    g_stream_ctx.filter_enabled = mp_obj_is_true(enabled_obj);
    g_stream_ctx.filter_median = (uint8_t)median;
    g_stream_ctx.filter_alpha = (float)alpha;
    if (!g_stream_ctx.filter_enabled) {
        vdboard_stream_release_filter_states();
        return mp_const_true;
    }
    if (g_stream_ctx.capacity_points == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("scan not initialized"));
    }
    if (g_stream_ctx.filter_states == NULL) {
        g_stream_ctx.filter_states = g_stream_filter_states;
    }
    vdboard_stream_reset_filter_states();
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vdboard_stream_configure_filter_obj, vdboard_stream_configure_filter);

static mp_obj_t vdboard_stream_load_calibration(mp_obj_t table_obj) {
    if (g_stream_ctx.capacity_points == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("scan not initialized"));
    }

    vdboard_stream_release_calibration();

    if (table_obj == mp_const_none) {
        return mp_const_true;
    }
    size_t sensor_len = 0;
    mp_obj_t *sensor_items = NULL;
    mp_obj_get_array(table_obj, &sensor_len, &sensor_items);
    if (sensor_len > g_stream_ctx.capacity_points) {
        mp_raise_ValueError(MP_ERROR_TEXT("calibration table too large"));
    }

    uint32_t total_points = 0;
    for (size_t sensor = 0; sensor < sensor_len; ++sensor) {
        if (sensor_items[sensor] == mp_const_none) {
            continue;
        }
        size_t point_len = 0;
        mp_obj_t *point_items = NULL;
        mp_obj_get_array(sensor_items[sensor], &point_len, &point_items);
        if (point_len > VDBOARD_STREAM_MAX_CAL_POINTS) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many calibration points"));
        }
        for (size_t idx = 0; idx < point_len; ++idx) {
            size_t pair_len = 0;
            mp_obj_t *pair_items = NULL;
            mp_obj_get_array(point_items[idx], &pair_len, &pair_items);
            if (pair_len != 2) {
                mp_raise_ValueError(MP_ERROR_TEXT("calibration point must be pair"));
            }
        }
        total_points += point_len;
        if (total_points > 65535) {
            mp_raise_ValueError(MP_ERROR_TEXT("calibration table too large"));
        }
    }

    vdboard_stream_cal_point_t *new_points = NULL;
    if (total_points > 0) {
        new_points = calloc(total_points, sizeof(vdboard_stream_cal_point_t));
        if (new_points == NULL) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("calibration alloc failed"));
        }
        g_stream_ctx.calibration_counts = g_stream_calibration_counts;
        g_stream_ctx.calibration_offsets = g_stream_calibration_offsets;
        memset(g_stream_ctx.calibration_counts, 0, g_stream_ctx.capacity_points * sizeof(uint8_t));
        memset(g_stream_ctx.calibration_offsets, 0, g_stream_ctx.capacity_points * sizeof(uint16_t));
    } else {
        return mp_const_true;
    }

    uint16_t cursor = 0;
    for (size_t sensor = 0; sensor < sensor_len; ++sensor) {
        if (sensor_items[sensor] == mp_const_none) {
            continue;
        }
        size_t point_len = 0;
        mp_obj_t *point_items = NULL;
        mp_obj_get_array(sensor_items[sensor], &point_len, &point_items);
        if (point_len == 0) {
            continue;
        }
        vdboard_stream_cal_point_t *points = &new_points[cursor];
        for (size_t idx = 0; idx < point_len; ++idx) {
            size_t pair_len = 0;
            mp_obj_t *pair_items = NULL;
            mp_obj_get_array(point_items[idx], &pair_len, &pair_items);
            if (pair_len != 2) {
                free(new_points);
                mp_raise_ValueError(MP_ERROR_TEXT("calibration point must be pair"));
            }
            points[idx].sample_mv = (float)mp_obj_get_float(pair_items[0]);
            points[idx].level = (float)mp_obj_get_float(pair_items[1]);
        }
        g_stream_ctx.calibration_offsets[sensor] = cursor;
        g_stream_ctx.calibration_counts[sensor] = (uint8_t)point_len;
        vdboard_stream_sort_cal_points(points, (uint8_t)point_len);
        cursor += (uint16_t)point_len;
    }
    g_stream_ctx.calibration_points = new_points;
    g_stream_ctx.calibration_point_capacity = (uint16_t)total_points;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vdboard_stream_load_calibration_obj, vdboard_stream_load_calibration);

static mp_obj_t vdboard_stream_update_imu_cache(mp_obj_t values_obj) {
    if (values_obj == mp_const_none) {
        g_stream_ctx.imu_valid = false;
        return mp_const_true;
    }
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(values_obj, &len, &items);
    if (len < 6) {
        g_stream_ctx.imu_valid = false;
        return mp_const_true;
    }
    for (size_t idx = 0; idx < 6; ++idx) {
        g_stream_ctx.imu_values[idx] = (float)mp_obj_get_float(items[idx]);
    }
    g_stream_ctx.imu_values[6] = 0.0f;
    if (len >= 7 && items[6] != mp_const_none) {
        if (mp_obj_is_float(items[6]) || mp_obj_is_int(items[6])) {
            g_stream_ctx.imu_values[6] = (float)mp_obj_get_float(items[6]);
        }
    }
    g_stream_ctx.imu_valid = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vdboard_stream_update_imu_cache_obj, vdboard_stream_update_imu_cache);

static mp_obj_t vdboard_stream_update_battery_cache(mp_obj_t values_obj) {
    if (values_obj == mp_const_none) {
        g_stream_ctx.battery_valid = false;
        return mp_const_true;
    }
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(values_obj, &len, &items);
    if (len < 3) {
        g_stream_ctx.battery_valid = false;
        return mp_const_true;
    }
    g_stream_ctx.battery_status = (uint8_t)(mp_obj_get_int(items[0]) & 0xff);
    g_stream_ctx.battery_fault = (uint8_t)(mp_obj_get_int(items[1]) & 0xff);
    g_stream_ctx.battery_mv = (uint16_t)(mp_obj_get_int(items[2]) & 0xffff);
    g_stream_ctx.battery_valid = true;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vdboard_stream_update_battery_cache_obj, vdboard_stream_update_battery_cache);

static bool vdboard_stream_build_packet(size_t *total_len_out) {
    if (g_scan_ctx.frame_storage == NULL || g_scan_ctx.count == 0 || g_stream_ctx.frame_scratch == NULL || g_stream_ctx.packet_scratch == NULL) {
        return false;
    }

    portENTER_CRITICAL(&g_scan_ctx.ring_lock);
    if (g_scan_ctx.count == 0) {
        portEXIT_CRITICAL(&g_scan_ctx.ring_lock);
        return false;
    }
    uint8_t *slot = g_scan_ctx.frame_storage + (g_scan_ctx.read_index * g_scan_ctx.frame_size);
    memcpy(g_stream_ctx.frame_scratch, slot, g_scan_ctx.frame_size);
    g_scan_ctx.read_index = (g_scan_ctx.read_index + 1) % g_scan_state.buffer_frames;
    g_scan_ctx.count -= 1;
    g_scan_state.consumed_frames += 1;
    g_scan_state.last_read_seq = ((vdboard_frame_header_t *)g_stream_ctx.frame_scratch)->seq;
    portEXIT_CRITICAL(&g_scan_ctx.ring_lock);

    vdboard_frame_header_t *header = (vdboard_frame_header_t *)g_stream_ctx.frame_scratch;
    uint16_t point_count = header->point_count;
    if (point_count > g_stream_ctx.capacity_points) {
        mp_raise_ValueError(MP_ERROR_TEXT("frame point count too large"));
    }
    size_t matrix_len = ((size_t)point_count) * sizeof(float);
    size_t imu_len = g_stream_ctx.imu_valid ? (7 * sizeof(float)) : 0;
    size_t battery_len = g_stream_ctx.battery_valid ? 4 : 0;
    size_t payload_len = matrix_len + imu_len + battery_len;
    size_t hmac_len = (g_stream_ctx.use_hmac ? g_stream_ctx.hmac_len : 0);
    size_t total_len = VDBOARD_PACKET_HEADER_LEN + payload_len + hmac_len;
    if (total_len > g_stream_ctx.packet_scratch_size || payload_len > 0xffff) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("packet too large"));
    }

    uint8_t flags = 0;
    if (imu_len) {
        flags |= VDBOARD_PACKET_FLAG_IMU;
    }
    if (battery_len) {
        flags |= VDBOARD_PACKET_FLAG_BATTERY;
    }
    if (hmac_len) {
        flags |= VDBOARD_PACKET_FLAG_HMAC;
    }

    uint8_t *packet = g_stream_ctx.packet_scratch;
    vdboard_put_u16_le(packet, VDBOARD_PACKET_MAGIC);
    packet[2] = VDBOARD_PACKET_VERSION;
    packet[3] = flags;
    memcpy(packet + 4, g_stream_ctx.device_uid, 6);
    vdboard_put_u32_le(packet + 10, header->seq);
    vdboard_put_u32_le(packet + 14, header->timestamp_ms);
    vdboard_put_u16_le(packet + 18, (uint16_t)payload_len);

    uint8_t *cursor = packet + VDBOARD_PACKET_HEADER_LEN;
    const uint16_t *payload_mv = (const uint16_t *)(g_stream_ctx.frame_scratch + sizeof(vdboard_frame_header_t));
    for (uint16_t idx = 0; idx < point_count; ++idx) {
        float value = (float)payload_mv[idx];
        value = vdboard_stream_apply_filter(idx, value);
        value = vdboard_stream_apply_calibration(idx, value);
        vdboard_put_f32_le(cursor, value);
        cursor += sizeof(float);
    }

    if (imu_len) {
        for (uint8_t idx = 0; idx < 7; ++idx) {
            vdboard_put_f32_le(cursor, g_stream_ctx.imu_values[idx]);
            cursor += sizeof(float);
        }
    }
    if (battery_len) {
        cursor[0] = g_stream_ctx.battery_status;
        cursor[1] = g_stream_ctx.battery_fault;
        vdboard_put_u16_le(cursor + 2, g_stream_ctx.battery_mv);
        cursor += 4;
    }
    if (hmac_len) {
        vdboard_stream_write_hmac(packet, VDBOARD_PACKET_HEADER_LEN + payload_len, cursor);
    }

    g_stream_ctx.packet_frames += 1;
    *total_len_out = total_len;
    return true;
}

static mp_obj_t vdboard_stream_pop_packet(void) {
    size_t total_len = 0;
    if (!vdboard_stream_build_packet(&total_len)) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(g_stream_ctx.packet_scratch, total_len);
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_stream_pop_packet_obj, vdboard_stream_pop_packet);

static mp_obj_t vdboard_stream_pop_packet_into(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_WRITE);
    size_t total_len = 0;
    if (!vdboard_stream_build_packet(&total_len)) {
        return mp_const_none;
    }
    if (bufinfo.len < total_len) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("packet buffer too small"));
    }
    memcpy(bufinfo.buf, g_stream_ctx.packet_scratch, total_len);
    return mp_obj_new_int_from_uint(total_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(vdboard_stream_pop_packet_into_obj, vdboard_stream_pop_packet_into);

static mp_obj_t vdboard_stream_stats(void) {
    mp_obj_t dict = mp_obj_new_dict(12);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_produced_frames), mp_obj_new_int_from_uint(g_scan_state.produced_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_consumed_frames), mp_obj_new_int_from_uint(g_scan_state.consumed_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_dropped_frames), mp_obj_new_int_from_uint(g_scan_state.dropped_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_packet_frames), mp_obj_new_int_from_uint(g_stream_ctx.packet_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ring_count), mp_obj_new_int(g_scan_ctx.count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_buffer_frames), mp_obj_new_int(g_scan_state.buffer_frames));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_point_count), mp_obj_new_int(g_scan_state.point_count));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_filter_enabled), mp_obj_new_bool(g_stream_ctx.filter_enabled));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_filter_median), mp_obj_new_int(g_stream_ctx.filter_median));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_filter_alpha), mp_obj_new_float(g_stream_ctx.filter_alpha));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_imu_cached), mp_obj_new_bool(g_stream_ctx.imu_valid));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_battery_cached), mp_obj_new_bool(g_stream_ctx.battery_valid));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_stream_stats_obj, vdboard_stream_stats);

static mp_obj_t vdboard_stream_memory_stats(void) {
    mp_obj_t dict = mp_obj_new_dict(10);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_heap_free), mp_obj_new_int_from_uint(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_heap_largest_free_block), mp_obj_new_int_from_uint(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_frame_scratch_bytes), mp_obj_new_int(g_scan_ctx.frame_size));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_packet_scratch_bytes), mp_obj_new_int(g_stream_ctx.packet_scratch_size));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_filter_state_bytes), mp_obj_new_int(
        g_stream_ctx.filter_states == NULL ? 0 : g_stream_ctx.capacity_points * sizeof(vdboard_stream_filter_state_t)
    ));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_calibration_bytes), mp_obj_new_int(
        (g_stream_ctx.calibration_point_capacity * sizeof(vdboard_stream_cal_point_t))
        + (g_stream_ctx.calibration_counts == NULL ? 0 : g_stream_ctx.capacity_points * sizeof(uint8_t))
        + (g_stream_ctx.calibration_offsets == NULL ? 0 : g_stream_ctx.capacity_points * sizeof(uint16_t))
    ));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ring_bytes), mp_obj_new_int(g_scan_state.buffer_frames * g_scan_ctx.frame_size));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_scan_payload_bytes), mp_obj_new_int(g_scan_ctx.payload_bytes));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_scan_interval_us), mp_obj_new_int_from_uint(g_scan_ctx.scan_interval_us));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_scan_due_us), mp_obj_new_int_from_uint((uint32_t)g_scan_ctx.next_scan_due_us));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vdboard_stream_memory_stats_obj, vdboard_stream_memory_stats);

static const mp_rom_map_elem_t vdboard_scan_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_scan) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&vdboard_scan_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&vdboard_scan_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&vdboard_scan_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_service), MP_ROM_PTR(&vdboard_scan_service_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&vdboard_scan_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_pop_frame_mv), MP_ROM_PTR(&vdboard_scan_pop_frame_mv_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek_latest_mv), MP_ROM_PTR(&vdboard_scan_peek_latest_mv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_cell_mv), MP_ROM_PTR(&vdboard_scan_sample_cell_mv_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_layout), MP_ROM_PTR(&vdboard_scan_set_layout_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_packet_options), MP_ROM_PTR(&vdboard_stream_set_packet_options_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure_filter), MP_ROM_PTR(&vdboard_stream_configure_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_calibration), MP_ROM_PTR(&vdboard_stream_load_calibration_obj) },
    { MP_ROM_QSTR(MP_QSTR_update_imu_cache), MP_ROM_PTR(&vdboard_stream_update_imu_cache_obj) },
    { MP_ROM_QSTR(MP_QSTR_update_battery_cache), MP_ROM_PTR(&vdboard_stream_update_battery_cache_obj) },
    { MP_ROM_QSTR(MP_QSTR_pop_packet), MP_ROM_PTR(&vdboard_stream_pop_packet_obj) },
    { MP_ROM_QSTR(MP_QSTR_pop_packet_into), MP_ROM_PTR(&vdboard_stream_pop_packet_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_stream_stats), MP_ROM_PTR(&vdboard_stream_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_memory_stats), MP_ROM_PTR(&vdboard_stream_memory_stats_obj) },
};

static MP_DEFINE_CONST_DICT(vdboard_scan_globals, vdboard_scan_globals_table);

const mp_obj_module_t vdboard_scan_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vdboard_scan_globals,
};
