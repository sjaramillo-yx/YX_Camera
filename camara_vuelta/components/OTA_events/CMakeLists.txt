set(srcs
  "src/OTA_events.c"
)

set(include_dirs include)

set(
  requires
    mqtt
    esp_event
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
)