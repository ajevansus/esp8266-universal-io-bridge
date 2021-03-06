#include "application.h"

#include "stats.h"
#include "util.h"
#include "user_main.h"
#include "config.h"
#include "uart.h"
#include "i2c.h"
#include "i2c_sensor.h"
#include "display.h"
#include "http.h"
#include "io.h"
#include "io_gpio.h"
#include "time.h"

#include "ota.h"

#include <user_interface.h>
#include <c_types.h>
#include <sntp.h>

#include <stdlib.h>

typedef enum
{
	ws_inactive,
	ws_scanning,
	ws_finished,
} wlan_scan_state_t;

typedef struct
{
	const char		*command1;
	const char		*command2;
	app_action_t	(*function)(const string_t *, string_t *);
	const char		*description;
} application_function_table_t;

static const application_function_table_t application_function_table[];
static wlan_scan_state_t wlan_scan_state = ws_inactive;

irom app_action_t application_content(const string_t *src, string_t *dst)
{
	const application_function_table_t *tableptr;
	int status_io, status_pin;

	if(config_get_int("trigger.status.io", -1, -1, &status_io) &&
			config_get_int("trigger.status.pin", -1, -1, &status_pin) &&
			(status_io != -1) && (status_pin != -1))
	{
		io_trigger_pin((string_t *)0, status_io, status_pin, io_trigger_on);
	}

	if(parse_string(0, src, dst) != parse_ok)
		return(app_action_empty);

	for(tableptr = application_function_table; tableptr->function; tableptr++)
		if(string_match(dst, tableptr->command1) ||
				string_match(dst, tableptr->command2))
			break;

	if(tableptr->function)
	{
		string_clear(dst);
		return(tableptr->function(src, dst));
	}

	string_cat(dst, ": command unknown\n");
	return(app_action_error);
}

irom static app_action_t application_function_config_dump(const string_t *src, string_t *dst)
{
	config_dump(dst);
	return(app_action_normal);
}

irom static app_action_t application_function_config_write(const string_t *src, string_t *dst)
{
	unsigned int size;

	if((size = config_write()) == 0)
	{
		string_cat(dst, "> failed\n");
		return(app_action_error);
	}

	string_format(dst, "> config write done, space used: %u, free: %u\n", size, SPI_FLASH_SEC_SIZE - size);
	return(app_action_normal);
}

irom static app_action_t application_function_config_query_int(const string_t *src, string_t *dst)
{
	int index1, index2;
	int value;

	string_clear(dst);

	if(parse_string(1, src, dst) != parse_ok)
		return(app_action_error);

	if(parse_int(2, src, &index1, 0) != parse_ok)
		index1 = -1;
	else
		if(parse_int(3, src, &index2, 0) != parse_ok)
			index2 = -1;

	if(!config_get_int(string_to_const_ptr(dst), index1, index2, &value))
	{
		string_clear(dst);
		string_cat(dst, "ERROR\n");
		return(app_action_error);
	}

	string_format(dst, "=%d OK\n", value);

	return(app_action_normal);
}

irom static app_action_t application_function_config_query_string(const string_t *src, string_t *dst)
{
	string_new(, varid, 64);
	int index1, index2;

	if(parse_string(1, src, &varid) != parse_ok)
	{
		string_clear(dst);
		string_cat(dst, "missing variable name\n");
		return(app_action_error);
	}

	if(parse_int(2, src, &index1, 0) != parse_ok)
		index1 = -1;
	else
		if(parse_int(3, src, &index2, 0) != parse_ok)
			index2 = -1;

	string_clear(dst);
	string_cat_strptr(dst, string_to_const_ptr(&varid));
	string_cat(dst, "=");

	if(!config_get_string(string_to_const_ptr(&varid), index1, index2, dst))
	{
		string_clear(dst);
		string_cat(dst, "ERROR\n");
		return(app_action_error);
	}

	string_cat(dst, " OK\n");

	return(app_action_normal);
}

irom static app_action_t application_function_config_set(const string_t *src, string_t *dst)
{
	int index1, index2, offset;
	string_new(, varid, 64);

	if(parse_string(1, src, &varid) != parse_ok)
	{
		string_cat(dst, "missing variable name\n");
		return(app_action_error);
	}

	if(parse_int(2, src, &index1, 0) != parse_ok)
	{
		string_cat(dst, "missing index1\n");
		return(app_action_error);
	}

	if(parse_int(3, src, &index2, 0) != parse_ok)
	{
		string_cat(dst, "missing index2\n");
		return(app_action_error);
	}

	if((offset = string_sep(src, 0, 4, ' ')) < 0)
	{
		string_cat(dst, "missing variable value\n");
		return(app_action_error);
	}

	dprintf("set offset: %d", offset);

	if(!config_set_string(string_to_const_ptr(&varid), index1, index2, src, offset, -1))
	{
		string_cat(dst, "ERROR\n");
		return(app_action_error);
	}

	string_cat(dst, "OK\n");

	return(app_action_normal);
}

