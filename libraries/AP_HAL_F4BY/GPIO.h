/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
#pragma once

#include "AP_HAL_F4BY.h"

#define F4BY_GPIO_PIEZO_PIN              110
#define F4BY_GPIO_D1_PIN				 70
#define F4BY_GPIO_D2_PIN			     71
#define F4BY_GPIO_D3_PIN				 72
#define F4BY_GPIO_D4_PIN				 73
#define F4BY_GPIO_D5_PIN				 74

/*
  start servo channels used as GPIO at 50. Pin 50 is
  the first FMU servo pin
 */
#define F4BY_GPIO_FMU_SERVO_PIN(n)       (n+50)

#if CONFIG_HAL_BOARD == HAL_BOARD_F4BY
 # define HAL_GPIO_A_LED_PIN        27
 # define HAL_GPIO_B_LED_PIN        26
 # define HAL_GPIO_C_LED_PIN        25
 # define HAL_GPIO_D_LED_PIN        24
 # define HAL_GPIO_LED_ON           LOW
 # define HAL_GPIO_LED_OFF          HIGH
#endif

class F4BY::F4BYGPIO : public AP_HAL::GPIO {
public:
    F4BYGPIO();
    void    init();
    void    pinMode(uint8_t pin, uint8_t output);
    int8_t  analogPinToDigitalPin(uint8_t pin);
    uint8_t read(uint8_t pin);
    void    write(uint8_t pin, uint8_t value);
    void    toggle(uint8_t pin);

    /* Alternative interface: */
    AP_HAL::DigitalSource* channel(uint16_t n);

    /* Interrupt interface: */
    bool attach_interrupt(uint8_t interrupt_num, AP_HAL::Proc p, uint8_t mode);

    /* return true if USB cable is connected */
    bool usb_connected(void);

    // used by UART code to avoid a hw bug in the AUAV-X2
    void set_usb_connected(void) { _usb_connected = true; }

private:
    int _led_fd = -1;
    int _tone_alarm_fd = -1;
    int _gpio_fmu_fd = -1;
    int _gpio_io_fd = -1;
    bool _usb_connected = false;
};

class F4BY::F4BYDigitalSource : public AP_HAL::DigitalSource {
public:
    F4BYDigitalSource(uint8_t v);
    void    mode(uint8_t output);
    uint8_t read();
    void    write(uint8_t value);
    void    toggle();
private:
    uint8_t _v;
};
