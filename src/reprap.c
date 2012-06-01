/*
	simduino.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <pthread.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "sim_elf.h"
#include "sim_hex.h"
#include "sim_gdb.h"

#include "reprap_gl.h"

#include "button.h"
#include "reprap.h"

#define __AVR_ATmega644__
#include "marlin/pins.h"

#include <stdbool.h>
#define PROGMEM
#include "marlin/Configuration.h"

/*
 * these are the sources of heat and cold to register to the heatpots
 */
enum {
	TALLY_HOTEND_PWM	= 1,
	TALLY_HOTBED,
	TALLY_HOTEND_FAN,
};

reprap_t reprap;

avr_t * avr = NULL;

typedef struct ardupin_t {
	uint32_t port : 7, pin : 3, analog : 1, adc : 3, pwm : 1, ardupin;
} ardupin_t, *ardupin_p;

ardupin_t arduidiot_644[32] = {
	[ 0] = { .ardupin =  0, .port = 'B', .pin =  0 },
	[ 1] = { .ardupin =  1, .port = 'B', .pin =  1 },
	[ 2] = { .ardupin =  2, .port = 'B', .pin =  2 },
	[ 3] = { .ardupin =  3, .port = 'B', .pin =  3 },
	[ 4] = { .ardupin =  4, .port = 'B', .pin =  4 },
	[ 5] = { .ardupin =  5, .port = 'B', .pin =  5 },
	[ 6] = { .ardupin =  6, .port = 'B', .pin =  6 },
	[ 7] = { .ardupin =  7, .port = 'B', .pin =  7 },

	[ 8] = { .ardupin =  8, .port = 'D', .pin =  0 },
	[ 9] = { .ardupin =  9, .port = 'D', .pin =  1 },
	[10] = { .ardupin = 10, .port = 'D', .pin =  2 },
	[11] = { .ardupin = 11, .port = 'D', .pin =  3 },
	[12] = { .ardupin = 12, .port = 'D', .pin =  4 },
	[13] = { .ardupin = 13, .port = 'D', .pin =  5 },
	[14] = { .ardupin = 14, .port = 'D', .pin =  6 },
	[15] = { .ardupin = 15, .port = 'D', .pin =  7 },

	[16] = { .ardupin = 16, .port = 'C', .pin =  0 },
	[17] = { .ardupin = 17, .port = 'C', .pin =  1 },
	[18] = { .ardupin = 18, .port = 'C', .pin =  2 },
	[19] = { .ardupin = 19, .port = 'C', .pin =  3 },
	[20] = { .ardupin = 20, .port = 'C', .pin =  4 },
	[21] = { .ardupin = 21, .port = 'C', .pin =  5 },
	[22] = { .ardupin = 22, .port = 'C', .pin =  6 },
	[23] = { .ardupin = 23, .port = 'C', .pin =  7 },

	[24] = { .ardupin = 24, .port = 'A', .pin =  7, .analog = 1, .adc = 7 },
	[25] = { .ardupin = 25, .port = 'A', .pin =  6, .analog = 1, .adc = 6 },
	[26] = { .ardupin = 26, .port = 'A', .pin =  5, .analog = 1, .adc = 5 },
	[27] = { .ardupin = 27, .port = 'A', .pin =  4, .analog = 1, .adc = 4 },
	[28] = { .ardupin = 28, .port = 'A', .pin =  3, .analog = 1, .adc = 3 },
	[29] = { .ardupin = 29, .port = 'A', .pin =  2, .analog = 1, .adc = 2 },
	[30] = { .ardupin = 30, .port = 'A', .pin =  1, .analog = 1, .adc = 1 },
	[31] = { .ardupin = 31, .port = 'A', .pin =  0, .analog = 1, .adc = 0 },
};

// gnu hackery to make sure the parameter is expanded
#define _TERMISTOR_TABLE(num) \
		temptable_##num
#define TERMISTOR_TABLE(num) \
		_TERMISTOR_TABLE(num)

struct avr_irq_t *
get_ardu_irq(
		struct avr_t * avr,
		int ardupin,
		ardupin_t pins[])
{
	if (pins[ardupin].ardupin != ardupin) {
		printf("%s pin %d isn't correct in table\n", __func__, ardupin);
		return NULL;
	}
	struct avr_irq_t * irq = avr_io_getirq(avr,
			AVR_IOCTL_IOPORT_GETIRQ(pins[ardupin].port), pins[ardupin].pin);
	if (!irq) {
		printf("%s pin %d PORT%C%d not found\n", __func__, ardupin, pins[ardupin].port, pins[ardupin].pin);
		return NULL;
	}
	return irq;
}

