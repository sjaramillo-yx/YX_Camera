set(srcs
  src/mqtt_worker.c
  src/nvs_manager.c
  src/provision_claimer.c
  src/recording_worker.c
)

set(include_dirs include)

set(
  requires
    esp_app_format
    json
    recording_events
)

set(
  priv_requires
    mqtt
    nvs_flash
)

set(priv_include_dirs private_include)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)