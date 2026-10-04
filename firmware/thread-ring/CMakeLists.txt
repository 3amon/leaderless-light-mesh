cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(thread_ring)

target_sources(app PRIVATE src/main.c src/sync.c src/genome.c src/social_scene.c
  src/groups.c src/group_engine.c
  src/app_state.c src/beacon_codec.c src/boot_status.c src/clock_estimator.c
  src/control_auth.c src/control_protocol.c src/group_policy.c src/heal_policy.c
  src/led_budget.c src/solo_mode.c src/always_on.c src/led_config.c src/led_layout.c src/led_power_mode.c src/network_mode.c
  src/network_mode_policy.c src/ota_policy.c
  src/ota_quiet.c
  src/usb_display.c src/usb_sleep.c
  src/peer_freshness.c src/peer_slot_policy.c src/sync_engine.c
  src/sync_schedule.c)

# main.c's DFU-mode hook dereferences struct img_mgmt_upload_req, whose full
# definition (img_mgmt.h) pulls in mcuboot's bootutil headers.
target_include_directories(app PRIVATE
  ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include
  ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/zephyr/include)
