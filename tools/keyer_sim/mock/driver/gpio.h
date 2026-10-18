#pragma once
typedef int gpio_num_t;
enum { GPIO_MODE_INPUT, GPIO_PULLUP_ONLY };
extern int g_pin[64];
static inline int  gpio_get_level(gpio_num_t p) { return g_pin[p]; }
static inline void gpio_reset_pin(gpio_num_t) {}
static inline void gpio_set_direction(gpio_num_t, int) {}
static inline void gpio_set_pull_mode(gpio_num_t, int) {}
