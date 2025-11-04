set(srcs
  src/ethernet_manager.c
)

set(include_dirs include)

set(
  requires
    esp_eth
    esp_event
    esp_netif
    lwip
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)