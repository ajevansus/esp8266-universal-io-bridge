#include "gpios.h"

#include "application.h"
#include "util.h"
#include "config.h"
#include "i2c.h"

#include <user_interface.h>
#include <osapi.h>
#include <gpio.h>
#include <pwm.h>
#include <ets_sys.h>

#include <stdlib.h>

typedef enum
{
	rtcgpio_input,
	rtcgpio_output
} rtcgpio_setup_t;

_Static_assert(sizeof(rtcgpio_setup_t) == 4, "sizeof(rtcgpio_setup_t) != 4");

typedef struct
{
	const	gpio_id_t	id;
	const	char		*name;
	const	int			index;
	const struct
	{
		int rtc_gpio;
	} flags;

	const	uint32_t	io_mux;
	const	uint32_t	io_func;

	struct
	{
		int count;
		int debounce;
	} counter;

	struct
	{
		int delay;
	} timer;

	struct
	{
		int					channel;
		int					min_duty;
		int					max_duty;
		int					delay_current;
		int					delay_top;
		gpio_direction_t	direction;
	} pwm;
} gpio_t;

typedef struct
{
	gpio_mode_t		mode;
	const char		*name;
	void			(*init_fn)(gpio_t *);
} gpio_mode_trait_t;

static void gpio_init_disabled(gpio_t *);
static void gpio_init_input(gpio_t *);
static void gpio_init_counter(gpio_t *);
static void gpio_init_output(gpio_t *);
static void gpio_init_timer(gpio_t *);
static void gpio_init_pwm(gpio_t *);
static void gpio_init_i2c(gpio_t *);

static gpio_t *find_gpio(gpio_id_t);
static void gpio_config_init(gpio_config_entry_t *);

static struct
{
	unsigned int pwm_subsystem_active:1;
	unsigned int counter_triggered:1;
} gpio_flags;

static gpio_mode_trait_t gpio_mode_trait[gpio_mode_size] =
{
	{ gpio_disabled,	"disabled",		gpio_init_disabled },
	{ gpio_input,		"input",		gpio_init_input },
	{ gpio_counter,		"counter",		gpio_init_counter },
	{ gpio_output,		"output",		gpio_init_output },
	{ gpio_timer,		"timer",		gpio_init_timer },
	{ gpio_pwm,			"pwm",			gpio_init_pwm },
	{ gpio_i2c,			"i2c",			gpio_init_i2c },
};

static gpio_t gpios[gpio_size] =
{
	{
		.id = gpio_0,
		.name = "gpio0",
		.index = 0,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_GPIO0_U,
		.io_func = FUNC_GPIO0,
	},
	{
		.id = gpio_1,
		.name = "gpio1",
		.index = 1,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_U0TXD_U,
		.io_func = FUNC_GPIO1
	},
	{
		.id = gpio_2,
		.name = "gpio2",
		.index = 2,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_GPIO2_U,
		.io_func = FUNC_GPIO2,
	},
	{
		.id = gpio_3,
		.name = "gpio3",
		.index = 3,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_U0RXD_U,
		.io_func = FUNC_GPIO3,
	},
	{
		.id = gpio_4,
		.name = "gpio4",
		.index = 4,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_GPIO4_U,
		.io_func = FUNC_GPIO4,
	},
	{
		.id = gpio_5,
		.name = "gpio5",
		.index = 5,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_GPIO5_U,
		.io_func = FUNC_GPIO5,
	},
	{
		.id = gpio_12,
		.name = "gpio12",
		.index = 12,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_MTDI_U,
		.io_func = FUNC_GPIO12,
	},
	{
		.id = gpio_13,
		.name = "gpio13",
		.index = 13,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_MTCK_U,
		.io_func = FUNC_GPIO13,
	},
	{
		.id = gpio_14,
		.name = "gpio14",
		.index = 14,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_MTMS_U,
		.io_func = FUNC_GPIO14,
	},
	{
		.id = gpio_15,
		.name = "gpio15",
		.index = 15,
		.flags = {
			.rtc_gpio = 0,
		},
		.io_mux = PERIPHS_IO_MUX_MTDO_U,
		.io_func = FUNC_GPIO15,
	},
	{
		.id = gpio_16,
		.name = "gpio16",
		.index = 16,
		.flags = {
			.rtc_gpio = 1,
		}
	}
};

static unsigned int analog_sampling_current = 0;
static unsigned int analog_sampling_total = 0;
static unsigned int analog_sampling_value = 0;

