#include "video_manager.h"
#include "esp_async_memcpy.h"
#include "esp_private/esp_cache_private.h"
#include "sensor_init.h"
#include <esp_cache.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "Video Manager" /**< Logging tag for this module. */;

/* ================ MACROS ================ */
#define ESP_GOTO_ON_ERROR_SAVE_STR(expr, label, log_tag, fmt, ...)                                 \
  do {                                                                                             \
    esp_err_t __err_rc = (expr);                                                                   \
    if (__err_rc != ESP_OK) {                                                                      \
      snprintf((err_str), sizeof(err_str), (fmt), ##__VA_ARGS__);                                  \
      ret = __err_rc;                                                                              \
      ESP_LOGE((log_tag), "%s (%s)", (err_str), esp_err_to_name(__err_rc));                        \
      goto label;                                                                                  \
    }                                                                                              \
  } while (0)

/* ================ STRUCTS ================ */
typedef struct {
  char             *data;         // Stage buffer
  uint64_t          staged;       // Staged bytes
  SemaphoreHandle_t write_smphr;  // A semaphore for writing this buffer
} stage_t;

typedef struct {
  stage_t active;    // The active stage
  stage_t inactive;  // The inactive stage
} staging_buffers_t;

/*================ Parameters ================*/
/** TODO: Turn this into Kconfig options */
#define DEFAULT_HRES            1920
#define DEFAULT_VRES            1080
#define DEFAULT_FPS             30
#define CSI_LANES               2
#define CSI_LANE_BITRATE_MBPS   450  // requested per-lane bitrate
#define CSI_INPUT_COLOR         CAM_CTLR_COLOR_RAW10
#define CSI_OUTPUT_COLOR        CAM_CTLR_COLOR_YUV420
#define BYTES_PER_PIXEL_YUV420  1.5      // YUY420
#define DEFAULT_BYTES_PER_FRAME 3110400  // 1920 * 1080 * 1.5
#define DEFAULT_ENCODER_BYTES   (DEFAULT_HRES * DEFAULT_VRES * 2)

// H.264 configuration
#define H264_FORMAT ESP_H264_RAW_FMT_O_UYY_E_VYY

// Buffering
#define FRAME_BUF_COUNT          2                  // Number of CSI DMA frame buffers
#define ENC_BUF_COUNT            2                  // Number of H264 encoder buffers
#define SD_STAGE_BYTES           (3 * 1024 * 1024)  // 2 MiB staging to reduce write calls
#define SD_FLUSH_THRESHOLD_BYTES (2 * 1024 * 1024)  // flush once >= 1 MiB

// Staging buffers
static const uint64_t STAGE_SIZE = CONFIG_VMAN_STAGE_SIZE_KB << 10;  // Size in bytes of stages
static const uint64_t STAGE_LIMIT =
    STAGE_SIZE * CONFIG_VMAN_STAGE_LIMIT / 100;  // Threshold for staging buffers

/*================= Globals =================*/

// File
FILE *fp = NULL;

// Types and structs
/**
 * @brief A data chunk for the encoder
 */
typedef struct {
  uint8_t *ptr;
  size_t   cap;
  size_t   len;  // filled bytes
  uint32_t pts;  // ms-based PTS for debugging/containers (raw .h264 ignores)
} enc_chunk_t;

static volatile bool s_primed = false;

// CSI and H264 frame buffer size
static const size_t MAX_FRAME_BYTES = ALIGN_UP((size_t)DEFAULT_BYTES_PER_FRAME, 64);
static const size_t MAX_ENC_BYTES   = ALIGN_UP((size_t)DEFAULT_ENCODER_BYTES, 64);

// Buffers for the CSI frames and encoder output
static uint8_t                 *s_frame_bufs[FRAME_BUF_COUNT];  // CSI DMAable YUV buffers
static esp_h264_enc_out_frame_t s_enc_bufs[ENC_BUF_COUNT];

// Queues
static QueueHandle_t s_free_frame_q     = NULL;  // items: uint8_t* (frame buffer)
static QueueHandle_t s_free_encoded_q   = NULL;  // items: esp_h264_enc_out_frame_t*
static QueueHandle_t s_filled_encoded_q = NULL;  // items: esp_h264_enc_out_frame_t*
static QueueHandle_t s_filled_frame_q   = NULL;  // items: uint8_t* (filled frame)
static QueueHandle_t s_filled_stage_q   = NULL;  // items: stage_t (filled stage)

// Semaphores
static SemaphoreHandle_t dma_semphr   = NULL;  // DMA async copy complete
static SemaphoreHandle_t write_semphr = NULL;
static SemaphoreHandle_t enc_semphr   = NULL;

// Tasks
static TaskHandle_t s_write_sink_h = NULL;  // The task in charge of writing to file
/// TODO: Change handle names
static TaskHandle_t write_task, capture;

// General configurations
static size_t   s_frame_bytes = MAX_FRAME_BYTES;
static size_t   s_enc_bytes   = DEFAULT_ENCODER_BYTES;
static uint16_t s_hres        = DEFAULT_HRES;
static uint16_t s_vres        = DEFAULT_VRES;
static uint16_t s_sensor_fps  = DEFAULT_FPS;
static uint16_t s_output_fps  = DEFAULT_FPS;

// Camera handle and configurations
static esp_cam_sensor_device_t *s_cam_dev;
static example_sensor_config_t  cam_sensor_config = {
     .i2c_port_num   = I2C_NUM_0,
     .i2c_sda_io_num = GPIO_NUM_7,
     .i2c_scl_io_num = GPIO_NUM_8,
     .port           = ESP_CAM_SENSOR_MIPI_CSI,
     .format_name    = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
};

// CSI
static esp_cam_ctlr_handle_t     s_cam   = NULL;
static esp_cam_ctlr_csi_config_t csi_cfg = {
    .ctlr_id                = 0,
    .h_res                  = DEFAULT_HRES,
    .v_res                  = DEFAULT_VRES,
    .lane_bit_rate_mbps     = CSI_LANE_BITRATE_MBPS,
    .input_data_color_type  = CSI_INPUT_COLOR,
    .output_data_color_type = CSI_OUTPUT_COLOR,
    .data_lane_num          = CSI_LANES,
    .byte_swap_en           = false,
    .queue_items            = FRAME_BUF_COUNT,  // >1 helps continuous capture
};

// ISP handle and configuration
static isp_proc_handle_t       s_isp_proc = NULL;
static esp_isp_processor_cfg_t isp_config = {
    .clk_hz                 = 80 * 1000 * 1000,
    .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
    .input_data_color_type  = ISP_COLOR_RAW10,
    .output_data_color_type = ISP_COLOR_YUV420,
    .has_line_start_packet  = true,
    .has_line_end_packet    = true,
    .h_res                  = DEFAULT_HRES,
    .v_res                  = DEFAULT_VRES,
    .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GBRG,
};

// H.264 encoder handle and configuration
static esp_h264_enc_handle_t s_enc   = NULL;
static esp_h264_enc_cfg_hw_t enc_cfg = {
    .gop = DEFAULT_FPS,
    .fps = DEFAULT_FPS,
    .res = {.width = DEFAULT_HRES, .height = DEFAULT_VRES},
    .rc = {.bitrate = DEFAULT_HRES * DEFAULT_VRES * 1.5 * 8 * 30 / 100, .qp_min = 25, .qp_max = 36},
    .pic_type = H264_FORMAT,
};

// MIPI LDO
esp_ldo_channel_handle_t ldo_mipi_phy = NULL;

// DMA copy handle
async_memcpy_handle_t driver = NULL;

// Staging buffers
staging_buffers_t staging;

// Flags
bool initialized = false;
bool recording   = false;

// Recording events
static recording_conf_t             rec_conf;
static recording_error_t            rec_error;
static recording_file_t             rec_file;
static esp_event_loop_handle_t      rec_event_h;
static esp_event_handler_instance_t rec_handler_h;

// Recording statistics
static int                current_fps         = 0;
static uint64_t           current_bitrate     = 0;
static esp_timer_handle_t s_rec_timeout_timer = NULL;

/*========================= ISR Callbacks =========================*/

// Callback implementation, running in ISR context
static bool my_async_memcpy_cb(async_memcpy_handle_t mcp_hdl, async_memcpy_event_t *event,
                               void *cb_args) {
  SemaphoreHandle_t sem              = (SemaphoreHandle_t)cb_args;
  BaseType_t        high_task_wakeup = pdFALSE;
  xSemaphoreGiveFromISR(
      sem,
      &high_task_wakeup);  // high_task_wakeup set to pdTRUE if some high priority task unblocked
  return high_task_wakeup == pdTRUE;
}

/*
 * Called when a transaction finishes (ISR). We do nothing here because
 * the capture task waits on esp_cam_ctlr_receive() to get the completed
 * 'trans' descriptor synchronously. Keep ISR short.
 */
static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *t,
                                        void *user_ctx) {
  return false;
}

