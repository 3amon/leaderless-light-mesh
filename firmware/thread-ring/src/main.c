/* thread-ring — stage 2 of the TIME-SYNC.md bring-up ladder.
 *
 * ring-test plus OpenThread: joins the existing ring-net mesh as an FTD
 * (rx-on-when-idle) and sends a dashboard-compatible heartbeat every 30 s.
 * The ring shows Thread state at a glance:
 *   white chase        = application booted
 *   amber slow breathe  = detached, searching for the mesh
 *   red double pulse    = recovery reboot / previous watchdog reset
 *   social scenes       = attached (child/router/leader), cycling every 60 s
 *
 * USB CDC is console + shell (try `ot state`, `ot ipaddr`) and the DFU
 * door: opening the port at 1200 baud reboots into the UF2 bootloader.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/usb/usb_dc.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <openthread/thread.h>
#include <openthread/netdata.h>
#include <openthread/platform/radio.h>
#include <openthread/ip6.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <nrfx.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_usbd.h>

#include "sync.h"
#include "escape_ladder.h"
#include "genome.h"
#include "groups.h"
#include "heal_policy.h"
#include "social_scene.h"
#include "app_state.h"
#include "boot_status.h"
#include "control_auth.h"
#include "control_protocol.h"
#include "led_config.h"
#include "led_layout.h"
#include "led_power_mode.h"
#include "led_budget.h"
#include "solo_mode.h"
#include "always_on.h"
#include "network_mode.h"
#include "ota_policy.h"
#include "ota_quiet.h"
#include "usb_display.h"
#include "usb_sleep.h"

#define PI_F 3.14159265f

LOG_MODULE_REGISTER(thread_ring, LOG_LEVEL_INF);

#define FRAME_MS 33
#define HEARTBEAT_MS 30000
#define HB_PORT 1234

/* Node name derived from the factory device ID: one binary, every board a
 * distinct identity. Format word-word-word satisfies the gateway dashboard's
 * node-name rule (lowercase alpha words only).
 */
static char g_name[40] = "unnamed-node";

static const char *const NAME_A[24] = {
	"dusty", "neon", "gentle", "drifting", "humming", "glowing",
	"quiet", "feral", "cosmic", "velvet", "molten", "misty",
	"electric", "wandering", "sleepy", "lucid", "radiant", "hidden",
	"salty", "wild", "tender", "howling", "amber", "opal",
};
static const char *const NAME_B[24] = {
	"lotus", "jelly", "lantern", "comet", "tide", "spark",
	"bloom", "moth", "halo", "beacon", "mirage", "aurora",
	"fern", "koi", "pearl", "dune", "star", "moon",
	"fox", "owl", "wisp", "reef", "iris", "vale",
};
static const char *const NAME_C[24] = {
	"ring", "glow", "light", "shine", "pulse", "gleam",
	"flare", "shimmer", "drift", "breeze", "bloom", "spark",
	"haze", "veil", "tide", "dawn", "dusk", "ember",
	"frost", "rain", "cloud", "wave", "leaf", "stone",
};

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
static const struct gpio_dt_spec carrier_led_power =
	GPIO_DT_SPEC_GET(DT_ALIAS(led_power), gpios);
static const struct gpio_dt_spec module_vcc_power =
	GPIO_DT_SPEC_GET(DT_ALIAS(module_vcc_power), gpios);
static struct led_rgb pixels[LED_LAYOUT_MAX_PIXELS];

#define LED_POWER_STARTUP_DELAY_MS 2000
#define LED_POWER_SETTLE_MS 100
#define LED_STARTUP_GLOW_MS 3000
#define LED_STARTUP_GLOW_FRAME_MS 30
#define LED_STARTUP_GLOW_LEVEL 8
#define DFU_POWER_HOLD_MS 30000

static int led_power_prepare(void)
{
	if (!gpio_is_ready_dt(&carrier_led_power) ||
	    !gpio_is_ready_dt(&module_vcc_power)) {
		return -ENODEV;
	}

	/* Establish both inactive levels before selecting exactly one backend.
	 * P0.17 drives the carrier Q2/Q1 path; P0.13 drives the SuperMini's
	 * switchable 3.3 V VCC output. */
	int rc = gpio_pin_configure_dt(&carrier_led_power, GPIO_OUTPUT_INACTIVE);

	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_configure_dt(&module_vcc_power, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}
	return 0;
}

static int led_power_enable(void)
{
	const struct gpio_dt_spec *selected =
		led_power_mode_get() == LED_POWER_MODULE_VCC
			? &module_vcc_power
			: &carrier_led_power;

	int rc = gpio_pin_set_dt(selected, 1);

	if (rc == 0) {
		k_sleep(K_MSEC(LED_POWER_SETTLE_MS));
		/* WS2812s can retain or display arbitrary latch state while their rail
		 * rises. Make the first transmitted frame black before any marker or
		 * animation is allowed to add load. */
		memset(pixels, 0, sizeof(pixels));
		(void)led_strip_update_rgb(strip, pixels, LED_LAYOUT_MAX_PIXELS);
		k_sleep(K_MSEC(1));
	}
	return rc;
}

static int led_power_start(void)
{
	int rc = led_power_prepare();

	return rc != 0 ? rc : led_power_enable();
}

static void led_power_stop(void)
{
	memset(pixels, 0, sizeof(pixels));
	if (device_is_ready(strip)) {
		(void)led_strip_update_rgb(strip, pixels, LED_LAYOUT_MAX_PIXELS);
		k_sleep(K_MSEC(1));
	}
	(void)gpio_pin_set_dt(&carrier_led_power, 0);
	(void)gpio_pin_set_dt(&module_vcc_power, 0);
}

/* Controlled resets must leave no powered LED-side path behind for MCUboot.
 * In particular, a module-vcc assembly can otherwise carry a large WS2812
 * load into the swap window.  Hold DIN low after removing both candidate
 * rails so an unpowered strip cannot be parasitically fed while the bootloader
 * validates or moves images. */
static void led_power_quiesce_for_reset(void)
{
	led_power_stop();
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 6));
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 6));
	k_sleep(K_MSEC(25));
}

/* live-tunable state (shell + UDP commands) */
static enum led_layout g_led_layout = LED_LAYOUT_RING16;
static int g_led_count = 16;
static float g_brightness = 0.45f;
static uint32_t g_led_budget_per_led = LED_BUDGET_PER_LED_DEFAULT;
static int g_pattern_override = -1;    /* -1 = hashed one-minute cycle */
static uint32_t g_frames;
static uint32_t g_hb_seq;
static int g_cur_pattern = -1;
static enum social_tier g_cur_social_tier = SOCIAL_SOLO;
static uint8_t g_cur_social_family;
static int64_t g_flash_period_ms;      /* 0 = off; else white flash at each
					* global-clock period boundary */
/* Shell-thread requests, applied by the main loop between frames. The
 * shell must never drive the LED strip, the layout/power backends, or
 * the shared heartbeat/beacon socket itself: those race the render loop
 * and the beacon path's hop-limit setsockopt/sendto pair (a heartbeat
 * sent with hops=1 dies silently at the first hop). */
static atomic_t g_shell_hb_req = ATOMIC_INIT(0);
static atomic_t g_shell_layout_req = ATOMIC_INIT(-1);
static atomic_t g_shell_power_req = ATOMIC_INIT(-1);
/* False until network_mode/sync/groups mutexes and state exist; shell
 * commands that touch them check this instead of racing early boot. */
static bool g_core_ready;
static uint32_t g_reset_cause;

/* --- battery sense (AIN7 / P0.31) -------------------------------------
 *
 * EXPERIMENTAL. It is not confirmed that these SuperMini nRF52840 clones
 * populate the battery divider that a genuine nice!nano carries. Everything
 * here therefore degrades to -1 rather than reporting a fabricated voltage:
 * an absent divider must look like "no reading", never like a flat battery,
 * because a bogus low number would be acted on.
 *
 * Reports the voltage at the pin, unscaled. Interpretation is deliberately
 * left to the operator until a divider is confirmed to exist.
 */
static const struct adc_dt_spec g_vbatt = ADC_DT_SPEC_GET(DT_NODELABEL(vbatt));
static bool g_vbatt_ready;

static void battery_init(void)
{
	if (!adc_is_ready_dt(&g_vbatt)) {
		return;
	}
	if (adc_channel_setup_dt(&g_vbatt) != 0) {
		return;
	}
	g_vbatt_ready = true;
}

static int battery_read_mv(void)
{
	if (!g_vbatt_ready) {
		return -1;
	}
	int16_t raw = 0;
	struct adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	if (adc_sequence_init_dt(&g_vbatt, &sequence) != 0) {
		return -1;
	}
	if (adc_read_dt(&g_vbatt, &sequence) != 0) {
		return -1;
	}

	int32_t mv = raw;

	if (adc_raw_to_millivolts_dt(&g_vbatt, &mv) != 0) {
		return -1;
	}
	/* A negative sample is noise on a floating pin, not a voltage. */
	return mv < 0 ? 0 : (int)mv;
}
static uint8_t g_reboot_reason;
static uint8_t g_mcuboot_phase;
static uint8_t g_mcuboot_detail;
static bool g_infrastructure_ok;
static uint32_t g_partition_id;
static enum app_state g_app_state = APP_STATE_BOOTING;
static struct usb_display_policy g_usb_display;
static struct usb_sleep_policy g_usb_sleep;
static struct ota_quiet_policy g_ota_quiet;
static int64_t g_usb_quiet_visual_start = -1;
static int64_t g_usb_sleep_visual_start = -1;
#if defined(CONFIG_THREAD_RING_HIL_FAULT_INJECTION)
static bool g_hil_thread_disabled;
static int64_t g_hil_partition_until;
#endif
#define CMD_PORT 4242

static void use_led_layout(enum led_layout layout)
{
	g_led_layout = layout;
	g_led_count = led_layout_pixel_count(layout);
	g_brightness = (float)led_layout_default_brightness(layout) / 100.f;
}

static bool usb_quiet_display_active(void)
{
	/* A permanently powered node ignores USB for display purposes, the
	 * way the externally powered rope100 layout always has. */
	return g_usb_display.quiet && !always_on_get() &&
	       led_layout_uses_usb_quiet_display(g_led_layout);
}

static int change_led_layout(enum led_layout layout)
{
	if (layout == g_led_layout) {
		return 0;
	}
	int rc = led_config_set_layout(layout);

	if (rc != 0) {
		return rc;
	}
	/* Pixels beyond the new logical end would otherwise retain their last
	 * latched color forever. Clear the transport's full 100-pixel capacity
	 * before shortening subsequent frames. */
	memset(pixels, 0, sizeof(pixels));
	(void)led_strip_update_rgb(strip, pixels, LED_LAYOUT_MAX_PIXELS);
	k_sleep(K_MSEC(1));
	use_led_layout(layout);
	g_usb_quiet_visual_start = -1;
	return 0;
}

static int change_led_power_mode(enum led_power_mode mode)
{
	if (mode == led_power_mode_get()) {
		return 0;
	}
	led_power_stop();
	int rc = led_power_mode_set(mode);

	/* The settings module publishes a new mode only after a successful write,
	 * so this also restores the old backend if persistence failed. */
	int start_rc = led_power_start();

	return rc != 0 ? rc : start_rc;
}

/* GPREGRET2 survives a warm reset without colliding with GPREGRET, which the
 * UF2 bootloader owns (0x57). Exact values make random power-on RAM/register
 * state harmless. */
enum reboot_reason {
	REBOOT_REASON_NONE = 0,
	REBOOT_REASON_HEAL = 0xa1,
	REBOOT_REASON_OTA = 0xa2,
	REBOOT_REASON_COMMAND = 0xa3,
	REBOOT_REASON_ROLLBACK = 0xa4,
	REBOOT_REASON_UF2 = 0xa5,
};

/* Patterns and flash mode run on the gossip-disciplined global clock, so
 * synced neighbours render in step. `scramble` perturbs it; the engine heals.
 */
