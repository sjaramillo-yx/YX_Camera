/**
 * @defgroup video_manager Video Manager Component
 * @file video_manager.h
 * @date October 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides helper functions to capture and encode frames.
 *
 * @ingroup video_manager
 */
#pragma once

/* Espressif includes */
#include <driver/isp.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_csi.h>
#include <esp_cam_ctlr_types.h>
#include <esp_err.h>
#include <esp_h264_alloc.h>
#include <esp_h264_enc_single_hw.h>
#include <esp_idf_version.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <esp_timer.h>
/* Standard includes*/
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
/* FreeRTOS includes*/
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
/* Custom includes */
#include "SD_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Video Manager.
 */
esp_err_t vman_init(void);

/**
 * @brief Start a recording
 */
esp_err_t vman_start_recording(void);

/**
 * @brief Stop a recording
 */
esp_err_t vman_stop_recording(void);

#ifdef __cplusplus
}
#endif
