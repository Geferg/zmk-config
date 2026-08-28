/*
 * Space OLED HUD - first-pass custom status screen for a vertically mounted
 * 128x32 SSD1306 used with mctechnology17/zmk-nice-oled's `nice_oled` shield.
 *
 * Logical drawing coordinates are 32x128.  Pixel writes are mapped directly
 * into the physical 128x32 framebuffer.  This preserves the vertical layout
 * without doing an expensive full-frame LVGL rotation at runtime.
 *
 * Left / central:
 *   - TYPING / GAMING mode derived from active layer (0..3 / 4..7)
 *   - Saturn-style planet with a small orbiting moon
 *   - four-position sub-layer indicator
 *   - USB / Bluetooth profile + rolling four-key history
 *   - TRAVELED session key count
 *   - POWER battery gauge
 *
 * Right / peripheral:
 *   - VELOCITY readout
 *   - spacecraft with animated exhaust
 *   - downward-moving star field
 *   - local battery + segmented battery bar
 *
 * NOTE: Right-side VELOCITY is deliberately an activity-derived value for
 * this first pass.  A normal ZMK peripheral does not have the central's true
 * WPM state.  It reacts to key presses physically generated on the right
 * half.  A later pass can relay true global WPM over the split link.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define SPACE_IS_CENTRAL 1
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/modifiers.h>
#else
#define SPACE_IS_CENTRAL 0
#include <zmk/events/position_state_changed.h>
#endif

#define LOGICAL_W 32
#define LOGICAL_H 128
#define PHYSICAL_W 128
#define PHYSICAL_H 32

#define BG_COLOR                                                                                   \
    (IS_ENABLED(CONFIG_NICE_OLED_WIDGET_INVERTED) ? lv_color_black() : lv_color_white())
#define FG_COLOR                                                                                   \
    (IS_ENABLED(CONFIG_NICE_OLED_WIDGET_INVERTED) ? lv_color_white() : lv_color_black())

struct hud_state {
    uint8_t battery;
#if SPACE_IS_CENTRAL
    uint8_t layer;
    uint8_t transport;
    uint8_t profile;
#endif
};

static struct hud_state hud;

static lv_obj_t *hud_canvas;
/* One physical OLED frame only: 128x32 instead of two 128x128 work buffers. */
static lv_color_t canvas_buf[PHYSICAL_W * PHYSICAL_H];
static bool hud_ready;
static uint32_t anim_tick;

/* -------------------------------------------------------------------------- */
/* Tiny framebuffer primitives                                                 */
/* -------------------------------------------------------------------------- */

static inline bool in_bounds(int x, int y) {
    return x >= 0 && x < LOGICAL_W && y >= 0 && y < LOGICAL_H;
}

static inline void set_px_color(int x, int y, lv_color_t color) {
    if (in_bounds(x, y)) {
        /*
         * Equivalent to the old 90-degree LVGL transform:
         * logical (x, y) -> physical (127 - y, x).
         * Doing this per plotted pixel is dramatically cheaper than rotating
         * a 128x128 canvas every animation frame.
         */
        int physical_x = (LOGICAL_H - 1) - y;
        int physical_y = x;
        canvas_buf[physical_y * PHYSICAL_W + physical_x] = color;
    }
}

static inline void px(int x, int y) { set_px_color(x, y, FG_COLOR); }
static inline void erase_px(int x, int y) { set_px_color(x, y, BG_COLOR); }

static void clear_frame(void) {
    lv_color_t bg = BG_COLOR;
    for (size_t i = 0; i < ARRAY_SIZE(canvas_buf); i++) {
        canvas_buf[i] = bg;
    }
}

static void line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fill_rect(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            px(xx, yy);
        }
    }
}

static void rect_outline(int x, int y, int w, int h) {
    line(x, y, x + w - 1, y);
    line(x, y + h - 1, x + w - 1, y + h - 1);
    line(x, y, x, y + h - 1);
    line(x + w - 1, y, x + w - 1, y + h - 1);
}