static int64_t global_ms(void)
{
	return sync_global_ms();
}

static uint32_t hash32(uint32_t x);

static void derive_name(void)
{
	uint32_t h = hash32(NRF_FICR->DEVICEID[0] ^ hash32(NRF_FICR->DEVICEID[1]));

	snprintf(g_name, sizeof(g_name), "%s-%s-%s",
		 NAME_A[h % 24], NAME_B[(h / 24) % 24], NAME_C[(h / 576) % 24]);
}

/* ---------- helpers (ports of the artifact's JS) ---------- */

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/* Pattern families + genome expansion live in genome.c; groups.c owns
 * membership/minting. The render loop composites float-RGB layers and
 * applies brightness at the very end.
 */

/* detached from the mesh: slow amber breathe, unmistakable on camera */
static struct frgb searching_frgb(float t)
{
	float e = 0.5f + 0.5f * sinf(2.f * PI_F * t / 3.2f);
	float L = 0.06f + 0.40f * e * e;

	return (struct frgb){ 1.0f * L, 0.52f * L, 0.06f * L };
}

static bool reboot_reason_valid(uint8_t reason)
{
	return reason >= REBOOT_REASON_HEAL && reason <= REBOOT_REASON_UF2;
}

static const char *reboot_reason_str(uint8_t reason)
{
	switch (reason) {
	case REBOOT_REASON_HEAL:     return "heal";
	case REBOOT_REASON_OTA:      return "ota";
	case REBOOT_REASON_COMMAND:  return "command";
	case REBOOT_REASON_ROLLBACK: return "rollback";
	case REBOOT_REASON_UF2:      return "uf2";
	default:                     return "none";
	}
}

/* Being alone is NOT a fault. A ring frequently has nobody to pair with, so a
 * rejoin reboot is ordinary housekeeping and must never raise the fault
 * indicator; only a hang, a lockup, or a reverted image does. */
static bool is_recovery_boot(void)
{
	return (g_reset_cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP)) ||
	       g_reboot_reason == REBOOT_REASON_ROLLBACK;
}

static void read_boot_cause(void)
{
	uint8_t retained = (uint8_t)NRF_POWER->GPREGRET2;
	uint8_t trace_detail = (uint8_t)NRF_POWER->GPREGRET;

	NRF_POWER->GPREGRET2 = 0;
	if ((retained & 0xf0U) == 0xd0U && (trace_detail & 0x80U) != 0U) {
		g_mcuboot_phase = retained;
		g_mcuboot_detail = trace_detail;
		NRF_POWER->GPREGRET = 0;
		g_reboot_reason = REBOOT_REASON_NONE;
	} else {
		g_mcuboot_phase = 0;
		g_mcuboot_detail = 0;
		g_reboot_reason = reboot_reason_valid(retained) ? retained
							      : REBOOT_REASON_NONE;
	}
	if (hwinfo_get_reset_cause(&g_reset_cause) == 0) {
		(void)hwinfo_clear_reset_cause();
	} else {
		g_reset_cause = 0;
	}
	LOG_INF("boot cause reset=0x%08x retained=%s/0x%02x mcuboot=%02x/%02x",
		g_reset_cause, reboot_reason_str(g_reboot_reason), g_reboot_reason,
		g_mcuboot_phase, g_mcuboot_detail);
}

/* VBUSDETECT reports electrical USB VBUS presence independently of host
 * enumeration. It is an honest "USB power attached" signal, but not proof
 * that the module's charger is actively sourcing current or has terminated. */
static bool usb_vbus_present(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

/* MCUboot's UF2 USB stack can hand the peripheral to the application with a
 * live pull-up, pending IRQ, and enabled endpoints.  Quiesce that inherited
 * hardware state before Zephyr's USB driver establishes its own software
 * state.  This runs after the watchdog is armed, so even a future silicon or
 * driver regression remains recoverable. */
static void sanitize_inherited_usbd(void)
{
	/* A bootloader-to-app reset can arrive while the host and the nRF USB
	 * regulator are still changing state. Never ask USBD to start before
	 * OUTPUTRDY, and give the host a real disconnect interval before the
	 * application reattaches. */
	int64_t supply_deadline = k_uptime_get() + 100;

	while (usb_vbus_present() &&
	       (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_OUTPUTRDY_Msk) == 0 &&
	       k_uptime_get() < supply_deadline) {
		k_sleep(K_MSEC(1));
	}
	NVIC_DisableIRQ(USBD_IRQn);
	NVIC_ClearPendingIRQ(USBD_IRQn);
	nrf_usbd_pullup_disable(NRF_USBD);
	nrf_usbd_int_disable(NRF_USBD, UINT32_MAX);
	nrf_usbd_shorts_disable(NRF_USBD, UINT32_MAX);
	nrf_usbd_ep_all_disable(NRF_USBD);
	(void)nrf_usbd_eventcause_get_and_clear(NRF_USBD);
	nrf_usbd_disable(NRF_USBD);
	k_sleep(K_MSEC(100));
	NVIC_ClearPendingIRQ(USBD_IRQn);
}

static void mark_reboot_reason(enum reboot_reason reason)
{
	/* Every deliberate reset comes through here; it is not a failed boot,
	 * so the escape ladder must not count it toward the DFU escape. */
	escape_ladder_clear(ESCAPE_LADDER_STATE);
	NRF_POWER->GPREGRET2 = (uint8_t)reason;
}

static void status_frame_update(uint32_t sleep_ms)
{
	g_cur_pattern = -1;
	g_frames++;
	led_strip_update_rgb(strip, pixels, g_led_count);
	k_sleep(K_MSEC(sleep_ms));
}

/* Quiet USB mode opens with three unmistakable green pulses, then settles to
 * a barely-on green marker. These raw levels intentionally ignore the normal
 * layout brightness: the marker should be consistent and gentle on either
 * LED assembly. */
static void render_usb_quiet(int64_t now_ms)
{
	enum {
		USB_QUIET_PULSE_MS = 900,
		USB_QUIET_PULSE_COUNT = 3,
	};
	if (g_usb_quiet_visual_start < 0) {
		g_usb_quiet_visual_start = now_ms;
	}
	int64_t elapsed = now_ms - g_usb_quiet_visual_start;
	uint8_t green = 5;

	if (elapsed < USB_QUIET_PULSE_MS * USB_QUIET_PULSE_COUNT) {
		float phase = (float)(elapsed % USB_QUIET_PULSE_MS) /
			      (float)USB_QUIET_PULSE_MS;
		float envelope = 0.5f - 0.5f * cosf(2.f * PI_F * phase);

		green = (uint8_t)(4.f + 90.f * envelope * envelope);
	}
	for (int k = 0; k < g_led_count; k++) {
		pixels[k] = (struct led_rgb){ .r = 0, .g = green, .b = 0 };
	}
	status_frame_update(FRAME_MS);
}

static bool render_usb_sleep_armed(int64_t now_ms)
{
	if (g_usb_sleep.knocks == 0 || g_usb_sleep_visual_start < 0) {
		return false;
	}
	int64_t elapsed = now_ms - g_usb_sleep_visual_start;

	if (elapsed < 0 || elapsed >= 1200) {
		return false;
	}
	bool on = (elapsed < 220) || (elapsed >= 440 && elapsed < 660);
	uint8_t green = on ? 80 : 2;

	for (int k = 0; k < g_led_count; k++) {
		pixels[k] = (struct led_rgb){ .r = 0, .g = green, .b = 0 };
	}
	status_frame_update(FRAME_MS);
	return true;
}

static void enter_system_off(void)
{
	/* The gesture completes on a detach edge. Never enter System OFF while
	 * VBUS is already high: VBUS is also our only buttonless wake source. */
	if (usb_vbus_present()) {
		LOG_WRN("refusing System OFF while USB is present");
		return;
	}
	LOG_INF("entering System OFF; USB insertion required to wake");

	/* A short green closing fade confirms the second knock before the LED
	 * rail disappears. Keep this comfortably inside the 15-second watchdog. */
	for (int step = 5; step >= 1; step--) {
		uint8_t green = (uint8_t)(step * 16);

		for (int k = 0; k < g_led_count; k++) {
			pixels[k] = (struct led_rgb){ .r = 0, .g = green, .b = 0 };
		}
		status_frame_update(70);
	}
	led_power_stop();

	/* DIN must not source an unpowered WS2812 through its input protection.
	 * The application does not resume after this direct pin handoff. */
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 6));
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 6));

	sanitize_inherited_usbd();
	NRF_POWER->EVENTS_USBDETECTED = 0;
	NRF_POWER->INTENSET = POWER_INTENSET_USBDETECTED_Msk;
	__DSB();
	__ISB();
	sys_poweroff();
}

/* Dim the ring smoothly to black from whatever it is currently showing. A
 * rejoin reboot should read as the light quietly going out and coming back,
 * not as an event: no colour change, no flash, just a fade. */
static void fade_to_black(uint32_t ms)
{
	struct led_rgb start[LED_LAYOUT_MAX_PIXELS];
	const int steps = 12;

	memcpy(start, pixels, sizeof(start));
	for (int s = steps - 1; s >= 0; s--) {
		for (int k = 0; k < g_led_count; k++) {
			pixels[k] = (struct led_rgb){
				.r = (uint8_t)(start[k].r * s / steps),
				.g = (uint8_t)(start[k].g * s / steps),
				.b = (uint8_t)(start[k].b * s / steps),
			};
		}
		status_frame_update(ms / steps);
	}
}

/* After the selected rail has settled and accepted a black frame, breathe a
 * deliberately tiny neutral glow before handing control to normal rendering.
 * Keeping this below the quiet-charge pulse level makes it a gentle power-on
 * acknowledgement rather than another status animation. */
static void show_led_startup_glow(const struct device *wdt, int wdt_ch)
{
	for (int elapsed = 0; elapsed < LED_STARTUP_GLOW_MS;
	     elapsed += LED_STARTUP_GLOW_FRAME_MS) {
		float phase = (float)elapsed / (float)LED_STARTUP_GLOW_MS;
		float envelope = sinf(PI_F * phase);
		uint8_t level = (uint8_t)(LED_STARTUP_GLOW_LEVEL * envelope);

		for (int k = 0; k < g_led_count; k++) {
			pixels[k] = (struct led_rgb){
				.r = level,
				.g = level,
				.b = level,
			};
		}
		status_frame_update(LED_STARTUP_GLOW_FRAME_MS);
		if (wdt_ch >= 0) {
			(void)wdt_feed(wdt, wdt_ch);
		}
	}
	memset(pixels, 0, sizeof(pixels));
	(void)led_strip_update_rgb(strip, pixels, LED_LAYOUT_MAX_PIXELS);
}

/* Ramps 0 -> 1 on every boot so the first normal composite cannot step the
 * newly enabled LED rail straight to its configured brightness. */
static float g_resume_fade;

/* Set for the whole re-attach window after a rejoin restart. Re-attaching takes
 * far longer than the 2 s attach crossfade, so without this the ring fades up
 * into its pattern and then slides straight back into amber "searching" —
 * announcing the reboot a few seconds late instead of hiding it. While this is
 * set the ring simply keeps rendering itself; solitude is a normal state, not
 * something to display. Cleared the moment it attaches. */
static bool g_heal_resume;

/* First 2.4 s of every application boot. A prior recovery/WDT uses an
 * unmistakable red double-pulse; ordinary/command boots chase white, and an
 * OTA test boot chases cyan. */