irom static gpio_config_entry_t *get_config(const gpio_t *gpio)
{
	return(&config.gpios.entry[gpio->id]);
}

iram static void pc_int_handler(uint32_t pc, void *arg)
{
	gpio_t *gpio;
	gpio_config_entry_t *cfg;
	int current;

	for(current = 0; current < gpio_size; current++)
	{
		gpio = &gpios[current];

		if(pc & (1 << gpio->index))
		{
			cfg = get_config(gpio);

			gpio->counter.count++;
			gpio->counter.debounce = cfg->counter.debounce;
		}
	}

	gpio_intr_ack(pc);

	gpio_flags.counter_triggered = true;
}

irom static void select_pin_function(const gpio_t *gpio)
{
	if(gpio->flags.rtc_gpio)
		return;

	pin_func_select(gpio->io_mux, gpio->io_func);
	PIN_PULLUP_DIS(gpio->io_mux);
}

irom void gpios_init(void)
{
	int current;
	int pwmchannel;
	gpio_t *gpio;
	gpio_config_entry_t *cfg;
	uint32_t pwm_io_info[gpio_pwm_size][3];
	uint32_t pwm_duty_init[gpio_pwm_size];
	uint32_t state_change_mask;
	int sda, scl;

	sda = scl = -1;

	gpio_init();

	state_change_mask = 0;

	for(current = 0, pwmchannel = 0; current < gpio_size; current++)
	{
		gpio = &gpios[current];
		cfg = get_config(gpio);

		if(cfg->mode != gpio_disabled)
			select_pin_function(gpio);

		if(cfg->mode == gpio_counter)
			state_change_mask |= (1 << gpio->index);

		if((cfg->mode == gpio_pwm) && (pwmchannel < gpio_pwm_size))
		{
			gpio->pwm.channel = pwmchannel;
			pwm_io_info[pwmchannel][0] = gpio->io_mux;
			pwm_io_info[pwmchannel][1] = gpio->io_func;
			pwm_io_info[pwmchannel][2] = gpio->index;
			pwm_duty_init[pwmchannel] = cfg->pwm.min_duty;
			pwmchannel++;
		}

		if(cfg->mode == gpio_i2c)
		{
			if(cfg->i2c.pin == gpio_i2c_sda)
				sda = gpio->index;

			if(cfg->i2c.pin == gpio_i2c_scl)
				scl = gpio->index;
		}
	}

	if(state_change_mask != 0)
		gpio_intr_handler_register(pc_int_handler, 0);

	if(pwmchannel > 0)
	{
		pwm_init(3000, pwm_duty_init, pwmchannel, pwm_io_info);
		gpio_flags.pwm_subsystem_active = true;
	}

	for(current = 0; current < gpio_size; current++)
		gpio_mode_trait[config.gpios.entry[current].mode].init_fn(&gpios[current]);

	if((sda >= 0) && (scl >= 0))
		i2c_init(sda, scl, config.i2c_delay);

	gpio_flags.counter_triggered = false;
}

irom static void gpio_config_init(gpio_config_entry_t *gpio)
{
	gpio->mode = gpio_disabled;
	gpio->counter.debounce = 100;
	gpio->counter.reset_on_get = false;
	gpio->output.startup_state = false;
	gpio->timer.direction = gpio_up;
	gpio->timer.delay = 0;
	gpio->timer.repeat = false;
	gpio->timer.autotrigger = false;
	gpio->pwm.min_duty = 0;
	gpio->pwm.max_duty = 0;
	gpio->pwm.delay = 0;
	gpio->i2c.pin = gpio_i2c_sda;
}

irom void gpios_config_init(gpio_config_t *cfg_gpios)
{
	int current;

	for(current = 0; current < gpio_size; current++)
		gpio_config_init(&cfg_gpios->entry[current]);
}

irom static void setclear_perireg(uint32_t reg, uint32_t clear, uint32_t set)
{
	uint32_t tmp;

	tmp = READ_PERI_REG(reg);
	tmp &= (uint32_t)~clear;
	tmp |= set;
    WRITE_PERI_REG(reg, tmp);
}

irom static void rtcgpio_config(rtcgpio_setup_t io)
{
	setclear_perireg(PAD_XPD_DCDC_CONF, 0x43, 0x01);
	setclear_perireg(RTC_GPIO_CONF, 0x01, 0x00);
	setclear_perireg(RTC_GPIO_ENABLE, 0x01, (io == rtcgpio_output) ? 0x01 : 0x00);
}