irom static app_action_t application_function_config_delete(const string_t *src, string_t *dst)
{
	int index1, index2, wildcard;
	string_new(, varid, 64);

	if(parse_string(1, src, &varid) != parse_ok)
	{
		string_clear(dst);
		string_cat(dst, "missing variable name\n");
		return(app_action_error);
	}

	if(parse_int(2, src, &index1, 0) != parse_ok)
		index1 = -1;

	if(parse_int(3, src, &index2, 0) != parse_ok)
		index2 = -1;

	if(parse_int(4, src, &wildcard, 0) != parse_ok)
		wildcard = 0;

	index1 = config_delete(string_to_const_ptr(&varid), index1, index2, wildcard != 0);

	string_format(dst, "%u config entries deleted\n", index1);

	return(app_action_normal);
}

irom static app_action_t application_function_help(const string_t *src, string_t *dst)
{
	const application_function_table_t *tableptr;

	for(tableptr = application_function_table; tableptr->function; tableptr++)
		string_format(dst, "> %s/%s: %s\n",
				tableptr->command1, tableptr->command2,
				tableptr->description);

	return(app_action_normal);
}

irom static app_action_t application_function_quit(const string_t *src, string_t *dst)
{
	return(app_action_disconnect);
}

irom static app_action_t application_function_reset(const string_t *src, string_t *dst)
{
	return(app_action_reset);
}

irom static app_action_t application_function_stats(const string_t *src, string_t *dst)
{
	stats_generate(dst);
	return(app_action_normal);
}

irom static app_action_t application_function_bridge_tcp_port(const string_t *src, string_t *dst)
{
	int tcp_port;

	if(parse_int(1, src, &tcp_port, 0) == parse_ok)
	{
		if((tcp_port < 0) || (tcp_port > 65535))
		{
			string_format(dst, "> invalid port %d\n", tcp_port);
			return(app_action_error);
		}

		if(tcp_port == 0)
			config_delete("tcp.bridge.port", -1, -1, false);
		else
			if(!config_set_int("tcp.bridge.port", -1, -1, tcp_port))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("tcp.bridge.port", -1, -1, &tcp_port))
		tcp_port = 0;

	string_format(dst, "> port: %d\n", tcp_port);

	return(app_action_normal);
}

irom static app_action_t application_function_bridge_tcp_timeout(const string_t *src, string_t *dst)
{
	int tcp_timeout;

	if(parse_int(1, src, &tcp_timeout, 0) == parse_ok)
	{
		if((tcp_timeout < 0) || (tcp_timeout > 65535))
		{
			string_format(dst, "> invalid timeout: %d\n", tcp_timeout);
			return(app_action_error);
		}

		if(tcp_timeout == 90)
			config_delete("tcp.bridge.timeout", -1, -1, false);
		else
			if(!config_set_int("tcp.bridge.timeout", -1, -1, tcp_timeout))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("tcp.bridge.timeout", -1, -1, &tcp_timeout))
		tcp_timeout = 90;

	string_format(dst, "> timeout: %d\n", tcp_timeout);

	return(app_action_normal);
}

irom static app_action_t application_function_command_tcp_port(const string_t *src, string_t *dst)
{
	int tcp_port;

	if(parse_int(1, src, &tcp_port, 0) == parse_ok)
	{
		if((tcp_port < 1) || (tcp_port > 65535))
		{
			string_format(dst, "> invalid port %d\n", tcp_port);
			return(app_action_error);
		}

		if(tcp_port == 24)
			config_delete("tcp.cmd.port", -1, -1, false);
		else
			if(!config_set_int("tcp.cmd.port", -1, -1, tcp_port))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("tcp.cmd.port", -1, -1, &tcp_port))
		tcp_port = 24;

	string_format(dst, "> port: %d\n", tcp_port);

	return(app_action_normal);
}