static bool render_boot_status(int64_t now)
{
	if (now >= APP_STATE_BOOT_STATUS_MS) {
		return false;
	}

	bool fault = is_recovery_boot();
	memset(pixels, 0, sizeof(pixels));
	if (g_reboot_reason == REBOOT_REASON_HEAL) {
		/* Came back from a rejoin attempt: hold the ring dark through the
		 * boot window so it fades back up into the pattern rather than
		 * announcing itself. The fade-out and this dark window are the
		 * two halves of one seamless restart. */
		status_frame_update(FRAME_MS);
		return true;
	}
	if (fault) {
		int phase = now % 1000;
		bool on = phase < 160 || (phase >= 300 && phase < 460);

		for (int k = 0; k < g_led_count; k++) {
			pixels[k] = (struct led_rgb){ .r = on ? 70 : 2, .g = 0, .b = 0 };
		}
	} else {
		int head = (now / 90) % g_led_count;
		bool cyan = g_reboot_reason == REBOOT_REASON_OTA ||
			    g_reboot_reason == REBOOT_REASON_UF2;

		for (int d = 0; d < 4; d++) {
			int k = (head - d + g_led_count) % g_led_count;
			uint8_t v = 65 / (d + 1);

			pixels[k] = cyan
				? (struct led_rgb){ .r = 0, .g = v, .b = v }
				: (struct led_rgb){ .r = v, .g = v, .b = v };
		}
	}
	status_frame_update(FRAME_MS);
	return true;
}

static void show_reboot_warning(enum reboot_reason reason)
{
	mark_reboot_reason(reason);
	if (reason == REBOOT_REASON_HEAL) {
		/* Solitude housekeeping: leave quietly. */
		fade_to_black(600);
		return;
	}
	for (int step = 0; step < 8; step++) {
		bool on = step == 0 || step == 1 || step == 3 || step == 4;
		struct led_rgb c = (reason == REBOOT_REASON_OTA ||
				    reason == REBOOT_REASON_UF2)
			? (struct led_rgb){ .r = 0, .g = on ? 55 : 0, .b = on ? 70 : 2 }
			: (struct led_rgb){ .r = on ? 70 : 2, .g = 0, .b = 0 };

		for (int k = 0; k < g_led_count; k++) {
			pixels[k] = c;
		}
		status_frame_update(100);
	}
}

/* Tell the host we are leaving and give the detach event time to reach the Pi
 * before resetting.  Zephyr's own 1200-baud bootloader hook follows the same
 * usb_dc_detach() -> NVIC_SystemReset() ordering. */
static void clean_reboot(enum reboot_reason reason, bool enter_uf2)
{
	show_reboot_warning(reason);
	led_power_quiesce_for_reset();
	if (enter_uf2) {
		NRF_POWER->GPREGRET = 0x57;  /* DFU_MAGIC_UF2_RESET */
	}

	int rc = usb_dc_detach();

	if (rc != 0) {
		LOG_WRN("USB detach before reboot failed: %d", rc);
	}
	k_sleep(K_MSEC(100));
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

/* This node's deterministic one-color identity, derived from its FICR seed. */
static struct social_scene g_solo;

/* ---------- 1200-baud touch -> UF2 bootloader ---------- */

static void dte_rate_cb(const struct device *dev, uint32_t rate)
{
	if (rate == 1200) {
		mark_reboot_reason(REBOOT_REASON_UF2);
		(void)gpio_pin_set_dt(&carrier_led_power, 0);
		(void)gpio_pin_set_dt(&module_vcc_power, 0);
		nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 6));
		nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 6));
		k_busy_wait(25000);
		NRF_POWER->GPREGRET = 0x57;  /* DFU_MAGIC_UF2_RESET */
		(void)usb_dc_detach();
		NVIC_SystemReset();
	}
}

/* ---------- Thread state + heartbeat ---------- */

static otDeviceRole ot_role(void)
{
	struct openthread_context *ctx = openthread_get_default_context();
	otDeviceRole role;

	openthread_api_mutex_lock(ctx);
	role = otThreadGetDeviceRole(ctx->instance);
	openthread_api_mutex_unlock(ctx);
	return role;
}

/* A Thread role alone is not proof that we joined the installation mesh: the
 * two rings can elect their own leader and look "attached" to one another.
 * The OTBR advertises both external routes and an on-mesh prefix in Network
 * Data. Require at least one entry owned by a different RLOC. */
static bool ot_has_infrastructure(void)
{
	struct openthread_context *ctx = openthread_get_default_context();
	bool found = false;

	openthread_api_mutex_lock(ctx);
	otDeviceRole role = otThreadGetDeviceRole(ctx->instance);
	if (role >= OT_DEVICE_ROLE_CHILD) {
		uint16_t self = otThreadGetRloc16(ctx->instance);
		otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
		otExternalRouteConfig route;

		g_partition_id = otThreadGetPartitionId(ctx->instance);
		while (otNetDataGetNextRoute(ctx->instance, &it, &route) == OT_ERROR_NONE) {
			if (route.mRloc16 != self) {
				found = true;
				break;
			}
		}
		if (!found) {
			otBorderRouterConfig prefix;

			it = OT_NETWORK_DATA_ITERATOR_INIT;
			while (otNetDataGetNextOnMeshPrefix(ctx->instance, &it, &prefix) ==
			       OT_ERROR_NONE) {
				if (prefix.mRloc16 != self) {
					found = true;
					break;
				}
			}
		}
	} else {
		g_partition_id = 0;
	}
	openthread_api_mutex_unlock(ctx);
	return found;
}

/* Rejoin backoff. A leader with zero neighbours is a singleton partition. That
 * can mean the radio got itself stuck, or it can simply mean nobody else is
 * here — and the node cannot tell the two apart. It does not need to: the
 * rejoin reboot is seamless (fade out, dark, fade back), so guessing wrong is
 * cheap. Assume a radio fault first and retry quickly; once solitude has
 * persisted it is almost certainly real, so slow down and let the ring get on
 * with rendering itself rather than restarting every minute all night.
 */
/* The schedule and its escalation live in heal_policy.c, which is pure and
 * host-tested. The attempt counter is PERSISTED (settings/NVS, alongside the
 * boot ordinal) rather than held in RAM: it has to survive the very reboot it
 * is counting, and __noinit RAM does not — MCUboot is chainloaded ahead of the
 * application and uses RAM itself, so the counter read back as zero on every
 * boot and the interval never escalated past the first step. */

/* ---- test injection into the policy's inputs (see control_protocol.h) ----
 * Plain statics, so they are gone after any reset. Nothing here reaches
 * heal_policy_update(); it only changes what the observation is filled in with,
 * which keeps the shipping decision path under test rather than a mock of it. */
static bool g_heal_sim_alone;      /* pretend the neighbour table is empty */
static bool g_heal_sim_fire;       /* pretend the interval already elapsed */

static void heal_sim_apply(enum control_heal_sim action, int64_t value)
{
	switch (action) {
	case CONTROL_HEAL_SIM_CLEAR:
		g_heal_sim_alone = false;
		g_heal_sim_fire = false;
		break;
	case CONTROL_HEAL_SIM_ALONE:
		g_heal_sim_alone = value != 0;
		break;
	case CONTROL_HEAL_SIM_FIRE:
		g_heal_sim_fire = true;
		break;
	case CONTROL_HEAL_SIM_ATTEMPTS:
		(void)boot_status_set_heal_attempts((uint8_t)value);
		break;
	}
	LOG_INF("heal sim: alone=%d fire=%d attempts=%u",
		g_heal_sim_alone ? 1 : 0, g_heal_sim_fire ? 1 : 0,
		boot_status_heal_attempts());
}

static void lonely_leader_heal(void)
{
	static int64_t lonely_since = -1;
	static int64_t next_check;

	if (k_uptime_get() < next_check) {
		return;
	}
	next_check = k_uptime_get() + 5000;

	struct openthread_context *ctx = openthread_get_default_context();
	bool lonely = false;
	bool reboot = false;

	openthread_api_mutex_lock(ctx);
	if (otThreadGetDeviceRole(ctx->instance) == OT_DEVICE_ROLE_LEADER) {
		otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
		otNeighborInfo info;

		lonely = otThreadGetNextNeighborInfo(ctx->instance, &it, &info)
			 != OT_ERROR_NONE;
	}
	/* Everything below is policy and persistence — release the OT API
	 * mutex first. Holding it across the NVS write stalled the radio
	 * for up to hundreds of ms (sector GC) exactly while the node was
	 * lonely and trying to find company. */
	openthread_api_mutex_unlock(ctx);

	/* Injected solitude reads exactly like the real thing from here on. */
	if (g_heal_sim_alone) {
		lonely = true;
	}
	if (lonely && lonely_since < 0) {
		lonely_since = k_uptime_get();
	}

	struct heal_observation obs = {
		.lonely_for_ms = (lonely && lonely_since >= 0)
					 ? k_uptime_get() - lonely_since
					 : 0,
		.attempts = boot_status_heal_attempts(),
		.lonely = lonely,
		.power_on_reset = false,   /* handled once at boot */
		/* A deliberate single-node install: solitude is the deployment,
		 * not a fault to restart out of. */
		.solo_expected = solo_mode_get(),
	};

	if (g_heal_sim_fire && lonely) {
		/* Report the interval as already elapsed. This still exercises
		 * the real comparison rather than bypassing it, so a 30 minute
		 * step is proven in seconds — and unlike backdating a start
		 * time it cannot underflow on a freshly booted uptime. */
		obs.lonely_for_ms = heal_policy_interval_ms(obs.attempts);
	}
	struct heal_decision decision = heal_policy_update(&obs);

	/* The setter ignores an unchanged value, so the overwhelmingly common
	 * not-lonely path costs no flash write. Persist BEFORE rebooting, or the
	 * next boot repeats this step forever. */
	(void)boot_status_set_heal_attempts(decision.attempts);
	if (!lonely) {
		lonely_since = -1;
	}
	if (decision.reboot) {
		/* otThreadBecomeDetached() has repeatedly wedged the radio on these
		 * boards, and calling it while holding the OpenThread API mutex can
		 * stall this loop until the hardware watchdog fires. A clean reboot
		 * reliably performs the full attach scan and preserves an explicit
		 * reason, so use that directly after sustained solitude. */
		LOG_INF("alone for %lld ms — quiet reboot to look for company "
			"(attempt %u)",
			(long long)decision.interval_ms, decision.attempts);
		reboot = true;
		lonely_since = -1;
	}
	if (reboot) {
		clean_reboot(REBOOT_REASON_HEAL, false);
	}
}

/* ---------- OTA DFU mode ----------
 * While an image upload is streaming in over SMP, the board mutes its own
 * radio chatter and removes power from the LED rail.  Keeping VCC dark for a
 * bounded grace period after the last chunk covers candidate inspection and
 * the subsequent reboot without carrying WS2812 load into MCUboot.  A stalled
 * upload restores normal rendering after the grace period.
 */
/* Written on the mcumgr SMP thread, read by the main loop: a single
 * aligned 32-bit word so the cross-thread read can never tear (the old
 * int64 had different high words before/after the first chunk, and one
 * torn read bounced the LED rail mid-upload). Unsigned subtraction keeps
 * ages correct across the ~49.7-day wrap. */
static uint32_t g_dfu_last_chunk = (uint32_t)-60000;

