#include <stdlib.h>
#include <string.h>

#include "py/runtime.h"
#include "py/binary.h"
#include "py/mphal.h"
#include "adc.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vdboard.h"

#define VDBOARD_SCAN_MAX_ROWS (32)
#define VDBOARD_SCAN_MAX_COLS (32)
#define VDBOARD_SCAN_TASK_STACK_WORDS (4096)
#define VDBOARD_SCAN_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define VDBOARD_SCAN_DEFAULT_FPS (60)
#define VDBOARD_SCAN_DEFAULT_SETTLE_US (20)
#define VDBOARD_SCAN_DEFAULT_BUFFER_FRAMES (8)
#define VDBOARD_SCAN_DEFAULT_CORE (1)
#define VDBOARD_SCAN_ACTIVE_LEVEL (0)
#define VDBOARD_SCAN_IDLE_LEVEL (1)

typedef struct _vdboard_scan_context_t {
    uint16_t row_pins[VDBOARD_SCAN_MAX_ROWS];
    uint16_t col_pins[VDBOARD_SCAN_MAX_COLS];
    const machine_adc_obj_t *row_adc[VDBOARD_SCAN_MAX_ROWS];
    uint8_t row_pin_count;
    uint8_t col_pin_count;
    uint32_t frame_size;
    uint8_t *frame_storage;
    TaskHandle_t task_handle;
    SemaphoreHandle_t hw_mutex;
    portMUX_TYPE ring_lock;
    uint16_t read_index;
    uint16_t write_index;
    uint16_t count;
    bool stop_requested;
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
    .frame_storage = NULL,
    .task_handle = NULL,
    .hw_mutex = NULL,
    .ring_lock = portMUX_INITIALIZER_UNLOCKED,
    .read_index = 0,
    .write_index = 0,
    .count = 0,
    .stop_requested = false,
};

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

static void vdboard_scan_task(void *arg) {
    (void)arg;
    TickType_t interval_ticks = pdMS_TO_TICKS(1000 / (g_scan_state.fps == 0 ? VDBOARD_SCAN_DEFAULT_FPS : g_scan_state.fps));
    if (interval_ticks == 0) {
        interval_ticks = 1;
    }
    TickType_t last_wake = xTaskGetTickCount();
    uint16_t payload_mv[VDBOARD_SCAN_MAX_ROWS * VDBOARD_SCAN_MAX_COLS];

    while (!g_scan_ctx.stop_requested) {
        if (xSemaphoreTake(g_scan_ctx.hw_mutex, portMAX_DELAY) == pdTRUE) {
            vdboard_capture_payload(payload_mv);
            xSemaphoreGive(g_scan_ctx.hw_mutex);
        }
        vdboard_commit_frame(payload_mv, (uint32_t)(esp_timer_get_time() / 1000ULL));
        vTaskDelayUntil(&last_wake, interval_ticks);
    }

    vdboard_scan_idle_all_columns();
    g_scan_state.started = false;
    g_scan_ctx.stop_requested = false;
    g_scan_ctx.task_handle = NULL;
    vTaskDelete(NULL);
}

static void vdboard_scan_stop_task(void) {
    if (g_scan_ctx.task_handle == NULL) {
        g_scan_state.started = false;
        g_scan_ctx.stop_requested = false;
        return;
    }
    g_scan_ctx.stop_requested = true;
    for (int i = 0; i < 200 && g_scan_ctx.task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    g_scan_state.started = false;
}

static void vdboard_scan_release_storage(void) {
    if (g_scan_ctx.frame_storage != NULL) {
        free(g_scan_ctx.frame_storage);
        g_scan_ctx.frame_storage = NULL;
    }
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

    g_scan_ctx.frame_size = sizeof(vdboard_frame_header_t) + (g_scan_state.point_count * sizeof(uint16_t));
    g_scan_ctx.frame_storage = calloc(g_scan_state.buffer_frames, g_scan_ctx.frame_size);
    if (g_scan_ctx.frame_storage == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("ring buffer alloc failed"));
    }
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
    if (g_scan_ctx.task_handle != NULL) {
        g_scan_state.started = true;
        return mp_const_true;
    }

    g_scan_ctx.stop_requested = false;
    BaseType_t result = xTaskCreatePinnedToCore(
        vdboard_scan_task,
        "vdscan",
        VDBOARD_SCAN_TASK_STACK_WORDS,
        NULL,
        VDBOARD_SCAN_TASK_PRIORITY,
        &g_scan_ctx.task_handle,
        g_scan_state.core_id
    );
    if (result != pdPASS) {
        g_scan_ctx.task_handle = NULL;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("scan task create failed"));
    }
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
};

static MP_DEFINE_CONST_DICT(vdboard_scan_globals, vdboard_scan_globals_table);

const mp_obj_module_t vdboard_scan_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&vdboard_scan_globals,
};
