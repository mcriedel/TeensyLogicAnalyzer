/* Teensy Logic Analyzer
 * Copyright (c) 2015 LAtimes2
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Resources used:
//   PIT timer 0
//   Port D pins
//   Serial : interface to SUMP user interface
//   Serial1/2/3 : debug info (if turned on)

//
//  User Configuration settings
//

//#define CREATE_TEST_FREQUENCIES  // if uncommented, it will output frequencies on pins 3 and 6
//#define TIMING_DISCRETES  // if uncommented, set pins for timing

// Debug serial port. Uncomment one of these lines
#define DEBUG_SERIAL(x) 0   // no debug output
//#define DEBUG_SERIAL(x) Serial1.x // debug output to Serial1
//#define DEBUG_SERIAL(x) Serial2.x // debug output to Serial2
//#define DEBUG_SERIAL(x) Serial3.x // debug output to Serial3

//
// Pin definitions (info only - do not change)
//

#define CHAN0 2
#define CHAN1 14
#define CHAN2 7
#define CHAN3 8
#define CHAN4 6
#define CHAN5 20
#define CHAN6 21
#define CHAN7 5

#define LED_PIN 13

#define TIMING_PIN_0 15
#define TIMING_PIN_1 16

//////////////////////////////////////
// End of settings
//////////////////////////////////////

// 58k buffer size
#define LA_SAMPLE_SIZE 58 * 1024

// use Port D for sampling
#define PORT_DATA_INPUT_REGISTER  GPIOD_PDIR

// PIT timer registers
#define TIMER_CONTROL_REGISTER    PIT_TCTRL0
#define TIMER_FLAG_REGISTER       PIT_TFLG0
#define TIMER_LOAD_VALUE_REGISTER PIT_LDVAL0

// SUMP protocol values
/* XON/XOFF are not supported. */
#define SUMP_RESET 0x00
#define SUMP_RUN   0x01
#define SUMP_ID    0x02
#define SUMP_DESC  0x04
#define SUMP_XON   0x11
#define SUMP_XOFF  0x13
#define SUMP_DIV   0x80
#define SUMP_CNT   0x81
#define SUMP_FLAGS 0x82
#define SUMP_TRIG  0xc0
#define SUMP_TRIG_VALS 0xc1
#define SUMP_TRIG_CONFIG 0xc2

// this is the main data storage array. Add 10 extra bytes
// just in case (triggering may use up to 8 extra)
byte logicData[LA_SAMPLE_SIZE + 10];

enum strategyType {
  STRATEGY_NORMAL,
  STRATEGY_PACKED,
  STRATEGY_ASSEMBLY,
} sumpStrategy = STRATEGY_NORMAL;

byte sumpNumChannels;
uint32_t sumpDivisor;
uint32_t sumpClockTicks;
uint32_t sumpFrequency;
uint32_t sumpSamples;
uint32_t sumpDelaySamples;
uint32_t sumpRequestedSamples;
uint32_t sumpRunning = 0;
byte sumpTrigMask = 0;
byte sumpTrigValue;

uint32_t previousBlinkTime = 0;

// working data for a 5-byte command from the SUMP Interface
struct _sumpRX {
  byte command[5];
  byte parameters;
  byte parCnt;
} sumpRX;

// for SUMP command state machine
enum _SUMP {
  C_IDLE = 0,
  C_PARAMETERS,
} sumpRXstate = C_IDLE;

// data to set up recording
struct sumpVariableStruct {
  uint32_t bufferSize;
  uint32_t delaySamples = 0;
  uint32_t delaySize = 0;
  byte extraByte1 = 0;
  byte extraByte2 = 0;
  int firstExtraByte = 1;
  uint16_t sampleMask = 0xFF;
  uint16_t sampleShift = 8;
  byte samplesPerByte = 1;
  bool swapBytes = false;
  int triggerCount = -1;     // -1 means trigger hasn't occurred yet
  byte *startOfBuffer;
  byte *endOfBuffer;
  byte *startPtr;
};