static enum mgmt_cb_return dfu_chunk_cb(uint32_t event, enum mgmt_cb_return prev_status,
					int32_t *rc, uint16_t *group, bool *abort_more,
					void *data, size_t data_size)
{
	if (event == MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK) {
		g_dfu_last_chunk = (uint32_t)k_uptime_get();
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback dfu_mgmt_cb = {
	.callback = dfu_chunk_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

static int hb_sock = -1;
static void send_heartbeat(void);

static void hb_init(void)
{
	hb_sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (hb_sock < 0) {
		LOG_ERR("heartbeat socket failed: %d", hb_sock);
		return;
	}
	int hops = 8;  /* multicast must cross the mesh, not die at the parent */
	zsock_setsockopt(hb_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			 &hops, sizeof(hops));
	/* same socket doubles as the remote command listener */
	struct sockaddr_in6 bind_addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(CMD_PORT),
		.sin6_addr = in6addr_any,
	};
	if (zsock_bind(hb_sock, (struct sockaddr *)&bind_addr,
		       sizeof(bind_addr)) < 0) {
		LOG_ERR("cmd bind failed");
	}
}

/* Remote commands are exact, bounded datagrams. Legacy and v1 read-only
 * queries remain compatible. Every mutation requires a per-device-keyed v2
 * envelope whose sequence is persisted before the side effect. */
static void send_control_response(const struct sockaddr_in6 *destination,
				  socklen_t destination_length,
				  const struct control_command *command,
				  const char *payload, bool error)
{
	char reply[384];
	bool encoded = error ? control_encode_error(reply, sizeof(reply), command,
						    payload)
			     : control_encode_reply(reply, sizeof(reply), command,
						    payload);

	if (!encoded) {
		/* Even the failure reply must keep the v2 envelope, or an
		 * authenticated client rejects it as unparseable and burns
		 * the sequence into a forced re-sync. */
		if (!control_encode_error(reply, sizeof(reply), command,
					  "error=reply_overflow")) {
			return;
		}
	}
	if (command->authenticated) {
		uint8_t tag[CONTROL_AUTH_TAG_BYTES];
		size_t used = strlen(reply);

		if (control_auth_sign(reply, used, tag) != 0 ||
		    used + 1 + CONTROL_AUTH_TAG_HEX_LENGTH >= sizeof(reply)) {
			return;
		}
		reply[used++] = ' ';
		for (size_t index = 0; index < sizeof(tag); index++) {
			int written = snprintf(reply + used, sizeof(reply) - used,
					       "%02x", tag[index]);

			if (written != 2) {
				return;
			}
			used += 2;
		}
		reply[used] = '\0';
	}
	zsock_sendto(hb_sock, reply, strlen(reply), 0,
		     (const struct sockaddr *)destination, destination_length);
}

static void poll_commands(void)
{
	char datagram[CONTROL_MAX_DATAGRAM + 1];
	char payload[384];
	struct sockaddr_in6 source;
	socklen_t source_length = sizeof(source);
	struct control_command command;

	if (hb_sock < 0) {
		return;
	}
	int received = zsock_recvfrom(
		hb_sock, datagram, sizeof(datagram), ZSOCK_MSG_DONTWAIT,
		(struct sockaddr *)&source, &source_length);

	if (received <= 0) {
		return;
	}
	enum control_parse_result parse_result = control_parse(
		datagram, (size_t)received, GENOME_N_BASES, &command);

	if (parse_result != CONTROL_PARSE_OK) {
		LOG_WRN("rejected command (%d bytes): %s", received,
			control_parse_result_name(parse_result));
		send_control_response(&source, source_length, &command,
				      control_parse_result_name(parse_result), true);
		return;
	}
	LOG_INF("cmd rx: type=%d versioned=%d", command.type,
		command.versioned ? 1 : 0);
	bool requires_auth = control_command_requires_auth(&command);

	if (!requires_auth && command.authenticated) {
		send_control_response(&source, source_length, &command,
				      "auth_not_required", true);
		return;
	}
	if (requires_auth && !command.authenticated) {
		send_control_response(&source, source_length, &command,
				      "auth_required", true);
		return;
	}
	if (requires_auth) {
		enum control_auth_result auth_result = control_auth_authorize(
			datagram, command.signed_length, command.sequence,
			command.auth_tag);

		if (auth_result == CONTROL_AUTH_ACCEPT_DUPLICATE) {
			send_control_response(&source, source_length, &command,
					      "duplicate", false);
			return;
		}
		if (auth_result != CONTROL_AUTH_ACCEPT_NEW) {
			send_control_response(&source, source_length, &command,
					      control_auth_result_name(auth_result), true);
			return;
		}
	}

	bool formatted = true;
	bool reboot_after_reply = false;
	bool sleep_after_reply = false;
	uint8_t reboot_reason = REBOOT_REASON_NONE;

	switch (command.type) {
	case CONTROL_FLASH:
		g_flash_period_ms = command.value;
		formatted = control_format(payload, sizeof(payload), "flash=%lld",
					   (long long)g_flash_period_ms);
		break;
	case CONTROL_SCRAMBLE: {
		sync_scramble_ms(command.value);
		struct sync_stats stats;

		sync_get_stats(&stats);
		formatted = control_format(payload, sizeof(payload), "theta=%lldms",
					   (long long)(stats.theta_us / 1000));
		break;
	}
	case CONTROL_UPDATE: {
		/* Self-arm the uploaded slot-1 image; direct trailer ownership avoids
		 * the delayed external SMP state-write race. */
		int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);

		formatted = control_format(payload, sizeof(payload), "update rc=%d %s",
					   rc,
					   rc == 0 ? "armed, rebooting" : "NOT armed");
		if (rc == 0) {
			reboot_after_reply = true;
			reboot_reason = REBOOT_REASON_OTA;
		}
		break;
	}
	case CONTROL_REBOOT:
		formatted = control_format(payload, sizeof(payload), "rebooting");
		reboot_after_reply = true;
		reboot_reason = REBOOT_REASON_COMMAND;
		break;
	case CONTROL_SLEEP: {
		bool dfu_active =
			(uint32_t)k_uptime_get() - g_dfu_last_chunk < 3000U;

		if (usb_vbus_present()) {
			send_control_response(&source, source_length, &command,
					      "usb_present", true);
			return;
		}
		if (!boot_is_img_confirmed()) {
			send_control_response(&source, source_length, &command,
					      "image_unconfirmed", true);
			return;
		}
		if (dfu_active) {
			send_control_response(&source, source_length, &command,
					      "dfu_active", true);
			return;
		}
		formatted = control_format(payload, sizeof(payload),
					   "sleeping wake=usb");
		sleep_after_reply = true;
		break;
	}
	case CONTROL_SKEW:
		sync_skew_set((double)command.value);
		formatted = control_format(payload, sizeof(payload), "skew=%dppm",
					   (int)sync_skew_get());
		break;
	case CONTROL_PATTERN:
		g_pattern_override = (int)command.value;
		formatted = control_format(payload, sizeof(payload), "pattern=%d",
					   g_pattern_override);
		break;
	case CONTROL_BRIGHTNESS:
		g_brightness = (float)command.value / 100.f;
		formatted = control_format(payload, sizeof(payload), "bright=%d",
					   (int)(g_brightness * 100.f));
		break;
	case CONTROL_SOLO_MODE:
		if (!command.query) {
			int rc = solo_mode_set(command.value != 0);

			if (rc != 0) {
				send_control_response(&source, source_length,
						      &command, "solo_storage",
						      true);
				return;
			}
		}
		formatted = control_format(payload, sizeof(payload), "solo=%s",
					   solo_mode_get() ? "on" : "off");
		break;
	case CONTROL_ALWAYS_ON:
		if (!command.query) {
			int rc = always_on_set(command.value != 0);

			if (rc != 0) {
				send_control_response(&source, source_length,
						      &command,
						      "always_on_storage", true);
				return;
			}
		}
		formatted = control_format(payload, sizeof(payload),
					   "alwayson=%s",
					   always_on_get() ? "on" : "off");
		break;
	case CONTROL_LED_BUDGET:
		if (!command.query) {
			g_led_budget_per_led = (uint32_t)command.value;
		}
		formatted = control_format(
			payload, sizeof(payload), "budget=%u total=%u",
			g_led_budget_per_led,
			led_budget_total(g_led_count, g_led_budget_per_led));
		break;
	case CONTROL_LED_LAYOUT:
		if (!command.query) {
			enum led_layout requested = command.value == 7
						    ? LED_LAYOUT_JEWEL7
						    : command.value == 100
							      ? LED_LAYOUT_ROPE100
							      : LED_LAYOUT_RING16;
			int rc = change_led_layout(requested);

			if (rc != 0) {
				send_control_response(&source, source_length, &command,
						      "layout_storage", true);
				return;
			}
		}
		formatted = control_format(payload, sizeof(payload),
					   "layout=%s leds=%d bright=%d",
					   led_layout_name(g_led_layout), g_led_count,
					   (int)(g_brightness * 100.f));
		break;
	case CONTROL_LED_POWER:
		if (!command.query) {
			enum led_power_mode requested = command.value == 1
							 ? LED_POWER_MODULE_VCC
							 : LED_POWER_CARRIER;
			int rc = change_led_power_mode(requested);

			if (rc != 0) {
				send_control_response(&source, source_length, &command,
						      "power_storage", true);
				return;
			}
		}
		formatted = control_format(payload, sizeof(payload), "power=%s",
					   led_power_mode_name(led_power_mode_get()));
		break;
	case CONTROL_HEARTBEAT:
		send_heartbeat();
		formatted = control_format(payload, sizeof(payload), "hb sent seq=%u",
					   g_hb_seq);
		break;
	case CONTROL_GOSSIP:
		sync_set_dest(command.value == CONTROL_GOSSIP_LOCAL
				      ? SYNC_DEST_LOCAL
			      : command.value == CONTROL_GOSSIP_REALM
				      ? SYNC_DEST_REALM
				      : SYNC_DEST_BOTH);
		formatted = control_format(payload, sizeof(payload), "gossip dest=%s",
					   sync_dest());
		break;
	case CONTROL_OTA_QUIET:
		if (!command.query) {
			ota_quiet_set(&g_ota_quiet, k_uptime_get(),
				      (uint32_t)command.value);
		}
		formatted = control_format(
			payload, sizeof(payload), "quiet=%us",
			ota_quiet_remaining_s(&g_ota_quiet, k_uptime_get()));
		break;
	case CONTROL_NETWORK_MODE: {
		enum network_mode_apply_result result = NETWORK_MODE_UNCHANGED;

		if (!command.query) {
			result = network_mode_set((enum network_mode)command.value,
					  (uint32_t)command.value2,
					  (uint8_t)command.value3,
					  (uint8_t)command.value4,
					  (uint8_t)command.value5,
					  (uint8_t)command.value6);
			if (result == NETWORK_MODE_STORAGE_ERROR) {
				send_control_response(&source, source_length, &command,
						      "mode_storage", true);
				return;
			}
		}
		struct network_mode_value value = network_mode_get();

		unsigned int blink_ms = value.ultra_blink_cs == 0U
			? 0U
			: (unsigned int)value.ultra_blink_cs * 10U;
		unsigned int duty_pct = (unsigned int)
			(network_mode_duty_fraction(value.ultra_duty_pct) *
			 100.f + 0.5f);
		const char *wave = network_mode_wave_name(value.wave);
		unsigned int accent_pct = (unsigned int)value.accent_pct;

		if (command.query) {
			formatted = control_format(payload, sizeof(payload),
						   "mode=%s priority=%u blink=%ums duty=%u wave=%s accent=%u",
						   network_mode_name(value.mode),
						   value.priority, blink_ms,
						   duty_pct, wave, accent_pct);
		} else {
			const char *result_name = result == NETWORK_MODE_ADOPTED
				? "adopted" : result == NETWORK_MODE_REJECTED
				? "rejected" : "unchanged";

			formatted = control_format(payload, sizeof(payload),
						   "mode=%s priority=%u blink=%ums duty=%u wave=%s accent=%u result=%s",
						   network_mode_name(value.mode),
						   value.priority, blink_ms,
						   duty_pct, wave, accent_pct,
						   result_name);
		}
		break;
	}
	case CONTROL_HEAL_SIM:
		heal_sim_apply((enum control_heal_sim)command.value,
			       command.value2);
		formatted = control_format(
			payload, sizeof(payload),
			"heal alone=%d fire=%d attempts=%u interval=%lld",
			g_heal_sim_alone ? 1 : 0, g_heal_sim_fire ? 1 : 0,
			boot_status_heal_attempts(),
			(long long)heal_policy_interval_ms(
				boot_status_heal_attempts()));
		break;
	case CONTROL_GROUP: {
		int length = groups_status(payload, sizeof(payload));

		formatted = length >= 0 && length < (int)sizeof(payload);
		break;
	}
	case CONTROL_BOOT: {
		int length = boot_status_format(payload, sizeof(payload));

		formatted = length >= 0 && length < (int)sizeof(payload);
		break;
	}
	case CONTROL_AUTH_STATUS:
		formatted = control_format(
			payload, sizeof(payload), "auth=%s seq=%016llx",
			control_auth_ready() ? "ready" : "unprovisioned",
			(unsigned long long)control_auth_sequence());
		break;
	case CONTROL_STATUS: {
		struct sync_stats stats;
		struct network_mode_value mode = network_mode_get();

		sync_get_stats(&stats);
		struct control_status status = {
			.name = g_name,
			.uptime_s = k_uptime_get() / 1000,
			.app_state = app_state_name(g_app_state),
			.theta_ms = stats.theta_us / 1000,
			.rate_ppm = (int)stats.rate_ppm,
			.locked_peers = stats.locked,
			.fresh_peers = stats.neighbors,
			.tracked_peers = stats.tracked,
			.oldest_age_ms = stats.oldest_age_ms,
			.freshness_lapses = stats.freshness_lapses,
			.sync_profile = stats.profile,
			.ota_quiet_s = ota_quiet_remaining_s(&g_ota_quiet,
							 k_uptime_get()),
			.network_mode = network_mode_name(mode.mode),
			.mode_priority = mode.priority,
			.infrastructure_ok = g_infrastructure_ok,
			.usb_present = usb_vbus_present(),
			.usb_quiet = usb_quiet_display_active(),
			.partition_id = g_partition_id,
			.reset_cause = g_reset_cause,
			.reset_reason = reboot_reason_str(g_reboot_reason),
			.flash_period_ms = g_flash_period_ms,
			.pattern = g_cur_pattern,
			.led_budget = g_led_budget_per_led,
			.solo_mode = solo_mode_get(),
			.always_on = always_on_get(),
			.brightness_pct = (int)(g_brightness * 100.f),
			.led_layout = led_layout_name(g_led_layout),
			.led_count = g_led_count,
			.led_power = led_power_mode_name(led_power_mode_get()),
			.sleep_armed = g_usb_sleep.knocks > 0,
			.frames = g_frames,
			.boot_sequence = boot_status_sequence(),
			.boot_stage = boot_status_stage_name(boot_status_stage()),
			.build_id = CONFIG_THREAD_RING_BUILD_ID,
			.auth_ready = control_auth_ready(),
			.battery_mv = battery_read_mv(),
		};

		formatted = control_encode_status(payload, sizeof(payload), &status);
		break;
	}
	}

	if (!formatted) {
		send_control_response(&source, source_length, &command,
				      "reply_overflow", true);
		return;
	}
	send_control_response(&source, source_length, &command, payload, false);
	if (reboot_after_reply) {
		clean_reboot(reboot_reason, false);
	}
	if (sleep_after_reply) {
		enter_system_off();
	}
}

/* format matches the gateway dashboard:
 * HB:<seq>:<name>@<ml-eid>@v<fw>@up<sec>@rssi<dbm>@hw<ok>
 */
#ifndef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#define CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION "dev"
#endif

static void send_heartbeat(void)
{
	char addr[40] = "";
	char buf[384];
	int8_t rssi = 0;
	char role_c = '?';

	if (hb_sock < 0) {
		return;
	}

	struct openthread_context *ctx = openthread_get_default_context();

	openthread_api_mutex_lock(ctx);
	const otIp6Address *mleid = otThreadGetMeshLocalEid(ctx->instance);

	if (mleid) {
		otIp6AddressToString(mleid, addr, sizeof(addr));
	}
	otDeviceRole role = otThreadGetDeviceRole(ctx->instance);

	switch (role) {
	case OT_DEVICE_ROLE_CHILD:
		role_c = 'c';
		/* parent RSSI is meaningful for children */
		otThreadGetParentLastRssi(ctx->instance, &rssi);
		break;
	case OT_DEVICE_ROLE_ROUTER:
	case OT_DEVICE_ROLE_LEADER:
		role_c = role == OT_DEVICE_ROLE_LEADER ? 'l' : 'r';
		/* routers have no parent — report the STRONGEST neighbour
		 * instead of a misleading 0 (which read as "0 dBm" on the
		 * dashboard and confused everyone)
		 */
		{
			otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
			otNeighborInfo info;
			int8_t best = INT8_MIN;

			while (otThreadGetNextNeighborInfo(ctx->instance, &it,
							   &info) == OT_ERROR_NONE) {
				if (info.mAverageRssi > best) {
					best = info.mAverageRssi;
				}
			}
			rssi = best == INT8_MIN ? 0 : best;
		}
		break;
	case OT_DEVICE_ROLE_DETACHED:
		role_c = 'd';
		break;
	default:
		break;
	}
	openthread_api_mutex_unlock(ctx);

	/* fw "led2" marks this as an LED-ring node to the dashboard (keep — it
	 * is the ring-detection key); img carries the REAL MCUboot image
	 * version, role the Thread role. Older parsers ignore unknown @fields.
	 */
	struct sync_stats st;
	struct network_mode_value mode = network_mode_get();

	sync_get_stats(&st);
	int n = snprintf(buf, sizeof(buf),
			 /* @bat is millivolts at the AIN7 sense pin, or -1 when
			  * unavailable. The dashboard has always parsed `bat`
			  * from heartbeats; the firmware simply never sent one,
			  * which is why it rendered null. */
			 "HB:%u:%s@%s@vled2@up%lld@rssi%d@hwok@bat%d@pat%d@ofs%lld@ppm%d@nbr%d@img%s@role%c@sid%u@agr%08x@glit%d@gpat%d@tier%s@gsize%u@mode%s@prio%u@br%d@usb%d@uq%d@part%08x@rst%08x@why%02x@boot%u/%u@bid%s",
			 g_hb_seq++, g_name, addr, k_uptime_get() / 1000, rssi,
			 battery_read_mv(),
			 g_cur_pattern, st.theta_us / 1000, (int)st.rate_ppm,
			 st.neighbors, CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION, role_c,
			 groups_small_id(), groups_agreed(), groups_lit() ? 1 : 0,
			 groups_group_pattern(),
			 social_tier_name(groups_tier()),
			 groups_member_count(), network_mode_name(mode.mode),
			 mode.priority, g_infrastructure_ok ? 1 : 0,
			 usb_vbus_present() ? 1 : 0,
			 usb_quiet_display_active() ? 1 : 0,
			 g_partition_id, g_reset_cause, g_reboot_reason,
			 boot_status_sequence(), boot_status_stage(),
			 CONFIG_THREAD_RING_BUILD_ID);
	if (n < 0 || n >= (int)sizeof(buf)) {
		LOG_ERR("heartbeat formatting overflow (%d/%u)", n,
			(unsigned int)sizeof(buf));
		return;
	}

	/* socket is shared with gossip beacon TX, which may have set hops=1
	 * (link-local mode) — reassert mesh-wide hops before every heartbeat */
	int hops = 8;

	zsock_setsockopt(hb_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			 &hops, sizeof(hops));

	struct sockaddr_in6 dst = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(HB_PORT),
	};
	zsock_inet_pton(AF_INET6, "ff03::1", &dst.sin6_addr);
	int ret = zsock_sendto(hb_sock, buf, n, 0,
			       (struct sockaddr *)&dst, sizeof(dst));
	LOG_INF("HB tx %s (%s)", buf, ret >= 0 ? "ok" : "fail");
}

