#include "st7789_display.h"
#if CONFIG_ION_ST7789

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "aux_adc.h"
#include "bat.h"
#include "board_button.h"
#include "motor.h"
#include "st7789.h"
#include "trip.h"

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

// How long the "Calibrated" notice stays on screen.
#define CALIBRATED_NOTICE_MS 4000

// Full scale of the speed gauge, in km/h * 10.
#define SPEED_GAUGE_MAX 400

#define GAP 4
#define DASH_BTN_W 72
// Area right of the button column, where the selected dashboard view is drawn.
#define VIEW_X (2 * GAP + DASH_BTN_W)
#define VIEW_W (ST7789_WIDTH - VIEW_X - GAP)
#define VIEW_H (ST7789_HEIGHT - 2 * GAP)

#define COLOR_TILE_BG lv_color_hex(0x1C1C1C)
#define COLOR_TITLE lv_color_hex(0x9E9E9E)
#define COLOR_VALUE lv_color_white()
#define COLOR_LIGHT_ON lv_color_hex(0xFFC107)
#define COLOR_LIGHT_OFF lv_color_hex(0x424242)
#define COLOR_LIGHT_TEXT_ON lv_color_black()
#define COLOR_VIEW_BTN lv_color_hex(0x37474F)
#define COLOR_VIEW_BTN_PRESSED lv_color_hex(0x607D8B)
#define COLOR_BAR lv_color_hex(0x43A047)
#define COLOR_GAUGE_BG lv_color_hex(0x333333)
#define COLOR_GAUGE lv_color_hex(0x29B6F6)
#define COLOR_BOARD_BTN lv_color_hex(0x2E7D32)
#define COLOR_BOARD_BTN_PRESSED lv_color_hex(0x66BB6A)
#define COLOR_NOTICE lv_color_hex(0x1565C0)

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
    uint32_t trip2;
    uint32_t total;
    uint32_t batMv;
    uint16_t motorDeciVolts;
    int16_t motorDeciDegrees;
    uint32_t motorTotal;
    uint8_t bat;
};

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static DashState pendingState;
static bool pendingDirty = false;
static bool pendingCalibrated = false;

// The dashboard views the view button cycles through. VIEW_AUX (the ADS1115
// voltages) only joins the cycle once an ADS1115 has been found, so it must stay last.
enum dash_view {
    VIEW_TILES = 0,
    VIEW_GAUGE,
    VIEW_TRIPS,
    VIEW_MOTOR,
#if CONFIG_ION_ST7789_ADS1115
    VIEW_AUX,
#endif
    VIEW_COUNT
};
static int currentView = VIEW_TILES;
static int shownViewCount = 0;

// LVGL objects, only touched from the LVGL task (or with the LVGL lock held).
static lv_obj_t *rideScreen;
static lv_obj_t *views[VIEW_COUNT];
static lv_obj_t *lightBtn;
static lv_obj_t *lightBtnText;
static lv_obj_t *viewBtnText;

// View 1: 2x2 tiles
static lv_obj_t *tileSpeedVal;
static lv_obj_t *tileAssistVal;
static lv_obj_t *tileTripVal;
static lv_obj_t *tileBatVal;
static lv_obj_t *tileBatBar;

// View 2: speed gauge
static lv_obj_t *gaugeArc;
static lv_obj_t *gaugeSpeedVal;
static lv_obj_t *gaugeAssistVal;
static lv_obj_t *gaugeBatVal;

// View 3: distances and battery voltage
static lv_obj_t *tripsTrip1Val;
static lv_obj_t *tripsTrip2Val;
static lv_obj_t *tripsTotalVal;
static lv_obj_t *tripsVoltVal;

// View 4: values read from the motor
static lv_obj_t *motorVoltVal;
static lv_obj_t *motorTempVal;
static lv_obj_t *motorTotalVal;

#if CONFIG_ION_ST7789_ADS1115
// View 4: ADS1115 A0..A3
static lv_obj_t *auxVals[AUX_ADC_CHANNELS];
#endif

static lv_obj_t *chargeScreen;
static lv_obj_t *chargeVal;
static lv_obj_t *chargeBar;

// Shown whenever the dashboard is "off" (at boot, and after the motor turned off).
static lv_obj_t *startScreen;
static lv_obj_t *startBatVal;
static lv_obj_t *startBatBar;

// "Calibrated" notice, on the top layer so it shows over whatever screen is up.
static lv_obj_t *calibratedNotice;
static uint32_t calibratedShownAt;

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