void setup()
{
  DEBUG_SERIAL(begin (1000000));  // baud rate of 1 Mbps
  DEBUG_SERIAL(println("Logic Analyzer"));

  // set up pins to record data on
  pinMode(CHAN0, INPUT);
  pinMode(CHAN1, INPUT);
  pinMode(CHAN2, INPUT);
  pinMode(CHAN3, INPUT);
  pinMode(CHAN4, INPUT);
  pinMode(CHAN5, INPUT);
  pinMode(CHAN6, INPUT);
  pinMode(CHAN7, INPUT);

  pinMode(LED_PIN, OUTPUT);

#ifdef TIMING_DISCRETES
  // Pin 0 high = waiting to sample, Pin 1 high = looking for trigger
  pinMode (TIMING_PIN_0, OUTPUT);
  pinMode (TIMING_PIN_1, OUTPUT);

#endif

  blinkled();

#ifdef CREATE_TEST_FREQUENCIES

  /* Use PWM to generate a test signal.
   *  If set on one of the analyzer pins, it will show up when recording.
   *  To test other pins, jumper a PWM output to an analyzer input.
   */

  // PWM available on pins 3-6,9,10,20-23
  // Port D: chan 4(6),5(20),6(21),7(5)

//  analogWriteFrequency (3, 1000000);
//  analogWrite (3, 128);

  analogWriteFrequency (6, 25000);
  analogWrite (CHAN4, 64);
  analogWrite (CHAN5, 124);
  analogWrite (CHAN6, 128);
  analogWrite (CHAN7, 192);

#endif

  SUMPreset();
}

// main loop
void loop()
{
  byte inByte;

  // loop forever
  while (1) {

    // check for input from SUMP interface
    if (Serial.available() > 0) {
      // blink LED when command is received
      blinkledFast();

      // read and process a byte from serial port
      inByte = Serial.read();

      // write command bytes to debug port
      DEBUG_SERIAL(print(inByte, HEX));
      DEBUG_SERIAL(print(","));

      SUMPprocessCommands(inByte);
    }

    // check for input from Debug port
    if (DEBUG_SERIAL(available()) > 0) {
      blinkledFast();
      SUMPprocessCommands(DEBUG_SERIAL(read()));
    }

    // if commanded to start, then record data
    if (sumpRunning) {
      SUMPrecordData();
    }

    // blink LED every 2 seconds if not recording
    if ((millis() - previousBlinkTime) > 2000) {
      previousBlinkTime = millis();
      blinkled();
    }
  }
}

void SUMPreset(void) {

  sumpRunning = 0;
}

void SUMPprocessCommands(byte inByte) {

  switch (sumpRXstate) { //this is a state machine that grabs the incoming commands one byte at a time

    case C_IDLE:

      processSingleByteCommand (inByte);
      break;

    case C_PARAMETERS:
      sumpRX.parCnt++;
      sumpRX.command[sumpRX.parCnt] = inByte; // store each parameter

      // if all parameters received
      if (sumpRX.parCnt == sumpRX.parameters)
      {
        processFiveByteCommand (sumpRX.command);

        sumpRXstate = C_IDLE;
      }  
      break;

    default:
      blinkled();
      sumpRXstate = C_IDLE;
      break;
  }
}

