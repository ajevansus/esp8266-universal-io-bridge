#ifndef util_h
#define util_h

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

#include <c_types.h>
#include <osapi.h>
#include <ets_sys.h>

typedef enum
{
	off = 0,
	no = 0,
	on = 1,
	yes = 1
} bool_t;

_Static_assert(sizeof(bool_t) == 4, "sizeof(bool_t) != 4");

#define irom __attribute__((section(".irom0.text")))
#define iram __attribute__((section(".text")))
#define noinline __attribute__ ((noinline))
#define attr_pure __attribute__ ((pure))
#define attr_const __attribute__ ((const))

// replacement for nasty #defines that give warnings

void pin_func_select(uint32_t pin_name, uint32_t pin_func);

// prototypes missing

int ets_vsnprintf(char *, size_t, const char *, va_list);

void ets_isr_attach(int, void *, void *);
void ets_isr_mask(unsigned intr);
void ets_isr_unmask(unsigned intr);
void ets_timer_arm_new(ETSTimer *, uint32_t, bool, int);
void ets_timer_disarm(ETSTimer *);
void ets_timer_setfn(ETSTimer *, ETSTimerFunc *, void *);
void ets_delay_us(uint16_t);

void *pvPortMalloc(size_t size, const char *file, unsigned int line);
void *pvPortZalloc(size_t size, const char *file, unsigned int line);
void *pvPortRealloc(void *ptr, size_t size, const char *file, unsigned int line);

// local utility functions missing from libc

int snprintf(char *, size_t, const char *, ...) __attribute__ ((format (printf, 3, 4)));
void *malloc(size_t);
void *zalloc(size_t);
void *realloc(void *, size_t);

// other convenience functions

void reset(void);
const char *yesno(bool_t value);
const char *onoff(bool_t value);
int dprintf(const char *fmt, ...);
void msleep(unsigned int);
unsigned int double_to_string(double value, unsigned int precision, double top_decimal, unsigned int size, char *dst);
double string_to_double(const char *);

#endif