/*
 * called when the AVR change any of the pins on port B
 * so lets update our buffer
 */
void
hotbed_change_hook(
		struct avr_irq_t * irq,
		uint32_t value,
		void * param)
{
	printf("%s %d\n", __func__, value);
//	pin_state = (pin_state & ~(1 << irq->irq)) | (value << irq->irq);
}



char avr_flash_path[1024];
int avr_flash_fd = 0;

// avr special flash initalization
// here: open and map a file to enable a persistent storage for the flash memory
void avr_special_init( avr_t * avr)
{
	// open the file
	avr_flash_fd = open(avr_flash_path, O_RDWR|O_CREAT, 0644);
	if (avr_flash_fd < 0) {
		perror(avr_flash_path);
		exit(1);
	}
	// resize and map the file the file
	(void)ftruncate(avr_flash_fd, avr->flashend + 1);
	ssize_t r = read(avr_flash_fd, avr->flash, avr->flashend + 1);
	if (r != avr->flashend + 1) {
		fprintf(stderr, "unable to load flash memory\n");
		perror(avr_flash_path);
		exit(1);
	}
}

// avr special flash deinitalization
// here: cleanup the persistent storage
void avr_special_deinit( avr_t* avr)
{
	puts(__func__);
	lseek(avr_flash_fd, SEEK_SET, 0);
	ssize_t r = write(avr_flash_fd, avr->flash, avr->flashend + 1);
	if (r != avr->flashend + 1) {
		fprintf(stderr, "unable to load flash memory\n");
		perror(avr_flash_path);
	}
	close(avr_flash_fd);
	uart_pty_stop(&reprap.uart_pty);
}

#define MEGA644_GPIOR0 0x3e

static void
reprap_relief_callback(
		struct avr_t * avr,
		avr_io_addr_t addr,
		uint8_t v,
		void * param)
{
//	printf("%s write %x\n", __func__, addr);
	static uint16_t tick = 0;
	if (!(tick++ & 0xf))
		usleep(100);
}

static void *
avr_run_thread(
		void * ignore)
{
	while (1) {
		avr_run(avr);
	}
	return NULL;
}

void
reprap_init(
		avr_t * avr,
		reprap_p r)
{
	r->avr = avr;
	uart_pty_init(avr, &r->uart_pty);
	uart_pty_connect(&r->uart_pty, '0');

	thermistor_init(avr, &r->therm_hotend, 0,
			(short*)TERMISTOR_TABLE(TEMP_SENSOR_0),
			sizeof(TERMISTOR_TABLE(TEMP_SENSOR_0)) / sizeof(short) / 2,
			OVERSAMPLENR, 25.0f);
	thermistor_init(avr, &r->therm_hotbed, 2,
			(short*)TERMISTOR_TABLE(TEMP_SENSOR_BED),
			sizeof(TERMISTOR_TABLE(TEMP_SENSOR_BED)) / sizeof(short) / 2,
			OVERSAMPLENR, 30.0f);
	thermistor_init(avr, &r->therm_spare, 1,
			(short*)temptable_5, sizeof(temptable_5) / sizeof(short) / 2,
			OVERSAMPLENR, 10.0f);

	heatpot_init(avr, &r->hotend, "hotend", 28.0f);
	heatpot_init(avr, &r->hotbed, "hotbed", 25.0f);

	/* connect heatpot temp output to thermistors */
	avr_connect_irq(r->hotend.irq + IRQ_HEATPOT_TEMP_OUT,
			r->therm_hotend.irq + IRQ_TERM_TEMP_VALUE_IN);
	avr_connect_irq(r->hotbed.irq + IRQ_HEATPOT_TEMP_OUT,
			r->therm_hotbed.irq + IRQ_TERM_TEMP_VALUE_IN);

	float axis_pp_per_mm[4] = DEFAULT_AXIS_STEPS_PER_UNIT;	// from Marlin!
	{
		avr_irq_t * e = get_ardu_irq(avr, X_ENABLE_PIN, arduidiot_644);
		avr_irq_t * s = get_ardu_irq(avr, X_STEP_PIN, arduidiot_644);
		avr_irq_t * d = get_ardu_irq(avr, X_DIR_PIN, arduidiot_644);
		avr_irq_t * m = get_ardu_irq(avr, X_MIN_PIN, arduidiot_644);

		stepper_init(avr, &r->step_x, "X", axis_pp_per_mm[0], 100, 220, 0);
		stepper_connect(&r->step_x, s, d, e, m, stepper_endstop_inverted);
	}
	{
		avr_irq_t * e = get_ardu_irq(avr, Y_ENABLE_PIN, arduidiot_644);
		avr_irq_t * s = get_ardu_irq(avr, Y_STEP_PIN, arduidiot_644);
		avr_irq_t * d = get_ardu_irq(avr, Y_DIR_PIN, arduidiot_644);
		avr_irq_t * m = get_ardu_irq(avr, Y_MIN_PIN, arduidiot_644);

		stepper_init(avr, &r->step_y, "Y", axis_pp_per_mm[1], 100, 220, 0);
		stepper_connect(&r->step_y, s, d, e, m, stepper_endstop_inverted);
	}
	{
		avr_irq_t * e = get_ardu_irq(avr, Z_ENABLE_PIN, arduidiot_644);
		avr_irq_t * s = get_ardu_irq(avr, Z_STEP_PIN, arduidiot_644);
		avr_irq_t * d = get_ardu_irq(avr, Z_DIR_PIN, arduidiot_644);
		avr_irq_t * m = get_ardu_irq(avr, Z_MIN_PIN, arduidiot_644);

		stepper_init(avr, &r->step_z, "Z", axis_pp_per_mm[2], 20, 110, 0);
		stepper_connect(&r->step_z, s, d, e, m, stepper_endstop_inverted);
	}
	{
		avr_irq_t * e = get_ardu_irq(avr, E0_ENABLE_PIN, arduidiot_644);
		avr_irq_t * s = get_ardu_irq(avr, E0_STEP_PIN, arduidiot_644);
		avr_irq_t * d = get_ardu_irq(avr, E0_DIR_PIN, arduidiot_644);

		stepper_init(avr, &r->step_e, "E", axis_pp_per_mm[3], 0, 0, 0);
		stepper_connect(&r->step_e, s, d, e, NULL, 0);
	}

}