void processSingleByteCommand (byte inByte){

  switch (inByte) { // switch on the current byte

    case SUMP_RESET: // reset
      SUMPreset();
      break;

    case SUMP_ID: // SLA0 or 1 backwards: 1ALS
    // special command for debugging
    case '3':
      DEBUG_SERIAL(println ("SUMP_ID"));

      Serial.write("1ALS");
      break;

    case SUMP_RUN: // arm the trigger
      set_led_on (); // ARMED, turn on LED

      // tell data recorder to start
      sumpRunning = 1;
      break;

    case SUMP_DESC:
    // special command for debugging
    case '2':
      DEBUG_SERIAL(println ("SUMP_DESC"));

      // device name string
      Serial.write(0x01);
      if (F_CPU == 96000000) {
        Serial.write("Teensy96");
      } else if (F_CPU == 120000000) {
        Serial.write("Teensy120");
      } else if (F_CPU == 72000000) {
        Serial.write("Teensy72");
      } else if (F_CPU == 48000000) {
        Serial.write("Teensy48");
      } else {
        Serial.write("Teensy");
      }
      Serial.write(0x00);
      // firmware version string
      Serial.write(0x02);
      Serial.write("beta1");
      Serial.write(0x00);
      // sample memory (4096)
      Serial.write(0x21);
      Serial.write(0x00);
      Serial.write(0x00);
      Serial.write(0x10);
      Serial.write(0x00);
      // sample rate (1MHz)
      Serial.write(0x23);
      Serial.write(0x00);
      Serial.write(0x0F);
      Serial.write(0x42);
      Serial.write(0x40);
      // number of probes (8)
      Serial.write(0x40);
      Serial.write(0x08);
      // protocol version (2)
      Serial.write(0x41);
      Serial.write(0x02);
      Serial.write(0x00);
      break;

    case SUMP_XON: // resume send data
      //   xflow=1;
      break;

    case SUMP_XOFF: // pause send data
      //   xflow=0;
      break;

    // special command for debugging
    case '1':
      DEBUG_SERIAL(write ("sumpSamples: "));
      DEBUG_SERIAL(println (sumpSamples));
      DEBUG_SERIAL(write ("sumpRequestedSamples: "));
      DEBUG_SERIAL(println (sumpRequestedSamples));
      DEBUG_SERIAL(write ("sumpDivisor: "));
      DEBUG_SERIAL(println (sumpDivisor, HEX));
      DEBUG_SERIAL(write ("sumpFrequency: "));
      DEBUG_SERIAL(println (sumpFrequency));
      DEBUG_SERIAL(write ("sumpClockTicks: "));
      DEBUG_SERIAL(println (sumpClockTicks));
      break;

    default: // 5-byte command

      sumpRX.command[0] = inByte; // store first command byte
      sumpRX.parameters = 4; // all long commands are 5 bytes, get 4 parameters
      sumpRX.parCnt = 0; // reset the parameter counter
      sumpRXstate = C_PARAMETERS;
      break;
  }
}

void processFiveByteCommand (byte command[])
{
  uint32_t divisor;

  switch (sumpRX.command[0]) {

    case SUMP_TRIG: // mask for bits to trigger on
      sumpTrigMask = sumpRX.command[1];
      break;

    case SUMP_TRIG_VALS: // value to trigger on
      sumpTrigValue = sumpRX.command[1];
      break;

    case SUMP_CNT:
      sumpSamples = sumpRX.command[2];
      sumpSamples <<= 8;
      sumpSamples |= sumpRX.command[1];
      sumpSamples = (sumpSamples + 1) * 4;

      sumpDelaySamples = sumpRX.command[4];
      sumpDelaySamples <<= 8;
      sumpDelaySamples |= sumpRX.command[3];
      sumpDelaySamples = (sumpDelaySamples + 1) * 4;

      // need this to get OLS client to line up trigger to time 0
////      sumpDelaySamples += 2;

      sumpRequestedSamples = sumpSamples;

      // prevent buffer overruns
      if (sumpSamples > (unsigned int) LA_SAMPLE_SIZE * 8) {
        sumpSamples = (unsigned int) LA_SAMPLE_SIZE * 8;
        sumpNumChannels = 1;
      } else if (sumpSamples > LA_SAMPLE_SIZE * 4) {
        sumpNumChannels = 1;
      } else if (sumpSamples > LA_SAMPLE_SIZE * 2) {
        sumpNumChannels = 2;
      } else if (sumpSamples > LA_SAMPLE_SIZE) {
        sumpNumChannels = 4;
      } else {
        sumpNumChannels = 8;
      }
      if (sumpDelaySamples > sumpSamples) {
        sumpDelaySamples = sumpSamples;
      }
      break;

    case SUMP_DIV:
      divisor = 0;
      divisor = sumpRX.command[3];
      divisor <<= 8;
      divisor |= sumpRX.command[2];
      divisor <<= 8;
      divisor |= sumpRX.command[1];

      // by setting device.dividerClockspeed = F_BUS, divisor is exactly equal to value to put in timer
      sumpDivisor = divisor;

      sumpFrequency = F_BUS / (divisor + 1);
      sumpClockTicks = F_CPU / sumpFrequency;

      break;
  }
}


