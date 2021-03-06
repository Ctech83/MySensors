/*
 * The MySensors Arduino library handles the wireless radio link and protocol
 * between your home built sensors/actuators and HA controller of choice.
 * The sensors forms a self healing radio network with optional repeaters. Each
 * repeater and gateway builds a routing tables in EEPROM which keeps track of the
 * network topology allowing messages to be routed to nodes.
 *
 * Created by Marcelo Aquino <marceloaqno@gmail.org>
 * Copyright (C) 2016 Marcelo Aquino
 * Full contributor list: https://github.com/mysensors/MySensors/graphs/contributors
 *
 * Documentation: http://www.mysensors.org
 * Support Forum: http://forum.mysensors.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Based on wiringPi Copyright (c) 2012 Gordon Henderson.
 */

#include <pthread.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stropts.h>
#include <errno.h>
#include "rpi_util.h"
#include "SPI.h"
#include "log.h"

extern "C" {
	int piHiPri(const int pri);
}

struct ThreadArgs {
	void (*func)();
	int gpioPin;
};

static pthread_mutex_t intMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t *threadIds[64] = {NULL};

// sysFds:
//	Map a file descriptor from the /sys/class/gpio/gpioX/value
static int sysFds[64] =
{
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

#ifdef __RPI_BPLUS
static uint8_t physToGpio[64] =
{
	255,		// 0
	255, 255,	// 1, 2
	  2, 255,
	  3, 255,
	  4,  14,
	255,  15,
	 17,  18,
	 27, 255,
	 22,  23,
	255,  24,
	 10, 255,
	  9,  25,
	 11,   8,
	255,   7,	// 25, 26
// B+
	  0,   1,
	  5, 255,
	  6,  12,
	 13, 255,
	 19,  16,
	 26,  20,
	255,  21,
// the P5 connector on the Rev 2 boards:
	255, 255,
	255, 255,
	255, 255,
	255, 255,
	255, 255,
	 28,  29,
	 30,  31,
	255, 255,
	255, 255,
	255, 255,
	255, 255,
};
#else
static uint8_t physToGpio[64] =
{
	255,		// 0
	255, 255,	// 1, 2
	  0, 255,
	  1, 255,
	  4,  14,
	255,  15,
	 17,  18,
	 21, 255,
	 22,  23,
	255,  24,
	 10, 255,
	  9,  25,
	 11,   8,
	255,   7,	// 25, 26
														   255, 255, 255, 255, 255,	// ... 31
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,	// ... 47
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,	// ... 63
};
#endif

void *interruptHandler(void *args) {
	int fd, ret;
	struct pollfd polls;
	char c;
	struct ThreadArgs *arguments = (struct ThreadArgs *)args;
	int gpioPin = arguments->gpioPin;
	void (*func)() = arguments->func;
	delete arguments;

	(void)piHiPri(55);	// Only effective if we run as root

	if ((fd = sysFds[gpioPin]) == -1) {
		logError("Failed to attach interrupt for pin %d\n", gpioPin);
		return NULL;
	}

	// Setup poll structure
	polls.fd     = fd;
	polls.events = POLLPRI;

	while (1) {
		// Wait for it ...
		ret = poll(&polls, 1, -1);
		if (ret < 0) {
			logError("Error waiting for interrupt: %s\n", strerror(errno));
			break;
		}
		// Do a dummy read to clear the interrupt
		//	A one character read appars to be enough.
		//	Followed by a seek to reset it.
		(void)read (fd, &c, 1) ;
  		lseek (fd, 0, SEEK_SET) ;
		// Call user function.
		func();
	}

	close(fd);

	return NULL;
}

void rpi_util::pinMode(uint8_t physPin, uint8_t mode) {
	uint8_t gpioPin = (physPin > 63)? 255 : physToGpio[physPin];
	if (gpioPin == 255) {
		logError("pinMode: invalid pin: %d\n", physPin);
		return;
	}
	// Check if SPI is in use and target pin is related to SPI
	if (SPIClass::is_initialized() && gpioPin >= RPI_GPIO_P1_26 && gpioPin <= RPI_GPIO_P1_23) {
		return;
	} else {
		bcm2835_gpio_fsel(gpioPin, mode);
	}
}

void rpi_util::digitalWrite(uint8_t physPin, uint8_t value) {
	uint8_t gpioPin = (physPin > 63)? 255 : physToGpio[physPin];
	if (gpioPin == 255) {
		logError("digitalWrite: invalid pin: %d\n", physPin);
		return;
	}
	// Check if SPI is in use and target pin is related to SPI
	if (SPIClass::is_initialized() && gpioPin >= RPI_GPIO_P1_26 && gpioPin <= RPI_GPIO_P1_23) {
		if (value == LOW && (gpioPin == RPI_GPIO_P1_24 || gpioPin == RPI_GPIO_P1_26)) {
			SPI.chipSelect(gpioPin);
		}
	} else {
		bcm2835_gpio_write(gpioPin, value);
		// Delay to allow any change in state to be reflected in the LEVn, register bit.
		delayMicroseconds(1);
	}
}

uint8_t rpi_util::digitalRead(uint8_t physPin) {
	uint8_t gpioPin = (physPin > 63)? 255 : physToGpio[physPin];
	if (gpioPin == 255) {
		logError("digitalRead: invalid pin: %d\n", physPin);
		return 0;
	}
	// Check if SPI is in use and target pin is related to SPI
	if (SPIClass::is_initialized() && gpioPin >= RPI_GPIO_P1_26 && gpioPin <= RPI_GPIO_P1_23) {
		return 0;
	} else {
		return bcm2835_gpio_lev(gpioPin);
	}
}

void rpi_util::attachInterrupt(uint8_t physPin, void (*func)(), uint8_t mode) {
	FILE *fd;
	char fName[40];
	char c;
	int count, i;

	uint8_t gpioPin = (physPin > 63)? 255 : physToGpio[physPin];
	if (gpioPin == 255) {
		logError("attachInterrupt: invalid pin: %d\n", physPin);
		return;
	}

	if (threadIds[gpioPin] == NULL) {
		threadIds[gpioPin] = new pthread_t;
	} else {
		// Cancel the existing thread for that pin
		pthread_cancel(*threadIds[gpioPin]);
		// Wait a bit
		delay(1L);
	}

	// Export pin for interrupt
	if ((fd = fopen("/sys/class/gpio/export", "w")) == NULL) {
		logError("attachInterrupt: Unable to export pin %d for interrupt: %s\n", physPin, strerror(errno));
		exit(1);
	}
	fprintf(fd, "%d\n", gpioPin); 
	fclose(fd);

	// Wait a bit the system to create /sys/class/gpio/gpio<GPIO number>
	delay(1L);

	snprintf(fName, sizeof(fName), "/sys/class/gpio/gpio%d/direction", gpioPin) ;
	if ((fd = fopen (fName, "w")) == NULL) {
		fprintf (stderr, "attachInterrupt: Unable to open GPIO direction interface for pin %d: %s\n", physPin, strerror(errno));
		exit(1) ;
	}
	fprintf(fd, "in\n") ;
	fclose(fd) ;

	snprintf(fName, sizeof(fName), "/sys/class/gpio/gpio%d/edge", gpioPin) ;
	if ((fd = fopen(fName, "w")) == NULL) {
		fprintf (stderr, "attachInterrupt: Unable to open GPIO edge interface for pin %d: %s\n", physPin, strerror(errno));
		exit(1) ;
	}
	switch (mode) {
		case CHANGE: fprintf(fd, "both\n"); break;
		case FALLING: fprintf(fd, "falling\n"); break;
		case RISING: fprintf(fd, "rising\n"); break;
		case NONE: fprintf(fd, "none\n"); break;
		default:
			logError("attachInterrupt: Invalid mode\n");
			fclose(fd);
			return;
	}
	fclose(fd);

	if (sysFds[gpioPin] == -1) {
		snprintf(fName, sizeof(fName), "/sys/class/gpio/gpio%d/value", gpioPin);
		if ((sysFds[gpioPin] = open(fName, O_RDWR)) < 0) {
			fprintf (stderr, "Error reading pin %d: %s\n", physPin, strerror(errno));
			exit(1);
		}
	}

	// Clear any initial pending interrupt
	ioctl(sysFds[gpioPin], FIONREAD, &count);
	for (i = 0; i < count; ++i) {
		if (read(sysFds[gpioPin], &c, 1) == -1) {
			logError("attachInterrupt: failed to read pin status: %s\n", strerror(errno));
		}
	}

	struct ThreadArgs *threadArgs = new struct ThreadArgs;
	threadArgs->func = func;
	threadArgs->gpioPin = gpioPin;

	// Create a thread passing the pin and function
	pthread_create(threadIds[gpioPin], NULL, interruptHandler, (void *)threadArgs);
}

void rpi_util::detachInterrupt(uint8_t physPin) {
	uint8_t gpioPin = (physPin > 63)? 255 : physToGpio[physPin];
	if (gpioPin == 255) {
		logError("detachInterrupt: invalid pin: %d\n", physPin);
		return;
	}

	// Cancel the thread
	if (threadIds[gpioPin] != NULL) {
		pthread_cancel(*threadIds[gpioPin]);
		delete threadIds[gpioPin];
		threadIds[gpioPin] = NULL;
	}

	// Close filehandle
	if (sysFds[gpioPin] != -1) {
		close(sysFds[gpioPin]);
		sysFds[gpioPin] = -1;
	}

	FILE *fp = fopen("/sys/class/gpio/unexport", "w");
	if (fp == NULL) {
		logError("Unable to unexport pin %d for interrupt\n", gpioPin);
		exit(1);
	}
	fprintf(fp, "%d", gpioPin); 
	fclose(fp);
}

uint8_t rpi_util::digitalPinToInterrupt(uint8_t physPin) {
	// No need to convert the pin to gpio, we do it in attachInterrupt().
	return physPin;
}

void rpi_util::interrupts() {
	pthread_mutex_unlock(&intMutex);
}

void rpi_util::noInterrupts() {
	pthread_mutex_lock(&intMutex);
}
