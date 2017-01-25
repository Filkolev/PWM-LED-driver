#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time64.h>
#include <linux/delay.h>
#include <asm/io.h>

#define MODULE_NAME "pwm_led_module"

#define SHORT_WAIT_LENGTH 10 /* microseconds */

#define BCM2708_PERI_BASE 0x3f000000

#define GPIO_BASE (BCM2708_PERI_BASE + 0x200000)
#define GPIO_REGION_SIZE 0xc

#define PWM_BASE (BCM2708_PERI_BASE + 0x20c000)
#define PWM_CTL_OFFSET 0x0
#define PWM_STA_OFFSET 0x4
#define PWM_RNG1_OFFSET 0x10
#define PWM_DAT1_OFFSET 0x14
#define PWM_REGION_SIZE 0x18
#define PWM_ENABLE 1
#define PWM_DISABLE 0

#define PWM_CLK_BASE (BCM2708_PERI_BASE + 0x1010a0)
#define PWM_CLK_CTL_OFFSET 0x0
#define PWM_CLK_DIV_OFFSET 0x4
#define PWM_CLK_REGION_SIZE 0x8
#define PWM_CLK_PASSWORD 0x5a000000
#define CLK_KILL (1 << 5)
#define CLK_ENABLE (1 << 4)
#define CLK_SRC_OSCILLATOR (1 << 0)

#define DIVI_BITS_POS 12
#define DIVF_BITS_POS 0
#define DIVI_DEFAULT 35
#define DIVF_DEFAULT 0

#define ALT_FUNC_5 2

#define DOWN_BUTTON_GPIO 23
#define UP_BUTTON_GPIO 24
#define LED_GPIO 18 /* Fixed for hardware PWM */

#define BUTTON_DEBOUNCE 200 /* milliseconds */

#define LED_MIN_LEVEL 0
#define LED_MAX_LEVEL_DEFAULT 5

#define LOW 0
#define HIGH 1

#define LED_BRIGHTNESS_RANGE 32
#define REGISTER_WIDTH 4 /* bytes */

#define NUM_BITS_PER_GPIO_GPFSEL 3
#define NUM_GPIOS_GPFSEL 10

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

static void setup_pwm_clock(void);
static void activate_pwm_channel(void);
static void deactivate_pwm_channel(void);

static void save_gpio_func_select(void);
static void restore_gpio_func_select(void);
static void gpio_select_function(int gpio, int function_number);

static void dump_pwm_registers(void);

static int map_memory_regions(void);
static void short_wait(void);

static void reset_pwm_clocks(void);
static void kill_pwm_clock(void);
static void enable_pwm_clock(void);

static void set_pwm_clock_divisors(int integer_part, int fractional_part);

/*
 * Data
 */
static int down_button_irq;
static int up_button_irq;

static struct timespec64 prev_down_button_irq;
static struct timespec64 prev_up_button_irq;

static atomic_t led_level = ATOMIC_INIT(LED_MIN_LEVEL);

static enum led_state led_state = OFF;
static enum event led_event = NONE;

static int func_select_initial_val;
static int func_select_bit_offset;
static int func_select_reg_offset;

static void __iomem *gpio_base;
static void __iomem *pwm_base;
static void __iomem *pwm_clk;

static DECLARE_WORK(led_level_work, led_level_func);
static DECLARE_WORK(led_switch_work, led_ctrl_func);

static void (*fsm_functions[NUM_STATES][NUM_EVENTS])(void) = {
	{ do_nothing, increase_led_brightness, do_nothing },
	{ do_nothing, increase_led_brightness, decrease_led_brightness },
	{ do_nothing, do_nothing, decrease_led_brightness }
};

/*
 * Module parameters
 */
static int down_button_gpio = DOWN_BUTTON_GPIO;
module_param(down_button_gpio, int, S_IRUGO);
MODULE_PARM_DESC(down_button_gpio,
		"The GPIO where the down button is connected (default = 23).");

static int up_button_gpio = UP_BUTTON_GPIO;
module_param(up_button_gpio, int, S_IRUGO);
MODULE_PARM_DESC(up_button_gpio,
		"The GPIO where the up button is connected (default = 24).");

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

	ret = map_memory_regions();
	if (ret)
		goto iomap_err;

	setup_pwm_clock();

	func_select_reg_offset = REGISTER_WIDTH * (LED_GPIO / NUM_GPIOS_GPFSEL);
	func_select_bit_offset = (LED_GPIO % NUM_GPIOS_GPFSEL) *
				NUM_BITS_PER_GPIO_GPFSEL;

	save_gpio_func_select();
	gpio_select_function(LED_GPIO, ALT_FUNC_5);

	activate_pwm_channel();
	short_wait();

	getnstimeofday64(&prev_down_button_irq);
	getnstimeofday64(&prev_up_button_irq);

	dump_pwm_registers();

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
	short_wait();

	reset_pwm_clocks();

	iounmap(pwm_base);
	iounmap(gpio_base);
	iounmap(pwm_clk);

	free_irq(down_button_irq, NULL);
	free_irq(up_button_irq, NULL);

	unset_pwm_led_gpios();

	pr_info("%s: PWM LED module unloaded\n", MODULE_NAME);
}

static void validate_led_max_level(void)
{
	if (led_max_level < LED_MIN_LEVEL)
		led_max_level = LED_MIN_LEVEL;

	if (led_max_level > LED_BRIGHTNESS_RANGE)
		led_max_level = LED_BRIGHTNESS_RANGE;
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

	return ret;
}