int main(int argc, char *argv[])
{
	char path[256];
	strcpy(path, argv[0]);
	strcpy(path, dirname(path));
	strcpy(path, dirname(path));
	printf("Stripped base directory to '%s'\n", path);
	chdir(path);

	int debug = 0;

	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-d"))
			debug++;
	avr = avr_make_mcu_by_name("atmega644");
	if (!avr) {
		fprintf(stderr, "%s: Error creating the AVR core\n", argv[0]);
		exit(1);
	}
//	snprintf(avr_flash_path, sizeof(avr_flash_path), "%s/%s", pwd, "simduino_flash.bin");
	strcpy(avr_flash_path,  "reprap_flash.bin");
	// register our own functions
	avr->special_init = avr_special_init;
	avr->special_deinit = avr_special_deinit;
	avr_init(avr);
	avr->frequency = 20000000;
	avr->aref = avr->avcc = avr->vcc = 5 * 1000;	// needed for ADC

	elf_firmware_t f;
	const char * fname = "/opt/reprap/tvrrug/Marlin/Marlin/applet/Marlin.elf";
	// try to load an ELF file, before trying the .hex
	if (elf_read_firmware(fname, &f) == 0) {
		printf("firmware %s f=%d mmcu=%s\n", fname, (int)f.frequency, f.mmcu);
		avr_load_firmware(avr, &f);
	} else {
		char path[1024];
		uint32_t base, size;
//		snprintf(path, sizeof(path), "%s/%s", pwd, "ATmegaBOOT_168_atmega328.ihex");
		strcpy(path, "marlin/Marlin.hex");
//		strcpy(path, "marlin/bootloader-644-20MHz.hex");
		uint8_t * boot = read_ihex_file(path, &size, &base);
		if (!boot) {
			fprintf(stderr, "%s: Unable to load %s\n", argv[0], path);
			exit(1);
		}
		printf("Firmware %04x(%04x in AVR talk): %d bytes (%d words)\n", base, base/2, size, size/2);
		memcpy(avr->flash + base, boot, size);
		free(boot);
		avr->pc = base;
		avr->codeend = avr->flashend;
	}
	//avr->trace = 1;

	// even if not setup at startup, activate gdb if crashing
	avr->gdb_port = 1234;
	if (debug) {
		printf("AVR is stopped, waiting on gdb on port %d. Use 'target remote :%d' in avr-gdb\n",
				avr->gdb_port, avr->gdb_port);
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}

	// Marlin doesn't loop, sleep, so we don't know when it's idle
	// I changed Marlin to do a spurious write to the GPIOR0 register so we can trap it
	avr_register_io_write(avr, MEGA644_GPIOR0, reprap_relief_callback, NULL);

	reprap_init(avr, &reprap);

	gl_init(argc, argv);
	pthread_t run;
	pthread_create(&run, NULL, avr_run_thread, NULL);

	gl_runloop();

}
