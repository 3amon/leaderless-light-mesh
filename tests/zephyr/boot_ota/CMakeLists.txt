# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(thread_ring_boot_ota)

set(THREAD_RING_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../firmware/thread-ring/src)
target_include_directories(app PRIVATE ${THREAD_RING_SRC})
target_sources(app PRIVATE
  src/main.c
  ${THREAD_RING_SRC}/boot_status.c
  ${THREAD_RING_SRC}/control_auth.c
  ${THREAD_RING_SRC}/led_config.c
  ${THREAD_RING_SRC}/led_layout.c
  ${THREAD_RING_SRC}/led_power_mode.c
  ${THREAD_RING_SRC}/network_mode.c
  ${THREAD_RING_SRC}/network_mode_policy.c
  ${THREAD_RING_SRC}/ota_policy.c)
