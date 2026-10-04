/* ring-test — stage 1 of the TIME-SYNC.md bring-up ladder.
 *
 * 16x WS2812B on P0.06 rendering genome pattern families as pure functions
 * of a free-running clock. No radio yet: this validates wiring, the LED
 * driver, and the pattern math. Auto-cycles families every 20 s.
 *
 * USB CDC console doubles as the DFU door: opening the port at 1200 baud
 * reboots into the UF2 bootloader (Adafruit convention), so flash.sh can
 * reprogram the board without anyone touching the reset button.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/logging/log.h>
#include <math.h>

#define PI_F 3.14159265f
#include <nrfx.h>

LOG_MODULE_REGISTER(ring_test, LOG_LEVEL_INF);

#define RING 16
#define FRAME_MS 33            /* ~30 fps */
#define BRIGHTNESS 0.45f       /* bench cap: kind to USB ports */

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));
static const struct gpio_dt_spec led_power =
	GPIO_DT_SPEC_GET(DT_ALIAS(led_power), gpios);
static struct led_rgb pixels[RING];

#define LED_POWER_SETTLE_MS 10

static int led_power_start(void)
{
	if (!gpio_is_ready_dt(&led_power)) {
		return -ENODEV;
	}
	int rc = gpio_pin_configure_dt(&led_power, GPIO_OUTPUT_INACTIVE);

	if (rc != 0) {
		return rc;
	}
	rc = gpio_pin_set_dt(&led_power, 1);
	if (rc == 0) {
		k_sleep(K_MSEC(LED_POWER_SETTLE_MS));
	}
	return rc;
}

/* ---------- helpers (ports of the artifact's JS) ---------- */

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static float fracf(float x) { return x - floorf(x); }

static float sstep(float x)
{
	x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
	return x * x * (3.f - 2.f * x);
}

static float ring_dist(float a, float b)
{
	float d = fabsf(fmodf(a - b, RING));
	if (d > RING / 2.f) {
		d = RING - d;
	}
	return d;
}

static struct led_rgb hsl(float h, float s, float l)
{
	h = fmodf(fmodf(h, 360.f) + 360.f, 360.f) / 360.f;
	float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
	float p = 2.f * l - q;
	float rgb[3] = { h + 1.f / 3.f, h, h - 1.f / 3.f };

	for (int i = 0; i < 3; i++) {
		float t = fmodf(fmodf(rgb[i], 1.f) + 1.f, 1.f);
		float v;

		if (t < 1.f / 6.f) {
			v = p + (q - p) * 6.f * t;
		} else if (t < 1.f / 2.f) {
			v = q;
		} else if (t < 2.f / 3.f) {
			v = p + (q - p) * (2.f / 3.f - t) * 6.f;
		} else {
			v = p;
		}
		rgb[i] = v * 255.f * BRIGHTNESS;
	}
	return (struct led_rgb){ .r = (uint8_t)rgb[0],
				 .g = (uint8_t)rgb[1],
				 .b = (uint8_t)rgb[2] };
}

/* ---------- pattern families: pure fns of (t seconds, led index) ---------- */

static struct led_rgb fam_jelly(float t, int k)
{
	float ph = fracf(t / 5.8f);
	float a = ph < 0.42f ? sstep(ph / 0.42f) : 1.f - sstep((ph - 0.42f) / 0.58f);
	float L = 0.055f + 0.32f * a +
		  0.012f * sinf((float)k / RING * 2.f * PI_F * 2.f + t) * a;
	return hsl(256.f - 16.f * a, 0.8f, L);
}

static struct led_rgb fam_lighthouse(float t, int k)
{
	float pos = fracf(t / 22.f) * RING;
	float d = ring_dist(k, pos);
	float g = expf(-(d * d) / 7.f);
	float L = 0.065f + 0.30f * g * (0.8f + 0.2f * sinf(2.f * PI_F * t / 5.5f));
	return hsl(228.f - 36.f * g, 0.8f - 0.35f * g, L);
}

static struct led_rgb fam_plankton(float t, int k)
{
	float u = t / 2.1f + k * 0.37f;
	int32_t slot = (int32_t)floorf(u);
	float fu = u - slot;
	uint32_t h = hash32((uint32_t)slot * 2654435761U ^ (uint32_t)k * 40503U ^ 0xBEEF);

	if (h % 9 == 0) {
		float tw = sinf(PI_F * fu);
		tw = tw * tw * (0.55f + (float)((h >> 4) & 3) / 6.7f);
		return hsl(186.f, 0.55f, 0.06f + 0.42f * tw);
	}
	return hsl(228.f, 0.85f, 0.06f);
}

static struct led_rgb fam_lotus(float t, int k)
{
	float e = 0.5f + 0.45f * sinf(2.f * PI_F * t / 5.9f);
	float swell = 0.9f + 0.1f * sinf(2.f * PI_F * t / 17.f);

	if (k % 2 == 0) {
		return hsl(316.f, 0.7f, (0.05f + 0.27f * e) * swell);
	}
	return hsl(224.f, 0.8f, (0.05f + 0.27f * (1.f - e)) * swell);
}

typedef struct led_rgb (*pattern_fn)(float t, int k);
static const pattern_fn patterns[] = {
	fam_jelly, fam_lighthouse, fam_plankton, fam_lotus,
};
static const char *const pattern_names[] = {
	"jelly-breath", "lighthouse-glass", "plankton", "lotus-breath",
};

/* ---------- 1200-baud touch -> UF2 bootloader ---------- */

static void dte_rate_cb(const struct device *dev, uint32_t rate)
{
	if (rate == 1200) {
		(void)gpio_pin_set_dt(&led_power, 0);
		NRF_POWER->GPREGRET = 0x57;  /* DFU_MAGIC_UF2_RESET */
		NVIC_SystemReset();
	}
}

int main(void)
{
	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device not ready");
		return 0;
	}
	int power_rc = led_power_start();

	if (power_rc != 0) {
		LOG_ERR("LED rail enable failed: %d", power_rc);
		return 0;
	}

	const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));

	if (device_is_ready(cdc)) {
		cdc_acm_dte_rate_callback_set(cdc, dte_rate_cb);
	}

	LOG_INF("ring-test up: %d LEDs, %d patterns, auto-cycling every 20 s",
		RING, (int)ARRAY_SIZE(patterns));

	int last_logged = -1;

	while (1) {
		float t = (float)k_uptime_get() / 1000.f;
		int idx = ((int)(t / 20.f)) % (int)ARRAY_SIZE(patterns);

		if (idx != last_logged) {
			last_logged = idx;
			LOG_INF("pattern: %s", pattern_names[idx]);
		}
		for (int k = 0; k < RING; k++) {
			pixels[k] = patterns[idx](t, k);
		}
		led_strip_update_rgb(strip, pixels, RING);
		k_sleep(K_MSEC(FRAME_MS));
	}
	return 0;
}
