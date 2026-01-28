set(srcs
  src/mqtt_worker.c
  src/provision_claimer.c
  src/recording_worker.c
  src/s3_uploader.c
  src/http_helpers.c
  src/jobs_manager.c
  src/configuration_worker.c
  src/command_worker.c
)

set(include_dirs include)

set(
  requires
    esp_app_format
    json
    command_events
    configuration_events
    recording_events
    mbedtls
    mqtt
    esp_http_client
    SD_manager
    NVS_manager
)

set(
  priv_requires
    OTA_events
)

set(priv_include_dirs private_include)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)