static esp_cam_ctlr_evt_cbs_t cbs = {.on_trans_finished = on_trans_finished};

static void rec_timeout_cb(void *arg) {
  (void)arg;
  if (!recording || rec_event_h == NULL) {
    return;
  }
  const char *transaction_id = rec_conf.transaction_id;
  if (transaction_id[0] == '\0') {
    return;
  }
  esp_err_t err = esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_STOP, (void *)transaction_id,
                                    strlen(transaction_id) + 1, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to post REC_STOP from timeout (%s)", esp_err_to_name(err));
  } else {
    ESP_LOGD(TAG, "REC_STOP event posted from timeout");
  }
}

static void stop_rec_timeout_timer(void) {
  if (s_rec_timeout_timer && esp_timer_is_active(s_rec_timeout_timer)) {
    esp_timer_stop(s_rec_timeout_timer);
  }
}

static void start_rec_timeout_timer(uint32_t timeout_seconds) {
  stop_rec_timeout_timer();
  if (timeout_seconds == 0 || s_rec_timeout_timer == NULL) {
    return;
  }
  esp_err_t err = esp_timer_start_once(s_rec_timeout_timer, timeout_seconds * 1000000ULL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to start recording timeout timer (%s)", esp_err_to_name(err));
  }
}

static void update_sensor_color_config(const esp_cam_sensor_device_t *dev) {
  const char *format_name = dev && dev->cur_format ? dev->cur_format->name : NULL;
  bool        is_raw8     = format_name && strstr(format_name, "RAW8");
  bool        is_raw10    = format_name && strstr(format_name, "RAW10");

  if (is_raw8) {
    csi_cfg.input_data_color_type    = CAM_CTLR_COLOR_RAW8;
    isp_config.input_data_color_type = ISP_COLOR_RAW8;
    ESP_LOGI(TAG, "Configured sensor format %s as RAW8 input", format_name);
    return;
  }

  if (is_raw10 || format_name) {
    csi_cfg.input_data_color_type    = CAM_CTLR_COLOR_RAW10;
    isp_config.input_data_color_type = ISP_COLOR_RAW10;
    ESP_LOGI(TAG, "Configured sensor format %s as RAW10 input", format_name ? format_name : "n/a");
    return;
  }

  ESP_LOGW(TAG, "Unknown sensor format, defaulting to RAW10 input");
  csi_cfg.input_data_color_type    = CAM_CTLR_COLOR_RAW10;
  isp_config.input_data_color_type = ISP_COLOR_RAW10;
}

/*================== Statics ==================*/
// Returns:
//   SIZE_MAX = "drop this whole buffer" (not ready)
//   [0..len-1] = start writing at this offset inside buf
static inline size_t h264_start_at_2nd_idr(const uint8_t *buf, size_t len) {
  enum { SKIP_IDRS = 1 };  // drop first GOP (first IDR)
  static uint32_t idr_seen;
  static bool     started;

  if (!buf || !len) {
    started  = false;
    idr_seen = 0;
    return (size_t)-1;
  }
  if (started)
    return 0;

  size_t last_sps = (size_t)-1, last_pps = (size_t)-1;

  for (size_t i = 0; i + 4 < len;) {
    size_t sc = (size_t)-1, sc_len = 0;

    if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
      sc     = i;
      sc_len = 3;
    } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
      sc     = i;
      sc_len = 4;
    }

    if (sc == (size_t)-1) {
      i++;
      continue;
    }

    size_t hdr = sc + sc_len;
    if (hdr >= len)
      break;

    uint8_t nal_type = buf[hdr] & 0x1F;

    if (nal_type == 7)
      last_sps = sc;  // SPS
    else if (nal_type == 8)
      last_pps = sc;           // PPS
    else if (nal_type == 5) {  // IDR
      idr_seen++;
      if (idr_seen <= SKIP_IDRS) {
        last_sps = last_pps = (size_t)-1;  // discard headers for skipped GOP
      } else if (last_sps != (size_t)-1 && last_pps != (size_t)-1) {
        started = true;
        return last_sps;  // start at SPS (includes PPS + IDR after it)
      }
    }

    i = hdr + 1;
  }

  return (size_t)-1;  // keep dropping until we can start with SPS+PPS+IDR
}

