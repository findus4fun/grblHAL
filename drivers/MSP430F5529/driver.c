/*
  driver.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Driver code for Texas Instruments MSP430F5529 16-bit processor

  Part of Grbl

  Copyright (c) 2016-2018 Terje Io
  Copyright (c) 2011-2015 Sungeun K. Jeon
  Copyright (c) 2009-2011 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <msp430.h>

#include "GRBL\grbl.h"
#include "driver.h"
#include "eeprom.h"
#include "serial.h"
//#include "usermcodes.h"
//#include "keypad.h"

static volatile bool ms_delay = false;
static volatile uint16_t debounce_count = 0;
static bool pwmEnabled = false, IOInitDone = false, busy = false;
static axes_signals_t step_port_invert, dir_port_invert, next_step_outbits;
static spindle_pwm_t spindle_pwm;
static uint16_t step_pulse_ticks;
static void (*delayCallback)(void) = 0;

static uint_fast16_t spindleSetSpeed (uint_fast16_t pwm_value);

// Inverts the probe pin state depending on user settings and probing cycle mode.
static uint8_t probe_invert;

static void driver_delay_ms (uint32_t ms, void (*callback)(void))
{
    if((ms_delay = ms > 0)) {
        SYSTICK_TIMER_CCR0 = ms;
        SYSTICK_TIMER_CTL |= TACLR|MC0;
        if(!(delayCallback = callback))
            while(ms_delay);
    } else if(callback)
        callback();
}

// Enable/disable stepper motors
static void stepperEnable (axes_signals_t enable)
{
    enable.mask ^= settings.stepper_enable_invert.mask;

    if(enable.x)
        STEPPERS_DISABLE_OUT_XY &= ~STEPPERS_DISABLE_PIN_XY;
    else
        STEPPERS_DISABLE_OUT_XY |= STEPPERS_DISABLE_PIN_XY;

    if(enable.z)
        STEPPERS_DISABLE_OUT_Z &= ~STEPPERS_DISABLE_PIN_Z;
    else
        STEPPERS_DISABLE_OUT_Z |= STEPPERS_DISABLE_PIN_Z;
}

// Starts stepper driver ISR timer and forces a stepper driver interrupt callback
static void stepperWakeUp ()
{
    stepperEnable((axes_signals_t){AXES_BITMASK});

    STEPPER_TIMER_CCR0 = 0xFFFF;        // Set a long initial delay,
    STEPPER_TIMER_CTL |= TACLR|MC0;     // start stepper ISR timer in up mode
    hal.stepper_interrupt_callback();   // and start the show
}

// Disables stepper driver interrupts
static void stepperGoIdle (void)
{
    STEPPER_TIMER_CTL &= ~(MC0|MC1);
}

// Sets up stepper driver interrupt timeout, AMASS version
static void stepperCyclesPerTick (uint32_t cycles_per_tick)
{
    STEPPER_TIMER_CTL |= TACLR; // start in up mode
    STEPPER_TIMER_CCR0 = cycles_per_tick < (1UL << 16) /*< 65536 (4.1ms @ 16MHz)*/ ? cycles_per_tick : 0xFFFF /*Just set the slowest speed possible.*/;
}

// Sets up stepper driver interrupt timeout, "Normal" version
static void stepperCyclesPerTickPrescaled (uint32_t cycles_per_tick)
{
    // Set timer prescaling for normal step generation
    if (cycles_per_tick < (1UL << 16)) { // < 65536  (2.6ms @ 25MHz)
        STEPPER_TIMER_EX0 = TAIDEX_0;  // DIV 1
        STEPPER_TIMER_CTL &= ~(ID0|ID1); // DIV 1
    } else if (cycles_per_tick < (1UL << 19)) { // < 524288 (32.8ms@16MHz)
        STEPPER_TIMER_EX0 = TAIDEX_0; // DIV 1
        STEPPER_TIMER_CTL |= ID0|ID1; // DIV 8
        cycles_per_tick = cycles_per_tick >> 3;
    } else {
        STEPPER_TIMER_EX0 = TAIDEX_7; // DIV 8
        STEPPER_TIMER_CTL &= ID0|ID1; // DIV 8
        cycles_per_tick = cycles_per_tick >> 6;
    }
    STEPPER_TIMER_CCR0 = cycles_per_tick < (1UL << 16) ? cycles_per_tick : 0xFFFF;
    STEPPER_TIMER_CTL |= TACLR|MC0;
}