irom static void rtcgpio_output_set(bool_t value)
{
	setclear_perireg(RTC_GPIO_OUT, 0x01, value ? 0x01 : 0x00);
}

irom static bool rtcgpio_input_get(void)
{
	return(!!(READ_PERI_REG(RTC_GPIO_IN_DATA) & 0x01));
}

irom static void set_output(const gpio_t *gpio, bool_t onoff)
{
	if(gpio->flags.rtc_gpio)
		rtcgpio_output_set(onoff);
	else
		gpio_output_set(onoff ? (1 << gpio->index) : 0x00,
						!onoff ? (1 << gpio->index) : 0x00,
						0x00, 0x00);
}

irom static bool_t get_input(const gpio_t *gpio)
{
	if(gpio->flags.rtc_gpio)
		return(rtcgpio_input_get());

	return(!!(gpio_input_get() & (1 << gpio->index)));
}

irom static inline void arm_counter(const gpio_t *gpio)
{
	// no use in specifying POSEDGE or NEGEDGE here (bummer),
	// they act exactly like ANYEDGE, I assume that's an SDK bug

	gpio_pin_intr_state_set(gpio->index, GPIO_PIN_INTR_ANYEDGE);
}

irom void gpios_periodic(void)
{
	gpio_t *gpio;
	const gpio_config_entry_t *cfg;
	unsigned int current;
	int duty;
	bool_t pwm_changed = false;

	current = system_adc_read();

	if(current == 1023)
		current = 1024;

	analog_sampling_total += current;

	if(++analog_sampling_current >= 256)
	{
		current = analog_sampling_total / 4;

		if(current > 65535)
			current = 65535;

		if(current < 256)
			current = 0;

		analog_sampling_current = 0;
		analog_sampling_total = 0;
		analog_sampling_value = current;
	}

	for(current = 0; current < gpio_size; current++)
	{
		gpio = &gpios[current];
		cfg = get_config(gpio);

		if((cfg->mode == gpio_counter) && (gpio->counter.debounce != 0))
		{
			if(gpio->counter.debounce >= 10)
				gpio->counter.debounce -= 10; // 10 ms per tick
			else
				gpio->counter.debounce = 0;

			if(gpio->counter.debounce == 0)
				arm_counter(gpio);
		}

		if((cfg->mode == gpio_timer) && (gpio->timer.delay > 0))
		{
			if(gpio->timer.delay >= 10)
				gpio->timer.delay -= 10; // 10 ms per tick
			else
				gpio->timer.delay = 0;

			if(gpio->timer.delay == 0)
			{
				set_output(gpio, !get_input(gpio));

				if(cfg->timer.repeat)
					gpio->timer.delay = cfg->timer.delay;
			}
		}

		if((cfg->mode == gpio_pwm) && (gpio->pwm.delay_top > 0))
		{
			if(++gpio->pwm.delay_current > gpio->pwm.delay_top)
			{
				gpio->pwm.delay_current = 0;

				duty = pwm_get_duty(gpio->pwm.channel);

				if(gpio->pwm.direction == gpio_up)
				{
					if(duty < gpio->pwm.min_duty)
						duty = gpio->pwm.min_duty;

					if(duty < 16)
						duty = 16;

					duty *= 115;
					duty /= 100;

					if(duty >= gpio->pwm.max_duty)
					{
						duty = gpio->pwm.max_duty;
						gpio->pwm.direction = gpio_down;
					}
				}
				else
				{
					if(duty > gpio->pwm.max_duty)
						duty = gpio->pwm.max_duty;

					duty *= 100;
					duty /= 115;

					if(duty <= gpio->pwm.min_duty)
					{
						duty = gpio->pwm.min_duty;
						gpio->pwm.direction = gpio_up;
					}

					if(duty < 16)
					{
						duty = 16;
						gpio->pwm.direction = gpio_up;
					}
				}

				pwm_changed = true;
				pwm_set_duty(duty, gpio->pwm.channel);
			}
		}
	}

	if(pwm_changed)
		pwm_start();

	if(gpio_flags.counter_triggered)
	{
		gpio_flags.counter_triggered = false;

		if(config.stat_trigger_gpio >= 0)
			gpios_trigger_output(config.stat_trigger_gpio);
	}
}

