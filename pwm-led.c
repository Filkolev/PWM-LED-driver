#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time64.h>

#define MODULE_NAME "pwm_led_module"

#define DOWN_BUTTON_GPIO 23
#define UP_BUTTON_GPIO 24
#define LED_GPIO 18

#define BUTTON_DEBOUNCE 200 /* milliseconds */

#define LED_MIN_LEVEL 0
#define LED_MAX_LEVEL 2
#define LED_LEVEL_STEP 1

enum direction {
	INPUT,
	OUTPUT
};

enum event {
	NONE,
	UP,
	DOWN,
	NUM_EVENTS
};

enum led_state {
	OFF,
	ON,
	MAX,
	NUM_STATES
};

/*
 * Function prototypes
 */
static int setup_pwm_led_gpios(void);
static int
setup_pwm_led_gpio(int gpio, const char *target, enum direction direction);

static void unset_pwm_led_gpios(void);
static int setup_pwm_led_irqs(void);
static int setup_pwm_led_irq(int gpio, int *irq);
static irqreturn_t button_irq_handler(int irq, void *data);
static void led_level_func(struct work_struct *work);

static void increase_led_brightness(void);
static void decrease_led_brightness(void);
static void do_nothing(void) { }
static void update_led_state(void);

/*
 * Data
 */
static int down_button_irq, up_button_irq;
static struct timespec64 prev_down_button_irq, prev_up_button_irq;

static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);
static enum led_state led_state = OFF;
static enum event led_event = NONE;

static DECLARE_WORK(led_level_work, led_level_func);

static void (*fsm_functions[NUM_STATES][NUM_EVENTS])(void) = {
	{ do_nothing, increase_led_brightness, do_nothing },
	{ do_nothing, increase_led_brightness, decrease_led_brightness },
	{ do_nothing, do_nothing, decrease_led_brightness }
};

/*
 * Sysfs attributes
 */

/*
 * Module params:
 * * pulse width...
 */
static int down_button_gpio = DOWN_BUTTON_GPIO;
module_param(down_button_gpio, int, S_IRUGO);
MODULE_PARM_DESC(down_button_gpio,
		"The GPIO where the down button is connected (default = 23).");

static int up_button_gpio = UP_BUTTON_GPIO;
module_param(up_button_gpio, int, S_IRUGO);
MODULE_PARM_DESC(up_button_gpio,
		"The GPIO where the up button is connected (default = 24).");

static int led_gpio = LED_GPIO;
module_param(led_gpio, int, S_IRUGO);
MODULE_PARM_DESC(led_gpio,
		"The GPIO where the LED is connected (default = 18).");

static int __init pwm_led_init(void)
{
	int ret;

	ret = 0;
	ret = setup_pwm_led_gpios();
	if (ret)
		goto out;

	ret = setup_pwm_led_irqs();
	if (ret)
		goto irq_err;

	pr_info("%s: PWM LED module loaded\n", MODULE_NAME);

	goto out;

irq_err:
	unset_pwm_led_gpios();
out:
	return ret;
}

static void __exit pwm_led_exit(void)
{
	free_irq(down_button_irq, NULL);
	free_irq(up_button_irq, NULL);
	unset_pwm_led_gpios();

	pr_info("%s: PWM LED module unloaded\n", MODULE_NAME);
}

static int setup_pwm_led_gpios(void)
{
	int ret;

	ret = setup_pwm_led_gpio(down_button_gpio, "down button", INPUT);
	if (ret)
		return ret;

	ret = setup_pwm_led_gpio(up_button_gpio, "up button", INPUT);
	if (ret)
		return ret;

	ret = setup_pwm_led_gpio(led_gpio, "led", OUTPUT);
	if (ret)
		return ret;

	return ret;
}

static int
setup_pwm_led_gpio(int gpio, const char *target, enum direction direction)
{
	int ret;

	ret = 0;
	if (!gpio_is_valid(gpio)) {
		pr_err("%s: %s (%d): Invalid GPIO for %s (%d)\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			target,
			gpio);
		return -EINVAL;
	}

	ret = gpio_request(gpio, "sysfs");
	if (ret < 0) {
		pr_err("%s: %s (%d): GPIO request failed for GPIO %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			gpio);
		return ret;
	}

	if (direction == INPUT) {
		gpio_direction_input(gpio);
	} else {
		gpio_direction_output(gpio, 0);
	}

	gpio_export(gpio, true);

	return ret;
}

static void unset_pwm_led_gpios(void)
{

	gpio_unexport(down_button_gpio);
	gpio_free(down_button_gpio);
	gpio_unexport(up_button_gpio);
	gpio_free(up_button_gpio);
	gpio_unexport(led_gpio);
	gpio_free(led_gpio);
}

static int setup_pwm_led_irqs(void)
{
	int ret;

	ret = setup_pwm_led_irq(down_button_gpio, &down_button_irq);
	if (ret)
		return ret;

	ret = setup_pwm_led_irq(up_button_gpio, &up_button_irq);
	if (ret) {
		free_irq(down_button_irq, NULL);
		return ret;
	}

	return ret;
}

static int setup_pwm_led_irq(int gpio, int *irq)
{
	int ret;

	ret = 0;
	*irq = gpio_to_irq(gpio);
	if (*irq < 0) {
		pr_err("%s: %s (%d): Failed to obtain IRQ for GPIO %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			gpio);
		return *irq;
	}

	ret = request_irq(*irq,
			button_irq_handler,
			IRQF_TRIGGER_RISING,
			"pwm-led-btn-handler",
			NULL);
	if (ret < 0) {
		pr_err("%s: %s (%d): Request IRQ failed for IRQ %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			*irq);

		return ret;
	}

	return ret;
}

static irqreturn_t button_irq_handler(int irq, void *data)
{
	struct timespec64 now, interval;
	long millis_since_last_irq;

	getnstimeofday64(&now);

	if (irq == down_button_irq) {
		interval = timespec64_sub(now, prev_down_button_irq);
		millis_since_last_irq = interval.tv_nsec / NSEC_PER_MSEC;
		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		prev_down_button_irq = now;
		led_event = DOWN;
		pr_info("Down button pressed\n");
	} else if (irq == up_button_irq) {
		interval = timespec64_sub(now, prev_up_button_irq);
		millis_since_last_irq = interval.tv_nsec / NSEC_PER_MSEC;
		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		prev_up_button_irq = now;
		led_event = UP;
		pr_info("Up button pressed\n");
	}

	schedule_work(&led_level_work);
	return IRQ_HANDLED;
}

static void led_level_func(struct work_struct *work)
{
	fsm_functions[led_state][led_event]();
	update_led_state();

	pr_info("led level after: %d\n", atomic_read(&led_level));
	/*
	 * TODO replace with setting of intervals for soft pwm, hrtimer will
	 * take care of the rest.
	 */
	gpio_set_value(led_gpio, atomic_read(&led_level) == 0 ? 0 : 1);
}

static void update_led_state(void)
{
	int level;

	level = atomic_read(&led_level);
	switch(level) {
	case LED_MAX_LEVEL:
		led_state = MAX;
		break;
	case LED_MIN_LEVEL:
		led_state = OFF;
		break;
	default:
		led_state = ON;
		break;
	}
}

static void increase_led_brightness(void)
{
	atomic_inc(&led_level);
}

static void decrease_led_brightness(void)
{
	atomic_dec(&led_level);
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
