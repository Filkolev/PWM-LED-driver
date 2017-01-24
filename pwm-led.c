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
#define LED_MAX_LEVEL_DEFAULT 5
#define PULSE_FREQUENCY_DEFAULT 100000 /* nanoseconds */
#define LOW 0
#define HIGH 1

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
static void led_ctrl_func(struct work_struct *work);

static void increase_led_brightness(void);
static void decrease_led_brightness(void);
static void do_nothing(void) { }
static void update_led_state(void);
static void validate_led_max_level(void);

/*
 * Data
 */
static int down_button_irq;
static int up_button_irq;

static struct timespec64 prev_down_button_irq;
static struct timespec64 prev_up_button_irq;
static struct timespec64 prev_led_switch;

static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);

static enum led_state led_state = OFF;
static enum event led_event = NONE;

static DECLARE_WORK(led_level_work, led_level_func);
static DECLARE_WORK(led_switch_work, led_ctrl_func);

static void (*fsm_functions[NUM_STATES][NUM_EVENTS])(void) = {
	{ do_nothing, increase_led_brightness, do_nothing },
	{ do_nothing, increase_led_brightness, decrease_led_brightness },
	{ do_nothing, do_nothing, decrease_led_brightness }
};

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

static int pulse_frequency = PULSE_FREQUENCY_DEFAULT;
module_param(pulse_frequency, int, S_IRUGO);
MODULE_PARM_DESC(pulse_frequency,
		"Frequency in nanoseconds of PWM (default = 100 000).");

static int led_max_level = LED_MAX_LEVEL_DEFAULT;
module_param(led_max_level, int, S_IRUGO);
MODULE_PARM_DESC(led_max_level,
		"Maximum brightness level of the LED (default = 5).");

static int __init pwm_led_init(void)
{
	int ret;

	validate_led_max_level();

	ret = setup_pwm_led_gpios();
	if (ret)
		goto out;

	ret = setup_pwm_led_irqs();
	if (ret)
		goto irq_err;

	getnstimeofday64(&prev_down_button_irq);
	getnstimeofday64(&prev_up_button_irq);
	getnstimeofday64(&prev_led_switch);

	schedule_work(&led_switch_work);
	pr_info("%s: PWM LED module loaded\n", MODULE_NAME);

	goto out;

irq_err:
	unset_pwm_led_gpios();
out:
	return ret;
}

static void __exit pwm_led_exit(void)
{
	cancel_work_sync(&led_level_work);
	cancel_work_sync(&led_switch_work);

	free_irq(down_button_irq, NULL);
	free_irq(up_button_irq, NULL);

	unset_pwm_led_gpios();

	pr_info("%s: PWM LED module unloaded\n", MODULE_NAME);
}

static void validate_led_max_level(void)
{
       if (led_max_level < LED_MIN_LEVEL)
               led_max_level = LED_MIN_LEVEL;
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
		gpio_direction_output(gpio, LOW);
	}

	return ret;
}

static void unset_pwm_led_gpios(void)
{
	gpio_free(down_button_gpio);
	gpio_free(up_button_gpio);
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
		millis_since_last_irq = ((long)interval.tv_sec * MSEC_PER_SEC) +
					(interval.tv_nsec / NSEC_PER_MSEC);

		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		prev_down_button_irq = now;
		led_event = DOWN;
	} else if (irq == up_button_irq) {
		interval = timespec64_sub(now, prev_up_button_irq);
		millis_since_last_irq = ((long)interval.tv_sec * MSEC_PER_SEC) +
					(interval.tv_nsec / NSEC_PER_MSEC);

		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		prev_up_button_irq = now;
		led_event = UP;
	}

	schedule_work(&led_level_work);
	return IRQ_HANDLED;
}

static void led_level_func(struct work_struct *work)
{
	int level, led_brightness_percent;

	fsm_functions[led_state][led_event]();
	update_led_state();

	level = atomic_read(&led_level);
	led_brightness_percent = 100 * level / led_max_level;

	pr_info("%s: LED brightness %d%% (level %d)\n",
		MODULE_NAME,
		led_brightness_percent,
		level);
}

static void update_led_state(void)
{
	int level;

	level = atomic_read(&led_level);
	if (level == LED_MIN_LEVEL) {
		led_state = OFF;
	} else if (level == led_max_level) {
		led_state = MAX;
	} else {
		led_state = ON;
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

static void led_ctrl_func(struct work_struct *work)
{
	int required_delay, nanos_since_led_switch, led_gpio_value, level;
	struct timespec64 now, diff;

	getnstimeofday64(&now);
	diff = timespec64_sub(now, prev_led_switch);
	nanos_since_led_switch = (diff.tv_sec * USEC_PER_SEC) + diff.tv_nsec;

	level = atomic_read(&led_level);
	if (level == LED_MIN_LEVEL || level == led_max_level) {
		gpio_set_value(led_gpio, level == LED_MIN_LEVEL ? LOW : HIGH);
		schedule_work(work);
		return;
	}

	led_gpio_value = gpio_get_value(led_gpio);
	if (led_gpio_value == LOW) {
		required_delay = pulse_frequency -
				(pulse_frequency * level / led_max_level);
	} else {
		required_delay = pulse_frequency * level / led_max_level;
	}

	if (nanos_since_led_switch >= required_delay) {
		gpio_set_value(led_gpio, !led_gpio_value);
		prev_led_switch = now;
	}

	schedule_work(work);
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