// Set stepper pulse output pins
// NOTE: step_outbits are: bit0 -> X, bit1 -> Y, bit2 -> Z, needs to be mapped to physical pins by bit shifting or other means
 static void stepperSetStepOutputs (axes_signals_t step_outbits)
{
    STEP_PORT_OUT = (STEP_PORT_OUT & ~HWSTEP_MASK) | (step_outbits.value ^ step_port_invert.value) << 1;
}

// Set stepper direction output pins
// NOTE1: step_outbits are: bit0 -> X, bit1 -> Y, bit2 -> Z, needs to be mapped to physical pins by bit shifting or other means
 static void stepperSetDirOutputs (axes_signals_t dir_outbits)
{
    DIRECTION_PORT_OUT = (DIRECTION_PORT_OUT & ~HWDIRECTION_MASK) | (dir_outbits.value ^ dir_port_invert.value);
}

// Sets stepper direction and pulse pins and starts a step pulse
// When delayed pulse the step register is written in the step delay interrupt handler
static void stepperPulseStart (stepper_t *stepper)
{
    static uint_fast16_t current_pwm = 0;

    if(stepper->spindle_pwm != current_pwm)
        current_pwm = spindleSetSpeed(stepper->spindle_pwm);

    stepperSetDirOutputs(stepper->dir_outbits);
    stepperSetStepOutputs(stepper->step_outbits);

    PULSE_TIMER_CTL |= TACLR|MC0;
}

// Sets stepper direction and pulse pins and starts a step pulse with and initial delay
static void stepperPulseStartDelayed (stepper_t *stepper)
{
    static uint_fast16_t current_pwm = 0;

    if(stepper->spindle_pwm != current_pwm)
        current_pwm = spindleSetSpeed(stepper->spindle_pwm);

    stepperSetDirOutputs(stepper->dir_outbits);
    next_step_outbits = stepper->step_outbits; // Store out_bits
    PULSE_TIMER_CTL |= TACLR|MC0;
}

// Enable/disable limit pins interrupt
static void limitsEnable (bool on)
{
    if (on && settings.flags.hard_limit_enable)
        LIMIT_PORT_IE |= HWLIMIT_MASK; // Enable Pin Change Interrupt
    else
        LIMIT_PORT_IE &= ~HWLIMIT_MASK; // Disable Pin Change Interrupt
}

// Returns limit state as an axes_signals_t variable.
// Each bitfield bit indicates an axis limit, where triggered is 1 and not triggered is 0.
inline static axes_signals_t limitsGetState()
{
    axes_signals_t signals = {0};
    uint8_t flags = LIMIT_PORT_IN;

    signals.x = (flags & X_LIMIT_PIN) == X_LIMIT_PIN;
    signals.y = (flags & Y_LIMIT_PIN) == Y_LIMIT_PIN;
    signals.z = (flags & Z_LIMIT_PIN) == Z_LIMIT_PIN;

    if (settings.limit_invert.value)
        signals.value ^= settings.limit_invert.value;

    return signals;
}

// Returns limit state as a control_signals_t variable.
// Each bitfield bit indicates a control signal, where triggered is 1 and not triggered is 0.
inline static control_signals_t systemGetState (void)
{
    uint8_t flags = CONTROL_PORT_IN & HWCONTROL_MASK;
    control_signals_t signals = {0};

    if(flags & RESET_PIN)
        signals.reset = On;
    else if(flags & SAFETY_DOOR_PIN)
        signals.safety_door_ajar = On;
    else if(flags & FEED_HOLD_PIN)
        signals.feed_hold = On;
    else if(flags & CYCLE_START_PIN)
        signals.cycle_start = On;

    if(settings.control_invert.value)
        signals.value ^= settings.control_invert.value;

    return signals;
}

