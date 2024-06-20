#include <limits.h>
#include <stdlib.h>
#include <string.h>


#include "delay.h"
#include "hal.h"
#include "rcswitch.h"

#include "timer_interrupts.h"

#include "ticks.h"


// FIXME: explain constants
volatile __xdata struct RC_SWITCH_T gRCSwitch = {0, 0, 0, 0, 60, 4300};
volatile __xdata uint16_t timings[RCSWITCH_MAX_CHANGES];

//static __xdata struct TRANSMIT_PACKET_T gTXPacket;
__xdata int nRepeatTransmit = 8;

//
const struct Protocol protocols[] = {
  { 350, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },    // protocol 1
  { 650, {  1, 10 }, {  1,  2 }, {  2,  1 }, false },    // protocol 2
  { 100, { 30, 71 }, {  4, 11 }, {  9,  6 }, false },    // protocol 3
  { 380, {  1,  6 }, {  1,  3 }, {  3,  1 }, false },    // protocol 4
  { 500, {  6, 14 }, {  1,  2 }, {  2,  1 }, false },    // protocol 5
  { 450, { 23,  1 }, {  1,  2 }, {  2,  1 }, true },     // protocol 6 (HT6P20B)
  { 150, {  2, 62 }, {  1,  6 }, {  6,  1 }, false },    // protocol 7 (HS2303-PT, i. e. used in AUKEY Remote)
  { 200, {  3, 130}, {  7, 16 }, {  3,  16}, false},     // protocol 8 Conrad RS-200 RX
  { 200, { 130, 7 }, {  16, 7 }, { 16,  3 }, true},      // protocol 9 Conrad RS-200 TX
  { 365, { 18,  1 }, {  3,  1 }, {  1,  3 }, true },     // protocol 10 (1ByOne Doorbell)
  { 270, { 36,  1 }, {  1,  2 }, {  2,  1 }, true },     // protocol 11 (HT12E)
  { 320, { 36,  1 }, {  1,  2 }, {  2,  1 }, true }      // protocol 12 (SM5212)
};


// count of number of protocol entries
const unsigned int numProto = sizeof(protocols) / sizeof(protocols[0]);




bool available(void)
{
    return gRCSwitch.nReceivedValue != 0;
}

void reset_available(void)
{
    gRCSwitch.nReceivedValue = 0;
}


unsigned long long get_received_value(void)
{
    return gRCSwitch.nReceivedValue;
}

unsigned char get_received_bitlength(void)
{
    return gRCSwitch.nReceivedBitlength;
}

unsigned int get_received_delay(void)
{
    return gRCSwitch.nReceivedDelay;
}

unsigned char get_received_protocol(void)
{
    return gRCSwitch.nReceivedProtocol;
}

int get_received_tolerance(void)
{
    return gRCSwitch.nReceiveTolerance;
}

unsigned int* getReceivedRawdata(void)
{
    return timings;
}


bool receive_protocol(const int p, unsigned int changeCount)
{
    // FIXME: do we copy to ram from flash so check is faster in loops below?
    struct Protocol pro;

    // FIXME: we should probably check for out of bound index e.g. p = 0
    memcpy(&pro, &protocols[p-1], sizeof(struct Protocol));
    
    unsigned long code = 0;
    
    // assuming the longer pulse length is the pulse captured in timings[0]
    const unsigned int syncLengthInPulses = ((pro.syncFactor.low) > (pro.syncFactor.high)) ? (pro.syncFactor.low) : (pro.syncFactor.high);
    const unsigned int delay = timings[0] / syncLengthInPulses;
    const unsigned int delayTolerance = delay * get_received_tolerance() / 100;
    
    
    /* For protocols that start low, the sync period looks like
     *               _________
     * _____________|         |XXXXXXXXXXXX|
     *
     * |--1st dur--|-2nd dur-|-Start data-|
     *
     * The 3rd saved duration starts the data.
     *
     * For protocols that start high, the sync period looks like
     *
     *  ______________
     * |              |____________|XXXXXXXXXXXXX|
     *
     * |-filtered out-|--1st dur--|--Start data--|
     *
     * The 2nd saved duration starts the data
     */
    const unsigned int firstDataTiming = (pro.invertedSignal) ? (2) : (1);

    for (unsigned int i = firstDataTiming; i < changeCount - 1; i += 2)
    {
        code <<= 1;
        
        if (abs(timings[i] - delay * pro.zero.high) < delayTolerance &&
            abs(timings[i + 1] - delay * pro.zero.low) < delayTolerance) {
            // zero
        } else if (abs(timings[i] - delay * pro.one.high) < delayTolerance &&
            abs(timings[i + 1] - delay * pro.one.low) < delayTolerance) {
            // one
            code |= 1;
        } else {
            // failed
            return false;
        }
    }
    
    // ignore very short transmissions: no device sends them, so this must be noise
    if (changeCount > 7)
    {
        gRCSwitch.nReceivedValue = code;
        gRCSwitch.nReceivedBitlength = (changeCount - 1) / 2;
        gRCSwitch.nReceivedDelay = delay;
        gRCSwitch.nReceivedProtocol = p;
        
        return true;
    }

    return false;
}

