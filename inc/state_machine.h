/*
 *  Ported on: 02.18.2023
 *      Author: Jonathan Armstrong
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

// this is returned along with an ack in response to a request firmware version command
// which is a good demonstration that serial pins and state machine are working
#define FIRMWARE_VERSION 0x03


// FIXME: add comment
typedef enum
{
	IDLE,
	SYNC_INIT,
	SYNC_FINISH,
	RECEIVE_LEN,
	RECEIVING,
	TRANSMIT,
	COMMAND,
    RF_FINISHED
} UART_STATE_T;


// commands which should be supported (eventually) are given here:
// https://github.com/Portisch/RF-Bridge-EFM8BB1/wiki/Commands
typedef enum
{
	NONE                       = 0x00,
	RF_CODE_ACK                = 0xA0,
	RF_CODE_LEARN              = 0xA1,
	RF_CODE_LEARN_KO           = 0xA2,
	RF_CODE_LEARN_OK           = 0xA3,
	RF_CODE_RFIN               = 0xA4,
	RF_CODE_RFOUT              = 0xA5,
	RF_CODE_SNIFFING_ON        = 0xA6,
	RF_CODE_SNIFFING_OFF       = 0xA7,
	RF_CODE_RFOUT_NEW          = 0xA8,
	RF_CODE_LEARN_NEW          = 0xA9,
	RF_CODE_LEARN_KO_NEW       = 0xAA,
	RF_CODE_LEARN_OK_NEW       = 0xAB,
	RF_CODE_RFOUT_BUCKET       = 0xB0,
	RF_CODE_SNIFFING_ON_BUCKET = 0xB1,
	RF_DO_BEEP                 = 0xC0,
    SINGLE_STEP_DEBUG          = 0xFE,
	RF_ALTERNATIVE_FIRMWARE    = 0xFF
} UART_COMMAND_T;

void uart_state_machine(const unsigned int rxdata);
//void radio_decode_report(void);
void radio_timings(void);


#endif // STATE_MACHINE_H
