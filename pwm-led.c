#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time64.h>
#include <asm/io.h>

#define MODULE_NAME "pwm_led_module"

#define BCM2708_PERI_BASE 0x3f000000
#define GPIO_BASE (BCM2708_PERI_BASE + 0x200000)
#define GPIO_REGION_SIZE 0xc
#define PWM_BASE (BCM2708_PERI_BASE + 0x20c000)
#define PWM_CTL_OFFSET 0x0
#define PWM_DAT1_OFFSET 0x14
#define PWM_REGION_SIZE 0x18

#define ALT_FUNC_0 (1 << 2)

#define DOWN_BUTTON_GPIO 23
#define UP_BUTTON_GPIO 24
#define LED_GPIO 12

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

static void activate_pwm_channel(void);
static void deactivate_pwm_channel(void);

static void save_gpio_func_select(void);
static void restore_gpio_func_select(void);
static void gpio_select_func(int gpio, int new_value);

/*
 * Data
 */
static int down_button_irq, up_button_irq;
static struct timespec64 prev_down_button_irq, prev_up_button_irq, prev_led_switch;

static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);
static enum led_state led_state = OFF;
static enum event led_event = NONE;
static int func_select_initial_val, func_select_bit_offset, func_select_reg_offset;
static void __iomem *pwm_base, *gpio_base;

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
		"The GPIO where the LED is connected (default = 12).");

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

	ret = setup_pwm_led_gpios();
	if (ret)
		goto out;

	ret = setup_pwm_led_irqs();
	if (ret)
		goto irq_err;

	pwm_base = ioremap(PWM_BASE, PWM_REGION_SIZE);
	if (!pwm_base) {
		pr_err("%s: %s (%d): Error mapping PWM memory\n",
			MODULE_NAME,
			__func__,
			__LINE__);

		goto iomap_err;
	}

	gpio_base = ioremap(GPIO_BASE, GPIO_REGION_SIZE);
	if (!gpio_base) {
		pr_err("%s: %s (%d): Error mapping PWM memory\n",
			MODULE_NAME,
			__func__,
			__LINE__);

		iounmap(pwm_base);
		goto iomap_err;

	}

	activate_pwm_channel();

	getnstimeofday64(&prev_down_button_irq);
	getnstimeofday64(&prev_up_button_irq);
	getnstimeofday64(&prev_led_switch);

	func_select_reg_offset = 4 * (led_gpio / 10);
	func_select_bit_offset = (led_gpio % 10) * 3;
	save_gpio_func_select();
	gpio_select_func(led_gpio, ALT_FUNC_0);

	schedule_work(&led_switch_work);
	pr_info("%s: PWM LED module loaded\n", MODULE_NAME);

	goto out;

iomap_err:
	free_irq(down_button_irq, NULL);
	free_irq(up_button_irq, NULL);
irq_err:
	unset_pwm_led_gpios();
out:
	return ret;
}

static void __exit pwm_led_exit(void)
{
	cancel_work_sync(&led_level_work);
	cancel_work_sync(&led_switch_work);
	restore_gpio_func_select();
	deactivate_pwm_channel();
	iounmap(pwm_base);
	iounmap(gpio_base);
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

static void activate_pwm_channel(void)
{
	int pwm_ctl_value;

	pwm_ctl_value = ioread32(pwm_base + PWM_CTL_OFFSET);
	pwm_ctl_value |= 1;
	iowrite32(pwm_ctl_value, pwm_base + PWM_CTL_OFFSET);
}

static void deactivate_pwm_channel(void)
{
	int pwm_ctl_value;

	pwm_ctl_value = ioread32(pwm_base + PWM_CTL_OFFSET);
	pwm_ctl_value &= ~1;
	iowrite32(pwm_ctl_value, pwm_base + PWM_CTL_OFFSET);
}

static void save_gpio_func_select(void)
{
	int val;

	val = ioread32(gpio_base + func_select_reg_offset);
	func_select_initial_val = (val >> func_select_bit_offset) & 7;
}

static void restore_gpio_func_select(void)
{
	int val;

	val = ioread32(gpio_base + func_select_reg_offset);
	val &= ~(7 << func_select_bit_offset);
	val |= func_select_initial_val << func_select_bit_offset;
	iowrite32(val, gpio_base + func_select_reg_offset);
}

static void gpio_select_func(int gpio, int new_value)
{
	int val;

	val = ioread32(gpio_base + func_select_reg_offset);
	val &= ~(7 << func_select_bit_offset);
	val |= new_value << func_select_bit_offset;
	iowrite32(val, gpio_base + func_select_reg_offset);
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
	if (level == led_max_level) {
		led_state = MAX;
	} else if (level == LED_MIN_LEVEL) {
		led_state = OFF;
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
	int level, led_brightness;

	level = atomic_read(&led_level);
	led_brightness = 100 * level / led_max_level * 32 / 100;

	iowrite32(led_brightness, pwm_base + PWM_DAT1_OFFSET);

	schedule_work(work);
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