void SUMPrecordData(void) {

  sumpVariableStruct sv;

  byte samplesPerByte = 1;
  byte sampleMask;
  byte sampleShift;
  
  // when using a trigger and less than 8 channels, need to store extra data due to non-byte boundaries
  // at beginning and end of buffer
  byte extraByte1 = 0;
  byte extraByte2 = 0;
  int firstExtraByte = 1;
  
  byte *startOfBuffer = logicData;

  byte *inputPtr;
  byte *endOfBuffer;
  byte *startPtr;

  // setup
    switch (sumpNumChannels) {
      case 1:
        samplesPerByte = 8;
        sampleMask = 0x01;
        sampleShift = 1;
        break;
      case 2:
        samplesPerByte = 4;
        sampleMask = 0x03;
        sampleShift = 2;
        break;
      case 4:
        samplesPerByte = 2;
        sampleMask = 0x0F;
        sampleShift = 4;
        break;
      default:
        samplesPerByte = 1;
        sampleMask = 0x0FF;
        sampleShift = 8;
        break;
    }

  sv.bufferSize = sumpSamples / samplesPerByte;

// temporary until logic is fixed
if (sumpTrigMask)
{
  sv.bufferSize += 8;
}
  
  endOfBuffer = startOfBuffer + sv.bufferSize;
  
  sv.delaySamples = sumpSamples - sumpDelaySamples;
  sv.delaySize = sv.delaySamples / samplesPerByte;
  
  // set pointer to beginning of data buffer
  startPtr = startOfBuffer;
  inputPtr = startPtr;

  sv.samplesPerByte = samplesPerByte;
  sv.startPtr = startPtr;
  sv.endOfBuffer = endOfBuffer;
  sv.startOfBuffer = startOfBuffer;
  sv.sampleMask = sampleMask;
  sv.sampleShift = sampleShift;
  sv.triggerCount = 0;

  // setup timer
  startTimer (sumpDivisor);

  if (sumpClockTicks <= 8)
  {
    sumpStrategy = STRATEGY_ASSEMBLY;
  }
  else
  {
    sumpStrategy = STRATEGY_NORMAL;

    recordLowSpeedData (sv);
//recordByteData (sv);
  }
  
  if (sumpRunning)
  {
     sendData (sv);
  }

  SUMPreset();
}


inline void waitForTimeout (void)
{
  #ifdef TIMING_DISCRETES     
    digitalWriteFast (TIMING_PIN_0, HIGH);
  #endif

  while (!timerExpired ());

  clearTimerFlag ();

  #ifdef TIMING_DISCRETES     
    digitalWriteFast (TIMING_PIN_0, LOW);
  #endif
}

void blinkled() {
  digitalWriteFast(LED_PIN, HIGH);
  delay(150);
  digitalWriteFast(LED_PIN, LOW);
  delay(100);
}

void blinkledFast() {
  digitalWriteFast(LED_PIN, HIGH);
  delay(10);
  digitalWriteFast(LED_PIN, LOW);
  delay(15);
}

inline void set_led_on () {
  digitalWriteFast (LED_PIN, HIGH);
}

inline void set_led_off () {
  digitalWriteFast (LED_PIN, LOW);
}

