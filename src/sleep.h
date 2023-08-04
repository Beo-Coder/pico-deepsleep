/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PICO_SLEEP_H_
#define _PICO_SLEEP_H_

#include "pico.h"
#include "hardware/rtc.h"








void sleep_goto_sleep_until(datetime_t *t_alarm, rtc_callback_t callback, bool xosc_en = true);

void sleep_goto_sleep_for(uint32_t seconds, rtc_callback_t callback, bool xosc_en = true);








#endif
