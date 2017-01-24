# A Basic LED Driver with Software PWM

This driver uses software pulse-width modulation to control the brightness of a
LED. Two buttons are used to change the LED brightness level.

This driver is compiled and tested on Raspberry Pi 3 Model B running Raspbian
(Linux raspberrypi 4.4.41-v7+ #942). The LED and push-buttons are connected to
the Pi via a breadboard.

## Pulse-width Modulation (PWM)

An easy-to-understand explanation of what PWM is can be found
[here](https://learn.sparkfun.com/tutorials/pulse-width-modulation)

## Hardware PWM

The Raspberry Pi 3 has a pulse-width modulator. A version of this driver which
makes use of it can be accessed in the `hw-pwm` branch.

The driver in the `master` branch emulates PWM by alternating HIGH and LOW
signals to the LED; the time between switches depends on the required brightness
level.

## Using the Driver

The driver's binary is compiled nativelly on the Raspberry and is included in
the repository. It can be recompiled by issuing `make` in the source directory.

The driver should be loaded using the following command (as root):  
`insmod pwm-led.ko [down_button_gpio=<gpio>] [up_button_gpio=<gpio>]
[led_gpio=<gpio>] [pulse_frequency=<frequency>] [led_max_level=<level>]`

The module can be unloaded using this command (as root): `rmmod pwm-led`

### Module Parameters

* `down_button_gpio`, `up_button_gpio` and `led_gpio` represent the GPIOs where
the components are connected. GPIO numbers are given per the
[BCM numbering scheme](https://pinout.xyz/#).

* `pulse_frequency` represents the amount of time (in nanoseconds) for which the
proportion of LOW and HIGH signals sent to the LED is calculated. E.g. with
pulse width of 100 ms and requested LED brightness of 40%, 40 ms will be spent
sending HIGH signal and 60 ms will be spent sending a LOW signal to the LED.  
Default value is 100 000 nanoseconds (0.1 ms).

* `led_max_level` determines the number of brightness levels the driver will
support. E.g., with maximum level 2 there will be 3 distinct brightness levels -
0%, 50% and 100%. With maximum level 3 there will be 4 brightness levels - 0%,
~33%, ~66% and 100%.  
Default is 5 (meaning a step of 20%).

## Implementation Details

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
current LED level and pulse frequency to determine how long a LOW and a HIGH
signal should last. With 100 000 ns pulse frequency and 20% requested brightness
it will keep sending HIGH signal for about 20 000 ns and then keep sending LOW
signal for about 80 000 ns.