/* ---------- debug/status shell: `ring ...` ---------- */

#if defined(CONFIG_SHELL)
/* The shell comes up with USB, tens of ms before network_mode/sync/groups
 * initialize their mutexes and state; commands touching them wait. */
static bool shell_core_ready(const struct shell *sh);

static const char *role_str(otDeviceRole r)
{
	switch (r) {
	case OT_DEVICE_ROLE_DISABLED: return "disabled";
	case OT_DEVICE_ROLE_DETACHED: return "detached";
	case OT_DEVICE_ROLE_CHILD:    return "child";
	case OT_DEVICE_ROLE_ROUTER:   return "router";
	case OT_DEVICE_ROLE_LEADER:   return "leader";
	default:                      return "?";
	}
}

static int cmd_ring_status(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	char addr[40] = "(none)";
	int8_t rssi = 0;
	struct openthread_context *ctx = openthread_get_default_context();

	openthread_api_mutex_lock(ctx);
	otDeviceRole role = otThreadGetDeviceRole(ctx->instance);
	const otIp6Address *mleid = otThreadGetMeshLocalEid(ctx->instance);

	if (mleid) {
		otIp6AddressToString(mleid, addr, sizeof(addr));
	}
	otThreadGetParentLastRssi(ctx->instance, &rssi);
	openthread_api_mutex_unlock(ctx);

	shell_print(sh, "name        %s", g_name);
	shell_print(sh, "uptime      %lld s", k_uptime_get() / 1000);
	shell_print(sh, "app state   %s", app_state_name(g_app_state));
	shell_print(sh, "thread      %s", role_str(role));
	shell_print(sh, "border      %s", g_infrastructure_ok ? "present" : "missing");
	shell_print(sh, "partition   %08x", g_partition_id);
	shell_print(sh, "last reset  %08x / %s", g_reset_cause,
		    reboot_reason_str(g_reboot_reason));
	shell_print(sh, "ml-eid      %s", addr);
	shell_print(sh, "parent rssi %d dBm", rssi);
	const char *pattern_name = "searching";
	/* snapshot: the main loop can set this to -1 between the check and
	 * the array index */
	int pattern_override = g_pattern_override;

	if (pattern_override >= 0) {
		pattern_name = genome_base_names[pattern_override];
	} else if (g_cur_pattern >= 0) {
		struct network_mode_value mode = network_mode_get();

		pattern_name = social_scene_family_name_for_mode(
			mode.mode != NETWORK_MODE_NORMAL, g_cur_social_tier,
			g_cur_social_family);
	}
	shell_print(sh, "pattern     %s%s", pattern_name,
		    pattern_override >= 0 ? " (forced)" : " (auto)");
	shell_print(sh, "social      %s / %u member%s",
		    social_tier_name(groups_tier()), groups_member_count(),
		    groups_member_count() == 1U ? "" : "s");
	struct network_mode_value mode = network_mode_get();

	shell_print(sh, "fleet mode  %s / priority %u",
		    network_mode_name(mode.mode), mode.priority);
	shell_print(sh, "brightness  %d%%", (int)(g_brightness * 100.f));
	shell_print(sh, "LED layout  %s (%d pixels)",
		    led_layout_name(g_led_layout), g_led_count);
	shell_print(sh, "LED power   %s", led_power_mode_name(led_power_mode_get()));
	shell_print(sh, "USB sleep   %s", g_usb_sleep.knocks > 0 ? "armed" : "idle");
	shell_print(sh, "frames      %u (~%u fps target)", g_frames, 1000 / FRAME_MS);
	shell_print(sh, "heartbeats  %u sent", g_hb_seq);
	return 0;
}

static int cmd_ring_pattern(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "usage: ring pattern <0-%d|auto>",
			    GENOME_N_BASES - 1);
		return -EINVAL;
	}
	if (strcmp(argv[1], "auto") == 0) {
		g_pattern_override = -1;
		shell_print(sh, "auto-cycling");
		return 0;
	}
	int idx = atoi(argv[1]);

	if (idx < 0 || idx >= GENOME_N_BASES) {
		return -EINVAL;
	}
	g_pattern_override = idx;
	shell_print(sh, "forced: %s", genome_base_names[idx]);
	return 0;
}

static int cmd_ring_bright(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "brightness %d%%", (int)(g_brightness * 100.f));
		return 0;
	}
	int pct = atoi(argv[1]);

	if (pct < 1 || pct > 100) {
		return -EINVAL;
	}
	g_brightness = pct / 100.f;
	shell_print(sh, "brightness set to %d%%", pct);
	return 0;
}

