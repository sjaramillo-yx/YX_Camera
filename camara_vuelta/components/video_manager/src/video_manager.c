#include "video_manager.h"
#include "esp_async_memcpy.h"
#include "esp_private/esp_cache_private.h"
#include "sensor_init.h"
#include <esp_cache.h>
#include <unistd.h>

static const char *TAG = "Video Manager" /**< Logging tag for this module. */;

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
#define HRES                   1920
#define VRES                   1080
#define CSI_LANES              2
#define CSI_LANE_BITRATE_MBPS  450  // requested per-lane bitrate
#define CSI_INPUT_COLOR        CAM_CTLR_COLOR_RAW10
#define CSI_OUTPUT_COLOR       CAM_CTLR_COLOR_YUV420  // encoder expects YUV or I420
#define BYTES_PER_PIXEL_YUV420 1.5                    // YUYV
#define BYTES_PER_FRAME        3110400                // 1920 * 1080 * 1.5

// H.264 configuration
#define H264_FPS    30
#define H264_GOP    30
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

// File path (assumes SD is mounted at /sdcard)
#define OUTPUT_PATH "/sdcard/cap.bin"

/*================= Globals =================*/

// File
FILE *fp = NULL;

// Types and structs
typedef struct {
  uint8_t *ptr;
  size_t   cap;
  size_t   len;  // filled bytes
  uint32_t pts;  // ms-based PTS for debugging/containers (raw .h264 ignores)
} enc_chunk_t;

static volatile bool s_primed = false;

// CSI frame buffer size
static const size_t FRAME_BYTES = ALIGN_UP((size_t)BYTES_PER_FRAME, 64);

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

// Camera handle
static esp_cam_ctlr_handle_t    s_cam = NULL;
static esp_cam_sensor_device_t *s_cam_dev;

// H.264 encoder handle and configuration
static esp_h264_enc_handle_t s_enc   = NULL;
static esp_h264_enc_cfg_hw_t enc_cfg = {
    .gop      = H264_GOP,
    .fps      = H264_FPS,
    .res      = {.width = HRES, .height = VRES},
    .rc       = {.bitrate = HRES * VRES * 1.5 * 8 * 30 / 100, .qp_min = 25, .qp_max = 36},
    .pic_type = H264_FORMAT,
};

// DMA copy handle
async_memcpy_handle_t driver = NULL;

// Staging buffers
staging_buffers_t staging;

// Flags
bool initialized = false;
bool recording   = false;

// Recording events
static recording_conf_t        rec_conf;
static recording_error_t       rec_error;
static recording_file_t        rec_file;
static esp_event_loop_handle_t rec_event_h;

/*================== Statics ==================*/
// Helper function to allocate aligned memory with error checking
static void *allocate_frame_buffer(size_t size, uint32_t *actual_size, const char *buffer_name) {
  void *buffer = esp_h264_aligned_calloc(16, 1, size, actual_size, ESP_H264_MEM_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate %s buffer memory (%zu bytes)", buffer_name, size);
  }
  return buffer;
}

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

/*========================= Tasks =========================*/

static void write_sink(void *p) {
  stage_t       stage;
  int64_t       now, time;
  long long int wrote;
  while (1) {
    if (xQueueReceive(s_filled_stage_q, &stage, portMAX_DELAY) != pdTRUE || fp == NULL) {
      continue;
    }
    ESP_LOGV(TAG, "[%s] Writing buffer at %p", pcTaskGetName(NULL), stage.data);
    /// TODO: Remove portMAX_DELAY and handle errors
    xSemaphoreTake(stage.write_smphr, portMAX_DELAY);
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
    xSemaphoreGive(stage.write_smphr);
  }
}