// Sets up the probe pin invert mask to
// appropriately set the pin logic according to setting for normal-high/normal-low operation
// and the probing cycle modes for toward-workpiece/away-from-workpiece.
static void probeConfigureInvertMask(bool is_probe_away)
{
  probe_invert = settings.flags.invert_probe_pin ? 0 : PROBE_PIN;

  if (is_probe_away)
      probe_invert ^= PROBE_PIN;
}

// Returns the probe pin state. Triggered = true.
bool probeGetState (void)
{
    return ((PROBE_PORT_IN & PROBE_PIN) ^ probe_invert) != 0;
}

// Static spindle (off, on cw & on ccw)

inline static void spindleOff (void)
{
    if(settings.spindle_invert.on)
        SPINDLE_ENABLE_OUT |= SPINDLE_ENABLE_PIN;
    else
        SPINDLE_ENABLE_OUT &= ~SPINDLE_ENABLE_PIN;
}

inline static void spindleOn (void)
{
    if(settings.spindle_invert.on)
        SPINDLE_ENABLE_OUT &= ~SPINDLE_ENABLE_PIN;
    else
        SPINDLE_ENABLE_OUT |= SPINDLE_ENABLE_PIN;
}

inline static void spindleDir (bool ccw)
{
    if(hal.driver_cap.spindle_dir) {
        if(ccw ^ settings.spindle_invert.ccw)
            SPINDLE_DIRECTION_OUT |= SPINDLE_DIRECTION_PIN;
        else
            SPINDLE_DIRECTION_OUT &= ~SPINDLE_DIRECTION_PIN;
    }
}

// Starts or stops spindle
static void spindleSetState (spindle_state_t state, float rpm, uint8_t speed_ovr)
{
    if (!state.on)
        spindleOff();
    else {
       spindleDir(state.ccw);
       spindleOn();
    }
}

// Variable spindle control functions

// Spindle speed to PWM conversion. Keep routine small and efficient.
static uint_fast16_t spindleComputePWMValue (float rpm, uint8_t speed_ovr)
{
    uint_fast16_t pwm_value;

    rpm *= (0.010f * speed_ovr); // Scale by spindle speed override value.
    // Calculate PWM register value based on rpm max/min settings and programmed rpm.
    if ((settings.rpm_min >= settings.rpm_max) || (rpm >= settings.rpm_max)) {
        // No PWM range possible. Set simple on/off spindle control pin state.
        sys.spindle_rpm = settings.rpm_max;
        pwm_value = spindle_pwm.max_value - 1;
    } else if (rpm <= settings.rpm_min) {
        if (rpm == 0.0f) { // S0 disables spindle
            sys.spindle_rpm = 0.0f;
            pwm_value = spindle_pwm.off_value;
        } else { // Set minimum PWM output
            sys.spindle_rpm = settings.rpm_min;
            pwm_value = spindle_pwm.min_value;
        }
    } else {
        // Compute intermediate PWM value with linear spindle speed model.
        // NOTE: A nonlinear model could be installed here, if required, but keep it VERY light-weight.
        sys.spindle_rpm = rpm;
        pwm_value = (uint32_t)floorf((rpm - settings.rpm_min) * spindle_pwm.pwm_gradient) + spindle_pwm.min_value;
        if(pwm_value >= spindle_pwm.max_value)
            pwm_value = spindle_pwm.max_value - 1;
    }

    return pwm_value;
}

// Sets spindle speed
static uint_fast16_t spindleSetSpeed (uint_fast16_t pwm_value)
{
    if (pwm_value == hal.spindle_pwm_off) {
        pwmEnabled = false;
        if(settings.flags.spindle_disable_with_zero_speed)
            spindleOff();
        PWM_TIMER_CCTL1 = 0;
    } else {
        if(!pwmEnabled)
            spindleOn();
        pwmEnabled = true;
        PWM_TIMER_CCR1 = pwm_value;
        PWM_TIMER_CCTL1 = OUTMOD_2;
    }

    return pwm_value;
}

