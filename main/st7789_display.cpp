#include "st7789_display.h"
#if CONFIG_ION_ST7789

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "bat.h"
#include "st7789.h"

// How often the LVGL task checks for a new snapshot from the main loop.
#define APPLY_PERIOD_MS 50

#define STATUS_H 28
#define TILE_GAP 4

#define COLOR_TILE_BG lv_color_hex(0x1C1C1C)
#define COLOR_TITLE lv_color_hex(0x9E9E9E)
#define COLOR_VALUE lv_color_white()
#define COLOR_LIGHT_ON lv_color_hex(0xFFD54F)
#define COLOR_LIGHT_OFF lv_color_hex(0x616161)
#define COLOR_BAR lv_color_hex(0x43A047)

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
static lv_obj_t *lightLabel;

static lv_obj_t *chargeScreen;
static lv_obj_t *chargeVal;
static lv_obj_t *chargeBar;

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
    lv_obj_set_style_pad_all(tile, 6, 0);

    lv_obj_t *titleLabel = createLabel(tile, &lv_font_montserrat_16, COLOR_TITLE, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *valueLabel = createLabel(tile, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(valueLabel, LV_ALIGN_CENTER, 0, 8);
    return valueLabel;
}

static void createRideScreen(lv_obj_t *scr) {
    rideScreen = createPlainContainer(scr);
    lv_obj_set_size(rideScreen, ST7789_WIDTH, ST7789_HEIGHT);

    const int32_t tileW = (ST7789_WIDTH - 3 * TILE_GAP) / 2;
    const int32_t tileH = (ST7789_HEIGHT - STATUS_H - 3 * TILE_GAP) / 2;
    const int32_t col0 = TILE_GAP;
    const int32_t col1 = 2 * TILE_GAP + tileW;
    const int32_t row0 = TILE_GAP;
    const int32_t row1 = 2 * TILE_GAP + tileH;

    speedVal = createTile(rideScreen, col0, row0, tileW, tileH, "SPEED km/h");
    assistVal = createTile(rideScreen, col1, row0, tileW, tileH, "ASSIST");
    tripVal = createTile(rideScreen, col0, row1, tileW, tileH, "TRIP km");
    batVal = createTile(rideScreen, col1, row1, tileW, tileH, "BATTERY");

    // Thin charge bar along the bottom of the battery tile.
    batBar = createBar(lv_obj_get_parent(batVal), tileW - 12, 6);
    lv_obj_align(batBar, LV_ALIGN_BOTTOM_MID, 0, 0);

    lightLabel = createLabel(rideScreen, &lv_font_montserrat_16, COLOR_LIGHT_OFF, "LIGHT OFF");
    lv_obj_align(lightLabel, LV_ALIGN_BOTTOM_MID, 0, -(STATUS_H - 16) / 2);
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

static void showOnly(lv_obj_t *visible) {
    if(visible == rideScreen) {
        lv_obj_remove_flag(rideScreen, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(rideScreen, LV_OBJ_FLAG_HIDDEN);
    }
    if(visible == chargeScreen) {
        lv_obj_remove_flag(chargeScreen, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(chargeScreen, LV_OBJ_FLAG_HIDDEN);
    }
}

static void applyState(const DashState &st) {
    char buf[16];

    if(!st.screen) {
        // Blank dashboard, same meaning as the CU3 "screen off".
        showOnly(NULL);
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

    setText(lightLabel, st.light ? "LIGHT ON" : "LIGHT OFF");
    lv_obj_set_style_text_color(lightLabel, st.light ? COLOR_LIGHT_ON : COLOR_LIGHT_OFF, 0);

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
    }
}

void initSt7789Display() {
    lv_display_t *disp = st7789_init();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    createRideScreen(scr);
    createChargeScreen(scr);
    // Start blank until the main loop says the screen is on.
    showOnly(NULL);
    lv_timer_create(applyTimerCb, APPLY_PERIOD_MS, NULL);
    lvgl_port_unlock();
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
