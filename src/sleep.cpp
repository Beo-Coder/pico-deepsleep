

#include "pico.h"
#include "sleep.h"
#include "hardware/rtc.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/xosc.h"
#include "rosc.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "pico/time.h"
//#include "Arduino.h"

static uint t_scb_orig;
static uint t_en0_orig;
static uint t_en1_orig;


static datetime_t t = {
        .year = 0,
        .month = 1,
        .day = 1,
        .dotw = 0,
        .hour = 0,
        .min = 0,
        .sec = 0
};

datetime_t _t_alarm = {
        .year = 0,
        .month = 0,
        .day = 0,
        .dotw = -1,
        .hour = 0,
        .min = 0,
        .sec = 0
};


static int8_t yOff; ///< Year offset from 2000
static int8_t m;    ///< Month 1-12
static int8_t d;    ///< Day 1-31
static int8_t hh;   ///< Hours 0-23
static int8_t mm;   ///< Minutes 0-59
static int8_t ss;   ///< Seconds 0-59

const uint8_t daysInMonth[11] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30};

static rtc_callback_t callbackFunc;


typedef enum {
    DORMANT_SOURCE_NONE,
    DORMANT_SOURCE_XOSC,
    DORMANT_SOURCE_ROSC
} dormant_source_t;

static dormant_source_t _dormant_source;

bool dormant_source_valid(dormant_source_t dormant_source) {
    return (dormant_source == DORMANT_SOURCE_XOSC) || (dormant_source == DORMANT_SOURCE_ROSC);
}


void sleep_run_from_dormant_source(dormant_source_t dormant_source) {
    assert(dormant_source_valid(dormant_source));
    _dormant_source = dormant_source;

    // FIXME: Just defining average rosc freq here.
    uint src_hz = (dormant_source == DORMANT_SOURCE_XOSC) ? XOSC_MHZ * MHZ : 7 * MHZ;
    uint clk_ref_src = (dormant_source == DORMANT_SOURCE_XOSC) ?
                       CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC :
                       CLOCKS_CLK_REF_CTRL_SRC_VALUE_ROSC_CLKSRC_PH;

    // CLK_REF = XOSC or ROSC
    clock_configure(clk_ref,
                    clk_ref_src,
                    0, // No aux mux
                    src_hz,
                    src_hz);

    // CLK SYS = CLK_REF
    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                    0, // Using glitchless mux
                    src_hz,
                    src_hz);

    // CLK USB = 0MHz
    clock_stop(clk_usb);

    // CLK ADC = 0MHz
    clock_stop(clk_adc);

    // CLK RTC = ideally XOSC (12MHz) / 256 = 46875Hz but could be rosc
    uint clk_rtc_src = (dormant_source == DORMANT_SOURCE_XOSC) ?
                       CLOCKS_CLK_RTC_CTRL_AUXSRC_VALUE_XOSC_CLKSRC :
                       CLOCKS_CLK_RTC_CTRL_AUXSRC_VALUE_ROSC_CLKSRC_PH;

    clock_configure(clk_rtc,
                    0, // No GLMUX
                    clk_rtc_src,
                    src_hz,
                    (src_hz / 265));

    // CLK PERI = clk_sys. Used as reference clock for Peripherals. No dividers so just select and enable
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    src_hz,
                    src_hz);

    pll_deinit(pll_sys);
    pll_deinit(pll_usb);

    // Assuming both xosc and rosc are running at the moment
    if (dormant_source == DORMANT_SOURCE_XOSC) {
        // Can disable rosc
        rosc_disable();
    } else {
        // Can disable xosc
        xosc_disable();


    }

    // Reconfigure uart with new clocks
    //setup_default_uart();
}

// Go to sleep until woken up by the RTC
void sleep_goto_sleep_until_org(datetime_t *t, rtc_callback_t callback) {
    // We should have already called the sleep_run_from_dormant_source function
    assert(dormant_source_valid(_dormant_source));

    // Turn off all clocks when in sleep mode except for RTC
    clocks_hw->sleep_en0 = CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS;
    clocks_hw->sleep_en1 = 0x0;

    rtc_set_alarm(t, callback);

    uint save = scb_hw->scr;
    // Enable deep sleep at the proc
    scb_hw->scr = save | M0PLUS_SCR_SLEEPDEEP_BITS;

    // Go to sleep
    __wfi();
}