static int cmd_ring_layout(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "layout %s, %d pixels, brightness %d%%",
			    led_layout_name(g_led_layout), g_led_count,
			    (int)(g_brightness * 100.f));
		return 0;
	}
	enum led_layout layout;

	if (strcmp(argv[1], "ring16") == 0) {
		layout = LED_LAYOUT_RING16;
	} else if (strcmp(argv[1], "jewel7") == 0) {
		layout = LED_LAYOUT_JEWEL7;
	} else if (strcmp(argv[1], "rope100") == 0) {
		layout = LED_LAYOUT_ROPE100;
	} else {
		return -EINVAL;
	}
	atomic_set(&g_shell_layout_req, (atomic_val_t)layout);
	shell_print(sh, "layout %s queued (applied between frames)", argv[1]);
	return 0;
}

static int cmd_ring_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "power %s", led_power_mode_name(led_power_mode_get()));
		return 0;
	}
	enum led_power_mode mode;

	if (strcmp(argv[1], "carrier") == 0) {
		mode = LED_POWER_CARRIER;
	} else if (strcmp(argv[1], "module-vcc") == 0) {
		mode = LED_POWER_MODULE_VCC;
	} else {
		return -EINVAL;
	}
	atomic_set(&g_shell_power_req, (atomic_val_t)mode);
	shell_print(sh, "power %s queued (applied between frames)", argv[1]);
	return 0;
}

static bool shell_core_ready(const struct shell *sh)
{
	if (!g_core_ready) {
		shell_print(sh, "still booting — try again in a moment");
		return false;
	}
	return true;
}

static int cmd_ring_hb(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	atomic_set(&g_shell_hb_req, 1);
	shell_print(sh, "heartbeat queued (seq %u)", g_hb_seq);
	return 0;
}

static int cmd_ring_flash(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "off") == 0) {
		g_flash_period_ms = 0;
		shell_print(sh, "flash off");
		return 0;
	}
	int64_t p = atoi(argv[1]);

	g_flash_period_ms = (p >= 200) ? p : 2000;
	shell_print(sh, "flashing every %lld ms on the global clock",
		    g_flash_period_ms);
	return 0;
}

static int cmd_ring_scramble(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	if (argc >= 2) {
		sync_scramble_ms(atoi(argv[1]));
	}
	struct sync_stats st;

	sync_get_stats(&st);
	shell_print(sh, "theta now %lld ms", st.theta_us / 1000);
	return 0;
}

static int cmd_ring_skew(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	if (argc >= 2) {
		sync_skew_set(strcmp(argv[1], "off") == 0 ? 0.0
							  : (double)atoi(argv[1]));
	}
	shell_print(sh, "clock skew %+d ppm", (int)sync_skew_get());
	return 0;
}

static int cmd_ring_sync(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	struct sync_stats st;

	sync_get_stats(&st);
	shell_print(sh, "rate trim   %+d ppm", (int)st.rate_ppm);
	if (st.skew_ppm != 0.0) {
		shell_print(sh, "TEST SKEW   %+d ppm", (int)st.skew_ppm);
	}
	shell_print(sh, "theta       %lld ms (%lld us)",
		    st.theta_us / 1000, st.theta_us);
	shell_print(sh, "neighbours  %d fresh, %d locked", st.neighbors, st.locked);
	shell_print(sh, "schedule    %s", st.profile);
	if (st.clone_suspects > 0) {
		shell_print(sh, "CLONE SUSPECTS %u (another radio claims our node id)",
			    st.clone_suspects);
	}
	if (st.nearest_off_us >= 0) {
		shell_print(sh, "nearest off %lld us", st.nearest_off_us);
	}
	shell_print(sh, "global ms   %lld", sync_global_ms());
	return 0;
}

static int cmd_ring_gossip(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	if (argc >= 2) {
		sync_set_dest(strcmp(argv[1], "local") == 0 ? SYNC_DEST_LOCAL :
			      strcmp(argv[1], "realm") == 0 ? SYNC_DEST_REALM :
			      SYNC_DEST_BOTH);
	}
	shell_print(sh, "gossip dest = %s", sync_dest());
	return 0;
}

static int cmd_ring_group(const struct shell *sh, size_t argc, char **argv)
{
	if (!shell_core_ready(sh)) {
		return 0;
	}
	char buf[320];

	groups_status(buf, sizeof(buf));
	shell_print(sh, "%s", buf);
	return 0;
}

static int cmd_ring_dfu(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "rebooting into UF2 bootloader...");
	clean_reboot(REBOOT_REASON_UF2, true);
	return 0;
}

#if defined(CONFIG_THREAD_RING_CONTROL_PROVISIONING)
static int cmd_ring_auth_status(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "control auth %s, sequence %016llx",
		    control_auth_ready() ? "ready" : "unprovisioned",
		    (unsigned long long)control_auth_sequence());
	return 0;
}

static int cmd_ring_auth_key(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: ring auth-key <64-hex-device-key>");
		return -EINVAL;
	}
	int rc = control_auth_provision_hex(argv[1]);

	if (rc != 0) {
		shell_error(sh, "control key rejected: %d", rc);
		return rc;
	}
	shell_print(sh, "control key provisioned; sequence reset");
	return 0;
}
#endif

#if defined(CONFIG_THREAD_RING_HIL_FAULT_INJECTION)
static int hil_set_thread_enabled(bool enabled)
{
	struct openthread_context *ctx = openthread_get_default_context();

	if (!ctx) {
		return -ENODEV;
	}
	openthread_api_mutex_lock(ctx);
	otError error = otThreadSetEnabled(ctx->instance, enabled);
	openthread_api_mutex_unlock(ctx);
	return error == OT_ERROR_NONE ? 0 : -EIO;
}

static int cmd_ring_hil_partition(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: ring hil-partition <5-300|cancel>");
		return -EINVAL;
	}
	if (strcmp(argv[1], "cancel") == 0) {
		int rc = hil_set_thread_enabled(true);

		if (rc == 0) {
			g_hil_thread_disabled = false;
			g_hil_partition_until = 0;
			shell_print(sh, "HIL partition cancelled; Thread enabled");
		}
		return rc;
	}
	char *end = NULL;
	long seconds = strtol(argv[1], &end, 10);

	if (!end || *end != '\0' || seconds < 5 || seconds > 300) {
		shell_error(sh, "duration must be 5-300 seconds or cancel");
		return -EINVAL;
	}
	int rc = hil_set_thread_enabled(false);

	if (rc != 0) {
		shell_error(sh, "could not disable Thread: %d", rc);
		return rc;
	}
	g_hil_thread_disabled = true;
	g_hil_partition_until = k_uptime_get() + seconds * MSEC_PER_SEC;
	shell_print(sh, "HIL Thread partition active for %ld seconds", seconds);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(ring_cmds,
	SHELL_CMD(status, NULL, "uptime, thread role, addr, pattern, fps", cmd_ring_status),
	SHELL_CMD(pattern, NULL, "force pattern <0-3> or 'auto'", cmd_ring_pattern),
	SHELL_CMD(bright, NULL, "get/set brightness percent <1-100>", cmd_ring_bright),
	SHELL_CMD(layout, NULL, "get/set LED layout <ring16|jewel7|rope100>",
		  cmd_ring_layout),
	SHELL_CMD(power, NULL, "get/set LED power <carrier|module-vcc>", cmd_ring_power),
	SHELL_CMD(hb, NULL, "send a heartbeat now", cmd_ring_hb),
	SHELL_CMD(flash, NULL, "flash mode: <period_ms>|off — for camera timing", cmd_ring_flash),
	SHELL_CMD(scramble, NULL, "shove the logical clock by <ms> (sync test)", cmd_ring_scramble),
	SHELL_CMD(skew, NULL, "bias the hw clock rate by <ppm>|off (sync test)", cmd_ring_skew),
	SHELL_CMD(sync, NULL, "gossip clock: rate trim, theta, neighbours", cmd_ring_sync),
	SHELL_CMD(gossip, NULL, "beacon rendezvous: <local|realm|both>", cmd_ring_gossip),
	SHELL_CMD(group, NULL, "membership, agreed set, group pattern state", cmd_ring_group),
	SHELL_CMD(dfu, NULL, "reboot into UF2 bootloader", cmd_ring_dfu),
#if defined(CONFIG_THREAD_RING_CONTROL_PROVISIONING)
	SHELL_CMD(auth-status, NULL, "show control authentication state",
		  cmd_ring_auth_status),
	SHELL_CMD(auth-key, NULL, "install a 256-bit per-device control key",
		  cmd_ring_auth_key),
#endif
#if defined(CONFIG_THREAD_RING_HIL_FAULT_INJECTION)
	SHELL_CMD(hil-partition, NULL, "USB-only bounded Thread detach: <5-300|cancel>",
		  cmd_ring_hil_partition),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ring, &ring_cmds, "thread-led-node debug/status", NULL);
#endif /* CONFIG_SHELL */

