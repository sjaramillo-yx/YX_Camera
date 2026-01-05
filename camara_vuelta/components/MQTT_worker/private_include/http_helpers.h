/**
 * @file http_helpers.h
 * @date December 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief HTTP helpers for S3 uploads
 * @ingroup mqtt_worker
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */
#include "SD_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Upload an S3 multipart upload part to a presigned URL
 */
esp_err_t http_put_part(const char *url, const char *file_path, size_t offset, size_t len,
                        int timeout_ms, char etag_out[CONFIG_S3_MAX_ETAG_LEN]);

#ifdef __cplusplus
}
#endif