static esp_err_t update_active_format(uint16_t hres, uint16_t vres, uint16_t sensor_fps,
                                      uint16_t output_fps) {
  // Check that the required frame bytes fit in the buffers
  size_t required_frame_bytes =
      ALIGN_UP((size_t)((size_t)hres * vres * BYTES_PER_PIXEL_YUV420), 64);
  ESP_RETURN_ON_FALSE(required_frame_bytes <= MAX_FRAME_BYTES, ESP_ERR_NO_MEM, TAG,
                      "Requested resolution %ux%u needs %zu bytes but only %zu are available", hres,
                      vres, required_frame_bytes, MAX_FRAME_BYTES);

  // Check that the required encoder bytes fit in the buffers
  size_t required_enc_bytes = (size_t)hres * vres * 2;
  ESP_RETURN_ON_FALSE(
      required_enc_bytes <= s_enc_bytes, ESP_ERR_NO_MEM, TAG,
      "Requested resolution %ux%u needs %zu encoder bytes but only %zu are available", hres, vres,
      required_enc_bytes, s_enc_bytes);

  // Save the configurations to static variables
  s_hres        = hres;
  s_vres        = vres;
  s_sensor_fps  = sensor_fps;
  s_output_fps  = output_fps;
  s_frame_bytes = required_frame_bytes;

  // Configure the encoder
  enc_cfg.res.width  = hres;
  enc_cfg.res.height = vres;
  enc_cfg.fps        = output_fps;
  enc_cfg.gop        = output_fps;

  return ESP_OK;
}

static esp_err_t vman_configure_resolution(uint16_t hres, uint16_t vres, uint16_t fps) {
  esp_err_t ret        = ESP_OK;
  uint16_t  sensor_fps = 0;  // Use the sensor's native FPS for the selected format

  ESP_GOTO_ON_FALSE(!recording, ESP_ERR_INVALID_STATE, fail, TAG,
                    "Cannot change resolution while recording");
  ESP_GOTO_ON_FALSE(s_cam_dev, ESP_ERR_INVALID_STATE, fail, TAG, "Sensor not initialized");

  // Disable the sensor stream before reconfiguring
  int stream_flag = 0;
  if (esp_cam_sensor_ioctl(s_cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_flag) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to stop sensor stream before reconfiguration");
  }

  // Tear down the CSI controller
  if (s_cam) {
    esp_cam_ctlr_disable(s_cam);
    esp_cam_ctlr_del(s_cam);
    s_cam = NULL;
  }

  // Tear down the ISP
  if (s_isp_proc) {
    esp_isp_disable(s_isp_proc);
    esp_isp_del_processor(s_isp_proc);
    s_isp_proc = NULL;
  }

  // Attempt to set the sensor format
  ESP_GOTO_ON_ERROR(set_sensor_format(hres, vres, &sensor_fps, s_cam_dev), fail, TAG,
                    "Failed to configure sensor format");
  update_sensor_color_config(s_cam_dev);
  uint16_t output_fps = fps ? fps : sensor_fps;
  if (output_fps == 0) {
    output_fps = sensor_fps ? sensor_fps : DEFAULT_FPS;
    ESP_LOGW(TAG, "Output FPS was zero, defaulting to %u", output_fps);
  }
  if (output_fps > sensor_fps && sensor_fps > 0) {
    ESP_LOGW(TAG, "Requested FPS %u exceeds sensor format FPS %u, limiting output to sensor rate",
             output_fps, sensor_fps);
    output_fps = sensor_fps;
  }
  ESP_GOTO_ON_ERROR(update_active_format(hres, vres, sensor_fps, output_fps), fail, TAG,
                    "Requested video format is not supported by current buffers");

  // Configure and create a new CSI controller
  csi_cfg.h_res = hres;
  csi_cfg.v_res = vres;
  ESP_GOTO_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam), fail, TAG,
                    "Failed to create CSI controller");
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL), fail, TAG,
                    "Failed to register CSI callbacks");

  // Configure and create a new ISP
  isp_config.h_res = hres;
  isp_config.v_res = vres;
  ESP_GOTO_ON_ERROR(esp_isp_new_processor(&isp_config, &s_isp_proc), fail, TAG,
                    "Failed to create ISP");
  ESP_GOTO_ON_ERROR(esp_isp_enable(s_isp_proc), fail, TAG, "Failed to enable ISP");

  // Enable the CSI controller
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_enable(s_cam), fail, TAG, "Couldn't enable the camera controller");

  // Log the new configuration
  ESP_LOGI(TAG, "Configured video pipeline for %ux%u @ %u fps (sensor), output fps %u", hres, vres,
           sensor_fps, output_fps);
  // Re-enable the sensor stream
  stream_flag = 1;
  ESP_GOTO_ON_ERROR(esp_cam_sensor_ioctl(s_cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_flag),
                    fail, TAG, "Failed to enable sensor stream after reconfiguration");
  return ESP_OK;

fail:
  return ret;
}

static void apply_encoder_runtime_config(const recording_conf_t *rec_params) {
  // If the recording parameters include target bitrate, use it. If not, calculate a new one
  enc_cfg.rc.bitrate =
      rec_params->target_bitrate
          ? rec_params->target_bitrate
          : rec_params->hres * rec_params->vres * BYTES_PER_PIXEL_YUV420 * 8 * s_output_fps / 100;
  enc_cfg.fps       = s_output_fps;
  enc_cfg.gop       = s_output_fps;
  enc_cfg.rc.qp_max = rec_params->qp_max;
  enc_cfg.rc.qp_min = rec_params->qp_min;
}

/*========================= Tasks =========================*/

static void write_sink(void *p) {
  stage_t       stage;
  int64_t       now, time;
  long long int wrote;
  while (1) {
    if (xQueueReceive(s_filled_stage_q, &stage, portMAX_DELAY) != pdTRUE || fp == NULL) {
      continue;
    }
    ESP_LOGD(TAG, "[%s] Writing buffer at %p", pcTaskGetName(NULL), stage.data);
    /// TODO: Remove portMAX_DELAY and handle errors
    ESP_LOGV(TAG, "[%s] Taking stage writing semaphore (%p)", pcTaskGetName(NULL),
             stage.write_smphr);
    xSemaphoreTake(stage.write_smphr, portMAX_DELAY);
    ESP_LOGV(TAG, "[%s] Got stage writing semaphore (%p)", pcTaskGetName(NULL), stage.write_smphr);
    now   = esp_timer_get_time();
    wrote = fwrite(stage.data, 1, stage.staged, fp);
    if (wrote != stage.staged) {
      ESP_LOGE(TAG, "[%s] SD write failed (%u/%u)", pcTaskGetName(NULL), (unsigned)wrote,
               (unsigned)stage.staged);
    } else {
      time = esp_timer_get_time() - now;
      ESP_LOGD(TAG, "[%s] Wrote %lld bytes in %f seconds = %lld kB/s", pcTaskGetName(NULL), wrote,
               (float)time / 1000000.0, (((long long int)wrote * 1000000LL) / time) >> 10);
    }
    stage.staged = 0;
    ESP_LOGV(TAG, "[%s] Giving stage writing semaphore (%p)", pcTaskGetName(NULL),
             stage.write_smphr);
    xSemaphoreGive(stage.write_smphr);
    ESP_LOGV(TAG, "[%s] Done", pcTaskGetName(NULL));
  }
}