static void write_to_sd_task(void *arg) {
  // Create the write sink and pass the file pointer as context
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
  esp_h264_enc_out_frame_t *curr_frame = NULL;
  uint64_t                  to_flush   = 0;
  while (1) {
    if (xQueueReceive(s_filled_encoded_q, &curr_frame, portMAX_DELAY) != pdTRUE || !curr_frame) {
      continue;
    }

    // Check if received data can be copied to the active stage
    if (staging.active.staged + curr_frame->length > STAGE_SIZE) {
      // If not, discard the data
      /// TODO: Check if this frame fits in the inactive stage
      ESP_LOGW(TAG, "[%s] Staging overflow, discarding data", pcTaskGetName(NULL));
      goto done;
    }

    // Now copy the data to the active staging buffer
    /// TODO: Remove portMAX_DELAY and handle errors
    xSemaphoreTake(staging.active.write_smphr, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_async_memcpy(driver, staging.active.data + staging.active.staged,
                                     curr_frame->raw_data.buffer, curr_frame->length,
                                     my_async_memcpy_cb, dma_semphr));
    xSemaphoreTake(dma_semphr, portMAX_DELAY);  // Wait until the buffer copy is done
    xSemaphoreGive(staging.active.write_smphr);
    staging.active.staged += curr_frame->length;

    // Check if threshold has been crossed
    if (staging.active.staged >= STAGE_LIMIT) {
      to_flush = staging.active.staged & ~(CONFIG_ALLOCATION_UNIT_SIZE - 1);
      ESP_LOGD(TAG, "[%s] Sending %lld bytes to sink", pcTaskGetName(NULL), to_flush);
      uint64_t moved = staging.active.staged - to_flush;
      // Copy remaining bytes to inactive stage
      /// TODO: Remove portMAX_DELAY and handle errors
      xSemaphoreTake(staging.inactive.write_smphr, portMAX_DELAY);
      ESP_ERROR_CHECK(esp_async_memcpy(driver, staging.inactive.data,
                                       staging.active.data + to_flush, moved, my_async_memcpy_cb,
                                       dma_semphr));
      xSemaphoreTake(dma_semphr, portMAX_DELAY);  // Wait until the buffer copy is done
      xSemaphoreGive(staging.inactive.write_smphr);
      staging.active.staged = to_flush;
      // Swap active and inactive stages
      stage_t temp          = staging.active;
      staging.active        = staging.inactive;
      staging.active.staged = moved;
      staging.inactive      = temp;
      // Send the stage to be written
      /// TODO: Remove portMAX_DELAY and handle errors
      /// NOTE: Since the inactive stage is always the one to write, the write_sink task could be
      /// notified instead of sending the stage through the queue.
      xQueueSendToBack(s_filled_stage_q, &staging.inactive, portMAX_DELAY);
    }

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
  free(staging.active.data);
  free(staging.active.data);
  // Delete tasks and return
  vTaskDelete(s_write_sink_h);
  vTaskDelete(NULL);
  return;
}

static void capture_encode_task(void *arg) {
  // Declare capture variables
  uint32_t frame_idx         = 0;
  uint8_t *frame             = NULL;
  uint64_t now               = esp_timer_get_time();
  uint64_t encoded           = 0;
  int      frames_per_second = 0;

  // Encoder parameter values (useful for debugging)
  esp_h264_enc_param_hw_handle_t param_hd;
  uint32_t                       set_bitrate = 0;
  uint8_t                        set_fps     = 0;

  // Enter the main capture loop
  while (1) {
    ESP_LOGD(TAG, "Taking enc_semphr in capture_encode_task");
    xSemaphoreTake(enc_semphr, portMAX_DELAY);
    // Receive a frame from the free frame queue
    ESP_LOGD(TAG, "Receiving frame from s_free_frame_q");
    xQueueReceive(s_free_frame_q, &frame, portMAX_DELAY);
    esp_cam_ctlr_trans_t trans = {
        .buffer = frame,
        .buflen = FRAME_BYTES,
    };
    // Ask the camera sensor to fill the buffer
    ESP_LOGD(TAG, "Receiving transaction from sensor");
    ESP_ERROR_CHECK(esp_cam_ctlr_receive(s_cam, &trans, ESP_CAM_CTLR_MAX_DELAY));

    // Build input frame view for encoder from the received bytes
    esp_h264_enc_in_frame_t in = {0};
    in.pts             = (frame_idx * 1000U) / H264_FPS;  // ms timebase is fine for raw stream
    in.raw_data.buffer = frame;
    in.raw_data.len    = FRAME_BYTES;

    // Encoded output container
    esp_h264_enc_out_frame_t *out;
    ESP_LOGD(TAG, "Receiving buffer from s_free_encoded_q");
    xQueueReceive(s_free_encoded_q, &out, portMAX_DELAY);       // Get a free encoded buffer
    esp_h264_err_t er = esp_h264_enc_process(s_enc, &in, out);  // Ask the encoder to fill it
    xQueueSendToBack(s_free_frame_q, &frame, portMAX_DELAY);    // Return the used sensor buffer

    // Check for encoder errors
    if (er != ESP_H264_ERR_OK) {
      ESP_LOGE(TAG, "[%s] H264 process failed (%d)", pcTaskGetName(NULL), (int)er);
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
        encoded = frames_per_second = 0;
        now                         = esp_timer_get_time();
      }
      encoded += out->length;
      frames_per_second++;
      // Send the encoded buffer to the queue to be written to the staging buffer
      xQueueSendToBack(s_filled_encoded_q, &out, portMAX_DELAY);
    }
    frame_idx++;
    ESP_LOGD(TAG, "Giving enc_semphr");
    xSemaphoreGive(enc_semphr);
  }
}