// Starts or stops spindle
static void spindleSetStateVariable (spindle_state_t state, float rpm, uint8_t speed_ovr)
{
    if (!state.on || rpm == 0.0f) {
        spindleSetSpeed(hal.spindle_pwm_off);
        spindleOff();
    } else {
        spindleDir(state.ccw);
        spindleSetSpeed(spindleComputePWMValue(rpm, speed_ovr));
    }
}

// Returns spindle state in a spindle_state_t variable
static spindle_state_t spindleGetState (void)
{
    spindle_state_t state = {0};

    state.on = pwmEnabled || (SPINDLE_ENABLE_In & SPINDLE_ENABLE_PIN) != 0;
    state.ccw = hal.driver_cap.spindle_dir && (SPINDLE_DIRECTION_IN & SPINDLE_DIRECTION_PIN) != 0;
    state.value ^= settings.spindle_invert.mask;

    return state;
}

// end spindle code

// Starts/stops coolant (and mist if enabled)
static void coolantSetState (coolant_state_t mode)
{
    mode.value ^= settings.coolant_invert.mask;

    if(mode.flood)
        COOLANT_FLOOD_OUT |= COOLANT_FLOOD_PIN;
    else
        COOLANT_FLOOD_OUT &= ~COOLANT_FLOOD_PIN;

    if(mode.mist)
        COOLANT_MIST_OUT |= COOLANT_MIST_PIN;
    else
        COOLANT_MIST_OUT &= ~COOLANT_MIST_PIN;
}

// Returns coolant state in a coolant_state_t variable
static coolant_state_t coolantGetState (void)
{
    coolant_state_t state = {0};

    state.flood = (COOLANT_FLOOD_IN & COOLANT_FLOOD_PIN) != 0;
    state.mist  = (COOLANT_MIST_IN & COOLANT_MIST_PIN) != 0;

    if(settings.coolant_invert.mask)
        state.value ^= settings.coolant_invert.mask;

    return state;
}

static void showMessage (const char *msg)
{
    hal.serial_write_string("[MSG:");
    hal.serial_write_string(msg);
    hal.serial_write_string("]\r\n");
}

// Helper functions for setting/clearing/inverting individual bits atomically (uninterruptable)
static void bitsSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    _DINT();
    *ptr |= bits;
    _EINT();
}

static uint_fast16_t bitsClearAtomic (volatile uint_fast16_t *ptr, uint_fast16_t bits)
{
    _DINT();
    uint_fast16_t prev = *ptr;
    *ptr &= ~bits;
    _EINT();
    return prev;
}

static uint_fast16_t valueSetAtomic (volatile uint_fast16_t *ptr, uint_fast16_t value)
{
    _DINT();
    uint_fast16_t prev = *ptr;
    *ptr = value;
    _EINT();
    return prev;
}

