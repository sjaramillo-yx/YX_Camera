set(srcs
  "src/command_events.c"
)

set(include_dirs include)

set(
  requires
    esp_event
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
)