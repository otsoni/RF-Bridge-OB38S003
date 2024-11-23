//=========================================================
// Jonathan Armstrong - Port to SDCC compiler
// Silicon Labs IDE 5.50.00 seems to the last IDE that makes compiling with SDCC possible
//=========================================================

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
#if !defined(TARGET_BOARD_EFM8BB1) && !defined(TARGET_BOARD_OB38S003) && !defined(TARGET_BOARD_EFM8BB1LCB)
    #error Please define TARGET_BOARD in makefile
#endif

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
// SFR declarations
#include <stdint.h>

// for printf_tiny()
//#include <stdio.h>

#include "delay.h"
#include "hal.h"
#include "portisch.h"
#include "portisch_command_format.h"
#include "portisch_protocols.h"
#include "portisch_serial.h"
#include "timer_interrupts.h"
#include "uart.h"
#include "util.h"


// example for sniffing (0xB1 command) and reply tranmission (0xB0 command) from my door sensor
//{"RfRaw":{"Data":"AA B1 03 0138 03B7 277C 28181909081908190819090908181908181818181908190818 55"}}

// 0xB1 to 0xB0 web convert tool:
// https://bbconv.hrbl.pl/
// got this output from the tool:
// rfraw AA B0 21 03 08 01 38 03 B7 27 7C 28 18 19 09 08 19 08 19 08 19 09 09 08 18 19 08 18 18 18 18 19 08 19 08 18 55

// uart state machine
__xdata uart_state_t uart_state = IDLE;
__xdata uart_command_t uart_command = NONE;
__xdata uart_command_t last_sniffing_command;

// we share rf_data[] array with uart and radio in order to save on bytes (e.g., and for long commands such as 0xB0)
//__xdata uint8_t uartPacket[10];


// used in multiple functions so made global for now
static __xdata uint8_t packetLength = 0;

// used for count down radio transmissions
__xdata uint8_t tr_repeats = 0;

// pointer
uint16_t* buckets_pointer;

// FIXME: comment on what this really does
bool blockReadingUART = false;

// sdcc manual section 3.8.1 general information
// requires interrupt definition to appear or be included in main
// sdccman sec. 3.8.1 indicates isr prototype must appear or be included in the file containing main
// it is probably more proper to achieve this through include files but also easy to omit
// and then things just will not work with no clear reason why, even though compilation is succcessful
#if defined(TARGET_BOARD_OB38S003)
    // supports timeout
    void timer1_isr(void) __interrupt (d_T1_Vector);
    // pca like capture mode for radio decoding
    void timer2_isr(void) __interrupt (d_T2_Vector);
    // hardware uart
    // void uart_isr(void)   __interrupt (d_UART0_Vector);

#elif defined(TARGET_BOARD_EFM8BB1) || defined(TARGET_BOARD_EFM8BB1LCB)
    // timer0 was used for PCA in portisch, instead just use system clock
    // timer1 must be used for uart on this controller
    void uart_isr(void) __interrupt (UART0_VECTOR);
    // timer2 is used on demand to provide delays
    void timer2_isr(void) __interrupt (TIMER2_VECTOR);
    void pca0_isr(void) __interrupt (PCA0_VECTOR);
    // timer3 was previously used on demand to provide delays
    //void TIMER3_ISR(void) __interrupt (TIMER3_VECTOR);
#else
    #error Please define TARGET_BOARD in makefile
#endif

//-----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// This function is called immediately after reset, before the initialization
// code (which runs before main() ). This is a
// useful place to disable the watchdog timer, which is enable by default
// and may trigger before main() in some instances.
//-----------------------------------------------------------------------------
unsigned char __sdcc_external_startup(void)
{
    // pg. 218, sec. 20.3 disable watchdog timer
    disable_global_interrupts();
    
    // using hardware abstraction
    disable_watchdog();
    
    return 0;
}


#if 0