static void unset_pwm_led_gpios(void)
{
	gpio_free(down_button_gpio);
	gpio_free(up_button_gpio);
	gpio_free(LED_GPIO);
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

static int map_memory_regions(void)
{
	pwm_base = ioremap(PWM_BASE, PWM_REGION_SIZE);
	if (!pwm_base) {
		pr_err("%s: %s (%d): Error mapping PWM memory\n",
			MODULE_NAME,
			__func__,
			__LINE__);

		return -EINVAL;
	}

	gpio_base = ioremap(GPIO_BASE, GPIO_REGION_SIZE);
	if (!gpio_base) {
		pr_err("%s: %s (%d): Error mapping GPIO memory\n",
			MODULE_NAME,
			__func__,
			__LINE__);

		iounmap(pwm_base);

		return -EINVAL;
	}

	pwm_clk = ioremap(PWM_CLK_BASE, PWM_CLK_REGION_SIZE);
	if (!gpio_base) {
		pr_err("%s: %s (%d): Error mapping PWM clock memory\n",
			MODULE_NAME,
			__func__,
			__LINE__);

		iounmap(pwm_base);
		iounmap(gpio_base);

		return -EINVAL;
	}

	return 0;
}

static void setup_pwm_clock(void)
{
	reset_pwm_clocks();
	kill_pwm_clock();
	short_wait();

	set_pwm_clock_divisors(DIVI_DEFAULT, DIVF_DEFAULT);
	short_wait();

	enable_pwm_clock();
	short_wait();
}

static void reset_pwm_clocks(void)
{
	iowrite32(0, pwm_clk + PWM_CLK_CTL_OFFSET);
	iowrite32(0, pwm_clk + PWM_CLK_DIV_OFFSET);
}

static void kill_pwm_clock(void)
{
	iowrite32(PWM_CLK_PASSWORD | CLK_KILL, pwm_clk + PWM_CLK_CTL_OFFSET);
}

static void enable_pwm_clock(void)
{
	int clk_ctrl_mask;

	clk_ctrl_mask = PWM_CLK_PASSWORD | CLK_ENABLE | CLK_SRC_OSCILLATOR;
	iowrite32(clk_ctrl_mask, pwm_clk);
}

static void set_pwm_clock_divisors(int integer_part, int fractional_part)
{
	int clk_div_mask;

	clk_div_mask = PWM_CLK_PASSWORD;
	clk_div_mask |= integer_part << DIVI_BITS_POS;
	clk_div_mask |= fractional_part << DIVF_BITS_POS;

	iowrite32(clk_div_mask, pwm_clk + PWM_CLK_DIV_OFFSET);
}

static void short_wait(void)
{
	/*
	 * From Documentation/timers/timers-howto.txt:
	 * SLEEPING FOR "A FEW" USECS ( < ~10us? ):
	 *	* Use udelay
	 *
	 *	- Why not usleep?
	 *		On slower systems, (embedded, OR perhaps a speed-
	 *		stepped PC!) the overhead of setting up the hrtimers
	 *		for usleep *may* not be worth it. Such an evaluation
	 *		will obviously depend on your specific situation, but
	 *		it is something to be aware of.
	 */
	udelay(SHORT_WAIT_LENGTH);
}

static void activate_pwm_channel(void)
{
	iowrite32(PWM_ENABLE, pwm_base + PWM_CTL_OFFSET);
}

static void deactivate_pwm_channel(void)
{
	iowrite32(PWM_DISABLE, pwm_base + PWM_CTL_OFFSET);
}

static void save_gpio_func_select(void)
{
	int val, mask;

	mask = 7;
	val = ioread32(gpio_base + func_select_reg_offset);
	func_select_initial_val = (val >> func_select_bit_offset) & mask;
}

static void restore_gpio_func_select(void)
{
	int val, mask;

	mask = 7;
	val = ioread32(gpio_base + func_select_reg_offset);
	val &= ~(mask << func_select_bit_offset);
	val |= func_select_initial_val << func_select_bit_offset;
	iowrite32(val, gpio_base + func_select_reg_offset);
}

static void gpio_select_function(int gpio, int function_number)
{
	int val, mask;

	mask = 7;
	val = ioread32(gpio_base + func_select_reg_offset);
	val &= ~(mask << func_select_bit_offset);
	val |= function_number << func_select_bit_offset;
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
	int level, led_brightness;

	level = atomic_read(&led_level);
	led_brightness = LED_BRIGHTNESS_RANGE * level / led_max_level;

	iowrite32(led_brightness, pwm_base + PWM_DAT1_OFFSET);
	short_wait();

	schedule_work(work);
}

static void dump_pwm_registers(void)
{
	pr_info("==========================================================\n");
	pr_info("PWM Register Dump:\n");

	short_wait();
	pr_info("PWM CTL: %d\n", ioread32(pwm_base + PWM_CTL_OFFSET));
	short_wait();

	pr_info("PWM STA (status): %d\n", ioread32(pwm_base + PWM_STA_OFFSET));
	short_wait();

	pr_info("PWM RNG1 (range for channel 1): %d\n",
		ioread32(pwm_base + PWM_RNG1_OFFSET));
	short_wait();

	pr_info("PWM DAT1 (data for channel 1): %d\n",
		ioread32(pwm_base + PWM_DAT1_OFFSET));
	short_wait();

	pr_info("==========================================================\n");
}

module_init(pwm_led_init);
module_exit(pwm_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Kolev");
MODULE_DESCRIPTION("A basic LED driver using pulse-width modulation.");
MODULE_VERSION("0.1");