// Configures perhipherals when settings are initialized or changed
static void settings_changed (settings_t *settings)
{
    step_port_invert = settings->step_invert;
    dir_port_invert = settings->dir_invert;

    spindle_pwm.period = (uint32_t)(3125000 / settings->spindle_pwm_freq);
    spindle_pwm.off_value = (uint32_t)(spindle_pwm.period * settings->spindle_pwm_off_value / 100.0f);
    spindle_pwm.min_value = (uint32_t)(spindle_pwm.period * settings->spindle_pwm_min_value / 100.0f);
    spindle_pwm.max_value = (uint32_t)(spindle_pwm.period * settings->spindle_pwm_max_value / 100.0f);
    spindle_pwm.pwm_gradient = (float)(spindle_pwm.max_value - spindle_pwm.min_value) / (settings->rpm_max - settings->rpm_min);

    hal.spindle_pwm_off = spindle_pwm.off_value;

    if(IOInitDone) {

        stepperEnable(settings->stepper_deenergize);

        step_pulse_ticks = settings->pulse_microseconds * 5 - 1;
        if(settings->pulse_delay_microseconds) {
            hal.stepper_pulse_start = &stepperPulseStartDelayed;
            PULSE_TIMER_CCR1 = settings->pulse_delay_microseconds * 5;
            PULSE_TIMER_CCR0 = step_pulse_ticks + PULSE_TIMER_CCR1;
            PULSE_TIMER_CCTL1 |= CCIE;                   // Enable CCR1 interrupt
        } else {
            PULSE_TIMER_CCTL1 &= ~CCIE;                  // Disable CCR1 interrupt
            hal.stepper_pulse_start = &stepperPulseStart;
            PULSE_TIMER_CCR0 = step_pulse_ticks;
        }

        if(hal.driver_cap.variable_spindle) {
            PWM_TIMER_CCR0 = spindle_pwm.period;
            PWM_TIMER_CCTL1 = 0;            // Set PWM output low and
            PWM_TIMER_CTL |= TACLR|MC0|MC1; // start PWM timer (with no pulse output)
        }

        /*************************
         *  Control pins config  *
         *************************/

        control_signals_t control_ies;
        control_ies.mask = ~(settings->control_disable_pullup.mask ^ settings->control_invert.mask);

        CONTROL_PORT_IE &= ~HWCONTROL_MASK;         // Disable control pins change interrupt
        CONTROL_PORT_DIR &= ~HWCONTROL_MASK;         // Disable control pins change interrupt

        if(settings->control_disable_pullup.cycle_start)
            CONTROL_PORT_OUT &= ~CYCLE_START_PIN;
        else
            CONTROL_PORT_OUT |= CYCLE_START_PIN;

        if(settings->control_disable_pullup.feed_hold)
            CONTROL_PORT_OUT &= ~FEED_HOLD_PIN;
        else
            CONTROL_PORT_OUT |= FEED_HOLD_PIN;

        if(settings->control_disable_pullup.reset)
            CONTROL_PORT_OUT &= ~RESET_PIN;
        else
            CONTROL_PORT_OUT |= RESET_PIN;

        if(settings->control_disable_pullup.safety_door_ajar)
            CONTROL_PORT_OUT &= ~SAFETY_DOOR_PIN;
        else
             CONTROL_PORT_OUT |= SAFETY_DOOR_PIN;

        if(control_ies.cycle_start)
            CONTROL_PORT_IES &= ~CYCLE_START_PIN;
        else
            CONTROL_PORT_IES |= CYCLE_START_PIN;

        if(control_ies.feed_hold)
            CONTROL_PORT_IES &= ~FEED_HOLD_PIN;
        else
            CONTROL_PORT_IES |= FEED_HOLD_PIN;

        if(control_ies.reset)
            CONTROL_PORT_IES &= ~RESET_PIN;
        else
            CONTROL_PORT_IES |= RESET_PIN;

        if(control_ies.safety_door_ajar)
            CONTROL_PORT_IES &= ~SAFETY_DOOR_PIN;
        else
            CONTROL_PORT_IES |= SAFETY_DOOR_PIN;

        CONTROL_PORT_REN |= HWCONTROL_MASK;     // Enable pull-ups/pull-down,
        CONTROL_PORT_IFG &= ~HWCONTROL_MASK;    // clear any pending interrupt
        CONTROL_PORT_IE |= HWCONTROL_MASK;      // and enable control pins change interrupt

        /***********************
         *  Limit pins config  *
         ***********************/

        axes_signals_t limit_ies;

        limit_ies.mask = ~(settings->limit_disable_pullup.mask ^ settings->limit_invert.mask);

         if(settings->limit_disable_pullup.x)
             LIMIT_PORT_OUT &= ~X_LIMIT_PIN;
         else
             LIMIT_PORT_OUT |= X_LIMIT_PIN;

         if(settings->limit_disable_pullup.y)
             LIMIT_PORT_OUT &= ~Y_LIMIT_PIN;
         else
             LIMIT_PORT_OUT |= Y_LIMIT_PIN;

         if(settings->limit_disable_pullup.z)
             LIMIT_PORT_OUT &= ~Z_LIMIT_PIN;
         else
             LIMIT_PORT_OUT |= Z_LIMIT_PIN;

         if(limit_ies.x)
             LIMIT_PORT_IES &= ~X_LIMIT_PIN;
         else
             LIMIT_PORT_IES |= X_LIMIT_PIN;

         if(limit_ies.y)
             LIMIT_PORT_IES &= ~Y_LIMIT_PIN;
         else
             LIMIT_PORT_IES |= Y_LIMIT_PIN;

         if(limit_ies.z)
             LIMIT_PORT_IES &= ~Z_LIMIT_PIN;
         else
             LIMIT_PORT_IES |= Z_LIMIT_PIN;

         LIMIT_PORT_REN |= HWLIMIT_MASK;

         /**********************
          *  Probe pin config  *
          **********************/

          if(hal.driver_cap.probe_pull_up)
              PROBE_PORT_OUT |= PROBE_PIN;
          else
              PROBE_PORT_OUT &= ~PROBE_PIN;
          PROBE_PORT_REN |= PROBE_PIN;

    }
}