irom static app_action_t application_function_command_tcp_timeout(const string_t *src, string_t *dst)
{
	int tcp_timeout;

	if(parse_int(1, src, &tcp_timeout, 0) == parse_ok)
	{
		if((tcp_timeout < 0) || (tcp_timeout > 65535))
		{
			string_format(dst, "> invalid timeout: %d\n", tcp_timeout);
			return(app_action_error);
		}

		if(tcp_timeout == 90)
			config_delete("tcp.cmd.timeout", -1, -1, false);
		else
			if(!config_set_int("tcp.cmd.timeout", -1, -1, tcp_timeout))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("tcp.cmd.timeout", -1, -1, &tcp_timeout))
		tcp_timeout = 90;

	string_format(dst, "> timeout: %d\n", tcp_timeout);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_baud_rate(const string_t *src, string_t *dst)
{
	int baud_rate;

	if(parse_int(1, src, &baud_rate, 0) == parse_ok)
	{
		if((baud_rate < 150) || (baud_rate > 1000000))
		{
			string_format(dst, "> invalid baud rate: %d\n", baud_rate);
			return(app_action_error);
		}

		if(baud_rate == 9600)
			config_delete("uart.baud", -1, -1, false);
		else
			if(!config_set_int("uart.baud", -1, -1, baud_rate))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("uart.baud", -1, -1, &baud_rate))
		baud_rate = 9600;

	string_format(dst, "> baudrate: %d\n", baud_rate);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_data_bits(const string_t *src, string_t *dst)
{
	int data_bits;

	if(parse_int(1, src, &data_bits, 0) == parse_ok)
	{
		if((data_bits < 5) || (data_bits > 8))
		{
			string_format(dst, "> invalid data bits: %d\n", data_bits);
			return(app_action_error);
		}

		if(data_bits == 8)
			config_delete("uart.bits", -1, -1, false);
		else
			if(!config_set_int("uart.bits", -1, -1, data_bits))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("uart.bits", -1, -1, &data_bits))
		data_bits = 8;

	string_format(dst, "data bits: %d\n", data_bits);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_stop_bits(const string_t *src, string_t *dst)
{
	int stop_bits;

	if(parse_int(1, src, &stop_bits, 0) == parse_ok)
	{
		if((stop_bits < 1) || (stop_bits > 2))
		{
			string_format(dst, "> stop bits out of range: %d\n", stop_bits);
			return(app_action_error);
		}

		if(stop_bits == 1)
			config_delete("uart.stop", -1, -1, false);
		else
			if(!config_set_int("uart.stop", -1, -1, stop_bits))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("uart.stop", -1, -1, &stop_bits))
		stop_bits = 1;

	string_format(dst, "> stop bits: %d\n", stop_bits);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_parity(const string_t *src, string_t *dst)
{
	uart_parity_t parity;
	int parity_int;

	if(parse_string(1, src, dst) == parse_ok)
	{
		parity = uart_string_to_parity(dst);

		if((parity < parity_none) || (parity >= parity_error))
		{
			string_cat(dst, ": invalid parity\n");
			return(app_action_error);
		}

		if(parity == parity_none)
			config_delete("uart.parity", -1, -1, false);
		else
		{
			parity_int = (int)parity;

			if(!config_set_int("uart.parity", -1, -1, parity_int))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
		}
	}

	if(config_get_int("uart.parity", -1, -1, &parity_int))
		parity = (uart_parity_t)parity_int;
	else
		parity = parity_none;

	string_copy(dst, "parity: ");
	uart_parity_to_string(dst, parity);
	string_cat(dst, "\n");

	return(app_action_normal);
}

static int i2c_address = 0;

irom static app_action_t application_function_i2c_address(const string_t *src, string_t *dst)
{
	int intin;

	if(parse_int(1, src, &intin, 16) == parse_ok)
	{
		if((intin < 2) || (intin > 127))
		{
			string_format(dst, "i2c-address: invalid address 0x%02x\n", intin);
			return(app_action_error);
		}

		i2c_address = intin;
	}

	string_format(dst, "i2c-address: address: 0x%02x\n", i2c_address);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_read(const string_t *src, string_t *dst)
{
	int size, current;
	i2c_error_t error;
	uint8_t bytes[32];
	uint32_t start, stop, clocks, spent;

	if(parse_int(1, src, &size, 0) != parse_ok)
	{
		string_cat(dst, "i2c-read: missing byte count\n");
		return(app_action_error);
	}

	if(size > (int)sizeof(bytes))
	{
		string_format(dst, "i2c-read: read max %d bytes\n", sizeof(bytes));
		return(app_action_error);
	}

	start = system_get_time();

	if((error = i2c_receive(i2c_address, size, bytes)) != i2c_error_ok)
	{
		string_cat(dst, "i2c_read");
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		return(app_action_error);
	}

	stop = system_get_time();

	string_format(dst, "> i2c_read: read %d bytes from %02x:", size, i2c_address);

	for(current = 0; current < size; current++)
		string_format(dst, " %02x", bytes[current]);

	string_cat(dst, "\n");

	clocks = (size + 1) * 9 + 4;
	spent = (stop - start) * 1000;

	string_format(dst, "> transferred %u bytes in %u scl clocks\n", size + 1, clocks);
	string_format(dst, "> time spent: %u microseconds, makes %u kHz i2c bus\n",
			spent / 1000, 1000000 / (spent / clocks));

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_write(const string_t *src, string_t *dst)
{
	i2c_error_t error;
	static uint8_t bytes[32];
	int current, out;

	for(current = 0; current < (int)sizeof(bytes); current++)
	{
		if(parse_int(current + 1, src, &out, 16) != parse_ok)
			break;

		bytes[current] = (uint8_t)(out & 0xff);
	}

	if((error = i2c_send(i2c_address, current, bytes)) != i2c_error_ok)
	{
		string_cat(dst, "i2c_write");
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		return(app_action_error);
	}

	string_format(dst, "i2c_write: written %d bytes to %02x\n", current, i2c_address);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_init(const string_t *src, string_t *dst)
{
	int intin, bus;
	i2c_error_t error;
	i2c_sensor_t sensor;

	if((parse_int(1, src, &intin, 0)) != parse_ok)
	{
		string_format(dst, "> invalid i2c sensor: %u\n", intin);
		return(app_action_error);
	}

	sensor = (i2c_sensor_t)intin;

	if((parse_int(2, src, &bus, 0)) != parse_ok)
		bus = 0;

	if(bus >= i2c_busses)
	{
		string_format(dst, "> invalid i2c sensor: %u/%u\n", bus, intin);
		return(app_action_error);
	}

	if((error = i2c_sensor_init(bus, sensor)) != i2c_error_ok)
	{
		string_format(dst, "sensor init %d:%d", bus, sensor);
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		return(app_action_error);
	}

	string_format(dst, "init sensor %u/%u ok\n", bus, sensor);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_read(const string_t *src, string_t *dst)
{
	int intin, bus;
	i2c_sensor_t sensor;

	if((parse_int(1, src, &intin, 0)) != parse_ok)
	{
		string_format(dst, "> invalid i2c sensor: %u\n", intin);
		return(app_action_error);
	}

	sensor = (i2c_sensor_t)intin;

	if((parse_int(2, src, &bus, 0)) != parse_ok)
		bus = 0;

	if(bus >= i2c_busses)
	{
		string_format(dst, "> invalid i2c sensor: %u/%u\n", bus, intin);
		return(app_action_error);
	}

	if(!i2c_sensor_read(dst, bus, sensor, true))
	{
		string_clear(dst);
		string_format(dst, "> invalid i2c sensor: %u/%u\n", bus, (int)sensor);
		return(app_action_error);
	}

	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_calibrate(const string_t *src, string_t *dst)
{
	unsigned int intin, bus;
	i2c_sensor_t sensor;
	double factor, offset;
	int int_factor, int_offset;

	if(parse_int(1, src, &bus, 0) != parse_ok)
	{
		string_format(dst, "> invalid i2c bus: %u\n", bus);
		return(app_action_error);
	}

	if(parse_int(2, src, &intin, 0) != parse_ok)
	{
		string_format(dst, "> invalid i2c sensor: %u\n", intin);
		return(app_action_error);
	}

	if(bus >= i2c_busses)
	{
		string_format(dst, "> invalid i2c bus: %u\n", bus);
		return(app_action_error);
	}

	if(intin >= i2c_sensor_size)
	{
		string_format(dst, "> invalid i2c sensor: %u/%u\n", bus, intin);
		return(app_action_error);
	}

	sensor = (i2c_sensor_t)intin;

	if((parse_float(3, src, &factor) == parse_ok) && (parse_float(4, src, &offset) != parse_ok))
	{
		int_factor = (int)(factor * 1000.0);
		int_offset = (int)(offset * 1000.0);

		config_delete("i2s.%u.%u.", bus, sensor, true);

		if((int_factor != 1000) && !config_set_int("i2s.%u.%u.factor", bus, sensor, int_factor))
		{
			string_cat(dst, "> cannot set factor\n");
			return(app_action_error);
		}

		if((int_offset != 0) && !config_set_int("i2s.%u.%u.offset", bus, sensor, int_offset))
		{
			string_cat(dst, "> cannot set offset\n");
			return(app_action_error);
		}
	}

	if(!config_get_int("i2s.%u.%u.factor", bus, sensor, &int_factor))
		int_factor = 1000;

	if(!config_get_int("i2s.%u.%u.factor", bus, sensor, &int_offset))
		int_offset = 0;

	string_format(dst, "> i2c sensor %u/%u calibration set to factor ", bus, (int)sensor);
	string_double(dst, int_factor / 1000.0, 4, 1e10);
	string_cat(dst, ", offset: ");
	string_double(dst, int_offset / 1000.0, 4, 1e10);
	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_dump(const string_t *src, string_t *dst)
{
	i2c_sensor_t sensor;
	int option, bus;
	bool_t all, verbose;
	int original_length = string_length(dst);

	all = false;
	verbose = false;

	if(parse_int(1, src, &option, 0) == parse_ok)
	{
		switch(option)
		{
			case(2):
				all = true;
			case(1):
				verbose = true;
			default:
				(void)0;
		}
	}

	for(bus = 0; bus < i2c_busses; bus++)
		for(sensor = 0; sensor < i2c_sensor_size; sensor++)
		{
			if(all || i2c_sensor_detected(bus, sensor))
			{
				i2c_sensor_read(dst, bus, sensor, verbose);
				string_cat(dst, "\n");
			}
		}

	if(string_length(dst) == original_length)
		string_cat(dst, "> no sensors detected\n");

	return(app_action_normal);
}

irom static app_action_t set_unset_flag(const string_t *src, string_t *dst, bool_t add)
{
	if(parse_string(1, src, dst) == parse_ok)
	{
		if(!config_flags_change(dst, add))
		{
			string_cat(dst, ": unknown flag\n");
			return(app_action_error);
		}
	}

	string_cat(dst, "flags:");
	config_flags_to_string(dst);
	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_set(const string_t *src, string_t *dst)
{
	return(set_unset_flag(src, dst, true));
}

irom static app_action_t application_function_unset(const string_t *src, string_t *dst)
{
	return(set_unset_flag(src, dst, false));
}

irom static app_action_t application_function_time_set(const string_t *src, string_t *dst)
{
	int Y, M, D, h, m, s;
	const char *source;

	if((parse_int(1, src, &h, 0) == parse_ok) &&
			(parse_int(2, src, &m, 0) == parse_ok))
	{
		if(parse_int(3, src, &s, 0) != parse_ok)
			s = 0;

		time_set_hms(h, m, s);
	}

	source = time_get(&h, &m, &s, &Y, &M, &D);

	string_format(dst, "%s: %04u/%02u/%02u %02u:%02u:%02u\n", source, Y, M, D, h, m, s);

	return(app_action_normal);
}

irom static void wlan_scan_done_callback(void *arg, STATUS status)
{
	struct bss_info *bss;

	static const char *status_msg[] =
	{
		"OK",
		"FAIL",
		"PENDING",
		"BUSY",
		"CANCEL"
	};

	static const char *auth_mode_msg[] =
	{
		"OTHER",
		"WEP",
		"WPA PSK",
		"WPA2 PSK",
		"WPA PSK + WPA2 PSK"
	};

	string_clear(&buffer_4k);
	string_format(&buffer_4k, "wlan scan result: %s\n", status <= CANCEL ? status_msg[status] : "<invalid>");
	string_format(&buffer_4k, "> %-16s  %-4s  %-4s  %-18s  %-6s  %s\n", "SSID", "CHAN", "RSSI", "AUTH", "OFFSET", "BSSID");

	for(bss = arg; bss; bss = bss->next.stqe_next)
		string_format(&buffer_4k, "> %-16s  %4u  %4d  %-18s  %6d  %02x:%02x:%02x:%02x:%02x:%02x\n",
				bss->ssid,
				bss->channel,
				bss->rssi,
				bss->authmode < AUTH_MAX ? auth_mode_msg[bss->authmode] : "<invalid auth>",
				bss->freq_offset,
				bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5]);

	wlan_scan_state = ws_finished;
}

irom static app_action_t application_function_wlan_ap_configure(const string_t *src, string_t *dst)
{
	string_new(, ssid, 64);
	string_new(, passwd, 64);
	int channel;

	if((parse_string(1, src, &ssid) == parse_ok) && (parse_string(2, src, &passwd) == parse_ok) &&
			(parse_int(3, src, &channel, 0) == parse_ok))
	{
		if((channel < 1) || (channel > 13))
		{
			string_format(dst, "> channel %d out of range (1-13)\n", channel);
			return(app_action_error);
		}

		if(string_length(&passwd) < 8)
		{
			string_format(dst, "> passwd \"%s\" too short (length must be >= 8)\n",
					string_to_ptr(&passwd));
			return(app_action_error);
		}

		if(!config_set_string("wlan.ap.ssid", -1, -1, &ssid, -1, -1))
		{
			string_cat(dst, "> cannot set config\n");
			return(app_action_error);
		}

		if(!config_set_string("wlan.ap.passwd", -1, -1, &passwd, -1, -1))
		{
			string_cat(dst, "> cannot set config\n");
			return(app_action_error);
		}

		if(!config_set_int("wlan.ap.channel", -1, -1, channel))
		{
			string_cat(dst, "> cannot set config\n");
			return(app_action_error);
		}
	}

	string_clear(&ssid);
	string_clear(&passwd);

	if(!config_get_string("wlan.ap.ssid", -1, -1, &ssid))
	{
		string_clear(&ssid);
		string_cat(&ssid, "<empty>");
	}

	if(!config_get_string("wlan.ap.passwd", -1, -1, &passwd))
	{
		string_clear(&passwd);
		string_cat(&passwd, "<empty>");
	}

	if(!config_get_int("wlan.ap.channel", -1, -1, &channel))
		channel = 0;

	string_format(dst, "> ssid: \"%s\", passwd: \"%s\", channel: %d\n",
			string_to_const_ptr(&ssid),
			string_to_const_ptr(&passwd),
			channel);

	return(app_action_normal);
}

irom static app_action_t application_function_wlan_client_configure(const string_t *src, string_t *dst)
{
	string_new(, ssid, 64);
	string_new(, passwd, 64);

	if((parse_string(1, src, &ssid) == parse_ok) && (parse_string(2, src, &passwd) == parse_ok))
	{
		if(string_length(&passwd) < 8)
		{
			string_format(dst, "> passwd \"%s\" too short (length must be >= 8)\n", string_to_ptr(&passwd));
			return(app_action_error);
		}

		if(!config_set_string("wlan.client.ssid", -1, -1, &ssid, -1, -1))
		{
			string_cat(dst, "> cannot set config\n");
			return(app_action_error);
		}

		if(!config_set_string("wlan.client.passwd", -1, -1, &passwd, -1, -1))
		{
			string_cat(dst, "> cannot set config\n");
			return(app_action_error);
		}
	}

	string_clear(&ssid);
	string_clear(&passwd);

	if(!config_get_string("wlan.client.ssid", -1, -1, &ssid))
	{
		string_clear(&ssid);
		string_cat(&ssid, "<empty>");
	}

	if(!config_get_string("wlan.client.passwd", -1, -1, &passwd))
	{
		string_clear(&passwd);
		string_cat(&passwd, "<empty>");
	}

	string_format(dst, "> ssid: \"%s\", passwd: \"%s\"\n",
			string_to_const_ptr(&ssid),
			string_to_const_ptr(&passwd));

	return(app_action_normal);
}

irom static app_action_t application_function_wlan_mode(const string_t *src, string_t *dst)
{
	unsigned int int_mode;
	config_wlan_mode_t mode;

	if(parse_string(1, src, dst) == parse_ok)
	{
		if(string_match(dst, "client"))
		{
			string_clear(dst);

			if(!config_set_int("wlan.mode", -1, -1, config_wlan_mode_client))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}

			if(!wlan_init())
			{
				string_cat(dst, "> cannot init\n");
				return(app_action_error);
			}

			return(app_action_disconnect);
		}

		if(string_match(dst, "ap"))
		{
			string_clear(dst);

			if(!config_set_int("wlan.mode", -1, -1, config_wlan_mode_ap))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}

			if(!wlan_init())
			{
				string_cat(dst, "> cannot init\n");
				return(app_action_error);
			}

			return(app_action_disconnect);
		}

		string_cat(dst, ": invalid wlan mode\n");
		return(app_action_error);
	}

	string_clear(dst);
	string_cat(dst, "> current mode: ");

	if(config_get_int("wlan.mode", -1, -1, &int_mode))
	{
		mode = (config_wlan_mode_t)int_mode;

		switch(mode)
		{
			case(config_wlan_mode_client):
			{
				string_cat(dst, "client mode");
				break;
			}

			case(config_wlan_mode_ap):
			{
				string_cat(dst, "ap mode");
				break;
			}

			default:
			{
				string_cat(dst, "unknown mode");
				break;
			}
		}
	}
	else
		string_cat(dst, "mode unset");

	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_wlan_list(const string_t *src, string_t *dst)
{
	if(wlan_scan_state != ws_finished)
	{
		string_cat(dst, "wlan scan: no results (yet)\n");
		return(app_action_normal);
	}

	string_copy_string(dst, &buffer_4k);
	wlan_scan_state = ws_inactive;
	return(app_action_normal);
}

irom static app_action_t application_function_wlan_scan(const string_t *src, string_t *dst)
{
	if(wlan_scan_state != ws_inactive)
	{
		string_cat(dst, "wlan-scan: already scanning\n");
		return(app_action_error);
	}

	if(ota_is_active())
	{
		string_cat(dst, "wlan-scan: ota active\n");
		return(app_action_error);
	}

	wlan_scan_state = ws_scanning;
	wifi_station_scan(0, wlan_scan_done_callback);
	string_cat(dst, "wlan scan started, use wlan-list to retrieve the results\n");

	return(app_action_normal);
}

irom attr_pure bool_t wlan_scan_active(void)
{
	return(wlan_scan_state != ws_inactive);
}

irom static app_action_t application_function_ntp_dump(const string_t *src, string_t *dst)
{
	ip_addr_t addr;
	int timezone;

	timezone = sntp_get_timezone();
	addr = sntp_getserver(0);

	string_cat(dst, "> server: ");
	string_ip(dst, addr);

	string_format(dst, "\n> time zone: GMT%c%d\n> ntp time: %s",
			timezone < 0 ? '-' : '+',
			timezone < 0 ? 0 - timezone : timezone,
			sntp_get_real_time(sntp_get_current_timestamp()));

	return(app_action_normal);
}

irom static app_action_t application_function_ntp_set(const string_t *src, string_t *dst)
{
	string_new(static, ip, 32);

	int					timezone, ix;
	ip_addr_to_bytes_t	a2b;

	if((parse_string(1, src, &ip) == parse_ok) && (parse_int(2, src, &timezone, 0) == parse_ok))
	{
		a2b.ip_addr = ip_addr(string_to_const_ptr(&ip));

		if((a2b.byte[0] == 0) && (a2b.byte[1] == 0) && (a2b.byte[2] == 0) && (a2b.byte[3] == 0))
			for(ix = 0; ix < 4; ix++)
				config_delete("ntp.server.%u", ix, -1, false);
		else
			for(ix = 0; ix < 4; ix++)
				if(!config_set_int("ntp.server.%u", ix, -1, a2b.byte[ix]))
				{
					string_clear(dst);
					string_cat(dst, "cannot set config\n");
					return(app_action_error);
				}

		if(timezone == 0)
			config_delete("ntp.tz", -1, -1, false);
		else
			if(!config_set_int("ntp.tz", -1, -1, timezone))
			{
				string_clear(dst);
				string_cat(dst, "cannot set config\n");
				return(app_action_error);
			}

		time_ntp_init();
	}

	return(application_function_ntp_dump(src, dst));
}

irom static app_action_t application_function_gpio_status_set(const string_t *src, string_t *dst)
{
	int trigger_io, trigger_pin;

	if((parse_int(1, src, &trigger_io, 0) == parse_ok) && (parse_int(2, src, &trigger_pin, 0) == parse_ok))
	{
		if((trigger_io < -1) || (trigger_io > io_id_size))
		{
			string_format(dst, "status trigger io %d/%d invalid\n", trigger_io, trigger_pin);
			return(app_action_error);
		}

		if((trigger_io < 0) || (trigger_pin < 0))
		{
			config_delete("trigger.status.io", -1, -1, false);
			config_delete("trigger.status.pin", -1, -1, false);
		}
		else
			if(!config_set_int("trigger.status.io", -1, -1, trigger_io) ||
					!config_set_int("trigger.status.pin", -1, -1, trigger_pin))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("trigger.status.io", -1, -1, &trigger_io))
		trigger_io = -1;

	if(!config_get_int("trigger.status.pin", -1, -1, &trigger_pin))
		trigger_pin = -1;

	string_format(dst, "status trigger at io %d/%d (-1 is disabled)\n",
			trigger_io, trigger_pin);

	return(app_action_normal);
}

irom static app_action_t application_function_gpio_assoc_set(const string_t *src, string_t *dst)
{
	int trigger_io, trigger_pin;

	if((parse_int(1, src, &trigger_io, 0) == parse_ok) && (parse_int(2, src, &trigger_pin, 0) == parse_ok))
	{
		if((trigger_io < -1) || (trigger_io > io_id_size))
		{
			string_format(dst, "association trigger io %d/%d invalid\n", trigger_io, trigger_pin);
			return(app_action_error);
		}

		if((trigger_io < 0) || (trigger_pin < 0))
		{
			config_delete("trigger.assoc.io", -1, -1, false);
			config_delete("trigger.assoc.pin", -1, -1, false);
		}
		else
			if(!config_set_int("trigger.assoc.io", -1, -1, trigger_io) ||
					!config_set_int("trigger.assoc.pin", -1, -1, trigger_pin))
			{
				string_cat(dst, "> cannot set config\n");
				return(app_action_error);
			}
	}

	if(!config_get_int("trigger.assoc.io", -1, -1, &trigger_io))
		trigger_io = -1;

	if(!config_get_int("trigger.assoc.pin", -1, -1, &trigger_pin))
		trigger_pin = -1;

	string_format(dst, "wlan association trigger at io %d/%d (-1 is disabled)\n",
			trigger_io, trigger_pin);

	return(app_action_normal);
}

static const application_function_table_t application_function_table[] =
{
	{
		"btp", "bridge-tcp-port",
		application_function_bridge_tcp_port,
		"set uart tcp bridge tcp port (default 23)"
	},
	{
		"btt", "bridge-tcp-timeout",
		application_function_bridge_tcp_timeout,
		"set uart tcp bridge tcp timeout (default 0)"
	},
	{
		"ctp", "command-tcp-port",
		application_function_command_tcp_port,
		"set command tcp port (default 24)"
	},
	{
		"ctt", "command-tcp-timeout",
		application_function_command_tcp_timeout,
		"set command tcp timeout (default 0)"
	},
	{
		"cd", "config-dump",
		application_function_config_dump,
		"dump config contents (stored in flash)"
	},
	{
		"cqs", "config-query-string",
		application_function_config_query_string,
		"query config string"
	},
	{
		"cqi", "config-query-int",
		application_function_config_query_int,
		"query config int"
	},
	{
		"cs", "config-set",
		application_function_config_set,
		"set config entry"
	},
	{
		"cde", "config-delete",
		application_function_config_delete,
		"delete config entry"
	},
	{
		"cw", "config-write",
		application_function_config_write,
		"write config to non-volatile storage"
	},
	{
		"db", "display-brightness",
		application_function_display_brightness,
		"set or show display brightness"
	},
	{
		"dd", "display-dump",
		application_function_display_dump,
		"shows all displays"
	},
	{
		"ddm", "display-default-message",
		application_function_display_default_message,
		"set default message",
	},
	{
		"dft", "display-flip-timeout",
		application_function_display_flip_timeout,
		"set the time between flipping of the slots",
	},
	{
		"ds", "display-set",
		application_function_display_set,
		"put content on display <slot> <timeout> <tag> <text>"
	},
	{
		"gas", "gpio-association-set",
		application_function_gpio_assoc_set,
		"set gpio to trigger on wlan association"
	},
	{
		"gss", "gpio-status-set",
		application_function_gpio_status_set,
		"set gpio to trigger on status update"
	},
	{
		"i2a", "i2c-address",
		application_function_i2c_address,
		"set i2c slave address",
	},
	{
		"i2r", "i2c-read",
		application_function_i2c_read,
		"read data from i2c slave",
	},
	{
		"i2w", "i2c-write",
		application_function_i2c_write,
		"write data to i2c slave",
	},
	{
		"im", "io-mode",
		application_function_io_mode,
		"config i/o pin",
	},
	{
		"ir", "io-read",
		application_function_io_read,
		"read from i/o pin",
	},
	{
		"it", "io-trigger",
		application_function_io_trigger,
		"trigger i/o pin",
	},
	{
		"iw", "io-write",
		application_function_io_write,
		"write to i/o pin",
	},
	{
		"isf", "io-set-flag",
		application_function_io_set_flag,
		"set i/o pin flag",
	},
	{
		"pp", "pwm-period",
		application_function_pwm_period,
		"set pwm period (rate = 200 ns / period)",
	},
	{
		"icf", "io-clear-flag",
		application_function_io_clear_flag,
		"clear i/o pin flag",
	},
	{
		"isi", "i2c-sensor-init",
		application_function_i2c_sensor_init,
		"(re-)init i2c sensor",
	},
	{
		"isr", "i2c-sensor-read",
		application_function_i2c_sensor_read,
		"read from i2c sensor",
	},
	{
		"isc", "i2c-sensor-calibrate",
		application_function_i2c_sensor_calibrate,
		"calibrate i2c sensor, use sensor factor offset",
	},
	{
		"isd", "i2c-sensor-dump",
		application_function_i2c_sensor_dump,
		"dump all i2c sensors",
	},
	{
		"nd", "ntp-dump",
		application_function_ntp_dump,
		"dump ntp information",
	},
	{
		"ns", "ntp-set",
		application_function_ntp_set,
		"set ntp <ip addr> <timezone GMT+x>",
	},
	{
		"?", "help",
		application_function_help,
		"help [command]",
	},
	{
		"or", "ota-read",
		application_function_ota_read,
		"ota-read length start chunk-size",
	},
	{
		"od", "ota-receive-data",
		application_function_ota_receive,
		"ota-receive-data",
	},
	{
		"ow", "ota-write",
		application_function_ota_write,
		"ota-write length [start]",
	},
	{
		"os", "ota-send-data",
		application_function_ota_send,
		"ota-send chunk_length data",
	},
	{
		"of", "ota-finish",
		application_function_ota_finish,
		"ota-finish md5sum",
	},
	{
		"oc", "ota-commit",
		application_function_ota_commit,
		"ota-commit",
	},
	{
		"q", "quit",
		application_function_quit,
		"quit",
	},
	{
		"r", "reset",
		application_function_reset,
		"reset",
	},
	{
		"s", "set",
		application_function_set,
		"set an option",
	},
	{
		"u", "unset",
		application_function_unset,
		"unset an option",
	},
	{
		"S", "stats",
		application_function_stats,
		"statistics",
	},
	{
		"ts", "time-set",
		application_function_time_set,
		"set time base [h m]",
	},
	{
		"ub", "uart-baud",
		application_function_uart_baud_rate,
		"set uart baud rate [1-1000000]",
	},
	{
		"ud", "uart-data",
		application_function_uart_data_bits,
		"set uart data bits [5/6/7/8]",
	},
	{
		"us", "uart-stop",
		application_function_uart_stop_bits,
		"set uart stop bits [1/2]",
	},
	{
		"up", "uart-parity",
		application_function_uart_parity,
		"set uart parity [none/even/odd]",
	},
	{
		"wac", "wlan-ap-configure",
		application_function_wlan_ap_configure,
		"configure access point mode wlan params, supply ssid, passwd and channel"
	},
	{
		"wcc", "wlan-client-configure",
		application_function_wlan_client_configure,
		"configure client mode wlan params, supply ssid and passwd"
	},
	{
		"wl", "wlan-list",
		application_function_wlan_list,
		"retrieve results from wlan-scan"
	},
	{
		"wm", "wlan-mode",
		application_function_wlan_mode,
		"set wlan mode: client or ap"
	},
	{
		"ws", "wlan-scan",
		application_function_wlan_scan,
		"scan wlan, use wlan-list to retrieve the results"
	},
	{
		"GET", "http-get",
		application_function_http_get,
		"get access over http"
	},
	{
		"", "",
		(void *)0,
		"",
	},
};