void sendData (struct sumpVariableStruct sv) {

   // number of pre-trigger samples beyond a byte boundary
   const int delayCount = sv.delaySamples % sv.samplesPerByte;

   // set unused channels to alternating 1's and 0's
   byte unusedValue = 0x55;

   byte value;
   int extraCount;
   int index;
   int numSamplesNotInBuffer;
   byte workingValue = 0;
   int workingCount;

  byte sampleMask = sv.sampleMask;
  byte sampleShift = sv.sampleShift;
  byte *inputPtr;

  // swap 4 bytes
  if (1)
  {
    byte temp;

    for (index = 0; index < sumpSamples / sv.samplesPerByte; index = index + 4)
    {
      temp = logicData[index];
      logicData[index] = logicData[index+3];
      logicData[index+3] = temp;
      temp = logicData[index+1];
      logicData[index+1] = logicData[index+2];
      logicData[index+2] = temp;
    }
  }
   
   if (0)
   {
      byte temp;

      for (index = 0; index < sumpSamples; index = index + 2)
      {
        temp = logicData[index];
        logicData[index] = logicData[index+1];
        logicData[index+1] = temp;
      }
   }

   // if samples were limited, send bogus data to indicate it is done.
   // Send these first since sent backwards in time
   for (index = sumpSamples; index < sumpRequestedSamples; index = index + 2)
   {
      // send alternating 1's and 0's
      Serial.write(0x55);
      Serial.write(0xAA);
   }

   workingCount = 0;
   
   // number of samples in the 2 extra words
   extraCount = sv.samplesPerByte * 2;

   // if using a trigger
//   if (sv.triggerCount >= 0)
if (0)
   {
      // Two extra words were sampled to account for triggering in the middle
      // of a byte. This logic determines how much of the extra data is needed
      numSamplesNotInBuffer = sv.samplesPerByte + sv.triggerCount - delayCount;

      // send the extra data
      while (extraCount > 0)
      {
         if (extraCount > sv.samplesPerByte)
         {
            workingValue = sv.extraByte2;
         } else {
            workingValue = sv.extraByte1;
         }
         for (index = 0; index < sv.samplesPerByte; index++)
         {
            // count backwards through the extra samples
            extraCount--;
            // until it gets to the ones it needs to send
            if (extraCount < numSamplesNotInBuffer)
            {
               value = (unusedValue & ~sampleMask) + (workingValue & sampleMask);
               Serial.write(value);
               workingCount++;
            }
            workingValue >>= sampleShift; // shift to next value
            unusedValue = ~unusedValue; // toggle 1's and 0's
         }
      }
   }

   inputPtr = sv.startPtr - 1;

   // adjust for circular buffer
   if (inputPtr >= sv.endOfBuffer) {
      inputPtr = sv.startOfBuffer;
   }

   // send back to SUMP, backwards
   while (1)
   {
      // account for wrap-around of circular buffer
      if (inputPtr < sv.startOfBuffer) {
         inputPtr = inputPtr + sv.bufferSize;
      }

      workingValue = *inputPtr;

         for (index = 0; index < sv.samplesPerByte; index++)
         {
            value = (unusedValue & ~sampleMask) + (workingValue & sampleMask);
            Serial.write(value);

            workingCount++;
            workingValue >>= sampleShift; // shift to next value
            unusedValue = ~unusedValue;   // toggle 1's and 0's

            // if done
            if (workingCount == sumpSamples)
            {
               break;
            }
         }

      // if done
      if (workingCount == sumpSamples)
      {
         break;
      }

      inputPtr--;
   }
}

///////////
//
// PIT Timer routines
//
//////////
inline void clearTimerFlag (void)
{
  TIMER_FLAG_REGISTER = PIT_TFLG_TIF;
}

inline bool timerExpired (void)
{
  return TIMER_FLAG_REGISTER;
}

void startTimer (uint32_t busTicks)
{
  // Enable PIT clock
  SIM_SCGC6 |= SIM_SCGC6_PIT;

  // Enable the PIT module (turn off disable)
  PIT_MCR &= ~PIT_MCR_MDIS;

  // Disable timer
  TIMER_CONTROL_REGISTER = 0;

  // Load value
  TIMER_LOAD_VALUE_REGISTER = busTicks;

  // Enable timer
  TIMER_CONTROL_REGISTER |= PIT_TCTRL_TEN;
}