// Initializes MCU peripherals for Grbl use
static bool driver_setup (settings_t *settings)
{
    /******************
     *  Stepper init  *
     ******************/

    STEP_PORT_DIR |= HWSTEP_MASK;
    DIRECTION_PORT_DIR |= HWDIRECTION_MASK;
    STEPPERS_DISABLE_DIR_XY |= STEPPERS_DISABLE_PIN_XY;
    STEPPERS_DISABLE_DIR_Z |= STEPPERS_DISABLE_PIN_Z;

    // Configure stepper driver timer

    STEPPER_TIMER_EX0 = TAIDEX_0; //
    STEPPER_TIMER_CTL &= ~(ID0|ID1|TAIFG);
    STEPPER_TIMER_CTL |= TACLR|TASSEL1;
    STEPPER_TIMER_CCTL0 |= CCIE;

    // Configure step pulse timer

    PULSE_TIMER_EX0 |= TAIDEX_4; // DIV 5
    PULSE_TIMER_CTL |= TACLR|TASSEL1; // for 0.2uS per count
    PULSE_TIMER_CCTL0 |= CCIE;

//    if(hal.driver_cap.step_pulse_delay)
//        PULSE_TIMER_CCTL1 |= CCIE;

   /****************************
    *  Software debounce init  *
    ****************************/

    if(hal.driver_cap.software_debounce)
        WDTCTL = WDT_ADLY_16;   // Watchdog timeout ~16ms

   /***********************
    *  Control pins init  *
    ***********************/

    CONTROL_PORT_DIR &= ~HWCONTROL_MASK;

   /*********************
    *  Limit pins init  *
    *********************/

    LIMIT_PORT_DIR &= ~HWLIMIT_MASK;

   /********************
    *  Probe pin init  *
    ********************/

    PROBE_PORT_DIR &= ~PROBE_PIN;
    if(hal.driver_cap.probe_pull_up) {
        PROBE_PORT_OUT |= PROBE_PIN;
        PROBE_PORT_REN |= PROBE_PIN;
    }

    /***********************
    *  Coolant pins init  *
    ***********************/

    COOLANT_FLOOD_DIR |= COOLANT_FLOOD_PIN;
    COOLANT_MIST_DIR |= COOLANT_MIST_PIN;

    if(hal.driver_cap.amass_level == 0)
        hal.stepper_cycles_per_tick = &stepperCyclesPerTickPrescaled;

   /******************
    *  Spindle init  *
    ******************/

    SPINDLE_ENABLE_DIR |= SPINDLE_ENABLE_PIN;
    SPINDLE_DIRECTION_DIR |= SPINDLE_DIRECTION_PIN;

    if((hal.driver_cap.variable_spindle)) {
        PWM_PORT_DIR |= PWM_PIN;
        PWM_SEL = PWM_PIN;
        PWM_TIMER_CTL |= TASSEL1|ID0|ID1;
    } else
        hal.spindle_set_state = &spindleSetState;

#ifdef HAS_KEYPAD

   /*********************
    *  I2C KeyPad init  *
    *********************/

    keypad_setup();

#endif

  // Set defaults

    IOInitDone = settings->version == 13;

    settings_changed(settings);

    setSerialReceiveCallback(hal.protocol_process_realtime);
    spindleSetState((spindle_state_t){0}, spindle_pwm.off_value, DEFAULT_SPINDLE_RPM_OVERRIDE);
    coolantSetState((coolant_state_t){0});
    stepperSetDirOutputs((axes_signals_t){0});

    return IOInitDone;

}

