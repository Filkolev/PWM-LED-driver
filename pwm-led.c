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
#define LED_MAX_LEVEL 1023
#define LED_LEVEL_STEP 1023

enum direction {
	INPUT,
	OUTPUT
};


/*
 * Function prototypes
 */
static int setup_pwm_led_gpios(void);
static int
setup_pwm_led_gpio(int gpio, const char *target, enum direction direction);

static void unset_pwm_led_gpios(void);
static int setup_pwm_led_irqs(void);
static irqreturn_t button_irq_handler(int irq, void *data);
static void led_level_func(struct work_struct *work);

/*
 * Data
 */
static int down_button_irq, up_button_irq;
static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);
static struct timespec64 prev_down_button_irq, prev_up_button_irq;

static DECLARE_WORK(led_level_work, led_level_func);

/*
 * Sysfs attributes
 */

/*
 * Params:
 * * level
 * * pulse width...
 * * button and led gpios
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

/* TODO Refactor this */
static int setup_pwm_led_irqs(void)
{
	int ret;

	ret = 0;
	down_button_irq = gpio_to_irq(down_button_gpio);
	if (down_button_irq < 0) {
		pr_err("%s: %s (%d): Failed to obtain IRQ for GPIO %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			down_button_gpio);
		return down_button_irq;
	}

	ret = request_irq(down_button_irq,
			button_irq_handler,
			IRQF_TRIGGER_RISING,
			"pwm-led-down-btn-handler",
			NULL);
	if (ret < 0) {
		pr_err("%s: %s (%d): Request IRQ failed for IRQ %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			down_button_irq);

		return ret;
	}

	up_button_irq = gpio_to_irq(up_button_gpio);
	if (up_button_irq < 0) {
		pr_err("%s: %s (%d): Failed to obtain IRQ for GPIO %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			up_button_gpio);
		free_irq(down_button_irq, NULL);
		return up_button_irq;
	}

	ret = request_irq(up_button_irq,
			button_irq_handler,
			IRQF_TRIGGER_RISING,
			"pwm-led-up-btn-handler",
			NULL);
	if (ret < 0) {
		pr_err("%s: %s (%d): Request IRQ failed for IRQ %d\n",
			MODULE_NAME,
			__func__,
			__LINE__,
			up_button_irq);
		free_irq(down_button_irq, NULL);
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

		//atomic_sub(LED_LEVEL_STEP, &led_level);
		atomic_set(&led_level, 0); /* TODO remove */
		prev_down_button_irq = now;
		pr_info("Down button pressed\n");
	} else if (irq == up_button_irq) {
		interval = timespec64_sub(now, prev_up_button_irq);
		millis_since_last_irq = interval.tv_nsec / NSEC_PER_MSEC;
		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		//atomic_add(LED_LEVEL_STEP, &led_level);
		atomic_set(&led_level, 1); /* TODO remove */
		prev_up_button_irq = now;
		pr_info("Up button pressed\n");
	}

	schedule_work(&led_level_work);
	return IRQ_HANDLED;
}

static void led_level_func(struct work_struct *work)
{
	int level;

	level = atomic_read(&led_level);
	gpio_set_value(led_gpio, level);
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
