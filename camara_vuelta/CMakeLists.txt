# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camara_vuelta)
# --- Cert bundle generation (skip until configured) --------------------------

# Prefer menuconfig values, but allow environment variable fallback
set(_cert_path "${CONFIG_AWS_IOT_CERT_PEM_PATH}")
set(_key_path  "${CONFIG_AWS_IOT_PRIV_KEY_PATH}")
set(_ca_path   "${CONFIG_AWS_IOT_ROOT_CA_PATH}")

if(_cert_path STREQUAL "")
  set(_cert_path "$ENV{AWS_IOT_CERT_PEM_PATH}")
endif()
if(_key_path STREQUAL "")
  set(_key_path "$ENV{AWS_IOT_PRIV_KEY_PATH}")
endif()
if(_ca_path STREQUAL "")
  set(_ca_path "$ENV{AWS_IOT_ROOT_CA_PATH}")
endif()

if(_cert_path STREQUAL "" OR _key_path STREQUAL "" OR _ca_path STREQUAL "")
  message(STATUS
    "AWS IoT cert paths not set yet; skipping certs partition generation. "
    "Run 'idf.py menuconfig' (or set env vars AWS_IOT_CERT_PEM_PATH/AWS_IOT_PRIV_KEY_PATH/AWS_IOT_ROOT_CA_PATH)."
  )
else()
  # Convert to absolute paths to avoid working-directory surprises
  get_filename_component(AWS_IOT_CERT_PEM "${_cert_path}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  get_filename_component(AWS_IOT_PRIV_KEY "${_key_path}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
  get_filename_component(AWS_IOT_ROOT_CA  "${_ca_path}"  ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

  set(GEN_CSV "${CMAKE_BINARY_DIR}/certificates.csv")
  configure_file("${CMAKE_SOURCE_DIR}/certificates.csv" "${GEN_CSV}" @ONLY)

  # No FLASH_IN_PROJECT: won't overwrite on every normal flash
  nvs_create_partition_image(certs "${GEN_CSV}")
endif()


idf_build_get_property(build_dir BUILD_DIR)

# In ESP-IDF, the app binary name is derived from PROJECT_NAME by default
# (so build output is typically ${build_dir}/${PROJECT_NAME}.bin)
set(app_bin "${build_dir}/${PROJECT_NAME}.bin")

set(dst_dir "${build_dir}")
set(dst_bin "${dst_dir}/${PROJECT_VER}.bin")

add_custom_command(
    OUTPUT "${dst_bin}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${app_bin}" "${dst_bin}"
    COMMAND ${CMAKE_COMMAND} -E echo "Copied: ${app_bin} -> ${dst_bin}"
    DEPENDS gen_project_binary
    VERBATIM
)

# ALL => runs as part of the default build ("idf.py build")
add_custom_target(copy_app_binary ALL DEPENDS "${dst_bin}")