// Initialize HAL pointers, setup serial comms and enable EEPROM
// NOTE: Grbl is not yet configured (from EEPROM data), driver_setup() will be called when done
bool driver_init (void)
{
    // Systick timer setup, uses ACLK / 32

    SYSTICK_TIMER_EX0 |= TAIDEX_3;
    SYSTICK_TIMER_CTL |= TACLR|ID0|ID1|TASSEL__ACLK; // for 1mS per count
    SYSTICK_TIMER_CCR0 = 1;
    SYSTICK_TIMER_CCTL0 |= CCIE;

    serialInit();

#ifdef HAS_EEPROM
    eepromInit();
#endif

    hal.info = "MSP430F5529";
    hal.driver_setup = driver_setup;
    hal.f_step_timer = 24000000;
    hal.rx_buffer_size = RX_BUFFER_SIZE;
    hal.delay_ms = driver_delay_ms;
    hal.settings_changed = settings_changed;

    hal.stepper_wake_up = stepperWakeUp;
    hal.stepper_go_idle = stepperGoIdle;
    hal.stepper_enable = stepperEnable;
    hal.stepper_set_outputs = stepperSetStepOutputs;
    hal.stepper_set_directions = stepperSetDirOutputs;
    hal.stepper_cycles_per_tick = stepperCyclesPerTick;
    hal.stepper_pulse_start = stepperPulseStart;

    hal.limits_enable = limitsEnable;
    hal.limits_get_state = limitsGetState;

    hal.coolant_set_state = coolantSetState;
    hal.coolant_get_state = coolantGetState;

    hal.probe_get_state = probeGetState;
    hal.probe_configure_invert_mask = probeConfigureInvertMask;

    hal.spindle_set_state = spindleSetStateVariable;
    hal.spindle_get_state = spindleGetState;
    hal.spindle_set_speed = spindleSetSpeed;
    hal.spindle_compute_pwm_value = spindleComputePWMValue;

    hal.system_control_get_state = systemGetState;

    hal.serial_read = serialGetC;
    hal.serial_write = serialPutC;
    hal.serial_write_string = serialWriteS;
    hal.serial_get_rx_buffer_available = serialRxFree;
    hal.serial_reset_read_buffer = serialRxFlush;
    hal.serial_cancel_read_buffer = serialRxCancel;

    hal.show_message = showMessage;

#ifdef HAS_EEPROM
    hal.eeprom.type = EEPROM_Physical;
    hal.eeprom.get_byte = eepromGetByte;
    hal.eeprom.put_byte = eepromPutByte;
    hal.eeprom.memcpy_to_with_checksum = eepromWriteBlockWithChecksum;
    hal.eeprom.memcpy_from_with_checksum = eepromReadBlockWithChecksum;
#else
    hal.eeprom.type = EEPROM_None;
#endif

    hal.set_bits_atomic = bitsSetAtomic;
    hal.clear_bits_atomic = bitsClearAtomic;
    hal.set_value_atomic = valueSetAtomic;

//    hal.userdefined_mcode_check = userMCodeCheck;
//    hal.userdefined_mcode_validate = userMCodeValidate;
//    hal.userdefined_mcode_execute = userMCodeExecute;

#ifdef HAS_KEYPAD
    hal.execute_realtime = process_keypress;
#endif

  // driver capabilities, used for announcing and negotiating (with Grbl) driver functionality
    hal.driver_cap.spindle_dir = On;
    hal.driver_cap.variable_spindle = On;
    hal.driver_cap.mist_control = On;
    hal.driver_cap.software_debounce = On;
    hal.driver_cap.step_pulse_delay = On;
    hal.driver_cap.amass_level = 3;
    hal.driver_cap.control_pull_up = On;
    hal.driver_cap.limits_pull_up = On;
    hal.driver_cap.probe_pull_up = On;

    __bis_SR_register(GIE);	// Enable interrupts

    // no need to move version check before init - compiler will fail any mismatch for existing entries
    return hal.version == 4;
}