static void write_to_sd_task(void *arg) {
  // Create the write sink
  xTaskCreate(write_sink, "vman.write.sink", 4096, NULL, 15, &s_write_sink_h);

  // Create the staging buffers
  ESP_LOGI(TAG, "[%s] Allocating staging buffers", pcTaskGetName(NULL));
  uint32_t out_alignment = 0;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, (size_t *)&out_alignment);
  uint32_t actual_size = ALIGN_UP(STAGE_SIZE, out_alignment);
  // Active stage
  staging.active.data = (char *)heap_caps_aligned_alloc(
      16, actual_size, MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  staging.active.staged = 0;
  xSemaphoreGive(staging.active.write_smphr = xSemaphoreCreateBinary());
  // Inactive stage
  staging.inactive.data = (char *)heap_caps_aligned_alloc(
      16, actual_size, MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  staging.inactive.staged = 0;
  xSemaphoreGive(staging.inactive.write_smphr = xSemaphoreCreateBinary());
  if (!staging.active.data || !staging.inactive.data) {
    ESP_LOGE(TAG, "[%s] Failed to allocate staging buffer (%u bytes)", pcTaskGetName(NULL),
             STAGE_SIZE);
    goto fail;
  }

  ESP_LOGI(TAG, "[%s] Writer ready, entering loop", pcTaskGetName(NULL));
  esp_h264_enc_out_frame_t *curr_frame   = NULL;
  uint64_t                  to_flush     = 0;
  bool                      in_recording = false;
  while (1) {
    if (recording && !in_recording) {
      in_recording = true;
      h264_start_at_2nd_idr(NULL, 0);
      staging.active.staged   = 0;
      staging.inactive.staged = 0;
    } else if (!recording && in_recording) {
      in_recording = false;
    }
    if (xQueueReceive(s_filled_encoded_q, &curr_frame, portMAX_DELAY) != pdTRUE || !curr_frame) {
      continue;
    }

    // Decide whether to drop / where to start writing in this encoded chunk
    size_t off = h264_start_at_2nd_idr(curr_frame->raw_data.buffer, curr_frame->length);
    if (off == SIZE_MAX)
      goto done;  // drop whole chunk until we're "started"
    if (off == (size_t)-1 || off >= (size_t)curr_frame->length)
      goto done;  // Skip frame if offset is invalid

    const uint8_t *p = curr_frame->raw_data.buffer + off;
    size_t         n = curr_frame->length - off;

    if (staging.active.staged + n > STAGE_SIZE) {  // use n (not curr_frame->length)
      ESP_LOGW(TAG, "[%s] Staging overflow, discarding data", pcTaskGetName(NULL));
      goto done;
    }

    xSemaphoreTake(staging.active.write_smphr, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_async_memcpy(driver, staging.active.data + staging.active.staged, p, n,
                                     my_async_memcpy_cb, dma_semphr));
    xSemaphoreTake(dma_semphr, portMAX_DELAY);
    staging.active.staged += n;

    // Check if threshold has been crossed
    if (staging.active.staged >= STAGE_LIMIT) {
      to_flush = staging.active.staged & ~(CONFIG_ALLOCATION_UNIT_SIZE - 1);
      ESP_LOGD(TAG, "[%s] Sending %lld bytes to sink", pcTaskGetName(NULL), to_flush);
      uint64_t moved = staging.active.staged - to_flush;
      // Copy remaining bytes to inactive stage
      /// TODO: Remove portMAX_DELAY and handle errors
      ESP_LOGV(TAG, "[%s] Taking inactive stage writing semaphore (%p)", pcTaskGetName(NULL),
               staging.inactive.write_smphr);
      xSemaphoreTake(staging.inactive.write_smphr, portMAX_DELAY);
      ESP_LOGV(TAG, "[%s] Got inactive stage writing semaphore (%p)", pcTaskGetName(NULL),
               staging.inactive.write_smphr);
      ESP_LOGV(TAG, "[%s] Starting DMA copy", pcTaskGetName(NULL));
      ESP_ERROR_CHECK(esp_async_memcpy(driver, staging.inactive.data,
                                       staging.active.data + to_flush, moved, my_async_memcpy_cb,
                                       dma_semphr));
      xSemaphoreTake(dma_semphr, portMAX_DELAY);  // Wait until the buffer copy is done
      ESP_LOGV(TAG, "[%s] DMA copy done", pcTaskGetName(NULL));
      staging.inactive.staged = moved;
      staging.active.staged   = to_flush;
      ESP_LOGV(TAG, "[%s] Giving inactive stage writing semaphore (%p)", pcTaskGetName(NULL),
               staging.inactive.write_smphr);
      xSemaphoreGive(staging.inactive.write_smphr);
      // Send the stage to be written
      /// TODO: Remove portMAX_DELAY and handle errors
      /// NOTE: Since the inactive stage is always the one to write, the write_sink task could be
      /// notified instead of sending the stage through the queue.
      ESP_LOGV(TAG, "[%s] Giving flushed stage writing semaphore (%p)", pcTaskGetName(NULL),
               staging.active.write_smphr);
      xSemaphoreGive(staging.active.write_smphr);
      xQueueSendToBack(s_filled_stage_q, &staging.active, portMAX_DELAY);
      // Swap active and inactive stages
      ESP_LOGD(TAG, "[%s] Swapping active stage", pcTaskGetName(NULL));
      stage_t tmp      = staging.active;
      staging.active   = staging.inactive;
      staging.inactive = tmp;
    }
    xSemaphoreGive(staging.active.write_smphr);

  done:
    xQueueSendToBack(s_free_encoded_q, &curr_frame, portMAX_DELAY);
  }

fail:
  // Stop the write sink task
  vTaskSuspend(s_write_sink_h);
  // Close the file
  if (fp)
    fclose(fp);
  // Free staging buffers
  heap_caps_free(staging.active.data);
  heap_caps_free(staging.inactive.data);
  // Delete tasks and return
  vTaskDelete(s_write_sink_h);
  vTaskDelete(NULL);
  return;
}

static void capture_encode_task(void *arg) {
  // Declare capture variables
  uint64_t                  frame_idx         = 0;
  uint8_t                  *frame             = NULL;
  esp_h264_enc_out_frame_t *out               = NULL;
  esp_h264_enc_in_frame_t   in                = {0};
  uint64_t                  now               = esp_timer_get_time();
  int                       frames_per_second = 0;
  uint64_t                  encoded           = 0;
  uint64_t                  next_encode_time  = 0;
  uint16_t                  last_output_fps   = s_output_fps;

  esp_h264_err_t enc_err = ESP_OK;
  // Encoder parameter values (useful for debugging)
  esp_h264_enc_param_hw_handle_t param_hd;
  uint32_t                       set_bitrate = 0;
  uint8_t                        set_fps     = 0;

  // Enter the main capture loop
  while (1) {
    ESP_LOGV(TAG, "Taking enc_semphr in capture_encode_task");
    xSemaphoreTake(enc_semphr, portMAX_DELAY);
    // Skip processing when recording is not active or the encoder/camera handles are not ready.
    if (!recording || s_cam == NULL || s_enc == NULL) {
      xSemaphoreGive(enc_semphr);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Receive a frame from the free frame queue
    ESP_LOGD(TAG, "Receiving frame from s_free_frame_q");
    xQueueReceive(s_free_frame_q, &frame, portMAX_DELAY);
    esp_cam_ctlr_trans_t trans = {
        .buffer = frame,
        .buflen = s_frame_bytes,
    };
    // Ask the camera sensor to fill the buffer
    ESP_LOGD(TAG, "[%s] Receiving %d bytes from sensor", pcTaskGetName(NULL), s_frame_bytes);
    /// TODO: Remove ESP_CAM_CTRL_MAX_DELAT and change ESP_ERROR_CHECK for ESP_GOTO_ON_ERROR
    ESP_ERROR_CHECK(esp_cam_ctlr_receive(s_cam, &trans, ESP_CAM_CTLR_MAX_DELAY));

    const uint16_t current_output_fps = s_output_fps ? s_output_fps : DEFAULT_FPS;
    if (current_output_fps != last_output_fps) {
      // Reset pacing when the requested output rate changes
      last_output_fps  = current_output_fps;
      next_encode_time = 0;
    }
    const uint64_t frame_interval_us = current_output_fps ? (1000000ULL / current_output_fps) : 0;

    // Decide whether to encode this frame based on the target output FPS
    const uint64_t capture_time = esp_timer_get_time();
    if (frame_interval_us > 0 && capture_time < next_encode_time) {
      ESP_LOGD(TAG, "Dropping frame to match target FPS (%u)", current_output_fps);
      xQueueSendToBack(s_free_frame_q, &frame, portMAX_DELAY);
      xSemaphoreGive(enc_semphr);
      continue;
    }

    // Build input frame view for encoder from the received bytes
    in.pts = (frame_idx * 1000U) / current_output_fps;  // ms timebase is fine for raw stream
    in.raw_data.buffer = frame;
    in.raw_data.len    = s_frame_bytes;  // The driver doesn't fill trans.received_bytes

    // Encoded output container
    ESP_LOGD(TAG, "[%s] Receiving buffer from s_free_encoded_q", pcTaskGetName(NULL));
    xQueueReceive(s_free_encoded_q, &out, portMAX_DELAY);  // Get a free encoded buffer
    ESP_LOGV(TAG, "[%s] Got buffer at %p from s_free_encoded_q", pcTaskGetName(NULL), out);
    enc_err = esp_h264_enc_process(s_enc, &in, out);  // Ask the encoder to fill it
    ESP_LOGD(TAG, "[%s] Encoder processing done (%s)", pcTaskGetName(NULL),
             esp_err_to_name(enc_err));
    xQueueSendToBack(s_free_frame_q, &frame, portMAX_DELAY);  // Return the used sensor buffer

    // Advance the next encode time if we are pacing
    if (frame_interval_us > 0) {
      next_encode_time = capture_time + frame_interval_us;
    }

    // Check for encoder errors
    if (enc_err != ESP_H264_ERR_OK) {
      ESP_LOGE(TAG, "[%s] H264 process failed (%d)", pcTaskGetName(NULL), (int)enc_err);
      xQueueSendToBack(s_free_encoded_q, &out, portMAX_DELAY);
      /// TODO: Handle errors
    } else {
      // Print stream information every second
      if (esp_timer_get_time() - now > 1000000ULL) {
        esp_h264_enc_hw_get_param_hd(s_enc, &param_hd);
        esp_h264_enc_get_bitrate(&param_hd->base, &set_bitrate);
        esp_h264_enc_get_fps(&param_hd->base, &set_fps);
        ESP_LOGI(TAG, "Bitrate = %lld bps = %lld kB/s, Set bitrate= %ld bps, FPS=%d, Set FPS=%d",
                 encoded * 8, encoded >> 10, set_bitrate, frames_per_second, set_fps);
        // Set global statistics
        current_bitrate = encoded * 8;
        current_fps     = frames_per_second;
        // Reset counters
        encoded = frames_per_second = 0;
        now                         = esp_timer_get_time();
      }
      encoded += out->length;
      frames_per_second++;
      // Send the encoded buffer to the queue to be written to the staging buffer
      xQueueSendToBack(s_filled_encoded_q, &out, portMAX_DELAY);
    }
    frame_idx++;
    ESP_LOGD(TAG, "[%s] Giving enc_semphr", pcTaskGetName(NULL));
    xSemaphoreGive(enc_semphr);
  }
}

/*========================= Bootstrapping =========================*/

static void allocate_pools(void) {
  // Allocate CSI frame buffers (DMA/PSRAM OK, 64-byte aligned)
  for (int i = 0; i < FRAME_BUF_COUNT; ++i) {
    s_frame_bufs[i] = (uint8_t *)heap_caps_aligned_alloc(
        64, MAX_FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_frame_bufs[i] == NULL) {
      ESP_LOGE(TAG, "Failed to alloc frame buffer %d (%u bytes)", i, (unsigned)MAX_FRAME_BYTES);
      abort();
    }
    ESP_LOGV(TAG, "Allocated s_frame_bufs[%d] at %p", i, s_frame_bufs[i]);
  }

  // Allocate encoded buffers
  uint32_t out_alignment = 0;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, (size_t *)&out_alignment);
  size_t actual_size = ALIGN_UP(MAX_ENC_BYTES, out_alignment);
  for (int i = 0; i < ENC_BUF_COUNT; ++i) {
    s_enc_bufs[i].raw_data.buffer = (uint8_t *)heap_caps_aligned_alloc(
        64, actual_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_enc_bufs[i].raw_data.len = actual_size;
    if (!s_enc_bufs[i].raw_data.buffer) {
      ESP_LOGE(TAG, "Failed to alloc enc buffer %d (%u bytes)", i, (unsigned)actual_size);
      abort();
    }
  }
  s_enc_bytes = actual_size;
}

static void create_queues(void) {
  s_free_frame_q     = xQueueCreate(FRAME_BUF_COUNT + 2, sizeof(uint8_t *));
  s_free_encoded_q   = xQueueCreate(ENC_BUF_COUNT + 2, sizeof(esp_h264_enc_out_frame_t *));
  s_filled_encoded_q = xQueueCreate(ENC_BUF_COUNT, sizeof(esp_h264_enc_out_frame_t *));
  s_filled_frame_q   = xQueueCreate(FRAME_BUF_COUNT, sizeof(uint8_t *));
  s_filled_stage_q   = xQueueCreate(2, sizeof(stage_t));
  if (!s_free_frame_q || !s_free_encoded_q || !s_filled_encoded_q || !s_filled_frame_q ||
      !s_filled_stage_q) {
    ESP_LOGE(TAG, "Queue creation failed");
    abort();
  }

  // Copying semaphore
  dma_semphr = xSemaphoreCreateBinary();
  // Writing semaphore
  write_semphr = xSemaphoreCreateBinary();
  // Encoding semaphroe
  enc_semphr = xSemaphoreCreateBinary();

  xSemaphoreGive(write_semphr);
  xSemaphoreGive(enc_semphr);
}

/*================== Event Handlers ==================*/
static void vman_rec_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                             void *event_data) {
  ESP_LOGD(TAG, "Received event %s:%d", (char *)event_base, event_id);
  esp_err_t started = ESP_OK;
  char      rec_filename[160];
  char     *stop_id;
  switch (event_id) {
  case REC_BEGIN:
    rec_conf = *(recording_conf_t *)event_data;
    // For now, print the values
    ESP_LOGI(TAG, "Received recording parameters:");
    ESP_LOGI(TAG, "\t\tResolution:%dx%d", rec_conf.hres, rec_conf.vres);
    ESP_LOGI(TAG, "\t\tFPS:%d", rec_conf.fps);
    ESP_LOGI(TAG, "\t\tQPs:%d (max), %d (min)", rec_conf.qp_max, rec_conf.qp_min);
    ESP_LOGI(TAG, "\t\tTimeout:%d", rec_conf.timeout_seconds);
    ESP_LOGI(TAG, "\t\tTransaction ID:%s", rec_conf.transaction_id);
    ESP_LOGI(TAG, "\t\tTarget bitrate:%d", rec_conf.target_bitrate);
    ESP_LOGI(TAG, "\t\tJob ID:%s", rec_conf.aws_job_id);
    // Reconfigure the video pipeline for the requested resolution
    esp_err_t cfg_err = vman_configure_resolution(rec_conf.hres, rec_conf.vres, rec_conf.fps);
    if (cfg_err != ESP_OK) {
      rec_error.error_code = cfg_err;
      strncpy(rec_error.transaction_id, rec_conf.transaction_id, sizeof(rec_error.transaction_id));
      snprintf(rec_error.error_message, sizeof(rec_error.error_message),
               "Failed to configure resolution to %ux%u (%s)", rec_conf.hres, rec_conf.vres,
               esp_err_to_name(cfg_err));
      snprintf(rec_error.errored_module, sizeof(rec_error.errored_module), TAG);
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_ERROR, (void *)&rec_error,
                        sizeof(rec_error), 100);
      break;
    }
    apply_encoder_runtime_config(&rec_conf);
    // Begin the recording
    snprintf(rec_filename, sizeof(rec_filename), "videos/%s.bin", rec_conf.transaction_id);
    started = vman_start_recording(rec_filename);
    if (started != ESP_OK) {
      /// TODO: Handle errors here
      break;
    }
    // Use the true configured parameters
    rec_conf.fps            = s_output_fps;
    rec_conf.target_bitrate = enc_cfg.rc.bitrate;
    strlcpy(rec_file.transaction_id, rec_conf.transaction_id, sizeof(rec_file.transaction_id));
    rec_file.transaction_id[sizeof(rec_file.transaction_id) - 1] = '\0';
    esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_STARTED, (void *)&rec_conf,
                      sizeof(rec_conf), 100);
    start_rec_timeout_timer(rec_conf.timeout_seconds);
    break;
  case REC_STOP:
    stop_rec_timeout_timer();
    stop_id = (char *)event_data;
    // Check transaction ID against current configuration
    if (strcmp(rec_conf.transaction_id, stop_id)) {
      ESP_LOGW(TAG, "Received stop signal for transaction ID %s but current recording has ID %s",
               stop_id, rec_conf.transaction_id);
      rec_error.error_code = ESP_ERR_INVALID_ARG;
      strncpy(rec_error.transaction_id, rec_conf.transaction_id, sizeof(rec_error.transaction_id));
      snprintf(rec_error.error_message, sizeof(rec_error.error_message),
               "Mismatched transaction ID: current=%s, received=%s", rec_conf.transaction_id,
               stop_id);
      snprintf(rec_error.errored_module, sizeof(rec_error.errored_module), TAG);
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_ERROR, (void *)&rec_error,
                        sizeof(rec_error), 100);
      break;
    }
    if ((rec_error.error_code = vman_stop_recording()) == ESP_OK) {
      rec_file.hres = rec_conf.hres;
      rec_file.vres = rec_conf.vres;
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_DONE, (void *)&rec_file,
                        sizeof(rec_file), 100);
    } else {
      ESP_LOGE(TAG, "Couldn't stop recording (%s)", esp_err_to_name(rec_error.error_code));
      strncpy(rec_error.transaction_id, rec_conf.transaction_id, sizeof(rec_error.transaction_id));
      snprintf(rec_error.error_message, sizeof(rec_error.error_message),
               "Couldn't stop recording with ID %s", stop_id);
      strncpy(rec_error.errored_module, TAG, sizeof(rec_error.errored_module));
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_ERROR, (void *)&rec_error,
                        sizeof(rec_error), 100);
      break;
    }
    break;
  default:
    break;
  }
}

