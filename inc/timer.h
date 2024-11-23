/*
 * timers.h
 *
 *  Ported on: 02.25.2023
 *      Author: Jonathan Armstrong
 */

#ifndef INC_TIMERS_H_
#define INC_TIMERS_H_

#include <stdint.h>

// Nexa uses 4 timings for one bit and the data is 32 bits so it is 128 timings long plus one pause and two for header.
// So at least 131 timings neede and we have room for only 128 timings in __xdata.
// It seems that 128 here is not working for some strange reason as it corrupts the last byte.
// Maybe something is corrupting the upper half of last byte of __xdata or I have some bug here.
#define TIMINGS_MAX_CHANGES 127
#define TIMINGS_MAX_EXTRA 8

//#define MAX_PERIOD_COUNT 48
//#define BUFFER_SIZE 24


extern bool available(void);
extern void reset_available(void);

extern unsigned long get_current_timer0(void);
extern unsigned long get_elapsed_timer0(unsigned long previousTime);

extern unsigned long get_current_timer1(void);
extern unsigned long get_elapsed_timer1(unsigned long previousTime);

extern volatile uint8_t received_byte_count;
extern volatile __xdata uint16_t timings[TIMINGS_MAX_CHANGES];
extern volatile uint16_t extra_timings[TIMINGS_MAX_EXTRA];


#endif