# Basic LED Driver with Hardware PWM

This driver uses pulse-width modulation to control the brightness of a LED. Two
buttons are used to change the LED brightness level.

This driver is compiled and tested on Raspberry Pi 3 Model B running Raspbian
(Linux raspberrypi 4.4.41-v7+ #942). The LED and push-buttons are connected to
the Pi via a breadboard.

## Pulse-width Modulation (PWM)

An easy-to-understand explanation of what PWM is can be found
[here](https://learn.sparkfun.com/tutorials/pulse-width-modulation)

## Software PWM

The Raspberry Pi 3 has a pulse-width modulator. This driver makes use of it by
writing to the relevant registers.

The driver version in the `master` branch uses software PWM - it emulates PWM
by alternating HIGH and LOW signals to the LED; the time between switches
depends on the required brightness level.

## Using the Driver

The driver's binary is compiled nativelly on the Raspberry and is included in
the repository. It can be recompiled by issuing `make` in the source directory.

The driver should be loaded using the following command (as root):  
`insmod pwm-led.ko [down_button_gpio=<gpio>] [up_button_gpio=<gpio>]
[led_max_level=<level>]`

The module can be unloaded using this command (as root): `rmmod pwm-led`

### Module Parameters

* `down_button_gpio` and `up_button_gpio` represent the GPIOs where the
buttons are connected. GPIO numbers are given per the
[BCM numbering scheme](https://pinout.xyz/#).  
**NB:** Note that the Raspberry Pi 3 (as far as I was able to determine from
Internet sources) only allows PWM on GPIO 18, therefore the LED **must be
connected there** and placing it anywhere else will not work.

* `led_max_level` determines the number of brightness levels the driver will
support. E.g., with maximum level 2 there will be 3 distinct brightness levels -
0%, 50% and 100%. With maximum level 3 there will be 4 brightness levels - 0%,
~33%, ~66% and 100%.  
Default is 5 (meaning a step of 20%), maximum is 32.

## Physical Addresses of the PWM and PWM clocks

Note: The PWM is relatively slow. This is the reason why it's a good idea to
wait a little bit after writing to PWM registers. The driver uses a short delay
of 10 us after every read or write operation.

### PWM Registers

Extensive information about the PWM peripheral on the Raspberry is available in
the
[BCM2835 ARM Peripherals](https://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf#page=138)
document.

The most important detail which is missing from the document is the base address
of the PWM - it is at offset 0x20c000 from the peripherals' base address.

#### CTL

This is the control register for the PWM device. The only bit that is useful is
bit 0, PWEN1 (enable PWM channel 1); all other bits can be left with their
default values. Details are on pages
[142-143](https://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf#page=142).

#### STA

This is the PWM status register. It can be used to check whether an error has
occured. When channel 1 of the PWM is activated with FIFO disabled (as this
driver does) the expected value of the register is 514 - high bits 2 (EMPT1,
FIFO empty) and 9 (STA1, channel 1 state).

Occasionally bit 8 (BERR, bus error) will be high as well. Empirically, this
does not seem to have a negative effect and the driver keeps working as
expected.

#### RNG1

The value in this register is used as the range of PWM channel 1. It is left at
its default value of 32. This means that, effectively, the driver will only be
able to differenciate 33 brightness levels (0 - 32); the maximum led level is
therefore capped at 32.

#### DAT1

The value in this register is sent to the device using the PWM algorithm
explained in the BCM document. Values should be between 0 and 32, higher values
mean higher brightness levels. E.g. writing 16 here will turn the LED on with
50% brightness.

### PWM Clock Registers

In order to use the PWM peripheral it is also necessary to setup the PWM clocks
appropriately.

Two registers are available - clock control and clock divisors.

The offset of the PWM clock control from the peripherals' base is 0x1010a0. Many
online sources cite offset 0x101028 which is not correct for the Pi 3.  
The clock divisor is at offset 0x1010a4.

The BCM peripherals document does not contain information about these clock
registers, however, they are analogous to the GPIO Clocks described in
[Section 6.3, page 105](https://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf#page=105).

#### CTL

Every command written to the CTL register should include the password, 0x5a, at
bits 24-31.

Bit 5, KILL, is used to kill the clock - this is done initially before
re-enabling it.

When the clock is enabled, a 1 is written to bits 0-3 (to select the oscillator
as clock source) and bit 4 (ENAB) is set high.

#### DIV

Every command written to the DIV register should include the password, 0x5a, at
bits 24-31.

The integer and fractional part of the divisor are sent in bits 12-23 and 0-11
respectively.

## Implementation Details

### PWM Setup

With the information outlined in the previous section, setting up the PWM boils
down to the following steps:

* Reset the PWM clock registers
* Kill the clock (apparently necessary before enabling it)
* Set the DIV register
* Enable the clock through the CTL register
* Set the function of GPIO 18 to ALT5 (PWM0)
* Activate PWM channel 1 using the PWM CTL register as described above
* Write the appropriate value in DAT1 to control the LED brightness

### Interrupt Handler and Finite-State Machine

When one of the push-buttons is pressed, an interrupt handler processes the
received IRQ and sets the proper FSM event (UP or DOWN) depending on which
button was pressed. A work is scheduled to handle the actual LED level change.

In the work queued by the IRQ handler, the appropriate FSM function is called,
then the FSM state is updated. The FSM function increases or decreases the
current LED level or does nothing. The LED level is later translated to a
brightness level (proportion of LOW and HIGH signals) in a separate work
function.

### LED Control Work Function

The actual lighting of the LED is performed in this work function. It uses the
current LED level to determine what value to write to the DAT1 register.