/*================== Public Functions ==================*/
esp_err_t vman_start_recording(char *filename) {
  esp_err_t ret          = ESP_OK;
  char      err_str[128] = "";
  // Check there's no other recording in progress
  ESP_GOTO_ON_FALSE(!recording, ESP_ERR_INVALID_STATE, fail, TAG, "Recording already in progress");
  // Check that the Video Manager was already initialized
  ESP_GOTO_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, fail, TAG,
                    "Video Manager has not been initialized");
  // Create the file for the recording
  ESP_GOTO_ON_ERROR_SAVE_STR(sdman_open_file(filename, "wb", &fp), fail, TAG,
                             "Couldn't create the file");
  snprintf(rec_file.filename, sizeof(rec_file.filename), filename);
  rec_file.size = rec_file.recorded_seconds = 0;
  // Create a new hardware encoder using the configuration
  ESP_GOTO_ON_ERROR_SAVE_STR(esp_h264_enc_hw_new(&enc_cfg, &s_enc), fail, TAG,
                             "Failed to create H264 encoder");
  // Open the encoder and check for errors
  ESP_GOTO_ON_ERROR_SAVE_STR(esp_h264_enc_open(s_enc), fail, TAG, "Failed opening H264 encoder");

  // Start the camera controller
  ESP_GOTO_ON_ERROR_SAVE_STR(esp_cam_ctlr_start(s_cam), fail, TAG,
                             "Couldn't start the camera controller");

  // Seed the queues
  for (int i = 0; i < FRAME_BUF_COUNT; ++i) {
    uint8_t *p = s_frame_bufs[i];
    ESP_LOGD(TAG, "Frame %d at %p", i, p);
    xQueueSendToBack(s_free_frame_q, &p, portMAX_DELAY);
  }

  for (int i = 0; i < ENC_BUF_COUNT; ++i) {
    esp_h264_enc_out_frame_t *c = &s_enc_bufs[i];
    ESP_LOGD(TAG, "Seeding output at %p with buffer at %p", c, c->raw_data.buffer);
    xQueueSendToBack(s_free_encoded_q, &c, portMAX_DELAY);
  }

  // Done
  ESP_LOGI(TAG, "Recording started");
  recording                 = true;
  rec_file.recorded_seconds = esp_timer_get_time();

  return ESP_OK;