irom static void trigger_timer(gpio_t *gpio, bool_t onoff)
{
	const gpio_config_entry_t *cfg = get_config(gpio);

	if(onoff)
	{
		set_output(gpio, cfg->timer.direction == gpio_up ? 1 : 0);
		gpio->timer.delay = cfg->timer.delay;
	}
	else
	{
		set_output(gpio, cfg->timer.direction == gpio_up ? 0 : 1);
		gpio->timer.delay = 0;
	}
}

irom static gpio_t *find_gpio(gpio_id_t index)
{
	int current;
	gpio_t *gpio;

	for(current = 0; current < gpio_size; current++)
	{
		gpio = &gpios[current];

		if(gpio->index == index)
			return(gpio);
	}

	return(0);
}

irom static gpio_mode_t gpio_mode_from_string(string_t *src)
{
	int current;
	const gpio_mode_trait_t *entry;

	for(current = 0; current < gpio_mode_size; current++)
	{
		entry = &gpio_mode_trait[current];

		if(string_match(src, entry->name))
			return(entry->mode);
	}

	return(gpio_mode_error);
}

irom static gpio_i2c_t gpio_i2c_pin_from_string(string_t *pin)
{
	if(string_match(pin, "sda"))
		return(gpio_i2c_sda);
	else if(string_match(pin, "scl"))
		return(gpio_i2c_scl);
	else
		return(gpio_i2c_error);
}

irom static void gpio_init_disabled(gpio_t *gpio)
{
}

irom static void gpio_init_input(gpio_t *gpio)
{
	if(gpio->flags.rtc_gpio)
		rtcgpio_config(rtcgpio_input);
	else
		gpio_output_set(0, 0, 0, 1 << gpio->index);
}

irom static void gpio_init_counter(gpio_t *gpio)
{
	gpio_output_set(0, 0, 0, 1 << gpio->index);
	arm_counter(gpio);
}

irom static void gpio_init_output(gpio_t *gpio)
{
	const gpio_config_entry_t *cfg = get_config(gpio);

	if(gpio->flags.rtc_gpio)
		rtcgpio_config(rtcgpio_output);
	else
		gpio_output_set(0, 0, 1 << gpio->index, 0);

	set_output(gpio, cfg->output.startup_state);
}

irom static void gpio_init_timer(gpio_t *gpio)
{
	const gpio_config_entry_t *cfg = get_config(gpio);

	gpio->timer.delay = 0;

	gpio_init_output(gpio);

	if(cfg->timer.direction == gpio_up)
		gpio_output_set(0, 1 << gpio->index, 0, 0);
	else
		gpio_output_set(1 << gpio->index, 0, 0, 0);

	if(cfg->timer.autotrigger)
		trigger_timer(gpio, true);
}

irom static void gpio_init_pwm(gpio_t *gpio)
{
	const gpio_config_entry_t *cfg = get_config(gpio);

	gpio->pwm.min_duty = cfg->pwm.min_duty;
	gpio->pwm.max_duty = cfg->pwm.max_duty;
	gpio->pwm.delay_top = cfg->pwm.delay;
	gpio->pwm.delay_current = 0;
	gpio->pwm.direction	= gpio_up;
}

irom static void gpio_init_i2c(gpio_t *gpio)
{
	uint32_t pin = GPIO_PIN_ADDR(GPIO_ID_PIN(gpio->index));

	/* set to open drain */
	GPIO_REG_WRITE(pin, GPIO_REG_READ(pin) | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE));

	gpio_output_set(1 << gpio->index, 0, 1 << gpio->index, 0);
}

typedef enum
{
	ds_id_gpio,
	ds_id_disabled,
	ds_id_input,
	ds_id_counter,
	ds_id_output,
	ds_id_timer,
	ds_id_pwm_inactive,
	ds_id_pwm_active,
	ds_id_pwm_duty_default,
	ds_id_pwm_duty_current,
	ds_id_i2c,
	ds_id_unknown,
	ds_id_header,
	ds_id_footer,
	ds_id_preline,
	ds_id_postline,

	ds_id_length,
	ds_id_invalid = ds_id_length
} dump_string_id_t;

typedef struct {
	struct
	{
		const char plain[ds_id_length][256];
		const char html[ds_id_length][256];
	} strings;
} dump_string_t;