// Distances are in 10m increments, so 100 units == 1.00km exactly.
static void formatKm2(char *buf, size_t size, uint32_t dist) {
    snprintf(buf, size, "%lu.%02lu", (unsigned long)(dist / 100), (unsigned long)(dist % 100));
}

static void formatKm1(char *buf, size_t size, uint32_t dist) {
    snprintf(buf, size, "%lu.%lu", (unsigned long)(dist / 100), (unsigned long)((dist % 100) / 10));
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

// A dark rounded panel.
static lv_obj_t *createPanel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *panel = createPlainContainer(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COLOR_TILE_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    return panel;
}

// A panel with a small title at the top and a big value centered below it.
static lv_obj_t *createTile(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const char *title) {
    lv_obj_t *tile = createPanel(parent, x, y, w, h);

    lv_obj_t *titleLabel = createLabel(tile, &lv_font_montserrat_16, COLOR_TITLE, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *valueLabel = createLabel(tile, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(valueLabel, LV_ALIGN_CENTER, 0, 8);
    return valueLabel;
}

// A full-width panel row: name on the left, big value right-aligned.
static lv_obj_t *createRow(lv_obj_t *parent, int32_t y, int32_t h, const char *name) {
    lv_obj_t *row = createPanel(parent, 0, y, VIEW_W, h);
    lv_obj_set_style_pad_hor(row, 10, 0);

    lv_obj_t *nameLabel = createLabel(row, &lv_font_montserrat_16, COLOR_TITLE, name);
    lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *valueLabel = createLabel(row, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(valueLabel, LV_ALIGN_RIGHT_MID, 0, 0);
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

// Number of views currently in the VIEW button's cycle.
static int availableViewCount() {
#if CONFIG_ION_ST7789_ADS1115
    if(!auxAdcFound()) {
        return VIEW_COUNT - 1;
    }
#endif
    return VIEW_COUNT;
}

static void showView(int view) {
    currentView = view;
    shownViewCount = availableViewCount();
    for(int i = 0; i < VIEW_COUNT; i++) {
        if(i == view) {
            lv_obj_remove_flag(views[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(views[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "VIEW\n%d/%d", view + 1, shownViewCount);
    setText(viewBtnText, buf);
}

// Purely local to the display: no need to involve the main loop.
static void viewButtonCb(lv_event_t *e) {
    showView((currentView + 1) % availableViewCount());
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

// A centered, multi-line capable label that takes its text color from the
// button, so it can follow the button's state styles.
static lv_obj_t *createButtonText(lv_obj_t *btn, const lv_font_t *font, const char *text) {
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return label;
}

// An on-screen copy of the board button (start screen "START", dashboard "+").
static lv_obj_t *createBoardButton(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *btn = createButton(parent, x, y, w, h);
    lv_obj_set_style_bg_color(btn, COLOR_BOARD_BTN, 0);
    lv_obj_set_style_bg_color(btn, COLOR_BOARD_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, COLOR_VALUE, 0);
    lv_obj_add_event_cb(btn, boardButtonCb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(btn, boardButtonCb, LV_EVENT_RELEASED, NULL);
    return btn;
}

static void createButtonColumn(lv_obj_t *parent) {
    const int32_t btnH = (ST7789_HEIGHT - 4 * GAP) / 3;

    // "+": the board button (assist level up).
    lv_obj_t *plusBtn = createBoardButton(parent, GAP, GAP, DASH_BTN_W, btnH);
    createButtonText(plusBtn, &lv_font_montserrat_32, "+");

    // Light switch. Its color and text show the light state (CHECKED = on).
    lightBtn = createButton(parent, GAP, 2 * GAP + btnH, DASH_BTN_W, btnH);
    lv_obj_set_style_bg_color(lightBtn, COLOR_LIGHT_OFF, 0);
    lv_obj_set_style_bg_color(lightBtn, COLOR_LIGHT_ON, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(lightBtn, COLOR_VALUE, 0);
    lv_obj_set_style_text_color(lightBtn, COLOR_LIGHT_TEXT_ON, LV_STATE_CHECKED);
    lv_obj_add_event_cb(lightBtn, lightButtonCb, LV_EVENT_CLICKED, NULL);
    lightBtnText = createButtonText(lightBtn, &lv_font_montserrat_16, "LIGHT\nOFF");

    // Cycles through the dashboard views.
    lv_obj_t *viewBtn = createButton(parent, GAP, 3 * GAP + 2 * btnH, DASH_BTN_W, btnH);
    lv_obj_set_style_bg_color(viewBtn, COLOR_VIEW_BTN, 0);
    lv_obj_set_style_bg_color(viewBtn, COLOR_VIEW_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(viewBtn, COLOR_VALUE, 0);
    lv_obj_add_event_cb(viewBtn, viewButtonCb, LV_EVENT_CLICKED, NULL);
    viewBtnText = createButtonText(viewBtn, &lv_font_montserrat_16, "VIEW");
}

static lv_obj_t *createViewContainer(lv_obj_t *parent) {
    lv_obj_t *view = createPlainContainer(parent);
    lv_obj_set_pos(view, VIEW_X, GAP);
    lv_obj_set_size(view, VIEW_W, VIEW_H);
    return view;
}

// View 1: speed, assist, trip and battery as a 2x2 grid of tiles.
static void createTilesView(lv_obj_t *view) {
    const int32_t tileW = (VIEW_W - GAP) / 2;
    const int32_t tileH = (VIEW_H - GAP) / 2;
    const int32_t col1 = tileW + GAP;
    const int32_t row1 = tileH + GAP;

    tileSpeedVal = createTile(view, 0, 0, tileW, tileH, "SPEED km/h");
    tileAssistVal = createTile(view, col1, 0, tileW, tileH, "ASSIST");
    tileTripVal = createTile(view, 0, row1, tileW, tileH, "TRIP km");
    tileBatVal = createTile(view, col1, row1, tileW, tileH, "BATTERY");

    // Thin charge bar along the bottom of the battery tile.
    tileBatBar = createBar(lv_obj_get_parent(tileBatVal), tileW - 8, 6);
    lv_obj_align(tileBatBar, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// View 2: a large speed gauge, with assist level and battery below it.
static void createGaugeView(lv_obj_t *view) {
    const int32_t arcSize = 172;

    gaugeArc = lv_arc_create(view);
    lv_obj_set_size(gaugeArc, arcSize, arcSize);
    lv_obj_align(gaugeArc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(gaugeArc, 135);
    lv_arc_set_bg_angles(gaugeArc, 0, 270);
    lv_arc_set_range(gaugeArc, 0, SPEED_GAUGE_MAX);
    lv_arc_set_value(gaugeArc, 0);
    // Display only: no knob, not draggable.
    lv_obj_remove_style(gaugeArc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(gaugeArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(gaugeArc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(gaugeArc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gaugeArc, COLOR_GAUGE_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(gaugeArc, COLOR_GAUGE, LV_PART_INDICATOR);

    gaugeSpeedVal = createLabel(gaugeArc, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(gaugeSpeedVal, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *unit = createLabel(gaugeArc, &lv_font_montserrat_16, COLOR_TITLE, "km/h");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 24);

    lv_obj_t *assistTitle = createLabel(view, &lv_font_montserrat_16, COLOR_TITLE, "ASSIST");
    lv_obj_align(assistTitle, LV_ALIGN_BOTTOM_LEFT, 4, -38);
    gaugeAssistVal = createLabel(view, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(gaugeAssistVal, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    lv_obj_t *batTitle = createLabel(view, &lv_font_montserrat_16, COLOR_TITLE, "BATTERY");
    lv_obj_align(batTitle, LV_ALIGN_BOTTOM_RIGHT, -4, -38);
    gaugeBatVal = createLabel(view, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(gaugeBatVal, LV_ALIGN_BOTTOM_RIGHT, -4, 0);
}

// View 3: trip 1, trip 2, total distance and battery voltage as rows.
static void createTripsView(lv_obj_t *view) {
    const int32_t rowH = (VIEW_H - 3 * GAP) / 4;

    tripsTrip1Val = createRow(view, 0, rowH, "TRIP 1 km");
    tripsTrip2Val = createRow(view, rowH + GAP, rowH, "TRIP 2 km");
    tripsTotalVal = createRow(view, 2 * (rowH + GAP), rowH, "TOTAL km");
    tripsVoltVal = createRow(view, 3 * (rowH + GAP), rowH, "BATTERY");
}

// View 4: motor voltage, temperature and total distance as rows, same row height as view 3.
static void createMotorView(lv_obj_t *view) {
    const int32_t rowH = (VIEW_H - 3 * GAP) / 4;

    motorVoltVal = createRow(view, 0, rowH, "MOTOR V");
    motorTempVal = createRow(view, rowH + GAP, rowH, "MOTOR TEMP");
    motorTotalVal = createRow(view, 2 * (rowH + GAP), rowH, "MOTOR km");
}

// Formats a value in tenths, with sign, e.g. -5 -> "-0.5".
static void formatTenths(char *buf, size_t size, int32_t value, const char *unit) {
    const char *sign = value < 0 ? "-" : "";
    const uint32_t mag = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
    snprintf(buf, size, "%s%lu.%lu %s", sign, (unsigned long)(mag / 10), (unsigned long)(mag % 10), unit);
}

#if CONFIG_ION_ST7789_ADS1115
// View 4: the four ADS1115 input voltages as rows.
static void createAuxView(lv_obj_t *view) {
    static const char *const names[AUX_ADC_CHANNELS] = {"A0", "A1", "A2", "A3"};
    const int32_t rowH = (VIEW_H - (AUX_ADC_CHANNELS - 1) * GAP) / AUX_ADC_CHANNELS;

    for(int i = 0; i < AUX_ADC_CHANNELS; i++) {
        auxVals[i] = createRow(view, i * (rowH + GAP), rowH, names[i]);
    }
}

static void updateAuxView() {
    float volts[AUX_ADC_CHANNELS];
    const bool ok = auxAdcGetVolts(volts);
    char buf[16];
    for(int i = 0; i < AUX_ADC_CHANNELS; i++) {
        if(ok) {
            snprintf(buf, sizeof(buf), "%.3f V", volts[i]);
            setText(auxVals[i], buf);
        } else {
            setText(auxVals[i], "-");
        }
    }
}
#endif

static void createRideScreen(lv_obj_t *scr) {
    rideScreen = createPlainContainer(scr);
    lv_obj_set_size(rideScreen, ST7789_WIDTH, ST7789_HEIGHT);

    createButtonColumn(rideScreen);

    for(int i = 0; i < VIEW_COUNT; i++) {
        views[i] = createViewContainer(rideScreen);
    }
    createTilesView(views[VIEW_TILES]);
    createGaugeView(views[VIEW_GAUGE]);
    createTripsView(views[VIEW_TRIPS]);
    createMotorView(views[VIEW_MOTOR]);
#if CONFIG_ION_ST7789_ADS1115
    createAuxView(views[VIEW_AUX]);
#endif

    showView(VIEW_TILES);
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

// Bottom banner over the dashboard area. Not clickable, so touches go through
// to the buttons/screen below it.
static void createCalibratedNotice() {
    calibratedNotice = createPlainContainer(lv_layer_top());
    lv_obj_remove_flag(calibratedNotice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(calibratedNotice, VIEW_W, 52);
    lv_obj_align(calibratedNotice, LV_ALIGN_BOTTOM_RIGHT, -GAP, -GAP);
    lv_obj_set_style_bg_color(calibratedNotice, COLOR_NOTICE, 0);
    lv_obj_set_style_bg_opa(calibratedNotice, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(calibratedNotice, 12, 0);

    lv_obj_t *text = createLabel(calibratedNotice, &lv_font_montserrat_32, COLOR_VALUE, "Calibrated");
    lv_obj_center(text);

    lv_obj_add_flag(calibratedNotice, LV_OBJ_FLAG_HIDDEN);
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

// Updates all dashboard views (hidden ones too, so switching views shows current values).
static void updateRideScreen(const DashState &st) {
    char buf[16];

    snprintf(buf, sizeof(buf), "%u%%", st.bat);
    setText(tileBatVal, buf);
    setText(gaugeBatVal, buf);
    lv_bar_set_value(tileBatBar, st.bat, LV_ANIM_OFF);

    snprintf(buf, sizeof(buf), "%u.%u", st.speed / 10, st.speed % 10);
    setText(tileSpeedVal, buf);
    setText(gaugeSpeedVal, buf);
    lv_arc_set_value(gaugeArc, st.speed > SPEED_GAUGE_MAX ? SPEED_GAUGE_MAX : st.speed);

    setText(tileAssistVal, assistLevelText(st.assist));
    setText(gaugeAssistVal, assistLevelText(st.assist));

    formatKm2(buf, sizeof(buf), st.trip1);
    setText(tileTripVal, buf);
    setText(tripsTrip1Val, buf);
    formatKm2(buf, sizeof(buf), st.trip2);
    setText(tripsTrip2Val, buf);
    formatKm1(buf, sizeof(buf), st.total);
    setText(tripsTotalVal, buf);

#if CONFIG_ION_ADC
    snprintf(buf, sizeof(buf), "%lu.%lu V", (unsigned long)(st.batMv / 1000), (unsigned long)((st.batMv % 1000) / 100));
    setText(tripsVoltVal, buf);
#else
    // No voltage measurement configured.
    setText(tripsVoltVal, "-");
#endif

    if(st.motorDeciVolts == MOTOR_VOLTAGE_UNKNOWN) {
        setText(motorVoltVal, "-");
    } else {
        formatTenths(buf, sizeof(buf), st.motorDeciVolts, "V");
        setText(motorVoltVal, buf);
    }

    if(st.motorDeciDegrees == MOTOR_TEMP_UNKNOWN) {
        setText(motorTempVal, "-");
    } else {
        // The built-in Montserrat fonts have no degree sign.
        formatTenths(buf, sizeof(buf), st.motorDeciDegrees, "C");
        setText(motorTempVal, buf);
    }

    if(st.motorTotal == MOTOR_TOTAL_UNKNOWN) {
        setText(motorTotalVal, "-");
    } else {
        // One decimal like TOTAL km in view 3, "11591.08" would not fit next to the name.
        formatKm1(buf, sizeof(buf), st.motorTotal);
        setText(motorTotalVal, buf);
    }

    setText(lightBtnText, st.light ? "LIGHT\nON" : "LIGHT\nOFF");
    if(st.light) {
        lv_obj_add_state(lightBtn, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(lightBtn, LV_STATE_CHECKED);
    }
}

static void applyState(const DashState &st) {
    if(!st.screen) {
        // Dashboard off (same meaning as the CU3 "screen off"): show the start screen.
        updateStartScreen();
        showOnly(startScreen);
        return;
    }

    if(st.type == ST7789_DSP_BAT_CHARGE) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", st.bat);
        setText(chargeVal, buf);
        lv_bar_set_value(chargeBar, st.bat, LV_ANIM_OFF);
        showOnly(chargeScreen);
        return;
    }

    updateRideScreen(st);
    showOnly(rideScreen);
}

// Runs inside the LVGL task (LVGL lock already held).
static void applyTimerCb(lv_timer_t *timer) {
    DashState st;
    bool dirty;
    bool calibrated;

    taskENTER_CRITICAL(&stateMux);
    dirty = pendingDirty;
    st = pendingState;
    pendingDirty = false;
    calibrated = pendingCalibrated;
    pendingCalibrated = false;
    taskEXIT_CRITICAL(&stateMux);

    if(dirty) {
        applyState(st);
    } else if(!lv_obj_has_flag(startScreen, LV_OBJ_FLAG_HIDDEN)) {
        updateStartScreen();
    }

#if CONFIG_ION_ST7789_ADS1115
    // The ADS1115 is detected in the background; refresh the "VIEW n/m" label once it shows up.
    if(availableViewCount() != shownViewCount) {
        showView(currentView);
    }
    // The ADS1115 values come from their own task, not from the main loop's snapshots.
    if(currentView == VIEW_AUX) {
        updateAuxView();
    }
#endif

    if(calibrated) {
        // (Re)start the notice's 4 seconds.
        calibratedShownAt = lv_tick_get();
        lv_obj_remove_flag(calibratedNotice, LV_OBJ_FLAG_HIDDEN);
    } else if(!lv_obj_has_flag(calibratedNotice, LV_OBJ_FLAG_HIDDEN) &&
              lv_tick_elaps(calibratedShownAt) >= CALIBRATED_NOTICE_MS) {
        lv_obj_add_flag(calibratedNotice, LV_OBJ_FLAG_HIDDEN);
    }
}

void initSt7789Display() {
    lv_display_t *disp = st7789_init();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    createRideScreen(scr);
    createChargeScreen(scr);
    createStartScreen(scr);
    createCalibratedNotice();
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

void showCalibratedSt7789() {
    taskENTER_CRITICAL(&stateMux);
    pendingCalibrated = true;
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
    st.trip2 = getTrip2();
    st.total = getTotal();
    st.batMv = getBatMv();
    st.motorDeciVolts = getMotorDeciVolts();
    st.motorDeciDegrees = getMotorDeciDegrees();
    st.motorTotal = getMotorTotal();
    st.bat = getBatPercentage();

    taskENTER_CRITICAL(&stateMux);
    pendingState = st;
    pendingDirty = true;
    taskEXIT_CRITICAL(&stateMux);
}

#endif