fail:
  ESP_LOGE(TAG, "Failed to start recording");
  if (fp) {
    fclose(fp);
    fp = NULL;
  }
  rec_error.error_code = ret;
  strncpy(rec_error.transaction_id, rec_conf.transaction_id, sizeof(rec_error.transaction_id));
  /// TODO: Propagate error messages to rec_error
  snprintf(rec_error.error_message, sizeof(rec_error.error_message), err_str, esp_err_to_name(ret));
  snprintf(rec_error.errored_module, sizeof(rec_error.errored_module), TAG);
  esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_ERROR, (void *)&rec_error, sizeof(rec_error),
                    100);
  /// TODO: If the encoded got created, delete it
  return ret;
}

esp_err_t vman_stop_recording(void) {
  esp_err_t ret = ESP_OK;
  // Confirm there's a recording in progress
  ESP_GOTO_ON_FALSE(recording, ESP_ERR_INVALID_STATE, fail, TAG,
                    "There's no recording in progress");

  stop_rec_timeout_timer();

  // Prevent new frames from being processed
  recording = false;
  ESP_LOGD(TAG, "Taking enc_semphr in vman_stop_recording");
  xSemaphoreTake(enc_semphr, portMAX_DELAY);

  // Stop the camera controller
  ESP_LOGD(TAG, "Stopping the camera controller");
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_stop(s_cam), cleanup, TAG, "Couldn't stop the camera controller");
  rec_file.recorded_seconds = (esp_timer_get_time() - rec_file.recorded_seconds) / 1000000UL;

  // Close and delete the hardware encoder
  if (s_enc) {
    ESP_LOGD(TAG, "Deleting the H264 encoder");
    esp_h264_enc_del(s_enc);
    s_enc = NULL;
  }

  // Wait for stages to be written by taking their write semaphores
  xSemaphoreTake(staging.inactive.write_smphr, portMAX_DELAY);
  xSemaphoreTake(staging.active.write_smphr, portMAX_DELAY);

  // Write remaining bytes from the staging buffers into the file
  uint64_t wrote;
  if (staging.inactive.staged > 0) {
    wrote = fwrite(staging.inactive.data, 1, staging.inactive.staged, fp);
    if (wrote != staging.inactive.staged) {
      ESP_LOGE(TAG, "SD write failed (%llu/%llu)", wrote, staging.inactive.staged);
    }
  }
  if (staging.active.staged > 0) {
    wrote = fwrite(staging.active.data, 1, staging.active.staged, fp);
    if (wrote != staging.active.staged) {
      ESP_LOGE(TAG, "SD write failed (%llu/%llu)", wrote, staging.active.staged);
    }
  }
  staging.active.staged = staging.inactive.staged = 0;
  rec_file.size                                   = ftell(fp);

  // Clear the queues
  xQueueReset(s_free_encoded_q);
  xQueueReset(s_free_frame_q);
  xQueueReset(s_filled_encoded_q);
  xQueueReset(s_filled_frame_q);
  xQueueReset(s_filled_stage_q);

  // Give the writing semaphores back
  xSemaphoreGive(staging.active.write_smphr);
  xSemaphoreGive(staging.inactive.write_smphr);

  // Done
  ESP_LOGI(TAG, "Recording stopped");
  xSemaphoreGive(enc_semphr);

  // Report resulting statistics
  ESP_LOGI(TAG, "Filesize: %lld bytes", rec_file.size);
  ESP_LOGI(TAG, "Video duration: %lld seconds", rec_file.recorded_seconds);