void capture_handler(const uint16_t currentCapture)
{
    const uint8_t gapMagicNumber  = 200;
    const uint8_t repeatThreshold   = 2;
    
    // for converting 8-bit timer values to 16-bits to allow subtraction
    uint16_t        previous;
    static uint16_t current = 0;
    
    // this eventually represents the level duration in microseconds (difference between edge transitions)
    unsigned long duration;
    

    // rc-switch variables
    static unsigned int repeatCount = 0;
    static unsigned int changeCount = 0;

    // FIXME: move to rcswitch.h
    const unsigned int separationLimit = gRCSwitch.nSeparationLimit;

    // go from 8-bit to 16-bit variables
    previous = current;
    current = currentCapture;
    
    // check for overflow condition
    if (current < previous)
    {
        // FIXME: no magic numbers
        // FIXME: seems like a bad idea to make wrap around calculation depend on variable type, what if it changes
        // if overflow, we must compute difference by taking into account wrap around at maximum variable size
        duration = USHRT_MAX  - previous + current;
    } else {
        duration = current - previous;
    }
    
    // FIXME: no magic numbers
    // e.g., EFM8BB1
	// e.g. (1/(24500000))*(49/2) = 1      microsec
	// e.g. (1/(24500000/12))*2   = 0.9796 microsec
	// (1/(24500000/12))*dec(0xFFFF) = 32.0987755 millisecs max
    
    // e.g., OBS38S003
    // e.g. prescale at (1/4) at 16 MHz, four counts are needed to get one microsecond
    // e.g. prescale at (1/24) at 16 MHz
    // e.g., (1/(16000000/24)) * dec(0xFFFF) = 98.30 milliseconds maximum can be counted
    // FIXME: show why 3/2 conversion works
    duration = countsToTime(duration);
    
    // from oscillscope readings it appears that first sync pulse of first radio packet is frequently not output properly by receiver
    // this could be because radio receiver needs to "warm up" (despite already being enabled?)
    // and it is known that radio packet transmissions are often repeated (between about four and twenty times) perhaps in part for this reason
    if (duration > separationLimit)
    {
        // A long stretch without signal level change occurred. This could
        // be the gap between two transmission.
        if (abs(duration - timings[0]) < gapMagicNumber)
        {
          // This long signal is close in length to the long signal which
          // started the previously recorded timings; this suggests that
          // it may indeed by a a gap between two transmissions (we assume
          // here that a sender will send the signal multiple times,
          // with roughly the same gap between them).
          repeatCount++;
          
          if (repeatCount == repeatThreshold)
          {
            for(unsigned int i = 1; i <= numProto; i++)
            {
              if (receive_protocol(i, changeCount))
              {
                // receive succeeded for protocol i
                break;
              }
            }
            
            repeatCount = 0;
          }
        }
        
        changeCount = 0;
    }

    // detect overflow
    if (changeCount >= RCSWITCH_MAX_CHANGES)
    {
        changeCount = 0;
        repeatCount = 0;
    }

    timings[changeCount++] = duration;
    
    // done in the interrupt already on efm8bb1
    // but must be explicitly cleared on ob38s003
    // so just always clear it
    //clear pca0 interrupt flag
    clear_capture_flag();
}