/* interrupt handlers */

// Main stepper driver ISR
#pragma vector=STEPPER_TIMER0_VECTOR
__interrupt void stepper_driver_isr (void)
{
    if(!busy) {
        busy = true;
        _EINT();
        hal.stepper_interrupt_callback();
        busy = false;
    }
}

/* The Stepper Port Reset Interrupt: This interrupt handles the falling edge of the step
   pulse. This should always trigger before the next general stepper driver interrupt and independently
   finish, if stepper driver interrupts is disabled after completing a move.
   NOTE: Interrupt collisions between the serial and stepper interrupts can cause delays by
   a few microseconds, if they execute right before one another. Not a big deal, but can
   cause issues at high step rates if another high frequency asynchronous interrupt is
   added to Grbl.
*/
// This interrupt is enabled when Grbl sets the motor port bits to execute
// a step. This ISR resets the motor port after a short period (settings.pulse_microseconds)
// completing one step cycle.
// NOTE: MSP430 has a shared interrupt for match and timeout

#pragma vector=PULSE_TIMER0_VECTOR
__interrupt void stepper_pulse_isr (void)
{
    stepperSetStepOutputs(step_port_invert);
    PULSE_TIMER_CTL &= ~(MC0|MC1);
}

#pragma vector=PULSE_TIMER1_VECTOR
__interrupt void stepper_pulse_isr_delayed (void)
{
    if(PULSE_TIMER_IV == TA0IV_TACCR1) {
        stepperSetStepOutputs(next_step_outbits);
        PULSE_TIMER_CCR0 = PULSE_TIMER_R + step_pulse_ticks;
    }
}

#pragma vector=WDT_VECTOR
__interrupt void software_debounce_isr (void)
{
    if(!--debounce_count) {
        SFRIE1 &= ~WDTIE;
        axes_signals_t state = limitsGetState();
        if(state.value) //TODO: add check for limit swicthes having same state as when limit_isr were invoked?
            hal.limit_interrupt_callback(state);
    }
}

#pragma vector=CONTROL_PORT_VECTOR
__interrupt void control_isr (void)
{
    uint8_t iflags = CONTROL_PORT_IFG & HWCONTROL_MASK;

    if(iflags) {
        CONTROL_PORT_IFG &= ~iflags;
        hal.control_interrupt_callback(systemGetState());
    }
}

#pragma vector=LIMIT_PORT_VECTOR
__interrupt void limit_isr (void)
{
    uint16_t iflags = LIMIT_PORT_IFG & HWLIMIT_MASK;

    if(iflags) {

        LIMIT_PORT_IFG &= ~iflags;

        if(hal.driver_cap.software_debounce) {
            WDTCTL = WDT_ADLY_16;   // Set watchdog timeout to ~16ms
            SFRIE1 |= WDTIE;        // and enable interrupt
            debounce_count = 3;     // Debounce = 3x watchdog timeout
        } else
            hal.limit_interrupt_callback(limitsGetState());
    }
}

// Interrupt handler for 1 ms interval timer
#pragma vector=SYSTICK_TIMER0_VECTOR
__interrupt void systick_isr (void)
{
    ms_delay = false;
    SYSTICK_TIMER_CTL &= ~(MC0|MC1);
    if(delayCallback) {
        delayCallback();
        delayCallback = 0;
    }
}