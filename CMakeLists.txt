cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esphome/wireguard allows only one IP per peer by default, and it uses that
# slot for our own tunnel address. Without room for server2's address
# (added in net.c), every outbound packet fails with ERR_RTE. Must be global
# because it changes the size of struct wireguard_peer.
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_WIREGUARD_MAX_SRC_IPS=4" APPEND)

project(kia_can_bridge)
