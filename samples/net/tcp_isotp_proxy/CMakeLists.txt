# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ethernet_stm32)

target_sources(app PRIVATE src/main.c)

add_subdirectory_ifdef(CONFIG_TCP_CMD_HANDLER tcp_cmd_handler)

add_subdirectory_ifdef(CONFIG_TX_MSG_QUEUE tx_queue)

add_subdirectory_ifdef(CONFIG_TCP_LINK tcp_link)

add_subdirectory_ifdef(CONFIG_CAN_ISOTP_PROXY iso_tp_proxy)