void serial_loopback(void)
{
	volatile unsigned int rxdata = UART_NO_DATA;

	// check if serial transmit buffer is empty
	if(!is_uart_tx_buffer_empty())
	{
	    if (is_uart_tx_finished())
	    {
	        // if not empty, set TI, which triggers interrupt to actually transmit
	        uart_init_tx_polling();
	    }
	}

	// check if something got received by UART
	// only read data from uart if idle
	if (!is_uart_ignored())
    {
		rxdata = uart_getc();
	} else {
		rxdata = UART_NO_DATA;
    }

	if (rxdata == UART_NO_DATA)
	{
		led_on();
	} else {
		uart_putc(rxdata & 0xff);
	}
    
}

#endif



void uart_state_machine(const uint8_t rxdata)
{
	// FIXME: add comment
	static __xdata uint8_t position = 0;

	// state machine for UART
	switch(uart_state)
	{
		// check if start sequence got received
		case IDLE:
			if (rxdata == RF_CODE_START)
            {
				uart_state = SYNC_INIT;
            }
			break;

		// sync byte got received, read command
		case SYNC_INIT:
			uart_command = rxdata;
			uart_state = SYNC_FINISH;

			// check if some data needs to be received
			switch(uart_command)
			{
				case RF_CODE_LEARN:
					break;
				case RF_CODE_RFOUT:
					// stop sniffing while handling received data
					PCA0_StopSniffing();
					rf_state = RF_IDLE;
					uart_state = RECEIVING;
					tr_repeats = RF_TRANSMIT_REPEATS;
					position = 0;
					packetLength = 9;
					break;
				case RF_DO_BEEP:
					// stop sniffing while handling received data
					//PCA0_StopSniffing();
					//rf_state = RF_IDLE;
					uart_state = RECEIVING;
					position = 0;
					packetLength = 2;
					break;
				case RF_ALTERNATIVE_FIRMWARE:
					break;
				case RF_CODE_SNIFFING_ON:
					sniffing_mode = ADVANCED;
					PCA0_DoSniffing();
					last_sniffing_command = RF_CODE_SNIFFING_ON;
					rf_state = RF_IDLE;
					break;
				case RF_CODE_SNIFFING_OFF:
					// set desired RF protocol PT2260
					sniffing_mode = STANDARD;
					// re-enable default RF_CODE_RFIN sniffing
					PCA0_DoSniffing();
					uart_command          = RF_CODE_RFIN;
					last_sniffing_command = RF_CODE_RFIN;
					rf_state = RF_IDLE;
					break;
				case RF_CODE_RFOUT_NEW:
					tr_repeats = RF_TRANSMIT_REPEATS;
					uart_state = RECEIVE_LENGTH;
					break;
				case RF_CODE_RFOUT_BUCKET:
					uart_state = RECEIVE_LENGTH;
					break;
				case RF_CODE_SNIFFING_ON_BUCKET:
					PCA0_DoSniffing();
					last_sniffing_command = RF_CODE_SNIFFING_ON_BUCKET;
					rf_state = RF_IDLE;
					break;
				case RF_CODE_LEARN_NEW:
					break;
				case RF_CODE_ACK:
                    // FIXME: I do not think this comment matches what happens in code, need to examine
					// re-enable default RF_CODE_RFIN sniffing
                    PCA0_DoSniffing();
					uart_command = last_sniffing_command;
					uart_state = IDLE;
					rf_state = RF_IDLE;
					break;

				// unknown command
				default:
					uart_command = NONE;
					uart_state = IDLE;
					break;
			}
			break;

		// Receiving UART data length
		case RECEIVE_LENGTH:
			position = 0;
			packetLength = rxdata;
			if (packetLength > 0)
			{
				// stop sniffing while handling received data
                // FIXME: comment on why
				PCA0_StopSniffing();
                
				rf_state = RF_IDLE;
				uart_state = RECEIVING;
			} else {
				uart_state = SYNC_FINISH;
			}
			break;

		// Receiving UART data
		case RECEIVING:
			RF_DATA[position] = rxdata;

			// DEBUG:
			//puthex2(RF_DATA[position]);
			//uart_putc(RF_DATA[position]);
			
			position++;

			// FIXME: uartPacket[] versus RF_DATA[] array length
			if (position == packetLength)
			{
				uart_state = SYNC_FINISH;
			} else if (position >= RF_DATA_BUFFERSIZE)
			{
                packetLength = RF_DATA_BUFFERSIZE;
                uart_state = SYNC_FINISH;
			}
			break;

		// wait and check for UART_SYNC_END
		case SYNC_FINISH:
			if (rxdata == RF_CODE_STOP)
			{
				uart_state = IDLE;
                
                // FIXME: comment on what this really does
                blockReadingUART = true;

				// check if ACK should be sent
				switch(uart_command)
				{
					case RF_CODE_LEARN:
					case RF_CODE_SNIFFING_ON:
					case RF_CODE_SNIFFING_OFF:
					case RF_CODE_RFIN:
					case RF_CODE_SNIFFING_ON_BUCKET:
						// send acknowledge
						uart_put_command(RF_CODE_ACK);
					case RF_CODE_ACK:
                        blockReadingUART = false;
						break;
					case RF_CODE_RFOUT_BUCKET:
                        // conversion tool seems to show data length, then number of buckets, then number of repeats after 0xB0 command
                        // FIXME: why plus ?
						tr_repeats = RF_DATA[1] + 1;
                        
                        uint8_t num_buckets = RF_DATA[0];
                        uint8_t byteIndex = 0;
                        
                        // this is a global variable with maximum size seven
                        buckets_pointer = (uint16_t *)(RF_DATA + 2);
                        
                        // because sdcc is little endian for 8051, we need to swap bucket values to access by pointer later
                        while (byteIndex < num_buckets)
                        {
                            buckets_pointer[byteIndex] = ((buckets_pointer[byteIndex] << 8) | (buckets_pointer[byteIndex] >> 8));
                            byteIndex++;
                        }
                        
                        // DEBUG:
                        //uart_putc(tr_repeats);
                        
						break;
				}
			}
			break;
	}
}

