#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

/*
 * Function prototypes
 */
static int setup_pwm_led_gpios(void);
static int setup_pwm_led_irqs(void);

/*
 * Data
 */

/*
 * Sysfs attributes
 */

/*
 * Params:
 * * level
 * * pulse width...
 */

static int __init pwm_led_init(void)
{
	int ret;

	ret = 0;
	return ret;
}

static void __exit pwm_led_exit(void)
{

}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
