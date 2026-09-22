#pragma once
using uint = unsigned;
constexpr bool GPIO_IN = false;
void gpio_init(uint);
void gpio_set_dir(uint, bool);
void gpio_pull_up(uint);
int gpio_get(uint);