static void circle_outline(int cx, int cy, int r) {
    int x = r;
    int y = 0;
    int err = 0;

    while (x >= y) {
        px(cx + x, cy + y);
        px(cx + y, cy + x);
        px(cx - y, cy + x);
        px(cx - x, cy + y);
        px(cx - x, cy - y);
        px(cx - y, cy - x);
        px(cx + y, cy - x);
        px(cx + x, cy - y);

        y++;
        if (err <= 0) {
            err += 2 * y + 1;
        }
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

static void clear_circle(int cx, int cy, int r) {
    int rr = r * r;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= rr) {
                erase_px(cx + x, cy + y);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 3x5 cockpit font                                                            */
/* -------------------------------------------------------------------------- */

#define GLYPH(r0, r1, r2, r3, r4)                                                               \
    ((uint16_t)((r0) << 12) | (uint16_t)((r1) << 9) | (uint16_t)((r2) << 6) |                   \
     (uint16_t)((r3) << 3) | (uint16_t)(r4))

static uint16_t glyph3x5(char c) {
    switch (c) {
    case 'A': return GLYPH(2, 5, 7, 5, 5);
    case 'B': return GLYPH(6, 5, 6, 5, 6);
    case 'C': return GLYPH(3, 4, 4, 4, 3);
    case 'D': return GLYPH(6, 5, 5, 5, 6);
    case 'E': return GLYPH(7, 4, 6, 4, 7);
    case 'F': return GLYPH(7, 4, 6, 4, 4);
    case 'G': return GLYPH(3, 4, 5, 5, 3);
    case 'H': return GLYPH(5, 5, 7, 5, 5);
    case 'I': return GLYPH(7, 2, 2, 2, 7);
    case 'J': return GLYPH(1, 1, 1, 5, 2);
    case 'K': return GLYPH(5, 5, 6, 5, 5);
    case 'L': return GLYPH(4, 4, 4, 4, 7);
    case 'M': return GLYPH(5, 7, 7, 5, 5);
    case 'N': return GLYPH(5, 7, 7, 7, 5);
    case 'O': return GLYPH(2, 5, 5, 5, 2);
    case 'P': return GLYPH(6, 5, 6, 4, 4);
    case 'Q': return GLYPH(2, 5, 5, 7, 3);
    case 'R': return GLYPH(6, 5, 6, 5, 5);
    case 'S': return GLYPH(3, 4, 2, 1, 6);
    case 'T': return GLYPH(7, 2, 2, 2, 2);
    case 'U': return GLYPH(5, 5, 5, 5, 7);
    case 'V': return GLYPH(5, 5, 5, 5, 2);
    case 'W': return GLYPH(5, 5, 7, 7, 5);
    case 'X': return GLYPH(5, 5, 2, 5, 5);
    case 'Y': return GLYPH(5, 5, 2, 2, 2);
    case 'Z': return GLYPH(7, 1, 2, 4, 7);

    case '0': return GLYPH(7, 5, 5, 5, 7);
    case '1': return GLYPH(2, 6, 2, 2, 7);
    case '2': return GLYPH(6, 1, 2, 4, 7);
    case '3': return GLYPH(6, 1, 2, 1, 6);
    case '4': return GLYPH(5, 5, 7, 1, 1);
    case '5': return GLYPH(7, 4, 6, 1, 6);
    case '6': return GLYPH(3, 4, 6, 5, 2);
    case '7': return GLYPH(7, 1, 2, 2, 2);
    case '8': return GLYPH(2, 5, 2, 5, 2);
    case '9': return GLYPH(2, 5, 3, 1, 6);

    case '!': return GLYPH(2, 2, 2, 0, 2);
    case '@': return GLYPH(7, 5, 7, 4, 3);
    case '#': return GLYPH(5, 7, 5, 7, 5);
    case '$': return GLYPH(3, 6, 2, 3, 6);
    case '%': return GLYPH(5, 1, 2, 4, 5);
    case '^': return GLYPH(2, 5, 0, 0, 0);
    case '&': return GLYPH(2, 5, 2, 5, 3);
    case '*': return GLYPH(5, 2, 7, 2, 5);
    case '(': return GLYPH(2, 4, 4, 4, 2);
    case ')': return GLYPH(2, 1, 1, 1, 2);
    case '-': return GLYPH(0, 0, 7, 0, 0);
    case '_': return GLYPH(0, 0, 0, 0, 7);
    case '+': return GLYPH(0, 2, 7, 2, 0);
    case '=': return GLYPH(0, 7, 0, 7, 0);
    case '[': return GLYPH(6, 4, 4, 4, 6);
    case ']': return GLYPH(3, 1, 1, 1, 3);
    case '{': return GLYPH(3, 2, 6, 2, 3);
    case '}': return GLYPH(6, 2, 3, 2, 6);
    case '\\': return GLYPH(4, 4, 2, 1, 1);
    case '|': return GLYPH(2, 2, 2, 2, 2);
    case ';': return GLYPH(0, 2, 0, 2, 4);
    case ':': return GLYPH(0, 2, 0, 2, 0);
    case '\'': return GLYPH(2, 2, 0, 0, 0);
    case '"': return GLYPH(5, 5, 0, 0, 0);
    case '`': return GLYPH(4, 2, 0, 0, 0);
    case '~': return GLYPH(0, 5, 2, 0, 0);
    case ',': return GLYPH(0, 0, 0, 2, 4);
    case '.': return GLYPH(0, 0, 0, 0, 2);
    case '<': return GLYPH(1, 2, 4, 2, 1);
    case '>': return GLYPH(4, 2, 1, 2, 4);
    case '/': return GLYPH(1, 1, 2, 4, 4);
    case '?': return GLYPH(6, 1, 2, 0, 2);
    case ' ': return 0;
    default:  return GLYPH(7, 1, 2, 0, 2);
    }
}

static int text_width_3x5(const char *text) {
    size_t len = strlen(text);
    return len == 0 ? 0 : (int)(len * 4 - 1);
}

static void draw_char_3x5(int x, int y, char c) {
    uint16_t bits = glyph3x5(c);
    for (int row = 0; row < 5; row++) {
        uint8_t row_bits = (bits >> (12 - row * 3)) & 0x7;
        for (int col = 0; col < 3; col++) {
            if (row_bits & (1 << (2 - col))) {
                px(x + col, y + row);
            }
        }
    }
}

static void draw_text_3x5(int x, int y, const char *text) {
    while (*text) {
        draw_char_3x5(x, y, *text++);
        x += 4;
    }
}

static void draw_text_centered(int y, const char *text) {
    int width = text_width_3x5(text);
    draw_text_3x5(MAX(0, (LOGICAL_W - width) / 2), y, text);
}

/* -------------------------------------------------------------------------- */
/* Common HUD pieces                                                           */
/* -------------------------------------------------------------------------- */

static void draw_power_gauge(uint8_t battery) {
    /* Shared compact battery gauge used identically on both halves. */
    rect_outline(1, 116, 30, 6);
    int fill = ((int)CLAMP(battery, 0, 100) * 28 + 50) / 100;
    if (fill > 0) {
        fill_rect(2, 117, fill, 4);
    }
    line(1, 127, 30, 127);
}

static void draw_star(int x, int y, int kind) {
    px(x, y);
    if (kind >= 1) {
        px(x - 1, y);
        px(x + 1, y);
        px(x, y - 1);
        px(x, y + 1);
    }
    if (kind >= 2) {
        px(x - 1, y - 1);
        px(x + 1, y - 1);
        px(x - 1, y + 1);
        px(x + 1, y + 1);
    }
}

static inline void present_frame_to_oled(void) {
    /* The framebuffer is already in native 128x32 orientation. */
    lv_obj_invalidate(hud_canvas);
}

/* -------------------------------------------------------------------------- */
/* Left / central: "Mission Control"                                           */
/* -------------------------------------------------------------------------- */

#if SPACE_IS_CENTRAL

#define RECENT_KEY_COUNT 4
#define LEFT_REFRESH_MS 100
#define LEFT_ANIMATION_TICKS 5

static struct k_spinlock key_history_lock;
static char recent_keys[RECENT_KEY_COUNT + 1] = "    ";
static uint32_t traveled;
static atomic_t left_dirty = ATOMIC_INIT(0);

static void draw_saturn(void) {
    const int cx = 16;
    const int cy = 35;
    const int r = 11;

    /* Sparse background stars around the larger visual centerpiece. */
    draw_star(4, 17, (anim_tick / 15) & 1);
    draw_star(28, 22, 0);
    draw_star(4, 52, 0);
    draw_star(28, 53, ((anim_tick / 10) & 1));

    /*
     * One coherent tilted ellipse forms the ring. Draw it first, then let the
     * planet body occlude the center so only the outside portions remain.
     */
    static const int8_t ring[][2] = {
        {31, 30}, {31, 32}, {30, 33}, {28, 35},
        {25, 37}, {22, 39}, {18, 40}, {14, 41},
        {10, 42}, {6, 42},  {4, 42},  {2, 41},
        {1, 40},  {1, 38},  {2, 37},  {4, 35},
        {7, 33},  {10, 31}, {14, 30}, {18, 29},
        {22, 28}, {26, 28}, {28, 28}, {30, 29},
    };
    for (size_t i = 0; i < ARRAY_SIZE(ring); i++) {
        size_t next = (i + 1) % ARRAY_SIZE(ring);
        line(ring[i][0], ring[i][1], ring[next][0], ring[next][1]);
    }

    clear_circle(cx, cy, r);
    circle_outline(cx, cy, r);

    /* Sparse gas-band texture: enough detail to read on the physical OLED. */
    for (int y = cy - r + 2; y <= cy + r - 2; y++) {
        for (int x = cx - r + 2; x <= cx + r - 2; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy < (r - 1) * (r - 1)) {
                if (((x + y * 2) % 5) == 0) {
                    px(x, y);
                }
            }
        }
    }

    /*
     * Leave the ring fully occluded by the planet body.  The ring is drawn
     * before clear_circle(), so only the portions outside the silhouette
     * remain visible.  This avoids a near-side chord reading as a straight
     * line across the face at this resolution.
     */

    /* Slow orbit; animation is intentionally sparse on the central half. */
    static const int8_t moon_dx[8] = {0, 8, 13, 9, 0, -9, -13, -8};
    static const int8_t moon_dy[8] = {-15, -12, 0, 11, 15, 11, 0, -12};
    uint8_t frame = (anim_tick / 10) & 7;
    int mx = cx + moon_dx[frame];
    int my = cy + moon_dy[frame];
    px(mx, my);
    px(mx + 1, my);
}

static void draw_layer_strip(uint8_t layer) {
    uint8_t local = layer & 0x3;

    for (int i = 0; i < 4; i++) {
        int x = 2 + i * 8;
        rect_outline(x, 58, 5, 5);
        if (i == local) {
            fill_rect(x + 1, 59, 3, 3);
        }
    }
}

static void copy_key_history(char out[RECENT_KEY_COUNT + 1], uint32_t *distance) {
    k_spinlock_key_t key = k_spin_lock(&key_history_lock);
    memcpy(out, recent_keys, sizeof(recent_keys));
    *distance = traveled;
    k_spin_unlock(&key_history_lock, key);
}

static char keycode_to_recent_char(const struct zmk_keycode_state_changed *ev) {
    uint32_t key = ev->keycode;
    zmk_mod_flags_t mods = ev->implicit_modifiers | zmk_hid_get_explicit_mods();
    bool shifted = (mods & (MOD_LSFT | MOD_RSFT)) != 0;

    if (key >= HID_USAGE_KEY_KEYBOARD_A && key <= HID_USAGE_KEY_KEYBOARD_Z) {
        return (char)('A' + (key - HID_USAGE_KEY_KEYBOARD_A));
    }

    if (key >= HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION &&
        key <= HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS) {
        static const char shifted_digits[] = "!@#$%^&*(";
        int index = (int)(key - HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION);
        return shifted ? shifted_digits[index] : (char)('1' + index);
    }

    if (key == HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS) {
        return shifted ? ')' : '0';
    }

    switch (key) {
    case HID_USAGE_KEY_KEYBOARD_SPACEBAR: return '_';
    case HID_USAGE_KEY_KEYBOARD_RETURN_ENTER: return '>';
    case HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE: return '<';
    case HID_USAGE_KEY_KEYBOARD_TAB: return '|';
    case HID_USAGE_KEY_KEYBOARD_ESCAPE: return 'X';
    case HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE: return shifted ? '_' : '-';
    case HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS: return shifted ? '+' : '=';
    case HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE: return shifted ? '{' : '[';
    case HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE: return shifted ? '}' : ']';
    case HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE: return shifted ? '|' : '\\';
    case HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON: return shifted ? ':' : ';';
    case HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE: return shifted ? '"' : '\'';
    case HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE: return shifted ? '~' : '`';
    case HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN: return shifted ? '<' : ',';
    case HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN: return shifted ? '>' : '.';
    case HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK: return shifted ? '?' : '/';
    default: return '\0';
    }
}

static void draw_left_hud(void) {
    clear_frame();

    char keys[RECENT_KEY_COUNT + 1];
    uint32_t distance;
    copy_key_history(keys, &distance);

    draw_text_centered(1, hud.layer >= 4 ? "GAMING" : "TYPING");
    line(1, 9, 30, 9);

    draw_saturn();
    draw_layer_strip(hud.layer);
    line(1, 66, 30, 66);

    /* Compact two-column status row: connection | recent logical keys. */
    char connection[4];
    if (hud.transport == ZMK_TRANSPORT_USB) {
        strcpy(connection, "USB");
    } else {
        snprintf(connection, sizeof(connection), "BT%u", (unsigned)hud.profile + 1);
    }
    draw_text_3x5(0, 71, connection);
    line(13, 67, 13, 79);
    draw_text_3x5(16, 71, keys);
    line(1, 80, 30, 80);

    draw_text_centered(85, "TRAVELED");
    char text[12];
    snprintf(text, sizeof(text), "%lu", (unsigned long)MIN(distance, 99999999u));
    draw_text_centered(93, text);
    line(1, 102, 30, 102);

    draw_text_centered(107, "POWER");
    draw_power_gauge(hud.battery);

    present_frame_to_oled();
}

static int key_history_listener_cb(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev == NULL || !ev->state || ev->usage_page != HID_USAGE_KEY ||
        is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    char display = keycode_to_recent_char(ev);

    /* Keep the input hot path tiny: update four bytes + a counter, never render. */
    k_spinlock_key_t key = k_spin_lock(&key_history_lock);
    traveled++;
    if (display != '\0') {
        memmove(recent_keys, recent_keys + 1, RECENT_KEY_COUNT - 1);
        recent_keys[RECENT_KEY_COUNT - 1] = display;
    }
    k_spin_unlock(&key_history_lock, key);

    atomic_set(&left_dirty, 1);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(space_key_history, key_history_listener_cb);
ZMK_SUBSCRIPTION(space_key_history, zmk_keycode_state_changed);

#else

/* -------------------------------------------------------------------------- */
/* Right / peripheral: "Flight View"                                           */
/* -------------------------------------------------------------------------- */

#define STAR_COUNT 7

struct star {
    int8_t x;
    int16_t y;
    uint8_t kind;
};

static struct star stars[STAR_COUNT] = {
    {3, 5, 0},
    {27, 14, 1},
    {6, 25, 0},
    {25, 36, 0},
    {3, 48, 1},
    {28, 61, 0},
    {7, 74, 0},
};

static uint16_t rng_state = 0x4D2Bu;
static atomic_t activity_pulses = ATOMIC_INIT(0);
static uint16_t velocity;

/*
 * The peripheral only sees locally-generated position events.  Treat each
 * right-half press as a sample of roughly half the keyboard activity, then
 * feed that estimate into a deliberately inertial velocity model.
 *
 * At the 200 ms animation cadence, 93% retention gives approximately the
 * same ~3 s decay as the previous 120 ms / 96% version, while cutting OLED
 * update frequency by one third.
 */
#define VELOCITY_MAX 240u
#define VELOCITY_PRESS_IMPULSE 6u
#define VELOCITY_GLOBAL_ESTIMATE 2u
#define VELOCITY_RETENTION_PERCENT 93u

static uint8_t rng8(void) {
    rng_state = (uint16_t)(rng_state * 109u + 89u);
    return (uint8_t)(rng_state >> 8);
}

static void update_velocity(void) {
    atomic_val_t presses = atomic_clear(&activity_pulses);

    /*
     * Approximate whole-keyboard activity by doubling the locally observed
     * right-half presses.  Each estimated press adds momentum, then the ship
     * retains most of that momentum every animation tick.
     */
    if (presses > 0) {
        uint32_t impulse = (uint32_t)presses * VELOCITY_GLOBAL_ESTIMATE *
                           VELOCITY_PRESS_IMPULSE;
        velocity = (uint16_t)MIN(VELOCITY_MAX, (uint32_t)velocity + impulse);
    }

    velocity = (uint16_t)(((uint32_t)velocity * VELOCITY_RETENTION_PERCENT + 50u) / 100u);

    /* Avoid a long tail of tiny non-zero values after typing stops. */
    if (velocity < 2) {
        velocity = 0;
    }
}

static void update_stars(void) {
    int step;

    if (velocity == 0) {
        step = (anim_tick % 3 == 0) ? 1 : 0;
    } else {
        step = 1 + velocity / 55;
    }

    for (int i = 0; i < STAR_COUNT; i++) {
        stars[i].y += step;

        if (stars[i].y > 79) {
            stars[i].y = 1 + (rng8() % 8);
            stars[i].x = 2 + (rng8() % 28);
            stars[i].kind = (rng8() % 7 == 0) ? 1 : 0;
        }
    }
}

static void draw_ship(void) {
    /*
     * Preserve the full-size craft, but move the scene upward so the lower
     * telemetry can mirror the left OLED without stealing space from the ship.
     */
    line(16, 6, 10, 19);
    line(16, 6, 22, 19);

    line(10, 19, 10, 52);
    line(22, 19, 22, 52);

    line(10, 52, 13, 58);
    line(22, 52, 19, 58);
    line(13, 58, 19, 58);

    /* Window / instrument port. */
    circle_outline(16, 24, 3);
    px(16, 24);

    /* Hull bands. */
    line(11, 35, 21, 35);
    line(11, 45, 21, 45);

    /* Fins. */
    line(10, 46, 6, 57);
    line(6, 57, 11, 54);
    line(22, 46, 26, 57);
    line(26, 57, 21, 54);

    /* Engine bell. */
    line(13, 59, 19, 59);
    line(14, 60, 18, 60);

    /* Exhaust responds to velocity and gets the rest of the scene height. */
    int flame = 3 + MIN(9, velocity / 22);
    int wobble = anim_tick & 1;

    line(15, 61, 14 - wobble, 61 + flame);
    line(17, 61, 18 + wobble, 61 + flame);
    line(16, 61, 16, 64 + flame);

    if (velocity > 70) {
        px(12, 65 + (anim_tick & 3));
        px(20, 67 + ((anim_tick + 2) & 3));
    }
}

static void draw_right_hud(void) {
    clear_frame();

    /* The entire upper region is flight view: no header competing with the ship. */
    for (int i = 0; i < STAR_COUNT; i++) {
        draw_star(stars[i].x, stars[i].y, stars[i].kind);
    }
    draw_ship();

    /* Mirror the left-side TRAVELED telemetry section. */
    line(1, 80, 30, 80);
    draw_text_centered(85, "VELOCITY");

    char text[8];
    snprintf(text, sizeof(text), "%03u", (unsigned)MIN(velocity, 999));
    draw_text_centered(93, text);
    line(1, 102, 30, 102);

    /* Exactly the same POWER section and gauge geometry as the left OLED. */
    draw_text_centered(107, "POWER");
    draw_power_gauge(hud.battery);

    present_frame_to_oled();
}

#endif

/* -------------------------------------------------------------------------- */
/* ZMK event listeners                                                         */
/* -------------------------------------------------------------------------- */

struct space_battery_state {
    uint8_t level;
};

static struct space_battery_state battery_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct space_battery_state){
        .level = ev != NULL ? ev->state_of_charge : zmk_battery_state_of_charge(),
    };
}

static void battery_update_cb(struct space_battery_state state) {
    hud.battery = state.level;

    if (!hud_ready) {
        return;
    }

#if SPACE_IS_CENTRAL
    draw_left_hud();
#else
    draw_right_hud();
#endif
}

ZMK_DISPLAY_WIDGET_LISTENER(space_battery, struct space_battery_state,
                            battery_update_cb, battery_get_state);
ZMK_SUBSCRIPTION(space_battery, zmk_battery_state_changed);

#if SPACE_IS_CENTRAL

struct space_layer_state {
    uint8_t layer;
};

static struct space_layer_state layer_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct space_layer_state){
        .layer = zmk_keymap_highest_layer_active(),
    };
}

