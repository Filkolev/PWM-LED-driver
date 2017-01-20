#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time64.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define MODULE_NAME "pwm_led_module"

#define DOWN_BUTTON_GPIO 23
#define UP_BUTTON_GPIO 24
#define LED_GPIO 18

#define BUTTON_DEBOUNCE 200 /* milliseconds */

#define LED_MIN_LEVEL 0
#define LED_MAX_LEVEL 2
#define PULSE_FREQUENCY_DEFAULT (HZ / 100)
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
static void turn_led_on_func(struct work_struct *work);
static void turn_led_off_func(struct work_struct *work);

static void increase_led_brightness(void);
static void decrease_led_brightness(void);
static void do_nothing(void) { }
static void update_led_state(void);

static enum hrtimer_restart frequency_func(struct hrtimer *hrtimer);
static enum hrtimer_restart duty_func(struct hrtimer *hrtimer);

/*
 * Data
 */
static int down_button_irq, up_button_irq;
static struct timespec64 prev_down_button_irq, prev_up_button_irq;

static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);
static enum led_state led_state = OFF;
static enum event led_event = NONE;
static struct hrtimer frequency_timer, duty_timer;
static ktime_t frequency_delay;

static DECLARE_WORK(led_level_work, led_level_func);
static DECLARE_WORK(turn_led_on_work, turn_led_on_func);
static DECLARE_WORK(turn_led_off_work, turn_led_off_func);

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
		"The frequency at which the RPi sends a LOW signal (default = 100 HZ)");

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

	hrtimer_init(&frequency_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	frequency_timer.function = frequency_func;
	hrtimer_start(&frequency_timer, ktime_set(0, 0), HRTIMER_MODE_REL);

	hrtimer_init(&duty_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	duty_timer.function = duty_func;
	hrtimer_start(&duty_timer, ktime_set(0, 0), HRTIMER_MODE_REL);

	getnstimeofday64(&prev_down_button_irq);
	getnstimeofday64(&prev_up_button_irq);

	pr_info("%s: PWM LED module loaded\n", MODULE_NAME);

	goto out;

irq_err:
	unset_pwm_led_gpios();
out:
	return ret;
}

static void __exit pwm_led_exit(void)
{
	hrtimer_cancel(&frequency_timer);
	hrtimer_cancel(&duty_timer);
	cancel_work_sync(&led_level_work);
	cancel_work_sync(&turn_led_off_work);
	cancel_work_sync(&turn_led_on_work);
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
		millis_since_last_irq = ((long)interval.tv_sec * MSEC_PER_SEC) +
					(interval.tv_nsec / NSEC_PER_MSEC);
		if (millis_since_last_irq < BUTTON_DEBOUNCE)
			return IRQ_HANDLED;

		prev_down_button_irq = now;
		led_event = DOWN;
		pr_info("Down button pressed\n");
	} else if (irq == up_button_irq) {
		interval = timespec64_sub(now, prev_up_button_irq);
		millis_since_last_irq = ((long)interval.tv_sec * MSEC_PER_SEC) +
					(interval.tv_nsec / NSEC_PER_MSEC);
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

static enum hrtimer_restart frequency_func(struct hrtimer *hrtimer)
{
	ktime_t duty_cycle_delay;
	int duty_cycle_percent, duty_cycle_ms, level;

	frequency_delay = ktime_set(pulse_frequency / MSEC_PER_SEC,
					pulse_frequency * NSEC_PER_MSEC);

	schedule_work(&turn_led_off_work);

	level = atomic_read(&led_level);
	if (level != 0) {
		duty_cycle_percent = pulse_frequency * level / LED_MAX_LEVEL;
		duty_cycle_ms = (pulse_frequency - duty_cycle_percent) / pulse_frequency;
		duty_cycle_delay = ktime_set(duty_cycle_ms / MSEC_PER_SEC,
					duty_cycle_ms * NSEC_PER_MSEC);
		hrtimer_forward_now(&duty_timer, duty_cycle_delay);
		hrtimer_restart(&duty_timer);
	}

	hrtimer_forward_now(hrtimer, frequency_delay);
	return HRTIMER_RESTART;
}

static enum hrtimer_restart duty_func(struct hrtimer *hrtimer)
{
	int level;

	level = atomic_read(&led_level);
	if (level > 0) {
		schedule_work(&turn_led_on_work);
	}

	return HRTIMER_NORESTART;
}

static void turn_led_on_func(struct work_struct *work)
{
	gpio_set_value(led_gpio, HIGH);
}

static void turn_led_off_func(struct work_struct *work)
{
	gpio_set_value(led_gpio, LOW);
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