static roflash const dump_string_t dump_strings =
{
	.strings =
	{
		.plain =
		{
			"> gpio: %d, name: %s, mode: ",
			"disabled",
			"input, state: %s",
			"counter, state: %s, counter: %d, debounce: %d/%d, reset on get: %s",
			"output, state: %s, startup: %s",
			"timer, direction: %s, delay: %d ms, repeat: %s, autotrigger: %s, active: %s, current state: %s",
			"pwm, inactive",
			"pwm, active, channel: %d, current frequency: %d Hz, current duty: %d",
			"\ndefault min duty: %d, max duty: %d, delay: %d",
			"\ncurrent min duty: %d, max duty: %d, delay: %d",
			"i2c, pin: %s",
			"unknown",
			"",
			"",
			"",
			"\n",
		},

		.html =
		{
			"<td>%d</td><td>%s</td>",
			"<td>disabled</td>",
			"<td>input</td><td>state: %s</td>",
			"<td>counter</td><td>state: %s</td><td>counter: %d</td><td>debounce: %d/%d</td><td>reset on get: %s</td>",
			"<td>output</td><td>state: %s</td><td>startup: %s</td>",
			"<td>timer</td><td>direction: %s</td><td>delay: %d ms</td><td> repeat: %s</td><td>autotrigger: %s</td><td>active: %s</td><td>current state: %s</td>",
			"<td>pwm</<td><td>inactive</td>",
			"<td>pwm</td><td>active</td><td>channel: %d</td><td>current frequency: %d Hz</td><td>current duty: %d</td>",
			"<td>default min duty: %d, max duty: %d, delay: %d</td>",
			"<td>current min duty: %d, max duty: %d, delay: %d</td>",
			"<td>i2c</td><td>pin %s</td>",
			"<td>unknown</td>",
			"<table border=\"1\"><tr><th>index</th><th>name</th><th>mode</th><th colspan=\"8\"></th></tr>",
			"</table>\n",
			"<tr>",
			"</trd>\n",
		}
	}
};

irom static void dump(string_t *dst, const gpio_config_t *cfgs, const gpio_t *gpio_in, bool html)
{
	int current;
	const gpio_t *gpio;
	const gpio_config_entry_t *cfg;
	const char (*strings)[256];

	if(html)
		strings = dump_strings.strings.html;
	else
		strings = dump_strings.strings.plain;

	string_cat_ptr(dst, strings[ds_id_header]);

	for(current = 0; current < gpio_size; current++)
	{
		gpio = &gpios[current];
		cfg = &cfgs->entry[current];

		if(!gpio_in || (gpio_in->id == gpio->id))
		{
			string_format_ptr(dst, strings[ds_id_preline], gpio->index, gpio->name);
			string_format_ptr(dst, strings[ds_id_gpio], gpio->index, gpio->name);

			switch(cfg->mode)
			{
				case(gpio_disabled):
				{
					string_cat_ptr(dst, strings[ds_id_disabled]);

					break;
				}

				case(gpio_input):
				{
					string_format_ptr(dst, strings[ds_id_input], onoff(get_input(gpio)));

					break;
				}

				case(gpio_counter):
				{
					string_format_ptr(dst, strings[ds_id_counter], onoff(get_input(gpio)), gpio->counter.count,
							cfg->counter.debounce, gpio->counter.debounce, onoff(cfg->counter.reset_on_get));

					break;
				}

				case(gpio_output):
				{
					string_format_ptr(dst, strings[ds_id_output], onoff(get_input(gpio)), onoff(cfg->output.startup_state));

					break;
				}

				case(gpio_timer):
				{
					string_format_ptr(dst, strings[ds_id_timer], cfg->timer.direction == gpio_up ? "up" : "down",
							cfg->timer.delay, onoff(cfg->timer.repeat), onoff(cfg->timer.autotrigger),
							onoff(gpio->timer.delay > 0), onoff(get_input(gpio)));
					break;
				}

				case(gpio_pwm):
				{
					if(gpio_flags.pwm_subsystem_active)
						string_format_ptr(dst, strings[ds_id_pwm_active], gpio->pwm.channel,
								1000000 / pwm_get_period(), pwm_get_duty(gpio->pwm.channel));
					else
						string_cat_ptr(dst, strings[ds_id_pwm_inactive]);

					string_format_ptr(dst, strings[ds_id_pwm_duty_default], cfg->pwm.min_duty, cfg->pwm.max_duty, cfg->pwm.delay);
					string_format_ptr(dst, strings[ds_id_pwm_duty_current], gpio->pwm.min_duty, gpio->pwm.max_duty, gpio->pwm.delay_top);

					break;
				}

				case(gpio_i2c):
				{
					string_format_ptr(dst, strings[ds_id_i2c], cfg->i2c.pin == gpio_i2c_sda ? "sda" : "scl");

					break;
				}


				default:
				{
					string_cat(dst, "unknown mode");

					break;
				}
			}

			string_cat_ptr(dst, strings[ds_id_postline]);
		}
	}

	string_cat_ptr(dst, strings[ds_id_footer]);
}

