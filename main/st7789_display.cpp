#include "st7789_display.h"
#if CONFIG_ION_ST7789

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "bat.h"
#include "board_button.h"
#include "st7789.h"

static const char *TAG = "st7789_display";

// The on-screen start button mimics the board button, including its hold time.
#ifndef CONFIG_ESP32_BUTTON_LONG_PRESS_DURATION_MS
#define CONFIG_ESP32_BUTTON_LONG_PRESS_DURATION_MS 2000
#endif

// The CYD's touch controller occupies these pins; catch configs that also use them.
#if ST7789_HAS_TOUCH
#define IS_TOUCH_PIN(p) ((p) == ST7789_TOUCH_PIN_SCLK || (p) == ST7789_TOUCH_PIN_MOSI || \
                         (p) == ST7789_TOUCH_PIN_MISO || (p) == ST7789_TOUCH_PIN_CS)
#if CONFIG_ION_LIGHT && IS_TOUCH_PIN(CONFIG_ION_LIGHT_PIN)
#error "CONFIG_ION_LIGHT_PIN is used by the CYD touch controller"
#endif
#if CONFIG_ION_RELAY && IS_TOUCH_PIN(CONFIG_ION_RELAY_PIN)
#error "CONFIG_ION_RELAY_PIN is used by the CYD touch controller"
#endif
#if CONFIG_ION_CHARGE && IS_TOUCH_PIN(CONFIG_ION_CHARGE_PIN)
#error "CONFIG_ION_CHARGE_PIN is used by the CYD touch controller"
#endif
#if CONFIG_ION_BUTTON && (IS_TOUCH_PIN(CONFIG_ION_BUTTON_BOARD_PIN) || IS_TOUCH_PIN(CONFIG_ION_BUTTON_EXTERNAL_PIN))
#error "A CONFIG_ION_BUTTON_*_PIN is used by the CYD touch controller"
#endif
#if IS_TOUCH_PIN(CONFIG_ION_LED_PIN) || IS_TOUCH_PIN(CONFIG_ION_RXD) || IS_TOUCH_PIN(CONFIG_ION_TXD)
#error "CONFIG_ION_LED_PIN / CONFIG_ION_RXD / CONFIG_ION_TXD is used by the CYD touch controller"
#endif
#endif

// How often the LVGL task checks for a new snapshot from the main loop.
#define APPLY_PERIOD_MS 50

#define TILE_GAP 4
#define DASH_BTN_W 72

#define COLOR_TILE_BG lv_color_hex(0x1C1C1C)
#define COLOR_TITLE lv_color_hex(0x9E9E9E)
#define COLOR_VALUE lv_color_white()
#define COLOR_LIGHT_ON lv_color_hex(0xFFC107)
#define COLOR_LIGHT_OFF lv_color_hex(0x424242)
#define COLOR_LIGHT_TEXT_ON lv_color_black()
#define COLOR_BAR lv_color_hex(0x43A047)
#define COLOR_BOARD_BTN lv_color_hex(0x2E7D32)
#define COLOR_BOARD_BTN_PRESSED lv_color_hex(0x66BB6A)

#define START_MARGIN 12
#define START_BTN_W 140

// Snapshot of what should be on screen. Written by the main (motor) loop,
// read by the LVGL task. The main loop never touches LVGL itself, so it never
// blocks on the LVGL mutex while a frame is being rendered/flushed.
struct DashState {
    st7789_display_type type;
    bool screen;
    bool light;
    uint8_t assist;
    uint16_t speed;
    uint32_t trip1;
    uint8_t bat;
};

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static DashState pendingState;
static bool pendingDirty = false;

// LVGL objects
static lv_obj_t *rideScreen;
static lv_obj_t *speedVal;
static lv_obj_t *assistVal;
static lv_obj_t *tripVal;
static lv_obj_t *batVal;
static lv_obj_t *batBar;
static lv_obj_t *lightBtn;
static lv_obj_t *lightBtnText;

static lv_obj_t *chargeScreen;
static lv_obj_t *chargeVal;
static lv_obj_t *chargeBar;