/**
 * Sets Repeat Transmits
 */
void setRepeatTransmit(const int repeat)
{
	nRepeatTransmit = repeat;
}

/**
  * Sets the protocol to send.
  */
//void setProtocol(const struct Protocol pro)
//{
//	protocol = pro;
//}

/**
  * Sets the protocol to send, from a list of predefined protocols
  */
//void setProtocol(const int nProtocol)
//{
//    if (nProtocol < 1 || nProtocol > numProto)
//    {
//        // TODO: trigger an error, e.g. "bad protocol" ?
//        nProtocol = 1;
//    }
//
//	memcpy(&protocol, &protocols[nProtocol-1], sizeof(struct Protocol));
//}

/**
 * Transmit a single high-low pulse.
 */
void transmit(struct Protocol *pro, struct HighLow pulses, uint16_t highdelay, uint16_t lowdelay)
{
	__xdata uint8_t firstLogicLevel  = (pro->invertedSignal) ? 0 : 1;
	__xdata uint8_t secondLogicLevel = (pro->invertedSignal) ? 1 : 0;

	//__xdata uint16_t previous;
	//__xdata uint16_t elapsed;
	//__xdata uint16_t delay;
  
    set_tdata(firstLogicLevel);
	// mirror transmitted pulses to another pin for easier probing by oscilloscope
	set_debug_pin01(!firstLogicLevel);
	//delay_us(pro->pulseLength * pulses.high);
	//delay = pro->pulseLength * pulses.high;
	//init_timer1_us(1, highdelay);
	//wait_timer1_finished();
	init_timer2_us(1, highdelay);
	wait_timer2_finished();


    set_tdata(secondLogicLevel);
    set_debug_pin01(!secondLogicLevel);
	//delay_us(pro->pulseLength * pulses.low);
	//delay = pro->pulseLength * pulses.low;
	//init_timer1_us(1, lowdelay);
	//wait_timer1_finished();
	init_timer2_us(1, lowdelay);
	wait_timer2_finished();
}

/**
 * @param sCodeWord   a binary code word consisting of the letter 0, 1
 */
//void send(const char* sCodeWord)
//{
//
//  // turn the tristate code word into the corresponding bit pattern, then send it
//  unsigned long code = 0;
//  unsigned int length = 0;
//
//  for (const char* p = sCodeWord; *p; p++) {
//    code <<= 1L;
//    if (*p != '0')
//      code |= 1L;
//
//    length++;
//  }
//
//  send(code, length);
//}


/**
 * Transmit the first 'length' bits of the integer 'code'. The
 * bits are sent from MSB to LSB, i.e., first the bit at position length-1,
 * then the bit at position length-2, and so on, till finally the bit at position 0.
 * e.g., for Tasmota: RfRaw AAA524E001400384D0035855
 */
void send(const int nProtocol, unsigned long code, const unsigned int length)
{
    // FIXME: it might just be easier to make this global
    // and possibly share with receive protocol, if they are never used at the same time
    struct Protocol pro;
	
	int nRepeat;
	int index;

    // also checks for out of bound index (e.g., less than one)
	//setProtocol(nProtocol);

    // FIXME: consider checking index out of bound
    memcpy(&pro, &protocols[nProtocol-1], sizeof(struct Protocol));

	// make sure the receiver is disabled while we transmit
	//radio_receiver_off();

	for (nRepeat = 0; nRepeat < nRepeatTransmit; nRepeat++)
    {
		for (index = length - 1; index >= 0; index--)
        {
		    if (code & (1L << index))
	      	{
			    transmit(&pro, pro.one, 136, 47);
	      	}
		    else
	      	{
			    transmit(&pro, pro.zero, 47, 136);
	      	}
	    }

		transmit(&pro, pro.syncFactor, 47, 1406);
	}

	// disable transmit after sending (i.e., for inverted protocols)
	tdata_off();

	// enable receiver again if we just disabled it
	//radio_receiver_on();
}