void sleepFinished() {

    rosc_write(&rosc_hw->ctrl, ROSC_CTRL_ENABLE_BITS);


    sleep_ms(100);
    scb_hw->scr = t_scb_orig;
    clocks_hw->sleep_en0 = t_en0_orig;
    clocks_hw->sleep_en1 = t_en1_orig;
    sleep_ms(200);
    clocks_init();
    sleep_ms(200);

    callbackFunc();
}


void sleep_goto_sleep_until(datetime_t *t_alarm, rtc_callback_t callback, bool xosc_en) {
    if (xosc_en) {
        sleep_run_from_dormant_source(DORMANT_SOURCE_XOSC);
    } else {
        sleep_run_from_dormant_source(DORMANT_SOURCE_ROSC);
    }

    callbackFunc = callback;

    t_scb_orig = scb_hw->scr;
    t_en0_orig = clocks_hw->sleep_en0;
    t_en1_orig = clocks_hw->sleep_en1;


    sleep_goto_sleep_until_org(t_alarm, &sleepFinished);

}

int is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint32_t days_in_years(uint16_t years) {
    uint32_t days = 0;
    for (uint16_t y = 2000; y < 2000 + years; ++y) {
        days += 365 + is_leap_year(y);
    }
    return days;
}

uint32_t days_in_months(uint16_t year, uint8_t month) {
    uint32_t days = 0;
    for (uint8_t m = 1; m < month; ++m) {
        days += daysInMonth[m - 1];
        if (m == 2 && is_leap_year(year))
            days++; // Add one more day for February in a leap year
    }
    return days;
}

uint32_t
date_to_seconds(uint16_t years, uint8_t months, uint8_t days, uint8_t hours, uint8_t minutes, uint8_t seconds) {
    uint32_t totalSeconds = 0;

    totalSeconds += days_in_years(years) * 24 * 60 * 60;
    totalSeconds += days_in_months(2000 + years, months) * 24 * 60 * 60;
    totalSeconds += (days - 1) * 24 * 60 * 60;
    totalSeconds += hours * 60 * 60;
    totalSeconds += minutes * 60;
    totalSeconds += seconds;

    return totalSeconds;
}

void seconds_to_date(uint32_t totalSeconds, uint16_t *years, uint8_t *months, uint8_t *days, uint8_t *hours,
                     uint8_t *minutes, uint8_t *seconds) {
    // Extract seconds, minutes, and hours
    *seconds = totalSeconds % 60;
    totalSeconds /= 60;
    *minutes = totalSeconds % 60;
    totalSeconds /= 60;
    *hours = totalSeconds % 24;

    // Extract days
    uint16_t daysCount = totalSeconds / 24;
    uint8_t leap;
    uint16_t yOff;

    // Calculate years
    for (yOff = 0;; ++yOff) {
        leap = yOff % 4 == 0;
        if (daysCount < 365U + leap)
            break;
        daysCount -= 365 + leap;
    }
    *years = yOff;

    // Calculate months and remaining days
    for (*months = 1; *months < 12; ++(*months)) {
        uint8_t daysPerMonth = daysInMonth[(*months) - 1];
        if (leap && (*months) == 2)
            ++daysPerMonth;
        if (daysCount < daysPerMonth)
            break;
        daysCount -= daysPerMonth;
    }
    *days = daysCount + 1;
}

void sleep_goto_sleep_for(uint32_t seconds, rtc_callback_t callback, bool xosc_en) {

    rtc_get_datetime(&_t_alarm);


    seconds += date_to_seconds(_t_alarm.year, _t_alarm.month, _t_alarm.day, _t_alarm.hour, _t_alarm.min, _t_alarm.sec);

    uint16_t year;
    uint8_t month, day, hour, minute, second;


    seconds_to_date(seconds, &year, &month, &day, &hour, &minute, &second);


    _t_alarm.year = year;
    _t_alarm.month = month;
    _t_alarm.day = day;
    _t_alarm.hour = hour;
    _t_alarm.min = minute;
    _t_alarm.sec = second;


    sleep_goto_sleep_until(&_t_alarm, callback, xosc_en);


}

