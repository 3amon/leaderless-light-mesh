/* Erase the settings partition, then return to the UF2 bootloader.
 * Success signal: the NICENANO drive reappears within seconds. */
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include <nrfx.h>

#define STORAGE_OFFSET 0xec000
#define STORAGE_LENGTH 0x8000

int main(void)
{
	const struct device *flash =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

	if (device_is_ready(flash)) {
		(void)flash_erase(flash, STORAGE_OFFSET, STORAGE_LENGTH);
	}
	k_sleep(K_MSEC(500));
	NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