irom void gpios_dump_string(string_t *dst, const gpio_config_t *cfgs)
{
	return(dump(dst, cfgs, 0, false));
}

irom void gpios_dump_html(string_t *dst, const gpio_config_t *cfgs)
{
	return(dump(dst, cfgs, 0, true));
}

irom bool gpios_trigger_output(int gpio_name)
{
	gpio_t *gpio;
	const gpio_config_entry_t *cfg;

	if(!(gpio = find_gpio(gpio_name)))
		return(false);

	cfg = get_config(gpio);

	switch(cfg->mode)
	{
		case(gpio_output):
		{
			set_output(gpio, true);
			break;
		}

		case(gpio_timer):
		{
			trigger_timer(gpio, true);
			break;
		}

		case(gpio_pwm):
		{
			pwm_set_duty(0xffff, gpio->pwm.channel);
			pwm_start();

			break;
		}

		default:
		{
			return(false);
		}
	}

	return(true);
}

irom bool gpios_set_wlan_trigger(int gpio_name)
{
	gpio_t *gpio;
	const gpio_config_entry_t *cfg;

	if(!(gpio = find_gpio(gpio_name)))
		return(false);

	cfg = get_config(gpio);

	if(cfg->mode != gpio_output)
		return(false);

	if(gpio->flags.rtc_gpio)
		return(false);

	wifi_status_led_install(gpio->index, gpio->io_mux, gpio->io_func);

	return(true);
}