int main(void)
{
	int64_t led_power_enable_at =
		k_uptime_get() + LED_POWER_STARTUP_DELAY_MS;

	/* First instruction of consequence: make sure a watchdog is counting
	 * before settings, LED prepare, USB, or anything else that can hang.
	 * After a watchdog reset the nRF WDT is disabled, and the old arm
	 * site sat four fallible init calls into main() — the exact window
	 * the drifting-owl-spark wedge died in (docs/boot-recovery-ladder.md).
	 * No-op whenever MCUboot's own arm or a prior warm boot's WDT
	 * survived; the Zephyr WDT block below then simply adopts channel 0. */
	escape_ladder_arm_wdt();
	uint32_t escape_attempts = escape_ladder_attempts(ESCAPE_LADDER_STATE);

#ifdef CONFIG_THREAD_RING_ESCAPE_HIL_BOOT_HANG
	/* Bench-gate fault injection: reproduce the unprotected-early-boot
	 * wedge with the ladder in place. Busy spin so the armed watchdog
	 * ends every attempt; the ladder must do the rest. */
	for (;;) {
	}
#endif

	read_boot_cause();
	if (g_reboot_reason == REBOOT_REASON_HEAL) {
		g_resume_fade = 0.f;   /* fade back up instead of snapping on */
		g_heal_resume = true;  /* ...and stay itself while re-attaching */
	}
	derive_name();
	LOG_INF("node name: %s (from FICR device id)", g_name);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device not ready");
		return 0;
	}
	/* Force both candidate rails inactive before settings, USB, Thread, or
	 * watchdog initialization. Startup enables only the selected rail after a
	 * deliberate delay; live backend changes continue to use led_power_start(). */
	int power_rc = led_power_prepare();

	if (power_rc != 0) {
		LOG_ERR("LED rail prepare failed: %d", power_rc);
		return 0;
	}
	int layout_rc = led_config_init();

	if (layout_rc != 0) {
		LOG_WRN("LED layout load failed (%d); using ring16", layout_rc);
	}
	use_led_layout(led_config_layout());
	int power_mode_rc = led_power_mode_init();

	if (power_mode_rc != 0) {
		LOG_WRN("LED power mode load failed (%d); using carrier", power_mode_rc);
	}
	int solo_rc = solo_mode_init();

	if (solo_rc != 0) {
		/* Falls back to fleet behaviour, which is the safer default:
		 * a node wrongly self-healing merely restarts, while one
		 * wrongly believing it is solo sits wedged forever. */
		LOG_WRN("solo mode load failed (%d); self-healing enabled",
			solo_rc);
	}
	int always_on_rc = always_on_init();

	if (always_on_rc != 0) {
		/* Falls back to the quiet-on-USB default. */
		LOG_WRN("always-on load failed (%d); USB quiet display enabled",
			always_on_rc);
	}
	(void)boot_status_init(g_reset_cause, g_reboot_reason,
			      g_mcuboot_phase, g_mcuboot_detail);
	boot_status_note_escape_attempts(escape_attempts);
	if (g_reset_cause & RESET_PIN) {
		/* Somebody just powered this on: a fresh deployment, not the
		 * continuation of a retry sequence. Persisting the counter means
		 * it would otherwise outlive the situation that earned it. */
		(void)boot_status_set_heal_attempts(0);
	}
	(void)control_auth_init();

	/* The hardware WDT is already counting (armed at main() entry, or
	 * inherited from MCUboot / the previous warm boot — the nRF WDT locks
	 * its config at first start and identical 15 s / channel-0 values are
	 * used everywhere). This block adopts it through the Zephyr driver so
	 * the supervision loop can feed it; install/setup on a running WDT
	 * allocates the same channel 0 and start is a harmless no-op. */
	const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
	int wdt_ch = -1;

	if (device_is_ready(wdt)) {
		struct wdt_timeout_cfg wcfg = {
			.window = { .min = 0, .max = 15000 },
			.flags = WDT_FLAG_RESET_SOC,
		};

		wdt_ch = wdt_install_timeout(wdt, &wcfg);
		if (wdt_ch >= 0 && wdt_setup(wdt, 0) == 0) {
			LOG_INF("hardware watchdog armed (15s)");
			(void)boot_status_record(BOOT_STAGE_WATCHDOG);
		} else {
			wdt_ch = -1;
			LOG_WRN("watchdog setup failed — running without");
		}
	}

	/* Deliberately enable USB from main instead of POST_KERNEL init.  This
	 * guarantees the scheduler, LED boot marker, and watchdog are alive before
	 * the nRF USBD driver attaches to a host after a warm reset. */
	sanitize_inherited_usbd();
	(void)boot_status_record(BOOT_STAGE_USB_QUIESCED);
	int usb_rc = usb_enable(NULL);

	if (usb_rc != 0) {
		LOG_ERR("USB enable failed: %d", usb_rc);
	} else {
		(void)boot_status_record(BOOT_STAGE_USB);
	}

	const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));

	if (device_is_ready(cdc)) {
		cdc_acm_dte_rate_callback_set(cdc, dte_rate_cb);
	}

	hb_init();

	uint32_t nid = hash32(NRF_FICR->DEVICEID[0] ^ hash32(NRF_FICR->DEVICEID[1]));
	uint32_t seed = hash32(nid ^ 0x5eedU);

	int mode_rc = network_mode_init(nid);

	if (mode_rc != 0) {
		LOG_WRN("fleet mode load failed (%d); using normal/0", mode_rc);
	}
	sync_init(nid, seed);
	groups_init(nid, seed);
	g_core_ready = true;
	social_scene_init(&g_solo, seed, SOCIAL_SOLO, 1U);
	struct social_scene_plan solo_plan;

	social_scene_plan_at(&g_solo, 0.f, &solo_plan);
	LOG_INF("solo scene: family=%s hue=%d period=%.1fs",
		social_scene_family_name(solo_plan.tier, solo_plan.family),
		(int)solo_plan.hue[0], (double)solo_plan.period_s);
	/* beacons TX through the bound heartbeat socket — the unbound gossip
	 * TX socket never got multicast onto wpan0 (heartbeats did) */
	sync_set_tx_sock(hb_sock);
	mgmt_callback_register(&dfu_mgmt_cb);

	/* Clone antennas need every dB and the radio resets to 0 dBm on every
	 * boot — a runtime `ot txpower 8` silently vanishes at the next reboot
	 * and turns links one-way (learned the hard way: the OTBR at +8 was
	 * heard fine while our 0 dBm replies never made it back).
	 */
	struct openthread_context *octx = openthread_get_default_context();

	if (octx) {
		openthread_api_mutex_lock(octx);
		otPlatRadioSetTransmitPower(octx->instance, 8);
		openthread_api_mutex_unlock(octx);
	}

	LOG_INF("thread-ring up: joining ring-net (ch15) as FTD");

	int last_logged = -1;
	bool was_attached = false;
	bool was_infrastructure = false;
	bool was_ota_quiet = false;
	int64_t next_hb = HEARTBEAT_MS;
	int64_t next_infra_check = 0;

	bool infrastructure_visible = false;
	bool dfu_power_held = false;
	bool ladder_cleared = false;
	struct ota_policy ota_machine;
	struct app_state_machine app_machine;

	bool usb_present_at_init = usb_vbus_present();

	battery_init();
	usb_display_init(&g_usb_display, k_uptime_get(), usb_present_at_init);
	usb_sleep_init(&g_usb_sleep, k_uptime_get(), usb_present_at_init);
	struct app_state_observation initial_observation = {
		.now_ms = 0,
		.recovery_boot = is_recovery_boot(),
		.attached = false,
		.dfu_active = false,
	};

	ota_policy_init(&ota_machine);
	ota_quiet_init(&g_ota_quiet);
	app_state_init(&app_machine, &initial_observation);
	g_app_state = app_machine.current;
	LOG_INF("application state: %s", app_state_name(g_app_state));

	int64_t startup_delay_remaining = led_power_enable_at - k_uptime_get();

	if (startup_delay_remaining > 0) {
		k_sleep(K_MSEC(startup_delay_remaining));
	}
	power_rc = led_power_enable();
	if (power_rc != 0) {
		LOG_ERR("LED rail enable failed: %d", power_rc);
		return 0;
	}
	show_led_startup_glow(wdt, wdt_ch);

	while (1) {
		int64_t now = k_uptime_get();

		if (wdt_ch >= 0) {
			wdt_feed(wdt, wdt_ch);
		}
		poll_commands();
		bool ota_quiet = ota_quiet_active(&g_ota_quiet, now);

		if (ota_quiet != was_ota_quiet) {
			LOG_INF("OTA quiet mode %s", ota_quiet ? "active" : "inactive");
			if (!ota_quiet) {
				next_hb = now;
			}
			was_ota_quiet = ota_quiet;
		}
		if (!ota_quiet) {
			lonely_leader_heal();
		}

#if defined(CONFIG_THREAD_RING_HIL_FAULT_INJECTION)
		if (g_hil_thread_disabled && now >= g_hil_partition_until) {
			if (hil_set_thread_enabled(true) == 0) {
				g_hil_thread_disabled = false;
				g_hil_partition_until = 0;
				LOG_INF("HIL partition ended; Thread re-enabled");
			}
		}
#endif

#if defined(CONFIG_THREAD_RING_FORCE_ROLLBACK_TEST)
		/* Phase 1 fault injection: prove that an intentionally unconfirmed
		 * test image returns to the prior confirmed image without human
		 * intervention. Normal builds compile this path out completely. */
		if (now > 8000 && !boot_is_img_confirmed()) {
			clean_reboot(REBOOT_REASON_ROLLBACK, false);
		}
#endif

#if defined(CONFIG_THREAD_RING_FORCE_NO_INFRA_TEST)
#define UNCONFIRMED_REBOOT_MS 30000
#else
#define UNCONFIRMED_REBOOT_MS 300000
#endif
		int64_t gms = global_ms();
		float t = (float)gms / 1000.f;
		otDeviceRole role = ot_role();
		bool attached = role >= OT_DEVICE_ROLE_CHILD;
		uint32_t dfu_age = (uint32_t)k_uptime_get() - g_dfu_last_chunk;
		bool dfu_active = dfu_age < DFU_POWER_HOLD_MS;

		if (dfu_active && !dfu_power_held) {
			LOG_INF("OTA transfer detected; holding LED rails off");
			led_power_stop();
			dfu_power_held = true;
		} else if (!dfu_active && dfu_power_held) {
			int rc = led_power_start();

			if (rc != 0) {
				LOG_ERR("LED rail restore after stalled OTA failed: %d", rc);
			} else {
				LOG_INF("OTA transfer idle; LED rail restored");
			}
			dfu_power_held = false;
		}

		/* apply queued shell requests on THIS thread, between frames */
		int shell_req = (int)atomic_set(&g_shell_layout_req, -1);

		if (shell_req >= 0) {
			int rc = change_led_layout((enum led_layout)shell_req);

			if (rc != 0) {
				LOG_ERR("shell layout change failed: %d", rc);
			}
		}
		shell_req = (int)atomic_set(&g_shell_power_req, -1);
		if (shell_req >= 0) {
			int rc = change_led_power_mode(
				(enum led_power_mode)shell_req);

			if (rc != 0) {
				LOG_ERR("shell power change failed: %d", rc);
			}
		}
		/* Provably recoverable: the USB control plane came up and the
		 * main loop has stayed alive for 30 s. Deliberately not gated
		 * on Thread — solitude is normal, wedge is not. A node that
		 * cannot even reach this bar keeps its failed-attempt count
		 * and earns the DFU escape instead of retrying forever. */
		if (!ladder_cleared && usb_rc == 0 && now >= 30000) {
			escape_ladder_clear(ESCAPE_LADDER_STATE);
			ladder_cleared = true;
		}
		bool usb_present = usb_vbus_present();
		bool usb_changed = usb_display_update(&g_usb_display, now, usb_present);
		enum usb_sleep_event sleep_event =
			usb_sleep_update(&g_usb_sleep, now, usb_present);

		if (usb_changed) {
			g_usb_quiet_visual_start = -1;
			next_hb = now;
			LOG_INF("USB power %s; display %s",
				 g_usb_display.usb_present ? "present" : "absent",
				 usb_quiet_display_active() ? "quiet" : "active");
		}
		if (sleep_event == USB_SLEEP_ARMED) {
			g_usb_sleep_visual_start = now;
			next_hb = now;
			LOG_INF("USB sleep gesture armed for %d ms",
				USB_SLEEP_SEQUENCE_MS);
		} else if (sleep_event == USB_SLEEP_CANCELLED) {
			g_usb_sleep_visual_start = -1;
			next_hb = now;
			LOG_INF("USB sleep gesture cancelled");
		} else if (sleep_event == USB_SLEEP_ENTER) {
			g_usb_sleep_visual_start = -1;
			if (!dfu_active && boot_is_img_confirmed()) {
				enter_system_off();
			} else {
				LOG_WRN("USB sleep gesture ignored during update safety window");
				usb_sleep_init(&g_usb_sleep, now, usb_present);
			}
		}

		if (now >= next_infra_check) {
			next_infra_check = now + 1000;
			infrastructure_visible = ot_has_infrastructure();

#if defined(CONFIG_THREAD_RING_FORCE_NO_INFRA_TEST)
			/* Exercise the real confirmation gate while leaving Thread and
			 * UDP operational for observation from the Pi. */
			infrastructure_visible = false;
#endif
		}

		struct ota_policy_observation ota_observation = {
			.now_ms = now,
			.rollback_timeout_ms = UNCONFIRMED_REBOOT_MS,
			.attached = attached,
			.infrastructure_visible = infrastructure_visible,
			.image_confirmed = boot_is_img_confirmed(),
		};
		struct ota_policy_decision ota_decision =
			ota_policy_update(&ota_machine, &ota_observation);

		g_infrastructure_ok = ota_decision.infrastructure_ok;
		/* An image still UNCONFIRMED after the deadline is unsafe even if
		 * infrastructure was briefly visible. Reboot so MCUboot reverts it. */
		if (ota_decision.rollback_due) {
			LOG_ERR("image still unconfirmed after %lld ms "
				"(infrastructure seen=%d) — rebooting to revert",
				(long long)UNCONFIRMED_REBOOT_MS,
				ota_machine.ever_infrastructure ? 1 : 0);
			clean_reboot(REBOOT_REASON_ROLLBACK, false);
		}
		if (ota_decision.record_attached) {
			(void)boot_status_record(BOOT_STAGE_ATTACHED);
		}
		if (ota_decision.record_infrastructure) {
			(void)boot_status_record(BOOT_STAGE_INFRASTRUCTURE);
		}
		if (ota_decision.confirmation_due) {
			/* A private ring-only partition is not sufficient proof that
			 * the image is remotely recoverable. Confirm only after OTBR
			 * Network Data is visible. */
#if defined(CONFIG_THREAD_RING_FORCE_ROLLBACK_TEST)
			/* Deliberately leave the image in test state. */
#else
			int rc = boot_write_img_confirmed();

			LOG_INF("OTA image confirm after infrastructure (rc %d)", rc);
			if (rc == 0) {
				(void)boot_status_record(BOOT_STAGE_CONFIRMED);
			}
#endif
		}
		if (ota_decision.record_confirmed) {
			(void)boot_status_record(BOOT_STAGE_CONFIRMED);
		}
		if (g_infrastructure_ok && !was_infrastructure) {
			LOG_INF("border-router data present, partition %08x",
				g_partition_id);
		}
		if (!g_infrastructure_ok && was_infrastructure) {
			LOG_WRN("border-router data disappeared from partition %08x",
				g_partition_id);
		}
		was_infrastructure = g_infrastructure_ok;

		struct app_state_observation observation = {
			.now_ms = now,
			.recovery_boot = is_recovery_boot(),
			.attached = attached,
			.dfu_active = dfu_active,
		};
		struct app_state_transition transition =
			app_state_update(&app_machine, &observation);

		g_app_state = transition.current;
		if (transition.changed) {
			LOG_INF("application state: %s -> %s",
				app_state_name(transition.previous),
				app_state_name(transition.current));
		}

		/* DFU and OTA quiet mode mute our own beacon TX. We keep receiving
		 * the others' beacons, so the clock stays disciplined. */
		sync_tick(attached && !dfu_active && !ota_quiet);
		network_mode_tick(attached && !dfu_active && !ota_quiet);

		if (g_app_state == APP_STATE_BOOTING ||
		    g_app_state == APP_STATE_RECOVERY) {
			(void)render_boot_status(now);
			continue;
		}

		/* snapshot once: the shell thread can zero this between a
		 * check and the modulo, and a 64-bit divide by zero is UB */
		int64_t flash_period = g_flash_period_ms;

		if (flash_period > 0) {
			/* camera-timing mode: hard white flash for the first
			 * 100 ms of every global-clock period
			 */
			bool on = (gms % flash_period) < 100;

			for (int k = 0; k < g_led_count; k++) {
				uint8_t v = on ? (uint8_t)(255.f * g_brightness) : 0;

				pixels[k] = (struct led_rgb){ .r = v, .g = v, .b = v };
			}
			g_frames++;
			led_strip_update_rgb(strip, pixels, g_led_count);
			k_sleep(K_MSEC(5));   /* tighter loop for sharp edges */
			continue;
		}

		if (attached != was_attached) {
			was_attached = attached;
			LOG_INF("thread %s (role %d)",
				attached ? "ATTACHED" : "detached", role);
			if (attached) {
				(void)boot_status_record(BOOT_STAGE_ATTACHED);
				if (!ota_quiet) {
					send_heartbeat();  /* announce immediately */
					next_hb = k_uptime_get() + HEARTBEAT_MS;
				}
			}
		}

		bool hb_requested = atomic_set(&g_shell_hb_req, 0) != 0;

		if (attached && !dfu_active && !ota_quiet &&
		    (hb_requested || k_uptime_get() >= next_hb)) {
			next_hb = k_uptime_get() + HEARTBEAT_MS;
			send_heartbeat();
		}

		if (g_app_state == APP_STATE_DFU) {
			/* Intentionally dark: the LED rail remains physically off
			 * throughout upload verification and the restart window. */
			k_sleep(K_MSEC(100));
			continue;
		}

		if (render_usb_sleep_armed(now)) {
			continue;
		}

		struct network_mode_value network_mode = network_mode_get();
		bool ultra = network_mode.mode == NETWORK_MODE_ULTRA;
		bool party = network_mode.mode == NETWORK_MODE_PARTY || ultra;
		/* blink 0 = no effect in any mode; otherwise the effect
		 * (hard blink or fade breathe) applies to whatever mode is
		 * active, synchronized on the shared clock */
		float effect_s = network_mode.ultra_blink_cs == 0U
			? 0.f
			: network_mode_blink_period_s(
				  network_mode.ultra_blink_cs);
		float duty =
			network_mode_duty_fraction(network_mode.ultra_duty_pct);
		bool fade = network_mode.wave == NETWORK_MODE_WAVE_FADE;

		float accent =
			network_mode_accent_fraction(network_mode.accent_pct);

		social_scene_set_party(&g_solo, party);
		social_scene_set_ultra(&g_solo, ultra);
		social_scene_set_accent(&g_solo, accent);
		social_scene_set_effect(&g_solo, effect_s, duty, fade);
		groups_set_party(party);
		groups_set_ultra(ultra);
		groups_set_accent(accent);
		groups_set_effect(effect_s, duty, fade);
		/* Charge-quiet is a display policy, not a network pause. Keep the
		 * group control plane alive so a charging node can resolve small-ID
		 * collisions and withdraw stale topology instead of continuing to
		 * advertise a frozen identity that blocks the visible fleet. */
		groups_tick();   /* rate-limited to ~1 Hz internally */

		if (usb_quiet_display_active()) {
			render_usb_quiet(now);
			continue;
		}

		/* attach/detach crossfade: amber "searching" breathes in and
		 * out over ~2 s instead of hard-cutting to the composite */
		static float attach_fade = -1.f;   /* 0 = searching, 1 = composite */
		/* Frame deadline in uptime ms; seeded on the first pass. */
		static int64_t next_frame_ms;

		if (next_frame_ms == 0) {
			next_frame_ms = k_uptime_get();
		}

		if (attach_fade < 0.f) {
			/* A rejoin restart resumes straight into its own pattern:
			 * showing "searching" would announce the very reboot the
			 * fade was hiding. */
			attach_fade = g_heal_resume ? 1.f : 0.f;
		}
		if (g_resume_fade < 1.f) {
			g_resume_fade += (float)FRAME_MS / 800.f;
			if (g_resume_fade > 1.f) {
				g_resume_fade = 1.f;
			}
		}
		if (g_heal_resume && attached) {
			/* Back on the mesh: ordinary crossfade behaviour resumes,
			 * including showing "searching" on any future detach. */
			g_heal_resume = false;
		}

		attach_fade += ((attached || g_heal_resume) ? 1.f : -1.f) *
			       (float)FRAME_MS / 2000.f;
		attach_fade = attach_fade < 0.f ? 0.f
			      : (attach_fade > 1.f ? 1.f : attach_fade);

		struct groups_render gr;

		groups_get_render(&gr);
		const struct social_scene *active_scene =
			groups_lit() ? &gr.group_scene : &g_solo;
		struct social_scene_plan active_plan;

		social_scene_plan_at(active_scene, t, &active_plan);
		g_cur_social_tier = active_plan.tier;
		g_cur_social_family = active_plan.family;
		g_cur_pattern = g_pattern_override >= 0
					? g_pattern_override
					: social_scene_pattern_id(active_scene, t);
		if (g_cur_pattern != last_logged) {
			last_logged = g_cur_pattern;
			if (g_pattern_override >= 0) {
				LOG_INF("manual pattern: %s",
					genome_base_names[g_cur_pattern]);
			} else {
				LOG_INF("social scene: tier=%s size=%u family=%s",
					social_tier_name(active_plan.tier),
					active_plan.member_count,
					social_scene_family_name_for_mode(
						active_plan.party, active_plan.tier,
						active_plan.family));
			}
		}

		for (int k = 0; k < g_led_count; k++) {
			struct frgb c;

			if (g_pattern_override >= 0) {
				/* demo: force a bare base family, full weight */
				c = genome_render_base(g_pattern_override, t, k,
						       g_led_count);
			} else {
				/* Composite: one-color solo beneath the
				 * crossfading, phase-locked social group scene. */
				c = social_scene_render(&g_solo, t, k,
						g_led_count);
				float approach =
					1.f + 0.08f * gr.approach_w *
						      (0.5f +
						       0.5f * sinf(
							       2.f * PI_F * t /
							       3.f));

				c.r *= gr.solo_w;
				c.g *= gr.solo_w;
				c.b *= gr.solo_w;
				c.r *= approach;
				c.g *= approach;
				c.b *= approach;
				if (gr.old_w > 0.f) {
					struct frgb o = social_scene_render(
						&gr.old_scene, t, k,
						g_led_count);

					c.r += o.r * gr.old_w;
					c.g += o.g * gr.old_w;
					c.b += o.b * gr.old_w;
				}
				if (gr.group_w > 0.f) {
					struct frgb o = social_scene_render(
						&gr.group_scene, t, k,
						g_led_count);

					c.r += o.r * gr.group_w;
					c.g += o.g * gr.group_w;
					c.b += o.b * gr.group_w;
				}
			}

			if (attach_fade < 1.f) {
				struct frgb s = searching_frgb(t);
				float af = attach_fade;

				c.r = c.r * af + s.r * (1.f - af);
				c.g = c.g * af + s.g * (1.f - af);
				c.b = c.b * af + s.b * (1.f - af);
			}

			/* Crossfading independently vivid scenes can still add a
			 * shared white component. Strip that pastel lift after the
			 * complete social composite, including attach transitions. */
			if (g_pattern_override < 0) {
				c = social_scene_vivid(c, active_plan.hue[0]);
			}

			/* additive layers overflow: scale proportionally —
			 * per-channel clipping would wash hues to white */
			float m = c.r > c.g ? c.r : c.g;

			m = m > c.b ? m : c.b;
			if (m > 1.f) {
				c.r /= m; c.g /= m; c.b /= m;
			}
			float out = 255.f * g_brightness * g_resume_fade;

			pixels[k] = (struct led_rgb){
				.r = (uint8_t)(c.r * out),
				.g = (uint8_t)(c.g * out),
				.b = (uint8_t)(c.b * out),
			};
		}
		if (!attached) {
			g_cur_pattern = -1;
			last_logged = -1;
		}
		/* Frame power cap. Current tracks the SUM of every channel on
		 * the strip, so a scene that lights all of the ring at once is
		 * the worst case regardless of how bright any single pixel
		 * looks. Scaling the finished frame by one factor keeps every
		 * pixel's ratios — hue and relative structure survive, only the
		 * level moves — which is why this sits here rather than inside
		 * the scene: by this point additive layers, the vivid guard and
		 * global brightness have all been applied, so this is the load
		 * the rail will actually see. */
		{
			uint32_t load = 0;

			for (int k = 0; k < g_led_count; k++) {
				load += (uint32_t)pixels[k].r +
					(uint32_t)pixels[k].g +
					(uint32_t)pixels[k].b;
			}
			uint32_t target = led_budget_scale_q8(
				load,
				led_budget_total(g_led_count,
						 g_led_budget_per_led));
			/* Slewed, not applied raw: an animating scene whose load
			 * straddles the ceiling would otherwise alternate between
			 * scaled and unscaled frames, which reads as stepping. */
			static uint32_t applied = 256U;

			applied = led_budget_slew_q8(applied, target,
						     LED_BUDGET_RELEASE_STEP_Q8);
			uint32_t scale = applied;

			if (scale < 256U) {
				for (int k = 0; k < g_led_count; k++) {
					pixels[k].r = (uint8_t)
						((pixels[k].r * scale) >> 8);
					pixels[k].g = (uint8_t)
						((pixels[k].g * scale) >> 8);
					pixels[k].b = (uint8_t)
						((pixels[k].b * scale) >> 8);
				}
			}
		}
		g_frames++;
		led_strip_update_rgb(strip, pixels, g_led_count);
		/* Sleep to the next frame BOUNDARY, not a fixed FRAME_MS after
		 * the work. A fixed trailing sleep makes the period
		 * (render + FRAME_MS), so the ring never reaches 1000/FRAME_MS
		 * and — worse — runs at a rate that varies with scene cost: the
		 * heaviest family rendered visibly slower than the lightest.
		 * Measured 24.5fps against a 30fps target, i.e. ~7.8ms of
		 * render being added to every frame.
		 *
		 * If a frame overruns its slot we resync to now rather than
		 * trying to catch up, so a slow frame cannot cascade into a
		 * burst of zero-length sleeps that starves the other threads. */
		next_frame_ms += FRAME_MS;
		int64_t frame_delay = next_frame_ms - k_uptime_get();

		if (frame_delay > 0) {
			k_sleep(K_MSEC(frame_delay));
		} else {
			next_frame_ms = k_uptime_get();
			k_yield();
		}
	}
	return 0;
}