cleanup:
  fflush(fp);
  fclose(fp);
  fp = NULL;

fail:
  return ret;
}

esp_err_t vman_getJSON(cJSON **vmanJSON) {
  esp_err_t ret = ESP_FAIL;
  /// TODO: Cleanup in case of error
  *vmanJSON = cJSON_CreateObject();
  if (*vmanJSON == NULL)
    goto end;

  cJSON *sensor_info = cJSON_AddObjectToObject(*vmanJSON, "sensorModel");
  if (sensor_info == NULL)
    goto end;

  // If no sensor available, return
  ESP_GOTO_ON_FALSE(s_cam_dev != NULL, ESP_ERR_INVALID_STATE, end, TAG, "No sensor detected");

  // Sensor name
  cJSON_AddStringToObject(sensor_info, "name", s_cam_dev->name);

  // Current Format
  cJSON *curr_format = cJSON_AddObjectToObject(sensor_info, "currentFormat");
  if (curr_format == NULL)
    goto end;
  cJSON *res = cJSON_AddArrayToObject(curr_format, "resolution");
  if (res == NULL)
    goto end;
  cJSON_AddItemToArray(res, cJSON_CreateNumber(s_cam_dev->cur_format->width));
  cJSON_AddItemToArray(res, cJSON_CreateNumber(s_cam_dev->cur_format->height));
  cJSON_AddNumberToObject(curr_format, "fps", s_cam_dev->cur_format->fps);

  // Available Formats
  cJSON                        *formats = cJSON_AddArrayToObject(sensor_info, "availableFormats");
  esp_cam_sensor_format_array_t cam_fmt_array = {0};
  esp_cam_sensor_query_format(s_cam_dev, &cam_fmt_array);
  const esp_cam_sensor_format_t *parray = cam_fmt_array.format_array;
  for (int i = 0; i < cam_fmt_array.count; i++) {
    cJSON_AddItemToArray(formats, curr_format = cJSON_CreateObject());
    cJSON_AddStringToObject(curr_format, "name", parray[i].name);
    res = cJSON_AddArrayToObject(curr_format, "resolution");
    cJSON_AddItemToArray(res, cJSON_CreateNumber(parray[i].width));
    cJSON_AddItemToArray(res, cJSON_CreateNumber(parray[i].height));
    cJSON_AddNumberToObject(curr_format, "fps", parray[i].fps);
  }

  return ESP_OK;

end:
  /// TODO: Cleanup
  return ret;
}