bool radio_state_machine(void)
{
	bool completed = false;

	// DEBUG:
	//const uint16_t pulsewidths_dummy[3] = {350, 1050, 10850};
	//const uint8_t  packet_dummy[3]  = {0xA5, 0x5A, 0xA5};

    // for transmission
    uint16_t pulsewidths[3];
    uint8_t  tr_packet[3];

	// helps allow sendbuckets call to be more readable
	uint8_t start_size;
	uint8_t bit0_size;
	uint8_t bit1_size;
	uint8_t end_size;
	uint8_t bitcount;

	// do transmit of the data
	switch(rf_state)
	{
		// init and start RF transmit
		case RF_IDLE:

			tr_repeats--;

			PCA0_StopSniffing();

			// byte 0..1:	Tsyn
			// byte 2..3:	Tlow
			// byte 4..5:	Thigh
			// byte 6..8:	24bit Data
			// FIXME: sdcc is little endian so we would need to swap bytes for this to work
			//pulsewidths[0] = *(uint16_t *)&uartPacket[2];
			//pulsewidths[1] = *(uint16_t *)&uartPacket[4];
			//pulsewidths[2] = *(uint16_t *)&uartPacket[0];
            
            // low, high, sync order in array (from uart order is sync, low, high)
            pulsewidths[0] = (RF_DATA[2] << 8) | RF_DATA[3];
            pulsewidths[1] = (RF_DATA[4] << 8) | RF_DATA[5];
            pulsewidths[2] = (RF_DATA[0] << 8) | RF_DATA[1];
            
            // data
            tr_packet[0] = RF_DATA[6];
            tr_packet[1] = RF_DATA[7];
            tr_packet[2] = RF_DATA[8];

			
			// help make function call more readable
			start_size = PROTOCOL_DATA[0].start.size;
			bit0_size  = PROTOCOL_DATA[0].bit0.size;
			bit1_size  = PROTOCOL_DATA[0].bit1.size;
			end_size   = PROTOCOL_DATA[0].end.size;
			bitcount   = PROTOCOL_DATA[0].bit_count;


			SendBuckets(pulsewidths, PROTOCOL_DATA[0].start.dat, start_size, PROTOCOL_DATA[0].bit0.dat, bit0_size, PROTOCOL_DATA[0].bit1.dat, bit1_size, PROTOCOL_DATA[0].end.dat, end_size, bitcount, tr_packet);
			
            // DEBUG:
            //SendBuckets(pulsewidths_dummy, PROTOCOL_DATA[0].start.dat, start_size, PROTOCOL_DATA[0].bit0.dat, bit0_size, PROTOCOL_DATA[0].bit1.dat, bit1_size, PROTOCOL_DATA[0].end.dat, end_size, bitcount, packet_dummy);
            
			// ping pong between idle and finished state until we reach zero repeat index
			rf_state = RF_FINISHED;
			
			break;

		// wait until data got transfered
		case RF_FINISHED:
			if (tr_repeats == 0)
			{
				// disable RF transmit
				tdata_off();

				completed = true;
			} else {
				rf_state = RF_IDLE;
			}
			break;
	}

	return completed;
}