static void layer_update_cb(struct space_layer_state state) {
    hud.layer = state.layer;
    if (hud_ready) {
        draw_left_hud();
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(space_layer, struct space_layer_state,
                            layer_update_cb, layer_get_state);
ZMK_SUBSCRIPTION(space_layer, zmk_layer_state_changed);

struct space_output_state {
    uint8_t transport;
    uint8_t profile;
};

static struct space_output_state output_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();

    return (struct space_output_state){
        .transport = (uint8_t)endpoint.transport,
        .profile = endpoint.transport == ZMK_TRANSPORT_BLE
                       ? (uint8_t)endpoint.ble.profile_index
                       : 0,
    };
}

static void output_update_cb(struct space_output_state state) {
    hud.transport = state.transport;
    hud.profile = state.profile;
    if (hud_ready) {
        draw_left_hud();
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(space_output, struct space_output_state,
                            output_update_cb, output_get_state);
ZMK_SUBSCRIPTION(space_output, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(space_output, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(space_output, zmk_ble_active_profile_changed);
#endif

#else

static int activity_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state &&
        ev->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        atomic_inc(&activity_pulses);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(space_activity, activity_listener_cb);
ZMK_SUBSCRIPTION(space_activity, zmk_position_state_changed);

#endif

/* -------------------------------------------------------------------------- */
/* Animation timer + custom screen entrypoint                                  */
/* -------------------------------------------------------------------------- */

static void animation_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    anim_tick++;

#if SPACE_IS_CENTRAL
    /* Coalesce fast typing bursts into at most one OLED redraw per 100 ms. */
    bool activity_changed = atomic_clear(&left_dirty) != 0;
    if (activity_changed || (anim_tick % LEFT_ANIMATION_TICKS) == 0) {
        draw_left_hud();
    }
#else
    update_velocity();
    update_stars();
    draw_right_hud();
#endif
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    hud_canvas = lv_canvas_create(screen);
    lv_obj_align(hud_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(hud_canvas, canvas_buf, PHYSICAL_W, PHYSICAL_H,
                         LV_IMG_CF_TRUE_COLOR);

    hud_ready = true;

    /* Prime event-backed state. */
    space_battery_init();

#if SPACE_IS_CENTRAL
    space_layer_init();
    space_output_init();
    draw_left_hud();
    lv_timer_create(animation_timer_cb, LEFT_REFRESH_MS, NULL);
#else
    draw_right_hud();
    lv_timer_create(animation_timer_cb, 200, NULL);
#endif

    return screen;
}