bool vman_is_recording() { return recording; }

esp_err_t vman_get_rec_json(cJSON **vman_rec_json) {
  esp_err_t ret = ESP_OK;
  // Check that a recording is in process
  ESP_GOTO_ON_FALSE(recording, ESP_ERR_INVALID_STATE, end, TAG,
                    "(%s) No recording currently in process!", __func__);
  ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(*vman_rec_json, "status", "ONGOING"), ESP_FAIL, end,
                    TAG, "(%s) Couldn't add status to JSON", __func__);
  ESP_GOTO_ON_FALSE(
      cJSON_AddStringToObject(*vman_rec_json, "transactionId", rec_conf.transaction_id), ESP_FAIL,
      end, TAG, "(%s) Couldn't add transactionId to JSON", __func__);
  ESP_GOTO_ON_FALSE(
      cJSON_AddNumberToObject(*vman_rec_json, "recordedSeconds",
                              (esp_timer_get_time() - rec_file.recorded_seconds) / 1000000UL),
      ESP_FAIL, end, TAG, "(%s) Couldn't add recordedSeconds to JSON", __func__);
  // Build FPS and bitrate information fields
  ESP_GOTO_ON_FALSE(cJSON_AddNumberToObject(*vman_rec_json, "fps", current_fps), ESP_FAIL, end, TAG,
                    "(%s) Couldn't add current_fps to fps_info", __func__);
  ESP_GOTO_ON_FALSE(cJSON_AddNumberToObject(*vman_rec_json, "bitrate", current_bitrate), ESP_FAIL,
                    end, TAG, "(%s) Couldn't add current_bitrate to bps_info", __func__);

end:
  return ret;
}

/*================== Initalize, Begin, Stop and Deinitialize ==================*/
esp_err_t vman_init(void) {
  esp_err_t ret = ESP_OK;
  // Initialize the buffers and queues
  /// TODO: Change function names
  ESP_LOGI(TAG, "Initializing buffers and queues.");
  allocate_pools();
  create_queues();

  //--------Async DMA copy engine-----------//
  async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  ESP_RETURN_ON_ERROR(esp_async_memcpy_install_gdma_axi(&config, &driver), TAG,
                      "Couldn't install async DMA copy engine");

  //--------MIPI LDO-----------//
  esp_ldo_channel_config_t ldo_mipi_phy_config = {
      .chan_id    = LDO_UNIT_3,
      .voltage_mv = 2500,
  };
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy), TAG,
                      "Couldn't adquire LDO channel");

  //--------Camera Sensor and SCCB Init-----------//
  sensor_init(&cam_sensor_config, &s_cam_dev);

  // Configure the pipeline
  ESP_RETURN_ON_ERROR(vman_configure_resolution(s_hres, s_vres, s_output_fps), TAG,
                      "Couldn't configure the VideoManager pipeline");

  //---------------FreeRTOS Tasks------------------//
  xTaskCreatePinnedToCore(write_to_sd_task, "vman.write.loop", 4096, NULL, 8, &write_task, 0);
  xTaskCreatePinnedToCore(capture_encode_task, "vman.capture.loop", 6144, NULL, 5, &capture, 1);

  //---------------Recording event loop------------------//
  ESP_RETURN_ON_ERROR(rec_eventloop_get_handle(&rec_event_h), TAG,
                      "Couldn't obtain recording eventloop handle");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(rec_event_h, RECORDING_EVENTS,
                                                               ESP_EVENT_ANY_ID, vman_rec_handler,
                                                               NULL, &rec_handler_h),
                      TAG, "Couldn't register recording event handler instance");
  esp_timer_create_args_t timer_args = {
      .callback = rec_timeout_cb,
      .arg      = NULL,
      .name     = "vman_rec_timeout",
  };
  ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_rec_timeout_timer), TAG,
                      "Couldn't create recording timeout timer");
  // Done
  initialized = true;

  return ret;
}

esp_err_t vman_deinit(void) {
  //---------------Recording event loop------------------//
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_unregister_with(rec_event_h, RECORDING_EVENTS,
                                                                 ESP_EVENT_ANY_ID, rec_handler_h),
                      TAG, "Couldn't unregister recording event handler instance");
  if (s_rec_timeout_timer) {
    esp_timer_stop(s_rec_timeout_timer);
    esp_timer_delete(s_rec_timeout_timer);
    s_rec_timeout_timer = NULL;
  }

  //---------------FreeRTOS Tasks------------------//
  vTaskDelete(capture);
  vTaskDelete(write_task);

  //--------Camera Sensor and SCCB Init-----------//
  sensor_deinit();

  //--------MIPI LDO-----------//
  ESP_RETURN_ON_ERROR(esp_ldo_release_channel(ldo_mipi_phy), TAG,
                      "Couldn't release MIPI LDO Channel");
  ldo_mipi_phy = NULL;

  //--------Async DMA copy engine-----------//
  ESP_RETURN_ON_ERROR(esp_async_memcpy_uninstall(driver), TAG,
                      "Couldn't unintstall async DMA copy engine");

  //--------Queues and buffers-----------//
  vQueueDelete(s_filled_encoded_q);
  vQueueDelete(s_free_encoded_q);
  vQueueDelete(s_filled_frame_q);
  vQueueDelete(s_free_frame_q);
  vQueueDelete(s_filled_stage_q);
  for (int i = 0; i < FRAME_BUF_COUNT; ++i) {
    if (s_frame_bufs[i])
      heap_caps_free(s_frame_bufs[i]);
  }
  for (int i = 0; i < ENC_BUF_COUNT; ++i) {
    if (s_enc_bufs[i].raw_data.buffer)
      heap_caps_free(s_enc_bufs[i].raw_data.buffer);
  }

  // Done
  initialized = false;

  return ESP_OK;
}