irom app_action_t application_function_gpio_mode(string_t *src, string_t *dst)
{
	gpio_mode_t mode;
	int gpio_index;
	gpio_t *gpio;
	gpio_config_entry_t *new_gpio_config;

	if(parse_int(1, src, &gpio_index, 0) != parse_ok)
	{
		dump(dst, &config.gpios, 0, false);
		return(app_action_normal);
	}

	if(!(gpio = find_gpio(gpio_index)))
	{
		string_format(dst, "gpio-mode: invalid gpio %d\n", gpio_index);
		return(app_action_error);
	}

	if(parse_string(2, src, dst) != parse_ok)
	{
		string_clear(dst);
		dump(dst, &config.gpios, gpio, false);
		return(app_action_normal);
	}

	if((mode = gpio_mode_from_string(dst)) == gpio_mode_error)
	{
		string_copy(dst, "gpio-mode: invalid mode\n");
		return(app_action_error);
	}

	string_clear(dst);

	config_read(&tmpconfig);
	new_gpio_config = &tmpconfig.gpios.entry[gpio->id];

	switch(mode)
	{
		case(gpio_counter):
		{
			if(gpio->flags.rtc_gpio)
			{
				string_cat(dst, "gpio-mode: counter mode invalid for gpio 16\n");
				return(app_action_error);
			}

			int reset_on_get;
			int debounce;

			if((parse_int(3, src, &reset_on_get, 0) != parse_ok) || (parse_int(4, src, &debounce, 0) != parse_ok))
			{
				string_cat(dst, "gpio-mode(counter): <reset on get> <debounce ms>\n");
				return(app_action_error);
			}

			new_gpio_config->counter.reset_on_get = !!reset_on_get;
			new_gpio_config->counter.debounce = debounce;

			break;
		}

		case(gpio_output):
		{
			int startup_state;

			if(parse_int(3, src, &startup_state, 0) != parse_ok)
			{
				string_cat(dst, "gpio-mode(output): <startup value>\n");
				return(app_action_error);
			}

			new_gpio_config->output.startup_state = startup_state;

			break;
		}

		case(gpio_timer):
		{
			gpio_direction_t direction;
			int delay, repeat, autotrigger;

			if(parse_string(3, src, dst) != parse_ok)
			{
				string_cat(dst, "gpio-mode: timer direction:up/down delay:ms repeat:0/1 autotrigger:0/1\n");
				return(app_action_error);
			}

			if(string_match(dst, "up"))
				direction = gpio_up;
			else if(string_match(dst, "down"))
				direction = gpio_down;
			else
			{
				string_cat(dst, ": timer direction invalid\n");
				return(app_action_error);
			}

			string_clear(dst);

			if((parse_int(4, src, &delay, 0) != parse_ok) ||
				(parse_int(5, src, &repeat, 0) != parse_ok) ||
				(parse_int(6, src, &autotrigger, 0) != parse_ok))
			{
				string_cat(dst, "gpio-mode: timer direction:up/down delay:ms repeat:0/1 autotrigger:0/1\n");
				return(app_action_error);
			}

			if(delay < 10)
			{
				string_cat(dst, "gpio-mode(timer): delay too small: %d ms, >= 10 ms\n");
				return(app_action_error);
			}

			new_gpio_config->timer.direction = direction;
			new_gpio_config->timer.delay = (uint32_t)delay;
			new_gpio_config->timer.repeat = (uint8_t)!!repeat;
			new_gpio_config->timer.autotrigger = (uint8_t)!!autotrigger;

			break;
		}

		case(gpio_pwm):
		{
			int min_duty;
			int max_duty;
			int delay;

			min_duty = 0;
			max_duty = 0;
			delay = 0;

			if(gpio->flags.rtc_gpio)
			{
				string_cat(dst, "gpio-mode: pwm mode not supported for this gpio\n");
				return(app_action_error);
			}

			parse_int(3, src, &min_duty, 0);
			parse_int(4, src, &max_duty, 0);
			parse_int(5, src, &delay, 0);

			if((min_duty < 0) || (min_duty > 65535))
			{
				string_format(dst, "gpio-mode(pwm): min_duty out of range: %d\n", min_duty);
				return(app_action_error);
			}

			if((min_duty < 0) || (max_duty > 65535))
			{
				string_format(dst, "gpio-mode(pwm): max_duty out of range: %d\n", max_duty);
				return(app_action_error);
			}

			if((delay < 0) || (delay > 100))
			{
				string_format(dst, "gpio-mode(pwm): out of range: %d%%\n", delay);
				return(app_action_error);
			}

			new_gpio_config->pwm.min_duty = (uint16_t)min_duty;
			new_gpio_config->pwm.max_duty = (uint16_t)max_duty;
			new_gpio_config->pwm.delay = (uint8_t)delay;

			break;
		}

		case(gpio_i2c):
		{
			gpio_i2c_t pin;

			if(gpio->flags.rtc_gpio)
			{
				string_cat(dst, "gpio-mode: i2c mode invalid for gpio 16\n");
				return(app_action_error);
			}

			if(parse_string(3, src, dst) != parse_ok)
			{
				string_copy(dst, "gpio-mode(i2c): usage: i2c sda|scl\n");
				return(app_action_error);
			}

			if((pin = gpio_i2c_pin_from_string(dst)) == gpio_i2c_error)
			{
				string_copy(dst, "gpio-mode(i2c): usage: i2c sda|scl\n");
				return(app_action_error);
			}

			string_clear(dst);

			new_gpio_config->i2c.pin = pin;

			break;
		}

		default:
		{
		}
	}

	new_gpio_config->mode = mode;
	config_write(&tmpconfig);

	dump(dst, &tmpconfig.gpios, gpio, false);
	string_cat(dst, "! gpio-mode: restart to activate new mode\n");

	return(app_action_normal);
}

irom app_action_t application_function_gpio_get(string_t *src, string_t *dst)
{
	int gpio_index;
	gpio_t *gpio;
	const gpio_config_entry_t *cfg;

	if(parse_int(1, src, &gpio_index, 0) != parse_ok)
	{
		string_cat(dst, "gpio-get: too few arguments\n");
		return(app_action_error);
	}

	if(!(gpio = find_gpio(gpio_index)))
	{
		string_format(dst, "gpio-get: invalid gpio %d\n", gpio_index);
		return(app_action_error);
	}

	cfg = get_config(gpio);

	switch(cfg->mode)
	{
		case(gpio_disabled):
		{
			string_format(dst, "gpio-get: gpio %s is disabled\n", gpio->name);
			return(app_action_error);
		}

		case(gpio_input):
		{
			string_format(dst, "gpio-get: gpio %s is %s\n", gpio->name, onoff(get_input(gpio)));
			return(app_action_normal);
		}

		case(gpio_counter):
		{
			string_format(dst, "gpio-get: gpio %s is %d (state: %s)\n", gpio->name, gpio->counter.count, onoff(get_input(gpio)));

			if(cfg->counter.reset_on_get)
				gpio->counter.count = 0;

			gpio->counter.debounce = 0;

			return(app_action_normal);
		}

		case(gpio_output):
		case(gpio_timer):
		{
			string_format(dst, "gpio-get: gpio %s is output\n", gpio->name);
			return(app_action_error);
		}

		case(gpio_pwm):
		{
			dump(dst, &config.gpios, gpio, false);
			return(app_action_normal);
		}

		case(gpio_i2c):
		{
			string_format(dst, "gpio-get: gpio %s is reserved for i2c\n", gpio->name);
			return(app_action_error);
		}

		default:
		{
		}
	}

	string_format(dst, "gpio-get: invalid mode %d\n", cfg->mode);
	return(app_action_error);
}

