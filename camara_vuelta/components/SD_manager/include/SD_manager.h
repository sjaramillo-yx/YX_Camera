/**
 * @defgroup SD_manager SD Card Manager Component
 * @file SD_manager.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides helper functions to mount and format an SD card as well
 * as reading and writing files to a FAT filesystem.
 *
 * @ingroup SD_manager
 */
#pragma once

/* Espressif includes */
#include <driver/sdmmc_host.h>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_private/esp_cache_private.h>
#include <esp_vfs_fat.h>
#include <hal/ldo_types.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
/* Standard includes*/
#include <cJSON.h>
#include <string.h>
/* FreeRTOS includes*/
/* Custom includes */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure and mount the FAT FS in the SD Card.
 */
esp_err_t sdman_mount();

/**
 * @brief Unmount the FAT FS in the SD Card and delete the power handle.
 */
esp_err_t sdman_umount();

/**
 * @brief Open a file in the SD card filesystem.
 *
 * @param path_in The path of the file to be opened.
 * @param mode IO mode for the file.
 * @param[out] file_handle A pointer to a `FILE` variable where the file handle will be stored to.
 */
esp_err_t sdman_open_file(const char *path_in, const char *mode, FILE **file_handle);

/**
 * @brief Check if file exists in SD card filesystem.
 *
 * @param filename The name of the file to be checked.
 * @param[out] file_size A pointer to a size_t variable where the filesize will be saved to.
 */
esp_err_t sdman_stat_file(char *filename, size_t *file_size);

/**
 * @brief Write data to the active staging buffer.
 *
 * @param data Pointer to the data to be written.
 * @param len  Number of bytes to be written
 * @param file Where the bytes should be written if the stage fills up
 */
esp_err_t sdman_write_bytes(char *data, long long int len, FILE *fp);

/**
 * @brief Write SD card information to a cJSON object
 *
 * @param[out] sdJSON A pointer to the cJSON structure where the information should be saved to
 */
esp_err_t sdman_getJSON(cJSON **sdJSON);

/**
 * @brief Notify the oldest video scan task to run.
 */
esp_err_t sdman_notify_oldest_video_scan_task(void);

/**
 * @brief Set the target free space for the SD Card
 *
 * @param free_kb Minimum free space on card in KB.
 */
esp_err_t sdman_set_free_space_target(uint64_t free_kb);

#ifdef __cplusplus
}
#endif
