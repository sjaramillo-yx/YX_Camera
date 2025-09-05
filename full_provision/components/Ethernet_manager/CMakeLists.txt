set(srcs
  src/ethernet_manager.c
)

set(include_dirs include)

set(
  requires
    esp_eth
    esp_event
    esp_netif
)

set(priv_include_dirs private_include)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)