void startup_blink(void)
{
    // double blink
    led_on();
    delay1ms(1000);
    led_off();
    
    led_on();
    delay1ms(1000);
    led_off();
}

//-----------------------------------------------------------------------------
// main() Routine
// ----------------------------------------------------------------------------
void main (void)
{
	// for buzzer (milliseconds)
	//const uint16_t startupDelay = 100;
	// longer for LED
	const uint16_t startupDelay = 3000;

	// changed by external hardware, so must specify volatile type so not optimized out
	volatile uint8_t rxdata = 0;
	volatile bool rx_data_available = false;

	// FIXME: add comment
    uint16_t bucket = 0;
    

	// FIXME: add comment
    __xdata uint16_t idleResetCount = 0;

	// prefer bool type in internel ram to take advantage of bit addressable locations
	bool result;

    // add comment?
    set_clock_mode();

	// call hardware initialization routine
#if defined(TARGET_BOARD_OB38S003)
    init_port_pins();
#elif defined(TARGET_BOARD_EFM8BB1) || defined(TARGET_BOARD_EFM8BB1LCB)
    // the crossbar on this microcontroller makes initialization more complicated
    init_port_pins_for_serial();
#else
    #error Please define TARGET_BOARD in makefile
#endif

	// set default pin states
	led_on();
	buzzer_off();
	tdata_off();
    
    // DEBUG:
    // on some boards, "debug pin" is actually buzzer
    // so we do not want to use it for debugging unless buzzer has been removed
    //debug_pin01_off();
    
    //
    startup_blink();
    delay1ms(500);
    
	// baud rate is 19200, 8 data bits, 1 stop bit, no parity
	// polled version basically sets TI flag so putchar() does not get stuck in an infinite loop
	//UART0_initStdio();
	// enable uart (with interrupts)
	//UART0_init(UART0_RX_ENABLE, UART0_WIDTH_8, UART0_MULTIPROC_DISABLE);
    init_uart();
    uart_rx_enabled();
    
    // FIXME: despite rcswitch doing it this way, it seems like we only want to enable after setting uart timer baud rate
    // sets TI=1 so ring buffer logic works
    init_serial_interrupt();
    // enable actual interrupt
    // enable_serial_interrupt();
    
#if defined(TARGET_BOARD_OB38S003)
    init_timer1_8bit_autoreload();
    init_timer2_as_capture();
    enable_timer1_interrupt();
#elif defined(TARGET_BOARD_EFM8BB1) || defined(TARGET_BOARD_EFM8BB1LCB)
    // pca used timer0 on portisch, we just use the pca counter itself now
    // uart with 19200 baud, uart must use timer1 on efm8bb1
    init_timer1_8bit_autoreload(TIMER1_UART0);
    timer1_run();
    
    //
    pca0_init();
    
    // FIXME: we are not using timer3 right now because ob38s003 does not have it
    enable_timer2_interrupt();
    //enable_timer3_interrupt();
    
    //FIXME: in rcswitch we did pca0_run() here, but it happens in DoSniffing() for portisch
#endif

    // FIXME: this is slightly different to rcswitch initialization, need to decide what makes the most sense
    enable_capture_interrupt();


	// start sniffing be default
	// set desired sniffing type to PT2260
	sniffing_mode = STANDARD;
    
    // FIXME: as best I can tell on development board, advanced sniffing does not even work with portisch hex
	//sniffing_mode = ADVANCED;
	PCA0_DoSniffing();
	rf_state = RF_IDLE;

    // FIXME: double check if this comment is accurate
	// basically chooses between standard (i.e., three buckets) and advanced output (i.e. up to seven buckets)
	last_sniffing_command = RF_CODE_RFIN;
	uart_command          = RF_CODE_RFIN;
	//last_sniffing_command = RF_CODE_SNIFFING_ON;
	//uart_command          = RF_CODE_SNIFFING_ON;

	
	// alternative with no sniffing on startup
	//PCA0_StopSniffing();
	//rf_state = RF_IDLE;


    // FIXME: we already enable this previously where portisch did
    // so can do it here or above, does it matter?
	// enable global interrupts
	enable_global_interrupts();
    
    // FIXME: function empty on efm8bb1, because unknown if receiver has enable pin
    radio_receiver_on();

    
    // DEBUG:
    // startup
    //requires code and memory space, which is in short supply
    //but good to check that polled uart is working
    //printf_tiny("startup...\r\n");
    //uart_put_command(RF_CODE_ACK);

	// FIXME: this is essentially our watchdog timer initialization
	//        but it needs to be done with the abstraction layer
	// instead of the default divide by 8, we do divide by 1 so that low frequency oscillator is at 80 kHz
	// sec. 20.4.1 WDTCN watchdog timer control
	// e.g., (1/80000)*4^(5+3) = 0.8192
	// so that watchdog timer will reset if not fed every 819 millisecs
	//LFO0CN |= OSCLD__DIVIDE_BY_1;

	// we disabled watchdog at startup in case external ram needs to be cleared etc.
	// now we enable
	//WDT0_start();
    enable_watchdog();
	
	while (true)
	{
		// reset Watch Dog Timer
		refresh_watchdog();


#if 0
		// DEBUG: infinite loop to echo back serial characters
		while (true)
		{
			serial_loopback();
		}
#endif

#if 1
		// check if something got received by UART
		// only read data from uart if idle
		if (uart_rx_data_available())
        {
			rxdata = uart_getc();
			rx_data_available = true;
		} else {
			rxdata = 0;
        }
        
#endif
        
        

#if 0
        // check if serial transmit buffer is empty
        if(!is_uart_tx_buffer_empty())
        {
            if (is_uart_tx_finished())
            {
                // if not empty, set TI, which triggers interrupt to actually transmit
                uart_init_tx_polling();
            }
        }
        
#endif



		if (!rx_data_available)
		{

#if 1
			// FIXME: the magic numbers make this difficult to understand
			// but seems to reset uart if it sits in non-idle state
			// for too long without receiving any more data
			if (uart_state == IDLE)
				idleResetCount = 0;
			else
			{
				idleResetCount += 1;
			
				if (idleResetCount > 30000)
				{
					idleResetCount = 0;
					uart_state = IDLE;
					uart_command = NONE;
				}
			}
#endif
		}
		else
		{
			uart_state_machine(rxdata);
			rx_data_available = false;
		}

		/*------------------------------------------
		 * check command byte
		 ------------------------------------------*/
		switch(uart_command)
		{
			case NONE:
				break;
			// do original sniffing
			case RF_CODE_RFIN:
			case RF_CODE_SNIFFING_ON:

				// check if a RF signal got decoded
				if ((RF_DATA_STATUS & RF_DATA_RECEIVED_MASK) != 0)
				{
					switch(uart_command)
					{
						case RF_CODE_RFIN:
							// we no longer share a buffer between radio and uart, however need to avoid writing to radio buffer while reading it
							disable_capture_interrupt();
							
							//we read RF_DATA[] so do not want decoding writing to it while trying to read it
							uart_put_RF_Data_Standard(RF_CODE_RFIN);
							
							enable_capture_interrupt();
							break;

						case RF_CODE_SNIFFING_ON:
							uart_put_RF_Data_Advanced(RF_CODE_SNIFFING_ON, RF_DATA_STATUS & 0x7F);
							break;
					}

					// clear RF status
					RF_DATA_STATUS = 0;

					// enable interrupt for RF receiving
					enable_capture_interrupt();
				}
				else
				{
					// disable interrupt for radio receiving while reading buffer
					disable_capture_interrupt();
					result = buffer_out(&bucket);
					// FIXME: reenable (should store previous and just restore that?)
					enable_capture_interrupt();

					// handle new received buckets
					if (result)
                    {
						HandleRFBucket(bucket & 0x7FFF, (bool)((bucket & 0x8000) >> 15));
                    }
				}
				break;
			case RF_CODE_RFOUT:

				// only do the job if all data got received by UART
				if (uart_state != IDLE)
					break;

				// if statement allows repeat transmissions
				if (radio_state_machine())
				{
					// indicate completed all transmissions
					uart_put_command(RF_CODE_ACK);

					// FIXME: need to examine this logic
					// restart sniffing in its previous mode
					PCA0_DoSniffing();
                    
                    blockReadingUART = false;

					// change back to previous command (i.e., not rfout)
					uart_command = last_sniffing_command;
				}
				break;
			// FIXME: need to add back in 0xA8 and 0xB0 command
			case RF_CODE_RFOUT_NEW:
				// only do the job if all data got received by UART
				if (uart_state != IDLE)
					break;

				// do transmit of the data
				switch(rf_state)
				{
					// init and start RF transmit
					case RF_IDLE:
						tr_repeats--;
						PCA0_StopSniffing();

						// byte 0:		PROTOCOL_DATA index
						// byte 1..:	Data
                        // FIXME: rcswitch treats "protocol 1" as index 0, so might need to make consistent with portisch
						SendBucketsByIndex(RF_DATA[0], &RF_DATA[1]);
                        
                        // ping pong between idle and finished state until we reach zero repeat index
                        rf_state = RF_FINISHED;
            
						break;

					// wait until data got transfered
					case RF_FINISHED:
						if (tr_repeats == 0)
						{
							// disable RF transmit
							//T_DATA = TDATA_OFF;
                            tdata_off();

                            // indicate completed all transmissions
                            uart_put_command(RF_CODE_ACK);
                            
                            blockReadingUART = false;

                            // FIXME: need to examine this logic
                            // restart sniffing in its previous mode
                            PCA0_DoSniffing();

                            // change back to previous command (i.e., not rfout)
                            uart_command = last_sniffing_command;
						}
						else
						{
							rf_state = RF_IDLE;
						}
						break;
				}
				break;
                
			case RF_CODE_RFOUT_BUCKET:
			{
				// only do the job if all data got received by UART
				if (uart_state != IDLE)
					break;

				// do transmit of the data
				switch(rf_state)
				{
					// init and start RF transmit
					case RF_IDLE:
						tr_repeats--;
						PCA0_StopSniffing();

                        uint8_t num_buckets = RF_DATA[0];
                        
                        // FIXME: I do not know what format this is, does it match 0xB0 on the wiki?
						// byte 0:				number of buckets: k
						// byte 1:				number of repeats: r
						// byte 2*(1..k):		bucket time high
						// byte 2*(1..k)+1:		bucket time low
						// byte 2*k+2..N:		RF buckets to send
                        //uint16_t* buckets = (uint16_t *)(RF_DATA + 2);
                        

                        // DEBUG:
                        //uart_putc(buckets_pointer[0] >> 8);
                        //uart_putc(buckets_pointer[0] & 0xff);
                        
                        // find the start of the data by skipping over the number of buckets times two and two bytes for numbers of buckets and number of repeats
                        uint8_t* rfdata = RF_DATA + (num_buckets << 1) + 2;
                        
                        // DEBUG:
                        //uart_putc(rfdata[0]);
                        
                        // subtract out two bytes for number of buckets and number of repeats
                        // then subtract out number of buckets multiplied by 2
                        uint8_t data_len = packetLength - 2 - (num_buckets << 1);
                        
                        // DEBUG:
                        //uart_putc(data_len);
                        
                        //
						SendRFBuckets(buckets_pointer, rfdata, data_len);
                        
                        // ping pong between idle and finished state until we reach zero repeat index
                        rf_state = RF_FINISHED;
                        
						break;

					// wait until data got transfered
					case RF_FINISHED:
						if (tr_repeats == 0)
						{
							// disable RF transmit
							//T_DATA = TDATA_OFF;
                            tdata_off();

                            // indicate completed all transmissions
                            uart_put_command(RF_CODE_ACK);

                            blockReadingUART = false;
                            
                            // FIXME: need to examine this logic
                            // restart sniffing in its previous mode
                            PCA0_DoSniffing();

                            // change back to previous command (i.e., not rfout)
                            uart_command = last_sniffing_command;
						}
						else
						{
							rf_state = RF_IDLE;
						}
						break;
				}
				break;
			}
			case RF_CODE_SNIFFING_ON_BUCKET:

				// check if a RF signal got decoded
				if ((RF_DATA_STATUS & RF_DATA_RECEIVED_MASK) != 0)
				{
					//
					disable_capture_interrupt();
					
					//
					uart_put_RF_buckets(RF_CODE_SNIFFING_ON_BUCKET);

					// clear RF status
					RF_DATA_STATUS = 0;

					// enable interrupt for RF receiving
					enable_capture_interrupt();
				}
				else
				{
					// do bucket sniffing handling
					result = buffer_out(&bucket);
					if (result)
					{
						Bucket_Received(bucket & 0x7FFF, (bool)((bucket & 0x8000) >> 15));
					}
				}

			break;

			// do a beep
			case RF_DO_BEEP:
				// only do the job if all data got received by UART
				if (uart_state != IDLE)
					break;

				// this is blocking unfortunately
				buzzer_on();

				// FIXME: sdcc is little endian for 8051 so we need to swap byte order
				delay1ms(*(uint16_t *)&RF_DATA[1]);
				buzzer_off();

				// send acknowledge
				// send uart command
				uart_put_command(RF_CODE_ACK);
                
                blockReadingUART = false;
                
				uart_command = last_sniffing_command;
				break;

			// host was requesting the firmware version
			case RF_ALTERNATIVE_FIRMWARE:

				// send firmware version
				uart_put_command(FIRMWARE_VERSION);
                
                blockReadingUART = false;
                
				uart_command = last_sniffing_command;
				break;

			default:
				// FIXME: not sure if this makes sense
				uart_command = NONE;
			break;
		} //switch(uart_command)
	} //while (1)
}
