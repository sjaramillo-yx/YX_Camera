# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camara_vuelta)

if(CONFIG_AWS_IOT_CERT_PEM_PATH STREQUAL "" OR
   CONFIG_AWS_IOT_PRIV_KEY_PATH STREQUAL "" OR
   CONFIG_AWS_IOT_ROOT_CA_PATH STREQUAL "")
  message(FATAL_ERROR "Set AWS IoT cert/key paths via menuconfig (or provide env vars).")
endif()

set(AWS_IOT_CERT_PEM  "${CONFIG_AWS_IOT_CERT_PEM_PATH}")
set(AWS_IOT_PRIV_KEY  "${CONFIG_AWS_IOT_PRIV_KEY_PATH}")
set(AWS_IOT_ROOT_CA   "${CONFIG_AWS_IOT_ROOT_CA_PATH}")

set(GEN_CSV "${CMAKE_BINARY_DIR}/certificates.csv")
configure_file("${CMAKE_SOURCE_DIR}/certificates.csv" "${GEN_CSV}" @ONLY)

nvs_create_partition_image(certs "${GEN_CSV}")