irom app_action_t application_function_gpio_set(string_t *src, string_t *dst)
{
	int gpio_index;
	gpio_t *gpio;
	const gpio_config_entry_t *cfg;

	if(parse_int(1, src, &gpio_index, 0) != parse_ok)
	{
		string_cat(dst, "gpio-set: <gpio> ...\n");
		return(app_action_error);
	}

	if(!(gpio = find_gpio(gpio_index)))
	{
		string_format(dst, "gpio-set: invalid gpio %d\n", gpio_index);
		return(app_action_error);
	}

	cfg = get_config(gpio);

	switch(cfg->mode)
	{
		case(gpio_disabled):
		{
			string_format(dst, "gpio-set: gpio %s is disabled\n", gpio->name);
			return(app_action_error);
		}

		case(gpio_input):
		{
			string_format(dst, "gpio-set: gpio %s is input\n", gpio->name);
			return(app_action_error);
		}

		case(gpio_counter):
		{
			int counter = 0;

			parse_int(2, src, &counter, 0);
			gpio->counter.count = counter;

			break;
		}

		case(gpio_output):
		{
			int value;

			if(parse_int(2, src, &value, 0) != parse_ok)
			{
				string_cat(dst, "gpio-set output: missing arguments\n");
				return(app_action_error);
			}

			set_output(gpio, value);

			break;
		}

		case(gpio_timer):
		{
			int value;

			if(parse_int(2, src, &value, 0) == parse_ok)
				trigger_timer(gpio, !!value);
			else
				trigger_timer(gpio, !gpio->timer.delay);

			break;
		}

		case(gpio_pwm):
		{
			int min_duty;
			int max_duty;
			int delay;

			min_duty = 0;
			max_duty = 0;
			delay = 0;

			parse_int(2, src, &min_duty, 0);
			parse_int(3, src, &max_duty, 0);
			parse_int(4, src, &delay, 0);

			if((min_duty < 0) || (min_duty > 65535))
			{
				string_format(dst, "gpio-set(pwm): min_duty out of range: %d\n", min_duty);
				return(app_action_error);
			}

			if((max_duty < 0) || (max_duty > 65535))
			{
				string_format(dst, "gpio-set(pwm): max_duty out of range: %d\n", max_duty);
				return(app_action_error);
			}

			if((delay < 0) || (delay > 100))
			{
				string_format(dst, "gpio-set(pwm): delay out of range: %d\n", delay);
				return(app_action_error);
			}

			gpio->pwm.min_duty = min_duty;
			gpio->pwm.max_duty = max_duty;
			gpio->pwm.delay_top = delay;
			gpio->pwm.direction = gpio_up;

			pwm_set_duty(min_duty, gpio->pwm.channel);
			pwm_start();

			break;
		}

		case(gpio_i2c):
		{
			string_format(dst, "gpio-set: gpio %s is reserved for i2c\n", gpio->name);
			return(app_action_error);
		}

		default:
		{
			string_format(dst, "gpio-set: cannot set gpio %s\n", gpio->name);
			return(app_action_error);
		}
	}

	dump(dst, &config.gpios, gpio, false);
	return(app_action_normal);
}

irom app_action_t application_function_gpio_dump(string_t *src, string_t *dst)
{
	dump(dst, &config.gpios, 0, false);
	return(app_action_normal);
}

irom app_action_t application_function_analog_read(string_t *src, string_t *dst)
{
	string_format(dst, "analog-read: value: [%u]\n", analog_sampling_value);
	return(app_action_normal);
}
