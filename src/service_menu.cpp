#include <Arduino.h>
#include <SPI.h>
#include <Encoder.h>
#include <LiquidCrystal.h>
#include <MD_Menu.h>
#include <ResponsiveAnalogRead.h>

#include "service_menu.h"
#include "common_objs.h"
#include "settings.h"
#include "constants.h"

bool disp_control(MD_Menu::userDisplayAction_t action, char *msg = nullptr);
MD_Menu::userNavAction_t input_control(uint16_t &step);

MD_Menu::value_t *io_test_wrapper(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *tp_calib_wrapper(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *reboot_wrapper(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *clear_eeprom_wrapper(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *handle_list(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *handle_int(MD_Menu::mnuId_t id, bool get);
MD_Menu::value_t *handle_bool(MD_Menu::mnuId_t id, bool get);

MD_Menu::value_t menu_buf;
static uint8_t curr_button = 0;

const MD_Menu::mnuHeader_t menus[] = {
    {10, "Service Menu", 10, 19, 0},
    {11, "Reboot", 20, 21, 0},
    {12, "Button Config", 31, 32, 0},
};

const MD_Menu::mnuItem_t menu_items[] = {
    {10, "Button Test", MD_Menu::MNU_INPUT, 10},
    {11, "TP Test", MD_Menu::MNU_INPUT, 11},
    {12, "TP Calibration", MD_Menu::MNU_INPUT, 12}, // action 12 runs the calibration function
    {13, "TP Mode", MD_Menu::MNU_INPUT, 13},
    {14, "Button Conf...", MD_Menu::MNU_MENU, 12},
    {15, "TP ADC Zero", MD_Menu::MNU_INPUT, 15},
    {16, "DS4 Redir.", MD_Menu::MNU_INPUT, 16},
    {18, "Clear EEPROM", MD_Menu::MNU_INPUT, 18},
    {19, "Reboot...", MD_Menu::MNU_MENU, 11},

    {20, "Main System", MD_Menu::MNU_INPUT, 20}, // action 20 resets the MCU
    {21, "Bootloader", MD_Menu::MNU_INPUT, 21}, // action 21 reboots into bootloader
    
    //{30, "Source", MD_Menu::MNU_INPUT, 30},
    {31, "ID", MD_Menu::MNU_INPUT, 31},
    {32, "Assignment", MD_Menu::MNU_INPUT, 32}
};

const char BUTTON_NAMES[] = "NUL|U|L|D|R|SQR|XRO|CIR|TRI|L1|R1|L2|R2|SHR|OPT|L3|R3|PS|TP";
const char TP_MODES[] = TP_MODE_TP_N "|" TP_MODE_DPAD_N "|" TP_MODE_LR_N;

const MD_Menu::mnuInput_t menu_actions[] = {
    {10, "Press SEL", MD_Menu::INP_RUN, io_test_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {11, "Press SEL", MD_Menu::INP_RUN, io_test_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {12, "Press SEL", MD_Menu::INP_RUN, tp_calib_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {13, "Mode", MD_Menu::INP_LIST, handle_list, 4, 0, 0, 0, 0, 0, TP_MODES},
    {15, "Thres.", MD_Menu::INP_INT, handle_int, 4, 0, 0, 1023, 0, 10, nullptr},
    {16, "Y/N", MD_Menu::INP_BOOL, handle_bool, 1, 0, 0, 0, 0, 0, nullptr},
    {18, "Clear EEPROM?", MD_Menu::INP_RUN, clear_eeprom_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {20, "Restart?", MD_Menu::INP_RUN, reboot_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {21, "Reboot to BL?", MD_Menu::INP_RUN, reboot_wrapper, 0, 0, 0, 0, 0, 0, nullptr},
    {31, "ID", MD_Menu::INP_INT, handle_int, 2, 0, 0, 15, 0, 10, nullptr},
    {32, "Btn.", MD_Menu::INP_LIST, handle_list, 3, 0, 0, 0, 0, 0, BUTTON_NAMES}
};

MD_Menu TestMenu(
    input_control, disp_control,
    menus, ARRAY_SIZE(menus),
    menu_items, ARRAY_SIZE(menu_items),
    menu_actions, ARRAY_SIZE(menu_actions)
);

static bool qei_sw_block;
static int8_t qei_sw_state;
static uint32_t qei_sw_hold;
static int8_t _task;

#define LCD_UPDATE_INTERVAL 100

#define TASK_MENU 1
#define TASK_BTNTEST 2
#define TASK_TPTEST 3
#define TASK_TPCALIB 10

#define LONG_PRESS_THRESHOLD 500

void scan_qei_sw(void) {
    if (digitalRead(QEI_SW) == LOW && !qei_sw_block) {
        // starts fresh
        if (qei_sw_state == 0) {
            qei_sw_state = -1;
            qei_sw_hold = millis();
        // still holding
        } else if (qei_sw_state == -1 && (millis() - qei_sw_hold > LONG_PRESS_THRESHOLD)) {
            qei_sw_state = -2;
        }
    } else if (digitalRead(QEI_SW) == HIGH) {
        if (qei_sw_block) {
            qei_sw_block = false;
        // was holding
        } else if (qei_sw_state < 0) {
            qei_sw_state = -qei_sw_state;
        }
    }
}

void reset_qei_sw(void) {
    qei_sw_state = 0;
    qei_sw_hold = 0;
}

void button_test(void) {
    static uint32_t lcd_last_update = 0;
    static bool transition = true;
    uint16_t buttons;
    char buf[17] = {0};

    if (transition) {
        LCD.clear();
        LCD.home();
        LCD.print("Button Test");
        transition = false;
    }

    // if QEI_SW got pressed, return to menu
    if (qei_sw_state > 0) {
        reset_qei_sw();
        _task = TASK_MENU;
        TestMenu.reset();
        transition = true;
        return;
    }

    if ((millis() - lcd_last_update) >= LCD_UPDATE_INTERVAL) {
        lcd_last_update = millis();

        digitalWrite(BTN_CSB, LOW);
        SPI.beginTransaction(SPI4021);
        buttons = SPI.transfer(0x00);
        buttons |= SPI.transfer(0x00) << 8;
        digitalWrite(BTN_CSB, HIGH);
        SPI.endTransaction();
        
        for (int8_t i=0; i<16; i++) {
            buf[i] = ((buttons & (1 << i)) ? '-' : 'X');
        }

        Serial1.println(buf);
        LCD.setCursor(0, 1);
        LCD.print(buf);
    }
}

void tp_test(void) {
    static uint32_t lcd_last_update = 0;
    static bool transition = true;

    if (transition) {
        LCD.clear();
        LCD.home();
        LCD.print("TP Test");
        transition = false;
    }

    // if QEI_SW got pressed, return to menu
    if (qei_sw_state > 0) {
        reset_qei_sw();
        _task = TASK_MENU;
        TestMenu.reset();
        transition = true;
        return;
    }

    if ((millis() - lcd_last_update) >= LCD_UPDATE_INTERVAL) {
        lcd_last_update = millis();

        SoftPotMagic.update();

        LCD.setCursor(0, 1);
        LCD.print("                ");
        LCD.setCursor(0, 1);
        LCD.print(SoftPotMagic.pos1());
        LCD.print(" ");
        LCD.print(SoftPotMagic.pos2());
        LCD.print(" ");
        LCD.print(SoftPotMagic.leftADC());
        LCD.print(" ");
        LCD.print(SoftPotMagic.rightADC());
    }
}

void tp_calib(void) {
    static bool transition = true;
    static int8_t calib_state = 0;
    static int8_t zero_count = 0;
    bool calib_result;
    const calib_t *c;
    calib_t *s;

    if (transition) {
        LCD.clear();
        LCD.home();
        LCD.print("TP Calibration");
        transition = false;
    }

    // abort by holding QEI_SW
    if (qei_sw_state == 2) {
        reset_qei_sw();
        _task = TASK_MENU;
        TestMenu.reset();
        transition = true;
        calib_state = 0;
        zero_count = 0;
        return;
    }

    // state machine, or event driven, whatever you want to call it
    switch (calib_state) {
        // Left not calibrated, not prompted
        case 0:
            LCD.setCursor(0, 1);
            LCD.print("Calib left");
            calib_state++;
            break;
        // Left not calibrated, prompted
        case 1:
            if (qei_sw_state == 1) {
                reset_qei_sw();
                calib_result = SoftPotMagic.autoCalibLeft();
                if (calib_result) {
                    calib_state++;
                }
            }
            break;
        case 2:
            LCD.setCursor(0, 1);
            LCD.print("Calib right");
            calib_state++;
            break;
        case 3:
            if (qei_sw_state == 1) {
                reset_qei_sw();
                calib_result = SoftPotMagic.autoCalibRight();
                if (calib_result) {
                    calib_state++;
                }
            }
            break;
        case 4:
            LCD.setCursor(0, 1);
            LCD.print("Calib zero (3 times)");
            calib_state++;
            break;
        case 5:
            if (qei_sw_state == 1) {
                reset_qei_sw();
                calib_result = SoftPotMagic.autoCalibZero(zero_count == 0 ? true : false);
                if (calib_result) {
                    zero_count++;
                }
                if (zero_count >= 3) {
                    calib_state++;
                }
            }
            break;
        case 6:
            LCD.setCursor(0, 1);
            LCD.print("Saving...  ");
            s = &(controller_settings.tp_calib);
            c = SoftPotMagic.getCalib();
            memcpy(s, c, sizeof(calib_t));
            settings_save(&controller_settings);
            LCD.setCursor(0, 1);
            LCD.print("Done. Press SEL");
            calib_state++;
            break;
        case 7:
            if (qei_sw_state == 1) {
                reset_qei_sw();
                _task = TASK_MENU;
                TestMenu.reset();
                transition = true;
                calib_state = 0;
                zero_count = 0;
            }
            break;
    }
}

bool disp_control(MD_Menu::userDisplayAction_t action, char *msg) {
    static char _blank[LCD_COLS+1] = {0};
    switch (action) {
        case MD_Menu::DISP_INIT:
            LCD.begin(16, 2);
            LCD.clear();
            LCD.noCursor();
            memset(_blank, ' ', LCD_COLS);
            break;
        case MD_Menu::DISP_CLEAR:   
            LCD.clear();
            break;
        case MD_Menu::DISP_L0:
            LCD.setCursor(0, 0);
            LCD.print(_blank);
            LCD.setCursor(0, 0);
            LCD.print(msg);
            break;
        case MD_Menu::DISP_L1:
            LCD.setCursor(0, 1);
            LCD.print(_blank);
            LCD.setCursor(0, 1);
            LCD.print(msg);
            break;
    }
    return true;
}

MD_Menu::userNavAction_t input_control(uint16_t &step) {
    long qei_step = QEI.read() / 4;
    MD_Menu::userNavAction_t action = MD_Menu::NAV_NULL;

    // short hold
    if (qei_sw_state > 0) {
        step = 1;
        if (qei_sw_state == 1) {
            action = MD_Menu::NAV_SEL;
        // long hold
        } else if (qei_sw_state == 2) {
            action = MD_Menu::NAV_ESC;
        }
        reset_qei_sw();
    } else if (qei_step > 0) {
        step = (qei_step > 0xffff) ? 0xffff : qei_step;
        action = MD_Menu::NAV_INC;
        QEI.write(0);
    } else if (qei_step < 0) {
        step = (qei_step < -0xffff) ? 0xffff : -qei_step;
        action = MD_Menu::NAV_DEC;
        QEI.write(0);
    }
    return action;
}

MD_Menu::value_t *handle_list(MD_Menu::mnuId_t id, bool get) {
    switch (id) {
        case 13:
            if (get) {
                menu_buf.value = controller_settings.default_tp_mode;
            } else {
                controller_settings.default_tp_mode = menu_buf.value;
                settings_save(&controller_settings);
            }
            break;
        case 32:
            if (get) {
                menu_buf.value = controller_settings.button_mapping[curr_button];
            } else {
                controller_settings.button_mapping[curr_button] = menu_buf.value;
                settings_save(&controller_settings);
            }
            break;
        default:
            return nullptr;
    }
    return &menu_buf;
}

MD_Menu::value_t *handle_int(MD_Menu::mnuId_t id, bool get) {
    switch (id) {
        case 15:
            if (get) {
                menu_buf.value = controller_settings.tp_calib.zeroLevel;
            } else {
                controller_settings.tp_calib.zeroLevel = menu_buf.value;
                settings_save(&controller_settings);
            }
            break;
        case 31:
            if (get) {
                menu_buf.value = curr_button & 0x0f;
            } else {
                curr_button = menu_buf.value & 0x0f;
            }
            break;
        default:
            return nullptr;
    }
    return &menu_buf;
}

MD_Menu::value_t *handle_bool(MD_Menu::mnuId_t id, bool get) {
    switch (id) {
        case 16:
            if (get) {
                menu_buf.value = controller_settings.ds4_passthrough;
            } else {
                controller_settings.ds4_passthrough = menu_buf.value;
                settings_save(&controller_settings);
            }
            break;
        default:
            return nullptr;
    }
    return &menu_buf;
}

MD_Menu::value_t *io_test_wrapper(MD_Menu::mnuId_t id, bool get) {
    if (!get) {
        switch (id) {
            case 10:
                _task = TASK_BTNTEST;
                break;
            case 11:
                _task = TASK_TPTEST;
                break;
        }
    }
    return nullptr;
}

MD_Menu::value_t *tp_calib_wrapper(MD_Menu::mnuId_t id, bool get) {
    if (!get && id == 12) {
        _task = TASK_TPCALIB;
    }
    return nullptr;
}

MD_Menu::value_t *reboot_wrapper(MD_Menu::mnuId_t id, bool get) {
    if (!get) {
        // issue reset request then halt
        switch (id) {
            case 20:
                disp_control(MD_Menu::DISP_CLEAR);
                LCD.setCursor(0, 0);
                LCD.print("Restart");
                (*(volatile uint32_t *) 0xe000ed0c) = 0x05fa0004;
                while (1);
                break;
            case 21:
                disp_control(MD_Menu::DISP_CLEAR);
                LCD.setCursor(0, 0);
                LCD.print("Jump to BL");
                _reboot_Teensyduino_();
                while (1);
                break;
        }
    }
    return nullptr;
}

MD_Menu::value_t *clear_eeprom_wrapper(MD_Menu::mnuId_t id, bool get) {
    if (!get) {
        settings_init(&controller_settings);
        settings_save(&controller_settings);
    }
    return nullptr;
}

static void service_menu_setup(void) {
    pinMode(QEI_SW, INPUT_PULLUP);
    pinMode(BTN_CSL, OUTPUT);
    pinMode(BTN_CSB, OUTPUT);
    digitalWrite(BTN_CSL, HIGH);
    digitalWrite(BTN_CSB, HIGH);

    settings_load(&controller_settings);

    qei_sw_block = true;
    disp_control(MD_Menu::DISP_INIT, nullptr);
    QEI.write(0);
    SPI.begin();

    digitalWrite(BTN_CSL, LOW);
    SPI.beginTransaction(SPI596);
    SPI.transfer(0x00);
    digitalWrite(BTN_CSL, HIGH);
    SPI.endTransaction();

    reset_qei_sw();
    
    SoftPotMagic.begin(SP_L, SP_R, respAnalogRead);
    SoftPotMagic.setCalib(&(controller_settings.tp_calib));
    SoftPotMagic.setMinGapRatio(.10f);

    TestMenu.begin();
    TestMenu.setMenuWrap(true);
    _task = TASK_MENU;
}

static void service_menu_loop(void) {
    // persistent tasks
    scan_qei_sw();
    RAL.update();
    RAR.update();

    // foreground task
    switch (_task) {
        case TASK_MENU:
            if (!TestMenu.isInMenu()) {
                QEI.write(0);
                TestMenu.runMenu(true);
            } else {
                TestMenu.runMenu();
            }
            break;
        case TASK_BTNTEST:
            button_test();
            break;
        case TASK_TPTEST:
            tp_test();
            break;
        case TASK_TPCALIB:
            tp_calib();
            break;
    }
}

void service_menu_main(void) {
    service_menu_setup();
    while (1) {
        service_menu_loop();
    }
}