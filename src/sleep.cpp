

#include "pico.h"
#include "sleep.h"
#include "hardware/rtc.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/xosc.h"
#include "rosc.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "platform/mbed_wait_api.h"

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
                    (src_hz/265));

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





void sleepFinished(){

    rosc_write(&rosc_hw->ctrl, ROSC_CTRL_ENABLE_BITS);


    wait_us(100*1000);

    scb_hw->scr = t_scb_orig;
    clocks_hw->sleep_en0 = t_en0_orig;
    clocks_hw->sleep_en1 = t_en1_orig;
    wait_us(200*1000);
    clocks_init();
    wait_us(200*1000);

    callbackFunc();
}


void sleep_goto_sleep_until(datetime_t *t_alarm, rtc_callback_t callback, bool xosc_en){
    if(xosc_en){
        sleep_run_from_dormant_source(DORMANT_SOURCE_XOSC);
    }else{
        sleep_run_from_dormant_source(DORMANT_SOURCE_ROSC);
    }

    callbackFunc = callback;

    t_scb_orig = scb_hw->scr;
    t_en0_orig = clocks_hw->sleep_en0;
    t_en1_orig = clocks_hw->sleep_en1;


    sleep_goto_sleep_until_org(t_alarm, &sleepFinished);

}

void sleep_goto_sleep_for(long seconds, rtc_callback_t callback, bool xosc_en){
    ss = seconds % 60;
    seconds /= 60;
    mm = seconds % 60;
    seconds /= 60;
    hh = seconds % 24;
    uint16_t days = seconds / 24;
    uint8_t leap;
    for (yOff = 0;; ++yOff) {
        leap = yOff % 4 == 0;
        if (days < 365U + leap)
            break;
        days -= 365 + leap;
    }
    for (m = 1; m < 12; ++m) {
        uint8_t daysPerMonth = daysInMonth[m - 1];
        if (leap && m == 2)
            ++daysPerMonth;
        if (days < daysPerMonth)
            break;
        days -= daysPerMonth;
    }
    d = days + 1;

    rtc_get_datetime(&_t_alarm);

    _t_alarm.year = _t_alarm.year + yOff;
    _t_alarm.month = _t_alarm.month + m-1;
    _t_alarm.day = _t_alarm.day + d-1;
    _t_alarm.hour = _t_alarm.hour + hh;
    _t_alarm.min = _t_alarm.min + mm;
    _t_alarm.sec = _t_alarm.sec + ss;


    sleep_goto_sleep_until(&_t_alarm, callback, xosc_en);




}