/*========================= Bootstrapping =========================*/

static void allocate_pools(void) {
  // Allocate CSI frame buffers (DMA/PSRAM OK, 64-byte aligned)
  for (int i = 0; i < FRAME_BUF_COUNT; ++i) {
    s_frame_bufs[i] = (uint8_t *)heap_caps_aligned_alloc(
        64, FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_frame_bufs[i] == NULL) {
      ESP_LOGE(TAG, "Failed to alloc frame buffer %d (%u bytes)", i, (unsigned)FRAME_BYTES);
      abort();
    }
  }

  // Allocate encoded buffers
  uint32_t actual_size;
  uint32_t out_alignment = 0;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, (size_t *)&out_alignment);
  size_t frame_len = 1920 * 1080 * 2;
  for (int i = 0; i < ENC_BUF_COUNT; ++i) {
    s_enc_bufs[i].raw_data.buffer = allocate_frame_buffer(frame_len, &actual_size, "encoded frame");
    s_enc_bufs[i].raw_data.len    = actual_size;
    if (!s_enc_bufs[i].raw_data.buffer) {
      ESP_LOGE(TAG, "Failed to alloc enc buffer %d (%u bytes)", i, (unsigned)frame_len);
      abort();
    }
  }
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
  ESP_LOGI(TAG, "Received event %s:%d", (char *)event_base, event_id);
  recording_conf_t              *rec_params;
  esp_h264_enc_param_hw_handle_t param_hd;
  uint8_t                        set_fps;
  esp_err_t                      started = ESP_OK;
  char                           rec_filename[128];
  char                          *stop_id;
  switch (event_id) {
  case REC_BEGIN:
    rec_params = (recording_conf_t *)event_data;
    // For now, print the values
    ESP_LOGI(TAG, "Received recording parameters:");
    ESP_LOGI(TAG, "\t\tResolution:%dx%d", rec_params->hres, rec_params->vres);
    ESP_LOGI(TAG, "\t\tFPS:%d", rec_params->fps);
    ESP_LOGI(TAG, "\t\tQPs:%d (max), %d (min)", rec_params->qp_max, rec_params->qp_min);
    ESP_LOGI(TAG, "\t\tTimeout:%d", rec_params->timeout_seconds);
    ESP_LOGI(TAG, "\t\tTransaction ID:%s", rec_params->transaction_id);
    ESP_LOGI(TAG, "\t\tTarget bitrate:%d", rec_params->target_bitrate);
    ESP_LOGI(TAG, "\t\tJob ID:%s", rec_params->aws_job_id);
    // Set the encoder configuration
    /// TODO: Configure the sensor or PPA also
    /*
    enc_cfg.fps        = rec_params->fps;
    enc_cfg.res.height = rec_params->vres;
    enc_cfg.res.width  = rec_params->hres;
    enc_cfg.rc.bitrate = rec_params->target_bitrate;
    enc_cfg.rc.qp_max  = rec_params->qp_max;
    enc_cfg.rc.qp_min  = rec_params->qp_min;
    */
    // Begin the recording
    snprintf(rec_filename, sizeof(rec_filename) + 4, "%s.bin", rec_params->transaction_id);
    started = vman_start_recording(rec_filename);
    if (started != ESP_OK) {
      /// TODO: Handle errors here
      break;
    }
    rec_conf = *rec_params;
    strcpy(rec_file.transaction_id, rec_conf.transaction_id);
    esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_STARTED, (void *)&rec_conf,
                      sizeof(rec_conf), 100);
    break;
  case REC_STOP:
    stop_id = (char *)event_data;
    // Check transaction ID against current configuration
    if (strcmp(rec_conf.transaction_id, stop_id)) {
      ESP_LOGW(TAG, "Received stop signal for transaction ID %s but current recording has ID %s",
               stop_id, rec_conf.transaction_id);
      rec_error.error_code = ESP_ERR_INVALID_ARG;
      snprintf(rec_error.error_message, sizeof(rec_error.error_message),
               "Mismatched transaction ID: current=%s, received=%s", rec_conf.transaction_id,
               stop_id);
      snprintf(rec_error.errored_module, sizeof(rec_error.errored_module), TAG);
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_ERROR, (void *)&rec_error,
                        sizeof(rec_error), 100);
      break;
    }
    if ((rec_error.error_code = vman_stop_recording()) == ESP_OK) {
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_DONE, (void *)&rec_file,
                        sizeof(rec_file), 100);
    } else {
      snprintf(rec_error.error_message, sizeof(rec_error.error_message),
               "Couldn't stop recording with ID %s", stop_id);
      snprintf(rec_error.errored_module, sizeof(rec_error.errored_module), TAG);
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

esp_err_t vman_init(void) {
  esp_err_t ret = ESP_OK;
  // Initialize the buffers and queues
  /// TODO: Change function names
  ESP_LOGI(TAG, "Initializing buffers and queues.");
  allocate_pools();
  create_queues();

  // Create the async DMA copy engine
  /// TODO: Handle errors instead of aborting
  async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&config, &driver));

  // Create the MIPI LDO to set the bus voltage
  esp_ldo_channel_handle_t ldo_mipi_phy        = NULL;
  esp_ldo_channel_config_t ldo_mipi_phy_config = {
      .chan_id    = LDO_UNIT_3,
      .voltage_mv = 2500,
  };
  ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

  //--------Camera Sensor and SCCB Init-----------//
  example_sensor_handle_t sensor_handle = {
      .sccb_handle    = NULL,
      .i2c_bus_handle = NULL,
  };
  example_sensor_config_t cam_sensor_config = {
      .i2c_port_num   = I2C_NUM_0,
      .i2c_sda_io_num = GPIO_NUM_7,
      .i2c_scl_io_num = GPIO_NUM_8,
      .port           = ESP_CAM_SENSOR_MIPI_CSI,
      .format_name    = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
  };
  // example_sensor_init(&cam_sensor_config, &sensor_handle);
  sensor_init(&cam_sensor_config, &s_cam_dev);

  // Configure and create the CSI controller
  esp_cam_ctlr_csi_config_t cfg = {
      .ctlr_id                = 0,
      .h_res                  = HRES,
      .v_res                  = VRES,
      .lane_bit_rate_mbps     = CSI_LANE_BITRATE_MBPS,
      .input_data_color_type  = CSI_INPUT_COLOR,
      .output_data_color_type = CSI_OUTPUT_COLOR,
      .data_lane_num          = CSI_LANES,
      .byte_swap_en           = false,
      .queue_items            = FRAME_BUF_COUNT,  // >1 helps continuous capture
  };
  ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&cfg, &s_cam));

  // Register callbacks that hand buffers to the driver and notify on completion
  esp_cam_ctlr_evt_cbs_t cbs = {.on_trans_finished = on_trans_finished};

  //---------------ISP Init------------------//
  isp_proc_handle_t       isp_proc   = NULL;
  esp_isp_processor_cfg_t isp_config = {
      .clk_hz                 = 80 * 1000 * 1000,
      .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
      .input_data_color_type  = ISP_COLOR_RAW10,
      .output_data_color_type = ISP_COLOR_YUV420,
      .has_line_start_packet  = false,
      .has_line_end_packet    = false,
      .h_res                  = HRES,
      .v_res                  = VRES,
      .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GBRG,
  };
  ESP_ERROR_CHECK(esp_isp_new_processor(&isp_config, &isp_proc));
  ESP_ERROR_CHECK(esp_isp_enable(isp_proc));

  //---------------Enable camera controller------------------//
  /// TODO: Go to a fail tag instead of using ESP_ERROR_CHECK
  ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL));
  ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam));
  // ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam));

  //---------------FreeRTOS Tasks------------------//
  xTaskCreatePinnedToCore(write_to_sd_task, "vman.write.loop", 4096, NULL, 8, &write_task, 0);
  xTaskCreatePinnedToCore(capture_encode_task, "vman.capture.loop", 6144, NULL, 5, &capture, 1);

  //---------------Recording event loop------------------//
  /// TODO:  Check for errors
  ESP_ERROR_CHECK(rec_eventloop_get_handle(&rec_event_h));
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
      rec_event_h, RECORDING_EVENTS, ESP_EVENT_ANY_ID, vman_rec_handler, NULL, NULL));
  // Done
  initialized = true;

  return ret;
}