// Shown whenever the dashboard is "off" (at boot, and after the motor turned off).
static lv_obj_t *startScreen;
static lv_obj_t *startBatVal;
static lv_obj_t *startBatBar;

// Assist mode: 0:'0', 1:'1', 2:'2', 3:'3', 4:'P', 5:'R', 7:'4'.
static const char *assistLevelText(uint8_t assist) {
    switch(assist) {
        case 0: return "0";
        case 1: return "1";
        case 2: return "2";
        case 3: return "3";
        case 4: return "P";
        case 5: return "R";
        case 7: return "4";
        default: return "-";
    }
}

// Only touch the label (and trigger a redraw) when the text actually changed.
static void setText(lv_obj_t *label, const char *text) {
    if(strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *createPlainContainer(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *createLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *createBar(lv_obj_t *parent, int32_t w, int32_t h) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x424242), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COLOR_BAR, LV_PART_INDICATOR);
    return bar;
}

// A dark rounded tile with a small title at the top and a big value centered below it.
static lv_obj_t *createTile(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *title) {
    lv_obj_t *tile = createPlainContainer(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_bg_color(tile, COLOR_TILE_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);

    lv_obj_t *titleLabel = createLabel(tile, &lv_font_montserrat_16, COLOR_TITLE, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *valueLabel = createLabel(tile, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(valueLabel, LV_ALIGN_CENTER, 0, 8);
    return valueLabel;
}

// The board button behaves exactly like CONFIG_ION_BUTTON_BOARD_PIN: releasing
// it is a short press (also after a long hold), and holding it for the button
// component's long press time is a long press.
static void boardButtonCb(lv_event_t *e) {
    switch(lv_event_get_code(e)) {
        case LV_EVENT_LONG_PRESSED:
            boardButtonEvent(BOARD_BUTTON_HELD);
            break;
        case LV_EVENT_RELEASED:
            boardButtonEvent(BOARD_BUTTON_UP);
            break;
        default:
            break;
    }
}

static void lightButtonCb(lv_event_t *e) {
    lightButtonPress();
}

static lv_obj_t *createButton(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    return btn;
}

// An on-screen copy of the board button (start screen "START", dashboard "+").
static lv_obj_t *createBoardButton(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *btn = createButton(parent, x, y, w, h);
    lv_obj_set_style_bg_color(btn, COLOR_BOARD_BTN, 0);
    lv_obj_set_style_bg_color(btn, COLOR_BOARD_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, boardButtonCb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(btn, boardButtonCb, LV_EVENT_RELEASED, NULL);
    return btn;
}

static void createRideScreen(lv_obj_t *scr) {
    rideScreen = createPlainContainer(scr);
    lv_obj_set_size(rideScreen, ST7789_WIDTH, ST7789_HEIGHT);

    // Left column: "+" (the board button: assist level up) above the light switch.
    const int32_t btnH = (ST7789_HEIGHT - 3 * TILE_GAP) / 2;

    lv_obj_t *plusBtn = createBoardButton(rideScreen, TILE_GAP, TILE_GAP, DASH_BTN_W, btnH);
    lv_obj_t *plusText = createLabel(plusBtn, &lv_font_montserrat_32, COLOR_VALUE, "+");
    lv_obj_center(plusText);

    // The light button's color and text show the light state (CHECKED = on).
    // Its label inherits the text color from the button, so it follows the state.
    lightBtn = createButton(rideScreen, TILE_GAP, 2 * TILE_GAP + btnH, DASH_BTN_W, btnH);
    lv_obj_set_style_bg_color(lightBtn, COLOR_LIGHT_OFF, 0);
    lv_obj_set_style_bg_color(lightBtn, COLOR_LIGHT_ON, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(lightBtn, COLOR_VALUE, 0);
    lv_obj_set_style_text_color(lightBtn, COLOR_LIGHT_TEXT_ON, LV_STATE_CHECKED);
    lv_obj_add_event_cb(lightBtn, lightButtonCb, LV_EVENT_CLICKED, NULL);
    lightBtnText = lv_label_create(lightBtn);
    lv_obj_set_style_text_font(lightBtnText, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(lightBtnText, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lightBtnText, "LIGHT\nOFF");
    lv_obj_center(lightBtnText);

    // Right: 2x2 value tiles.
    const int32_t tilesX = 2 * TILE_GAP + DASH_BTN_W;
    const int32_t tileW = (ST7789_WIDTH - tilesX - 2 * TILE_GAP) / 2;
    const int32_t tileH = (ST7789_HEIGHT - 3 * TILE_GAP) / 2;
    const int32_t col0 = tilesX;
    const int32_t col1 = tilesX + tileW + TILE_GAP;
    const int32_t row0 = TILE_GAP;
    const int32_t row1 = 2 * TILE_GAP + tileH;

    speedVal = createTile(rideScreen, col0, row0, tileW, tileH, "SPEED km/h");
    assistVal = createTile(rideScreen, col1, row0, tileW, tileH, "ASSIST");
    tripVal = createTile(rideScreen, col0, row1, tileW, tileH, "TRIP km");
    batVal = createTile(rideScreen, col1, row1, tileW, tileH, "BATTERY");

    // Thin charge bar along the bottom of the battery tile.
    batBar = createBar(lv_obj_get_parent(batVal), tileW - 8, 6);
    lv_obj_align(batBar, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void createChargeScreen(lv_obj_t *scr) {
    chargeScreen = createPlainContainer(scr);
    lv_obj_set_size(chargeScreen, ST7789_WIDTH, ST7789_HEIGHT);

    lv_obj_t *title = createLabel(chargeScreen, &lv_font_montserrat_32, COLOR_TITLE, "CHARGING");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    chargeVal = createLabel(chargeScreen, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(chargeVal, LV_ALIGN_CENTER, 0, 0);

    chargeBar = createBar(chargeScreen, ST7789_WIDTH - 60, 24);
    lv_obj_align(chargeBar, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_add_flag(chargeScreen, LV_OBJ_FLAG_HIDDEN);
}

// Calibration aid for the touch orientation settings in st7789.c.
static void logTouchCb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if(indev != NULL) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI(TAG, "touch at %ld,%ld", (long)p.x, (long)p.y);
    }
}

static void createStartScreen(lv_obj_t *scr) {
    startScreen = createPlainContainer(scr);
    lv_obj_set_size(startScreen, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_add_event_cb(startScreen, logTouchCb, LV_EVENT_PRESSED, NULL);

    // Left: the board button, full height.
    lv_obj_t *btn = createBoardButton(startScreen, START_MARGIN, START_MARGIN, START_BTN_W, ST7789_HEIGHT - 2 * START_MARGIN);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_add_event_cb(btn, logTouchCb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *icon = createLabel(btn, &lv_font_montserrat_32, COLOR_VALUE, LV_SYMBOL_POWER);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -16);
    lv_obj_t *btnText = createLabel(btn, &lv_font_montserrat_16, COLOR_VALUE, "START");
    lv_obj_align(btnText, LV_ALIGN_CENTER, 0, 22);

    // Right: name and battery state.
    const int32_t rightX = START_MARGIN + START_BTN_W + START_MARGIN;
    lv_obj_t *info = createPlainContainer(startScreen);
    lv_obj_set_pos(info, rightX, START_MARGIN);
    lv_obj_set_size(info, ST7789_WIDTH - rightX - START_MARGIN, ST7789_HEIGHT - 2 * START_MARGIN);

    lv_obj_t *title = createLabel(info, &lv_font_montserrat_32, COLOR_VALUE, "ION");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *batTitle = createLabel(info, &lv_font_montserrat_16, COLOR_TITLE, "BATTERY");
    lv_obj_align(batTitle, LV_ALIGN_TOP_MID, 0, 90);

    startBatVal = createLabel(info, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(startBatVal, LV_ALIGN_TOP_MID, 0, 112);

    startBatBar = createBar(info, ST7789_WIDTH - rightX - START_MARGIN - 16, 12);
    lv_obj_align(startBatBar, LV_ALIGN_TOP_MID, 0, 160);
}

static void showOnly(lv_obj_t *visible) {
    lv_obj_t *const all[] = {rideScreen, chargeScreen, startScreen};
    for(lv_obj_t *obj : all) {
        if(obj == visible) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void updateStartScreen() {
    // getBatPercentage() only reads a byte kept up to date by the main loop,
    // so the start screen stays current even while no display updates come in.
    const uint8_t bat = getBatPercentage();
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", bat);
    setText(startBatVal, buf);
    lv_bar_set_value(startBatBar, bat, LV_ANIM_OFF);
}

static void applyState(const DashState &st) {
    char buf[16];

    if(!st.screen) {
        // Dashboard off (same meaning as the CU3 "screen off"): show the start screen.
        updateStartScreen();
        showOnly(startScreen);
        return;
    }

    snprintf(buf, sizeof(buf), "%u%%", st.bat);

    if(st.type == ST7789_DSP_BAT_CHARGE) {
        setText(chargeVal, buf);
        lv_bar_set_value(chargeBar, st.bat, LV_ANIM_OFF);
        showOnly(chargeScreen);
        return;
    }

    setText(batVal, buf);
    lv_bar_set_value(batBar, st.bat, LV_ANIM_OFF);

    snprintf(buf, sizeof(buf), "%u.%u", st.speed / 10, st.speed % 10);
    setText(speedVal, buf);

    setText(assistVal, assistLevelText(st.assist));

    // trip1 is in 10m increments, so 100 units == 1.00km exactly.
    snprintf(buf, sizeof(buf), "%lu.%02lu", (unsigned long)(st.trip1 / 100), (unsigned long)(st.trip1 % 100));
    setText(tripVal, buf);

    setText(lightBtnText, st.light ? "LIGHT\nON" : "LIGHT\nOFF");
    if(st.light) {
        lv_obj_add_state(lightBtn, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(lightBtn, LV_STATE_CHECKED);
    }

    showOnly(rideScreen);
}

// Runs inside the LVGL task (LVGL lock already held).
static void applyTimerCb(lv_timer_t *timer) {
    DashState st;
    bool dirty;

    taskENTER_CRITICAL(&stateMux);
    dirty = pendingDirty;
    st = pendingState;
    pendingDirty = false;
    taskEXIT_CRITICAL(&stateMux);

    if(dirty) {
        applyState(st);
    } else if(!lv_obj_has_flag(startScreen, LV_OBJ_FLAG_HIDDEN)) {
        updateStartScreen();
    }
}

void initSt7789Display() {
    lv_display_t *disp = st7789_init();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    createRideScreen(scr);
    createChargeScreen(scr);
    createStartScreen(scr);
    // Start screen until the main loop says the dashboard is on.
    updateStartScreen();
    showOnly(startScreen);

    lv_indev_t *touch = st7789_get_touch();
    if(touch != NULL) {
        lv_indev_set_long_press_time(touch, CONFIG_ESP32_BUTTON_LONG_PRESS_DURATION_MS);
    } else {
        ESP_LOGW(TAG, "No touch input, the start button only works via the board button");
    }

    lv_timer_create(applyTimerCb, APPLY_PERIOD_MS, NULL);
    lvgl_port_unlock();
}

void showStartScreenSt7789() {
    taskENTER_CRITICAL(&stateMux);
    pendingState.screen = false;
    pendingDirty = true;
    taskEXIT_CRITICAL(&stateMux);
}

void displayUpdateSt7789(st7789_display_type type, bool screen, bool light, uint8_t assist, uint16_t speed, uint32_t trip1) {
    DashState st;
    st.type = type;
    st.screen = screen;
    st.light = light;
    st.assist = assist;
    st.speed = speed;
    st.trip1 = trip1;
    st.bat = getBatPercentage();

    taskENTER_CRITICAL(&stateMux);
    pendingState = st;
    pendingDirty = true;
    taskEXIT_CRITICAL(&stateMux);
}

#endif