esp_err_t vman_start_recording(char *filename) {
  esp_err_t ret = ESP_OK;
  // Check there's no other recording in progress
  ESP_GOTO_ON_FALSE(!recording, ESP_ERR_INVALID_STATE, fail, TAG, "Recording already in progress");
  // Check that the Video Manager was already initialized
  ESP_GOTO_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, fail, TAG,
                    "Video Manager has not been initialized");
  // Create the file for the recording
  ESP_GOTO_ON_ERROR(sdman_open_file(filename, "wb", &fp), fail, TAG, "Couldn't create the file");
  snprintf(rec_file.filename, sizeof(rec_file.filename), filename);
  rec_file.size = rec_file.recorded_seconds = 0;
  // Create a new hardware encoder using the configuration
  ESP_GOTO_ON_ERROR(esp_h264_enc_hw_new(&enc_cfg, &s_enc), fail, TAG,
                    "Failed to create H264 encoder");
  // Open the encoder and check for errors
  ESP_GOTO_ON_ERROR(esp_h264_enc_open(s_enc), fail, TAG, "Failed opening H264 encoder");

  // Start the camera controller
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_start(s_cam), fail, TAG, "Couldn't start the camera controller");

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
  /// TODO: If the encoded got created, delete it
  return ret;
}

esp_err_t vman_stop_recording(void) {
  esp_err_t ret = ESP_OK;
  // Confirm there's a recording in progress
  ESP_GOTO_ON_FALSE(recording, ESP_ERR_INVALID_STATE, fail, TAG,
                    "There's no recording in progress");

  // Close the hardware encoder
  ESP_LOGD(TAG, "Taking enc_semphr in vman_stop_recording");
  xSemaphoreTake(enc_semphr, portMAX_DELAY);
  esp_h264_enc_close(s_enc);
  esp_h264_enc_del(s_enc);

  // Stop the camera controller
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_stop(s_cam), cleanup, TAG, "Couldn't stop the camera controller");
  rec_file.recorded_seconds = (esp_timer_get_time() - rec_file.recorded_seconds) / 1000000UL;

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
  rec_file.size = ftell(fp);

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
  recording = false;

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
