#define _GNU_SOURCE
#include <dlfcn.h>
#include <libgen.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include "app_glue/android_native_app_glue.h"
#include "tar_extract/tar_xz.h"

#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#define LOG_TAG "shellp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#include "../repos/yaft/glyph.h"

#define BASE_FONT_W 8
#define BASE_FONT_H 16
#define MAX_ROWS 160
#define MAX_COLS 240
#define SCROLLBACK_LINES 2000

static inline uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

// Colors (ARGB)
#define COLOR_BLACK       0xFF000000
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_GREEN       0xFF00FF00
#define COLOR_AMBER       0xFFFFB000
#define COLOR_RED         0xFFFF3333
#define COLOR_BLUE        0xFF3399FF
#define COLOR_CYAN        0xFF00E5FF
#define COLOR_DARK_GRAY   0xFF222222
#define COLOR_MID_GRAY    0xFF444444
#define COLOR_LIGHT_GRAY  0xFFAAAAAA
#define COLOR_BTN_PRESSED 0xFFE0E0E0

typedef struct {
    char ch;
    uint32_t fg;
    uint32_t bg;
} TermCell;

typedef struct {
    const char *label;
    int x1, x2;
    int key_id;
} TouchButton;

enum {
    BTN_ESC = 1,
    BTN_TAB,
    BTN_CTRL,
    BTN_ALT,
    BTN_PIPE,
    BTN_SLASH,
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_COUNT = 10
};

typedef struct {
    struct android_app *app;
    int pty_master;
    pid_t child_pid;

    int screen_w;
    int screen_h;
    int visible_h;
    int font_scale;
    int font_w;
    int font_h;
    int top_margin;
    int bottom_bar_h;

    int cols;
    int rows;
    int cursor_col;
    int cursor_row;
    bool cursor_visible;

    // Mobile state modifiers
    bool ctrl_active;
    bool alt_active;
    int pressed_button_idx;

    uint32_t current_fg;
    uint32_t current_bg;

    // ANSI parser
    bool in_esc;
    bool in_csi;
    bool in_osc;
    int csi_param[8];
    int csi_param_count;
    int csi_current;

    TermCell grid[MAX_ROWS][MAX_COLS];

    // Scrollback history (Termux style circular ring buffer)
    TermCell history[SCROLLBACK_LINES][MAX_COLS];
    int history_count;
    int history_head;
    int scroll_offset;

    // Touch gesture tracking (Termux style dragging and kinetic fling)
    float touch_down_x;
    float touch_down_y;
    float touch_last_x;
    float touch_last_y;
    uint64_t touch_down_time_ms;
    uint64_t touch_last_time_ms;
    float touch_accum_dy;
    bool touch_is_scrolling;
    float fling_velocity_y;
    uint64_t last_fling_time_ms;

    TouchButton buttons[BTN_COUNT];

    char files_dir[512];
    char debian_dir[512];
    char rootfs_dir[512];
    char bin_dir[512];
    char tmp_dir[512];
    char resolv_conf[512];
    char proot_bin[512];
    char busybox_bin[512];
    char lib_proot[512];
    char lib_loader[512];
    char lib_busybox[512];
} TermState;

static TermState g_state;

// Forward declarations
static void term_write_string(TermState *ts, const char *str);
static void render_mobile_frame(TermState *ts, ANativeWindow_Buffer *buf);
static void refresh_display(TermState *ts);

static JNIEnv* get_jni_env(struct android_app *app, bool *needs_detach) {
    if (needs_detach) *needs_detach = false;
    if (!app || !app->activity || !app->activity->vm) return NULL;
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    jint res = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (res == JNI_OK && env) {
        return env;
    }
    if (res == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK && env) {
            return env;
        }
    }
    return NULL;
}

static void release_jni_env(struct android_app *app, bool needs_detach) {
    // Thread remains safely attached for the entire lifetime of the process
    (void)app;
    (void)needs_detach;
}

// Get visible display height (above soft keyboard) via Android getWindowVisibleDisplayFrame
static int get_visible_height(struct android_app *app, int default_h) {
    bool needs_detach = false;
    JNIEnv *env = get_jni_env(app, &needs_detach);
    if (!env) return default_h;

    int visible_h = default_h;
    jclass actCls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (actCls) {
        jmethodID getWin = (*env)->GetMethodID(env, actCls, "getWindow", "()Landroid/view/Window;");
        if (getWin) {
            jobject win = (*env)->CallObjectMethod(env, app->activity->clazz, getWin);
            if (win) {
                jclass winCls = (*env)->GetObjectClass(env, win);
                jmethodID getDecor = (*env)->GetMethodID(env, winCls, "getDecorView", "()Landroid/view/View;");
                if (getDecor) {
                    jobject decor = (*env)->CallObjectMethod(env, win, getDecor);
                    if (decor) {
                        jclass rectCls = (*env)->FindClass(env, "android/graphics/Rect");
                        if (rectCls) {
                            jmethodID rectInit = (*env)->GetMethodID(env, rectCls, "<init>", "()V");
                            if (rectInit) {
                                jobject rect = (*env)->NewObject(env, rectCls, rectInit);
                                if (rect) {
                                    jclass viewCls = (*env)->GetObjectClass(env, decor);
                                    jmethodID getVisibleFrame = (*env)->GetMethodID(env, viewCls, "getWindowVisibleDisplayFrame", "(Landroid/graphics/Rect;)V");
                                    if (getVisibleFrame) {
                                        (*env)->CallVoidMethod(env, decor, getVisibleFrame, rect);
                                        jfieldID bottomField = (*env)->GetFieldID(env, rectCls, "bottom", "I");
                                        if (bottomField) {
                                            int bottom = (*env)->GetIntField(env, rect, bottomField);
                                            if (bottom > 300 && bottom <= default_h) {
                                                visible_h = bottom;
                                            }
                                        }
                                    }
                                    if (viewCls) (*env)->DeleteLocalRef(env, viewCls);
                                    (*env)->DeleteLocalRef(env, rect);
                                }
                            }
                            (*env)->DeleteLocalRef(env, rectCls);
                        }
                        (*env)->DeleteLocalRef(env, decor);
                    }
                }
                (*env)->DeleteLocalRef(env, winCls);
                (*env)->DeleteLocalRef(env, win);
            }
        }
        (*env)->DeleteLocalRef(env, actCls);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    release_jni_env(app, needs_detach);
    return visible_h;
}

// Trigger Android haptic vibration feedback for tactile touch sensation
static void trigger_haptic(struct android_app *app) {
    bool needs_detach = false;
    JNIEnv *env = get_jni_env(app, &needs_detach);
    if (!env) return;

    jclass actCls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (actCls) {
        jmethodID getWin = (*env)->GetMethodID(env, actCls, "getWindow", "()Landroid/view/Window;");
        if (getWin) {
            jobject win = (*env)->CallObjectMethod(env, app->activity->clazz, getWin);
            if (win) {
                jclass winCls = (*env)->GetObjectClass(env, win);
                jmethodID getDecor = (*env)->GetMethodID(env, winCls, "getDecorView", "()Landroid/view/View;");
                if (getDecor) {
                    jobject decor = (*env)->CallObjectMethod(env, win, getDecor);
                    if (decor) {
                        jclass viewCls = (*env)->GetObjectClass(env, decor);
                        jmethodID haptic = (*env)->GetMethodID(env, viewCls, "performHapticFeedback", "(I)Z");
                        if (haptic) {
                            (*env)->CallBooleanMethod(env, decor, haptic, 3 /* KEYBOARD_TAP */);
                        }
                        if (viewCls) (*env)->DeleteLocalRef(env, viewCls);
                        (*env)->DeleteLocalRef(env, decor);
                    }
                }
                (*env)->DeleteLocalRef(env, winCls);
                (*env)->DeleteLocalRef(env, win);
            }
        }
        (*env)->DeleteLocalRef(env, actCls);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    release_jni_env(app, needs_detach);
}

// Show Android soft keyboard on screen via InputMethodManager
static void show_keyboard(struct android_app *app) {
    bool needs_detach = false;
    JNIEnv *env = get_jni_env(app, &needs_detach);
    if (!env) return;

    jclass actCls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (actCls) {
        jmethodID showMethod = (*env)->GetMethodID(env, actCls, "showSoftKeyboard", "()V");
        if (showMethod) {
            (*env)->CallVoidMethod(env, app->activity->clazz, showMethod);
        } else {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            jmethodID getSys = (*env)->GetMethodID(env, actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
            if (getSys) {
                jstring name = (*env)->NewStringUTF(env, "input_method");
                jobject imm = (*env)->CallObjectMethod(env, app->activity->clazz, getSys, name);
                (*env)->DeleteLocalRef(env, name);
                if (imm) {
                    jclass immCls = (*env)->GetObjectClass(env, imm);
                    jmethodID toggle = (*env)->GetMethodID(env, immCls, "toggleSoftInput", "(II)V");
                    if (toggle) {
                        (*env)->CallVoidMethod(env, imm, toggle, 2, 0);
                    }
                    (*env)->DeleteLocalRef(env, immCls);
                    (*env)->DeleteLocalRef(env, imm);
                }
            }
        }
        (*env)->DeleteLocalRef(env, actCls);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    release_jni_env(app, needs_detach);
}

// Hide Android soft keyboard on screen safely
static void hide_keyboard(struct android_app *app) {
    bool needs_detach = false;
    JNIEnv *env = get_jni_env(app, &needs_detach);
    if (!env) return;

    jclass actCls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (actCls) {
        jmethodID hideMethod = (*env)->GetMethodID(env, actCls, "hideSoftKeyboard", "()V");
        if (hideMethod) {
            (*env)->CallVoidMethod(env, app->activity->clazz, hideMethod);
        } else {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            jmethodID getSys = (*env)->GetMethodID(env, actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
            jmethodID getWin = (*env)->GetMethodID(env, actCls, "getWindow", "()Landroid/view/Window;");
            if (getSys && getWin) {
                jstring name = (*env)->NewStringUTF(env, "input_method");
                jobject imm = (*env)->CallObjectMethod(env, app->activity->clazz, getSys, name);
                (*env)->DeleteLocalRef(env, name);
                jobject win = (*env)->CallObjectMethod(env, app->activity->clazz, getWin);
                if (imm && win) {
                    jclass winCls = (*env)->GetObjectClass(env, win);
                    jmethodID getDecor = (*env)->GetMethodID(env, winCls, "getDecorView", "()Landroid/view/View;");
                    if (getDecor) {
                        jobject decor = (*env)->CallObjectMethod(env, win, getDecor);
                        if (decor) {
                            jclass viewCls = (*env)->GetObjectClass(env, decor);
                            jmethodID getWindowToken = (*env)->GetMethodID(env, viewCls, "getWindowToken", "()Landroid/os/IBinder;");
                            if (getWindowToken) {
                                jobject token = (*env)->CallObjectMethod(env, decor, getWindowToken);
                                if (token) {
                                    jclass immCls = (*env)->GetObjectClass(env, imm);
                                    jmethodID hideSoftInput = (*env)->GetMethodID(env, immCls, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");
                                    if (hideSoftInput) {
                                        (*env)->CallBooleanMethod(env, imm, hideSoftInput, token, 0);
                                    }
                                    (*env)->DeleteLocalRef(env, immCls);
                                    (*env)->DeleteLocalRef(env, token);
                                }
                            }
                            if (viewCls) (*env)->DeleteLocalRef(env, viewCls);
                            (*env)->DeleteLocalRef(env, decor);
                        }
                    }
                    (*env)->DeleteLocalRef(env, winCls);
                }
                if (imm) (*env)->DeleteLocalRef(env, imm);
                if (win) (*env)->DeleteLocalRef(env, win);
            }
        }
        (*env)->DeleteLocalRef(env, actCls);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    release_jni_env(app, needs_detach);
}

// Direct, fast, crash-proof ASCII mapping in pure C (Zero JNI, zero GC pauses, zero exceptions)
static char keycode_to_ascii(int keycode, int meta) {
    bool shift = (meta & AMETA_SHIFT_ON) != 0;
    switch (keycode) {
        case AKEYCODE_ENTER:
        case AKEYCODE_NUMPAD_ENTER: return '\r';
        case AKEYCODE_DEL: return 0x7F; // Backspace
        case AKEYCODE_TAB: return '\t';
        case AKEYCODE_SPACE: return ' ';
        case AKEYCODE_ESCAPE: return 0x1B;
        case AKEYCODE_COMMA: return shift ? '<' : ',';
        case AKEYCODE_PERIOD: return shift ? '>' : '.';
        case AKEYCODE_MINUS: return shift ? '_' : '-';
        case AKEYCODE_EQUALS: return shift ? '+' : '=';
        case AKEYCODE_SLASH: return shift ? '?' : '/';
        case AKEYCODE_BACKSLASH: return shift ? '|' : '\\';
        case AKEYCODE_SEMICOLON: return shift ? ':' : ';';
        case AKEYCODE_APOSTROPHE: return shift ? '"' : '\'';
        case AKEYCODE_GRAVE: return shift ? '~' : '`';
        case AKEYCODE_LEFT_BRACKET: return shift ? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
        case AKEYCODE_STAR: return '*';
        case AKEYCODE_POUND: return '#';
        case AKEYCODE_PLUS: return '+';
        case AKEYCODE_AT: return '@';
        case AKEYCODE_NUMPAD_DIVIDE: return '/';
        case AKEYCODE_NUMPAD_MULTIPLY: return '*';
        case AKEYCODE_NUMPAD_SUBTRACT: return '-';
        case AKEYCODE_NUMPAD_ADD: return '+';
        case AKEYCODE_NUMPAD_DOT: return '.';
        case AKEYCODE_NUMPAD_COMMA: return ',';
        case AKEYCODE_NUMPAD_EQUALS: return '=';
        case AKEYCODE_NUMPAD_LEFT_PAREN: return '(';
        case AKEYCODE_NUMPAD_RIGHT_PAREN: return ')';
    }
    if (keycode >= AKEYCODE_NUMPAD_0 && keycode <= AKEYCODE_NUMPAD_9) {
        return '0' + (keycode - AKEYCODE_NUMPAD_0);
    }
    if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9) {
        if (!shift) return '0' + (keycode - AKEYCODE_0);
        const char *nums_shift = ")!@#$%^&*(";
        return nums_shift[keycode - AKEYCODE_0];
    }
    if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z) {
        return (shift ? 'A' : 'a') + (keycode - AKEYCODE_A);
    }
    return 0;
}

static void term_layout_buttons(TermState *ts) {
    const char *labels[] = { "ESC", "TAB", "CTRL", "ALT", " | ", " / ", " ^ ", " v ", " < ", " > " };
    int btn_ids[] = { BTN_ESC, BTN_TAB, BTN_CTRL, BTN_ALT, BTN_PIPE, BTN_SLASH, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
    int btn_w = ts->screen_w / BTN_COUNT;

    for (int i = 0; i < BTN_COUNT; i++) {
        ts->buttons[i].label = labels[i];
        ts->buttons[i].x1 = i * btn_w;
        ts->buttons[i].x2 = (i == BTN_COUNT - 1) ? ts->screen_w : (i + 1) * btn_w;
        ts->buttons[i].key_id = btn_ids[i];
    }
}

static void term_scroll_up(TermState *ts) {
    // Save line 0 into circular scrollback history (Termux behavior)
    memcpy(ts->history[ts->history_head], ts->grid[0], sizeof(TermCell) * ts->cols);
    if (ts->cols < MAX_COLS) {
        for (int c = ts->cols; c < MAX_COLS; c++) {
            ts->history[ts->history_head][c].ch = ' ';
            ts->history[ts->history_head][c].fg = COLOR_WHITE;
            ts->history[ts->history_head][c].bg = COLOR_BLACK;
        }
    }
    ts->history_head = (ts->history_head + 1) % SCROLLBACK_LINES;
    if (ts->history_count < SCROLLBACK_LINES) {
        ts->history_count++;
    }

    // If user is currently scrolled up reading past logs,
    // pin their viewport to the text being read
    if (ts->scroll_offset > 0) {
        if (ts->scroll_offset < ts->history_count) {
            ts->scroll_offset++;
        }
    }

    // Shift live grid upwards
    for (int r = 0; r < ts->rows - 1; r++) {
        memcpy(ts->grid[r], ts->grid[r + 1], sizeof(TermCell) * ts->cols);
    }
    for (int c = 0; c < ts->cols; c++) {
        ts->grid[ts->rows - 1][c].ch = ' ';
        ts->grid[ts->rows - 1][c].fg = ts->current_fg;
        ts->grid[ts->rows - 1][c].bg = ts->current_bg;
    }
}

static void term_newline(TermState *ts) {
    ts->cursor_row++;
    if (ts->cursor_row >= ts->rows) {
        ts->cursor_row = ts->rows - 1;
        term_scroll_up(ts);
    }
}

static void term_clear_screen(TermState *ts) {
    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++) {
            ts->grid[r][c].ch = ' ';
            ts->grid[r][c].fg = COLOR_WHITE;
            ts->grid[r][c].bg = COLOR_BLACK;
        }
    }
    ts->cursor_row = 0;
    ts->cursor_col = 0;
}

static void term_handle_csi(TermState *ts, char cmd) {
    int p0 = ts->csi_param_count > 0 ? ts->csi_param[0] : 0;
    int p1 = ts->csi_param_count > 1 ? ts->csi_param[1] : 0;

    switch (cmd) {
        case 'A': // Up
            ts->cursor_row -= (p0 == 0 ? 1 : p0);
            if (ts->cursor_row < 0) ts->cursor_row = 0;
            break;
        case 'B': // Down
            ts->cursor_row += (p0 == 0 ? 1 : p0);
            if (ts->cursor_row >= ts->rows) ts->cursor_row = ts->rows - 1;
            break;
        case 'C': // Forward
            ts->cursor_col += (p0 == 0 ? 1 : p0);
            if (ts->cursor_col >= ts->cols) ts->cursor_col = ts->cols - 1;
            break;
        case 'D': // Backward
            ts->cursor_col -= (p0 == 0 ? 1 : p0);
            if (ts->cursor_col < 0) ts->cursor_col = 0;
            break;
        case 'H':
        case 'f':
            ts->cursor_row = (p0 > 0 ? p0 - 1 : 0);
            ts->cursor_col = (p1 > 0 ? p1 - 1 : 0);
            if (ts->cursor_row >= ts->rows) ts->cursor_row = ts->rows - 1;
            if (ts->cursor_col >= ts->cols) ts->cursor_col = ts->cols - 1;
            break;
        case 'J': // Erase in display
            if (p0 == 0) {
                // Erase from cursor to end of screen
                for (int c = ts->cursor_col; c < ts->cols; c++) {
                    ts->grid[ts->cursor_row][c].ch = ' ';
                    ts->grid[ts->cursor_row][c].fg = ts->current_fg;
                    ts->grid[ts->cursor_row][c].bg = ts->current_bg;
                }
                for (int r = ts->cursor_row + 1; r < ts->rows; r++) {
                    for (int c = 0; c < ts->cols; c++) {
                        ts->grid[r][c].ch = ' ';
                        ts->grid[r][c].fg = ts->current_fg;
                        ts->grid[r][c].bg = ts->current_bg;
                    }
                }
            } else if (p0 == 1) {
                // Erase from start of screen to cursor
                for (int r = 0; r < ts->cursor_row; r++) {
                    for (int c = 0; c < ts->cols; c++) {
                        ts->grid[r][c].ch = ' ';
                        ts->grid[r][c].fg = ts->current_fg;
                        ts->grid[r][c].bg = ts->current_bg;
                    }
                }
                for (int c = 0; c <= ts->cursor_col && c < ts->cols; c++) {
                    ts->grid[ts->cursor_row][c].ch = ' ';
                    ts->grid[ts->cursor_row][c].fg = ts->current_fg;
                    ts->grid[ts->cursor_row][c].bg = ts->current_bg;
                }
            } else if (p0 == 2) {
                term_clear_screen(ts);
            } else if (p0 == 3) {
                // ANSI xterm clear scrollback
                term_clear_screen(ts);
                ts->history_count = 0;
                ts->history_head = 0;
                ts->scroll_offset = 0;
            }
            break;
        case 'K': // Erase in line
            if (p0 == 0) {
                // Erase from cursor to end of line
                for (int c = ts->cursor_col; c < ts->cols; c++) {
                    ts->grid[ts->cursor_row][c].ch = ' ';
                    ts->grid[ts->cursor_row][c].fg = ts->current_fg;
                    ts->grid[ts->cursor_row][c].bg = ts->current_bg;
                }
            } else if (p0 == 1) {
                // Erase from start of line to cursor
                for (int c = 0; c <= ts->cursor_col && c < ts->cols; c++) {
                    ts->grid[ts->cursor_row][c].ch = ' ';
                    ts->grid[ts->cursor_row][c].fg = ts->current_fg;
                    ts->grid[ts->cursor_row][c].bg = ts->current_bg;
                }
            } else if (p0 == 2) {
                // Erase entire line
                for (int c = 0; c < ts->cols; c++) {
                    ts->grid[ts->cursor_row][c].ch = ' ';
                    ts->grid[ts->cursor_row][c].fg = ts->current_fg;
                    ts->grid[ts->cursor_row][c].bg = ts->current_bg;
                }
            }
            break;
        case 'm':
            if (ts->csi_param_count == 0) {
                ts->current_fg = COLOR_WHITE;
                ts->current_bg = COLOR_BLACK;
            } else {
                for (int i = 0; i < ts->csi_param_count; i++) {
                    int p = ts->csi_param[i];
                    if (p == 0) { ts->current_fg = COLOR_WHITE; ts->current_bg = COLOR_BLACK; }
                    else if (p == 1) { /* bold */ }
                    else if (p == 31 || p == 91) ts->current_fg = COLOR_RED;
                    else if (p == 32 || p == 92) ts->current_fg = COLOR_GREEN;
                    else if (p == 33 || p == 93) ts->current_fg = COLOR_AMBER;
                    else if (p == 34 || p == 94) ts->current_fg = COLOR_BLUE;
                    else if (p == 36 || p == 96) ts->current_fg = COLOR_CYAN;
                    else if (p == 37 || p == 97) ts->current_fg = COLOR_WHITE;
                }
            }
            break;
    }
}

static void term_putc(TermState *ts, char c) {
    if (ts->in_osc) {
        // OSC string ends on BEL (0x07) or ST (\033\)
        if (c == 0x07 || c == 0x1B) {
            ts->in_osc = false;
        }
        return;
    }

    if (ts->in_esc) {
        if (ts->in_csi) {
            if (c >= '0' && c <= '9') {
                ts->csi_current = ts->csi_current * 10 + (c - '0');
                return;
            } else if (c == ';') {
                if (ts->csi_param_count < 8) ts->csi_param[ts->csi_param_count++] = ts->csi_current;
                ts->csi_current = 0;
                return;
            } else if (c == '?') {
                // Ignore private CSI prefix (e.g. \033[?2004h)
                return;
            } else {
                if (ts->csi_param_count < 8) ts->csi_param[ts->csi_param_count++] = ts->csi_current;
                term_handle_csi(ts, c);
                ts->in_esc = false;
                ts->in_csi = false;
                return;
            }
        } else if (c == '[') {
            ts->in_csi = true;
            ts->csi_param_count = 0;
            ts->csi_current = 0;
            return;
        } else if (c == ']') {
            ts->in_osc = true;
            ts->in_esc = false;
            return;
        } else {
            ts->in_esc = false;
            return;
        }
    }

    if (c == 0x1B) { ts->in_esc = true; ts->in_csi = false; ts->in_osc = false; return; }
    if (c == '\r') { ts->cursor_col = 0; return; }
    if (c == '\n') { term_newline(ts); return; }
    if (c == '\b' || c == 0x7F) {
        if (ts->cursor_col > 0) {
            ts->cursor_col--;
            ts->grid[ts->cursor_row][ts->cursor_col].ch = ' ';
        }
        return;
    }
    if (c == '\t') {
        ts->cursor_col = (ts->cursor_col + 8) & ~7;
        if (ts->cursor_col >= ts->cols) { ts->cursor_col = 0; term_newline(ts); }
        return;
    }

    if ((unsigned char)c >= 32) {
        if (ts->cursor_col >= ts->cols) {
            ts->cursor_col = 0;
            term_newline(ts);
        }
        ts->grid[ts->cursor_row][ts->cursor_col].ch = c;
        ts->grid[ts->cursor_row][ts->cursor_col].fg = ts->current_fg;
        ts->grid[ts->cursor_row][ts->cursor_col].bg = ts->current_bg;
        ts->cursor_col++;
    }
}

static void term_write_string(TermState *ts, const char *str) {
    while (*str) term_putc(ts, *str++);
}

static void refresh_display(TermState *ts) {
    if (ts->app && ts->app->window != NULL) {
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(ts->app->window, &buffer, NULL) == 0) {
            render_mobile_frame(ts, &buffer);
            ANativeWindow_unlockAndPost(ts->app->window);
        }
    }
}

static void term_init(TermState *ts, int screen_w, int screen_h) {
    ts->screen_w = screen_w;
    ts->screen_h = screen_h;
    ts->visible_h = screen_h;

    // Mobile font scale: crisp and readable on high density phone displays
    if (screen_w >= 1080) {
        ts->font_scale = 3;
    } else if (screen_w >= 720) {
        ts->font_scale = 2;
    } else {
        ts->font_scale = 2;
    }

    ts->font_w = BASE_FONT_W * ts->font_scale;
    ts->font_h = BASE_FONT_H * ts->font_scale;

    ts->top_margin = 92; // Top safe area / status bar and camera notch padding
    ts->bottom_bar_h = 56 * (ts->font_scale > 2 ? 2 : 1); // Touch keybar height

    int avail_w = screen_w;
    int avail_h = screen_h - ts->top_margin - ts->bottom_bar_h;
    if (avail_h < ts->font_h * 5) avail_h = ts->font_h * 5;

    int cols = avail_w / ts->font_w;
    int rows = avail_h / ts->font_h;

    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;

    ts->cols = cols;
    ts->rows = rows;
    ts->cursor_col = 0;
    ts->cursor_row = 0;
    ts->cursor_visible = true;
    ts->ctrl_active = false;
    ts->alt_active = false;
    ts->pressed_button_idx = -1;
    ts->current_fg = COLOR_WHITE;
    ts->current_bg = COLOR_BLACK;
    ts->in_esc = false;
    ts->in_csi = false;
    ts->in_osc = false;

    // Initialize Termux-style scrollback and gesture states
    ts->history_count = 0;
    ts->history_head = 0;
    ts->scroll_offset = 0;
    ts->touch_accum_dy = 0.0f;
    ts->touch_is_scrolling = false;
    ts->fling_velocity_y = 0.0f;

    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++) {
            ts->grid[r][c].ch = ' ';
            ts->grid[r][c].fg = COLOR_WHITE;
            ts->grid[r][c].bg = COLOR_BLACK;
        }
    }

    term_layout_buttons(ts);

    // Initial TTY Banner displayed immediately on screen
    term_write_string(ts, "\x1b[1;32m[  0.000000]\x1b[0m \x1b[1mshellp Linux Subsystem (ARM64)\x1b[0m\r\n");
    term_write_string(ts, "\x1b[1;32m[  0.001000]\x1b[0m Sandbox: 100% Rootless Storage\r\n");
}

static void term_resize(TermState *ts, int screen_w, int screen_h, int visible_h) {
    if (screen_w <= 0 || screen_h <= 0) return;
    int old_rows = ts->rows;
    int old_cursor_row = ts->cursor_row;

    ts->screen_w = screen_w;
    ts->screen_h = screen_h;
    ts->visible_h = (visible_h > 300) ? visible_h : screen_h;

    int avail_w = screen_w;
    int avail_h = ts->visible_h - ts->top_margin - ts->bottom_bar_h;
    if (avail_h < ts->font_h * 5) avail_h = ts->font_h * 5;

    int cols = avail_w / ts->font_w;
    int rows = avail_h / ts->font_h;

    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;

    int new_rows = rows;
    int new_cols = cols;

    // When the keyboard deploys (new_rows < old_rows):
    // Shift the buffer up so that the active cursor row and prompt
    // stay cleanly visible right above the touch keybar!
    if (old_rows > 0 && new_rows < old_rows && old_cursor_row >= new_rows) {
        int shift = old_cursor_row - (new_rows - 1);
        if (shift > 0 && shift < MAX_ROWS) {
            for (int s = 0; s < shift; s++) {
                memcpy(ts->history[ts->history_head], ts->grid[s], sizeof(TermCell) * ts->cols);
                if (ts->cols < MAX_COLS) {
                    for (int c = ts->cols; c < MAX_COLS; c++) {
                        ts->history[ts->history_head][c].ch = ' ';
                        ts->history[ts->history_head][c].fg = COLOR_WHITE;
                        ts->history[ts->history_head][c].bg = COLOR_BLACK;
                    }
                }
                ts->history_head = (ts->history_head + 1) % SCROLLBACK_LINES;
                if (ts->history_count < SCROLLBACK_LINES) ts->history_count++;
            }
            for (int r = 0; r < MAX_ROWS - shift; r++) {
                memcpy(ts->grid[r], ts->grid[r + shift], sizeof(TermCell) * MAX_COLS);
            }
            for (int r = MAX_ROWS - shift; r < MAX_ROWS; r++) {
                for (int c = 0; c < MAX_COLS; c++) {
                    ts->grid[r][c].ch = ' ';
                    ts->grid[r][c].fg = COLOR_WHITE;
                    ts->grid[r][c].bg = COLOR_BLACK;
                }
            }
            ts->cursor_row = new_rows - 1;
        }
    }

    ts->cols = new_cols;
    ts->rows = new_rows;
    if (ts->cursor_col >= ts->cols) ts->cursor_col = ts->cols - 1;
    if (ts->cursor_row >= ts->rows) ts->cursor_row = ts->rows - 1;
    if (ts->scroll_offset > ts->history_count) ts->scroll_offset = ts->history_count;
    if (ts->scroll_offset < 0) ts->scroll_offset = 0;

    term_layout_buttons(ts);

    if (ts->pty_master >= 0) {
        struct winsize ws = {
            .ws_row = ts->rows,
            .ws_col = ts->cols,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        ioctl(ts->pty_master, TIOCSWINSZ, &ws);
    }
}

// Draw a scaled glyph pixel by pixel with pixel-perfect clipping and boundary validation
static void draw_scaled_char(uint32_t *pixels, int stride, int max_x, int max_y, int x, int y, int scale, char ch, uint32_t fg, uint32_t bg) {
    if (!pixels || x >= max_x || y >= max_y) return;
    unsigned char c = (unsigned char)ch;
    if (c < 32 || c >= 128) c = ' ';
    const struct glyph_t *g = &glyphs[c - 32];

    for (int gly = 0; gly < BASE_FONT_H; gly++) {
        int py = y + gly * scale;
        if (py >= max_y) break;
        uint16_t row_bits = g->bitmap[gly];
        for (int glx = 0; glx < BASE_FONT_W; glx++) {
            int px = x + glx * scale;
            if (px >= max_x) break;
            uint32_t color = (row_bits & (0x80 >> glx)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                int cur_y = py + sy;
                if (cur_y < 0 || cur_y >= max_y) continue;
                uint32_t *dst = pixels + cur_y * stride + px;
                for (int sx = 0; sx < scale; sx++) {
                    if (px + sx >= 0 && px + sx < max_x) {
                        dst[sx] = color;
                    }
                }
            }
        }
    }
}

// Render complete frame with responsive touch keybar pinned above soft keyboard
static void render_mobile_frame(TermState *ts, ANativeWindow_Buffer *buf) {
    if (!buf || !buf->bits || buf->width <= 0 || buf->height <= 0 || buf->stride <= 0) return;
    uint32_t *pixels = (uint32_t *)buf->bits;
    int stride = buf->stride;
    int max_x = buf->width;
    int max_y = buf->height;

    int eff_visible_y = ts->visible_h > 300 ? ts->visible_h : max_y;
    if (eff_visible_y > max_y) eff_visible_y = max_y;

    // Clear whole screen to pure black
    for (int y = 0; y < max_y; y++) {
        uint32_t *line = pixels + y * stride;
        for (int x = 0; x < max_x; x++) line[x] = COLOR_BLACK;
    }

    int content_limit_y = eff_visible_y - ts->bottom_bar_h;
    if (content_limit_y < 0) content_limit_y = 0;

    static int log_frame = 0;
    if (++log_frame % 60 == 1) {
        LOGI("render: rows=%d cols=%d cursor=(%d,%d) scroll=%d hist=%d first_ch='%c'(%d)",
             ts->rows, ts->cols, ts->cursor_row, ts->cursor_col, ts->scroll_offset, ts->history_count,
             ts->grid[0][0].ch, (int)ts->grid[0][0].ch);
    }

    // Render terminal grid characters (with Termux-style scrollback buffer mapping)
    for (int r = 0; r < ts->rows; r++) {
        int py = ts->top_margin + r * ts->font_h;
        if (py + ts->font_h > content_limit_y || py >= max_y) break;

        int view_line = r - ts->scroll_offset;
        TermCell *row_cells = NULL;

        if (view_line >= 0) {
            if (view_line < ts->rows) {
                row_cells = ts->grid[view_line];
            }
        } else {
            // view_line is negative: -1 is latest history line, -history_count is oldest
            if (-view_line <= ts->history_count) {
                int idx = ((ts->history_head + view_line) % SCROLLBACK_LINES + SCROLLBACK_LINES) % SCROLLBACK_LINES;
                row_cells = ts->history[idx];
            }
        }

        for (int c = 0; c < ts->cols; c++) {
            int px = c * ts->font_w;
            if (px + ts->font_w > max_x) break;

            if (row_cells) {
                TermCell *cell = &row_cells[c];
                draw_scaled_char(pixels, stride, max_x, max_y, px, py, ts->font_scale, cell->ch, cell->fg, cell->bg);
            } else {
                draw_scaled_char(pixels, stride, max_x, max_y, px, py, ts->font_scale, ' ', COLOR_WHITE, COLOR_BLACK);
            }
        }
    }

    // Solid block cursor (only displayed when viewing the live active prompt)
    if (ts->scroll_offset == 0 && ts->cursor_visible && ts->cursor_row >= 0 && ts->cursor_row < ts->rows &&
        ts->cursor_col >= 0 && ts->cursor_col < ts->cols) {
        int cx = ts->cursor_col * ts->font_w;
        int cy = ts->top_margin + ts->cursor_row * ts->font_h;
        if (cx >= 0 && cx + ts->font_w <= max_x &&
            cy >= 0 && cy + ts->font_h <= content_limit_y && cy < max_y) {
            for (int sy = 0; sy < ts->font_h; sy++) {
                int cur_y = cy + sy;
                if (cur_y >= max_y) break;
                uint32_t *line = pixels + cur_y * stride + cx;
                for (int sx = 0; sx < ts->font_w; sx++) {
                    if (cx + sx < max_x) line[sx] = COLOR_WHITE;
                }
            }
        }
    }

    // Render modern scrollbar thumb on right edge when reviewing history (Termux style)
    if (ts->scroll_offset > 0 && ts->history_count > 0) {
        int track_top = ts->top_margin;
        int track_h = content_limit_y - track_top;
        if (track_h > 40) {
            int total_lines = ts->history_count + ts->rows;
            int thumb_h = (int)((float)ts->rows / (float)total_lines * track_h);
            if (thumb_h < 36) thumb_h = 36;
            if (thumb_h > track_h) thumb_h = track_h;

            float progress = (float)(ts->history_count - ts->scroll_offset) / (float)ts->history_count;
            int thumb_y = track_top + (int)(progress * (track_h - thumb_h));
            if (thumb_y < track_top) thumb_y = track_top;
            if (thumb_y + thumb_h > content_limit_y) thumb_y = content_limit_y - thumb_h;

            int bar_w = 6;
            int bar_x1 = max_x - bar_w - 2;
            int bar_x2 = max_x - 2;

            for (int sy = thumb_y; sy < thumb_y + thumb_h && sy < max_y; sy++) {
                uint32_t *line = pixels + sy * stride;
                for (int sx = bar_x1; sx < bar_x2 && sx < max_x; sx++) {
                    line[sx] = 0xFFAAAAAA; // Modern clean light gray scrollbar indicator
                }
            }
        }
    }

    // Render Mobile Touch Keybar pinned directly above soft keyboard (or screen bottom)
    int bar_y1 = eff_visible_y - ts->bottom_bar_h;
    if (bar_y1 < 0) bar_y1 = 0;
    if (bar_y1 >= max_y) bar_y1 = max_y - 1;

    // Draw bar separator line
    if (bar_y1 >= 0 && bar_y1 < max_y) {
        for (int x = 0; x < max_x; x++) {
            pixels[bar_y1 * stride + x] = COLOR_MID_GRAY;
        }
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        TouchButton *btn = &ts->buttons[i];
        int bx1 = btn->x1;
        int bx2 = btn->x2;
        if (bx1 < 0) bx1 = 0;
        if (bx1 >= max_x) continue;
        if (bx2 > max_x) bx2 = max_x;

        // Visual feedback: highlight active modifiers or pressed key
        uint32_t btn_bg = COLOR_DARK_GRAY;
        uint32_t text_fg = COLOR_WHITE;

        if (i == ts->pressed_button_idx) {
            btn_bg = COLOR_BTN_PRESSED; // High contrast tactile flash on touch
            text_fg = COLOR_BLACK;
        } else if (btn->key_id == BTN_CTRL && ts->ctrl_active) {
            btn_bg = COLOR_RED;
            text_fg = COLOR_WHITE;
        } else if (btn->key_id == BTN_ALT && ts->alt_active) {
            btn_bg = COLOR_AMBER;
            text_fg = COLOR_BLACK;
        }

        for (int by = bar_y1 + 1; by < eff_visible_y && by < max_y; by++) {
            uint32_t *line = pixels + by * stride;
            for (int bx = bx1; bx < bx2 - 1 && bx < max_x; bx++) {
                line[bx] = btn_bg;
            }
            if (bx2 - 1 >= 0 && bx2 - 1 < max_x) {
                line[bx2 - 1] = COLOR_MID_GRAY; // button border
            }
        }

        // Draw button label centered
        int label_len = strlen(btn->label);
        int scale = (ts->font_scale > 2 ? 2 : 1);
        int label_w = label_len * (BASE_FONT_W * scale);
        int label_x = bx1 + ((bx2 - bx1) - label_w) / 2;
        int label_y = bar_y1 + (ts->bottom_bar_h - (BASE_FONT_H * scale)) / 2;

        for (int l = 0; l < label_len; l++) {
            int ch_x = label_x + l * (BASE_FONT_W * scale);
            if (ch_x >= 0 && ch_x < max_x && label_y >= 0 && label_y < max_y) {
                draw_scaled_char(pixels, stride, max_x, max_y, ch_x, label_y, scale, btn->label[l], text_fg, btn_bg);
            }
        }
    }
}

// In-app direct rootfs download via Android Java HTTPS stack with real-time TTY progress
static bool download_rootfs_in_app(TermState *ts, const char *url_str, const char *dest_path) {
    if (!ts->app || !ts->app->activity || !ts->app->activity->vm) return false;
    JavaVM *vm = ts->app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return false;

    bool success = false;
    jclass urlCls = (*env)->FindClass(env, "java/net/URL");
    jmethodID urlInit = (*env)->GetMethodID(env, urlCls, "<init>", "(Ljava/lang/String;)V");
    jstring jUrl = (*env)->NewStringUTF(env, url_str);
    jobject urlObj = (*env)->NewObject(env, urlCls, urlInit, jUrl);
    (*env)->DeleteLocalRef(env, jUrl);

    if (urlObj) {
        jmethodID openConn = (*env)->GetMethodID(env, urlCls, "openConnection", "()Ljava/net/URLConnection;");
        jobject conn = (*env)->CallObjectMethod(env, urlObj, openConn);
        if (conn) {
            jclass connCls = (*env)->GetObjectClass(env, conn);
            jmethodID setConnectTimeout = (*env)->GetMethodID(env, connCls, "setConnectTimeout", "(I)V");
            jmethodID setReadTimeout = (*env)->GetMethodID(env, connCls, "setReadTimeout", "(I)V");
            if (setConnectTimeout) (*env)->CallVoidMethod(env, conn, setConnectTimeout, 20000);
            if (setReadTimeout) (*env)->CallVoidMethod(env, conn, setReadTimeout, 30000);

            jmethodID getInputStream = (*env)->GetMethodID(env, connCls, "getInputStream", "()Ljava/io/InputStream;");
            jobject inStream = (*env)->CallObjectMethod(env, conn, getInputStream);

            jmethodID getContentLength = (*env)->GetMethodID(env, connCls, "getContentLengthLong", "()J");
            jlong totalBytes = getContentLength ? (*env)->CallLongMethod(env, conn, getContentLength) : 42912980;
            if (totalBytes <= 0) totalBytes = 42912980;

            if (inStream) {
                FILE *fp = fopen(dest_path, "wb");
                if (fp) {
                    jclass inCls = (*env)->GetObjectClass(env, inStream);
                    jmethodID readMethod = (*env)->GetMethodID(env, inCls, "read", "([BII)I");
                    jbyteArray buf = (*env)->NewByteArray(env, 65536);
                    jlong downloaded = 0;
                    int last_pct = -1;

                    while (1) {
                        jint n = (*env)->CallIntMethod(env, inStream, readMethod, buf, 0, 65536);
                        if (n <= 0) break;

                        jbyte *bytes = (*env)->GetByteArrayElements(env, buf, NULL);
                        fwrite(bytes, 1, n, fp);
                        (*env)->ReleaseByteArrayElements(env, buf, bytes, JNI_ABORT);

                        downloaded += n;
                        int pct = (int)((downloaded * 100) / totalBytes);
                        if (pct != last_pct) {
                            last_pct = pct;
                            char msg[128];
                            snprintf(msg, sizeof(msg), "\r\x1b[1;36m[ DOWNLOAD ]\x1b[0m %3d%% (%.1f MB / %.1f MB) ",
                                pct, (double)downloaded / (1024 * 1024), (double)totalBytes / (1024 * 1024));
                            term_write_string(ts, msg);
                            refresh_display(ts);
                        }
                    }
                    fclose(fp);
                    (*env)->DeleteLocalRef(env, buf);
                    (*env)->DeleteLocalRef(env, inCls);
                    success = (downloaded >= 35 * 1024 * 1024);
                }
                jmethodID closeMethod = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, inStream), "close", "()V");
                if (closeMethod) (*env)->CallVoidMethod(env, inStream, closeMethod);
                (*env)->DeleteLocalRef(env, inStream);
            }
            (*env)->DeleteLocalRef(env, connCls);
            (*env)->DeleteLocalRef(env, conn);
        }
        (*env)->DeleteLocalRef(env, urlObj);
    }
    (*env)->DeleteLocalRef(env, urlCls);
    (*vm)->DetachCurrentThread(vm);
    return success;
}

// Setup sandbox directories, DNS resolver, and native execution symlinks
static void setup_environment(TermState *ts, struct android_app *app, const char *internal_dir) {
    snprintf(ts->files_dir, sizeof(ts->files_dir), "%s", internal_dir);
    snprintf(ts->debian_dir, sizeof(ts->debian_dir), "%s/debian", internal_dir);
    snprintf(ts->rootfs_dir, sizeof(ts->rootfs_dir), "%s/debian/debian-bookworm-aarch64", internal_dir);
    snprintf(ts->bin_dir, sizeof(ts->bin_dir), "%s/bin", internal_dir);
    snprintf(ts->tmp_dir, sizeof(ts->tmp_dir), "%s/tmp", internal_dir);
    snprintf(ts->resolv_conf, sizeof(ts->resolv_conf), "%s/resolv.conf", internal_dir);
    snprintf(ts->proot_bin, sizeof(ts->proot_bin), "%s/bin/proot", internal_dir);
    snprintf(ts->busybox_bin, sizeof(ts->busybox_bin), "%s/bin/busybox", internal_dir);

    mkdir(ts->files_dir, 0755);
    mkdir(ts->debian_dir, 0755);
    mkdir(ts->rootfs_dir, 0755);
    mkdir(ts->bin_dir, 0755);
    mkdir(ts->tmp_dir, 0755);

    // Setup DNS resolv.conf in host sandbox
    FILE *rf = fopen(ts->resolv_conf, "w");
    if (rf) {
        fprintf(rf, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
        fclose(rf);
    }

    setenv("PROOT_TMP_DIR", ts->tmp_dir, 1);
    setenv("TMPDIR", ts->tmp_dir, 1);
    setenv("HOME", ts->files_dir, 1);

    // Locate exact native library directory via JNI ApplicationInfo
    char lib_dir[512] = {0};
    if (app && app->activity && app->activity->vm) {
        JavaVM *vm = app->activity->vm;
        JNIEnv *env = NULL;
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK && env) {
            jclass actCls = (*env)->GetObjectClass(env, app->activity->clazz);
            jmethodID getAppInfo = (*env)->GetMethodID(env, actCls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
            if (getAppInfo) {
                jobject appInfo = (*env)->CallObjectMethod(env, app->activity->clazz, getAppInfo);
                if (appInfo) {
                    jclass appInfoCls = (*env)->GetObjectClass(env, appInfo);
                    jfieldID natLibDirField = (*env)->GetFieldID(env, appInfoCls, "nativeLibraryDir", "Ljava/lang/String;");
                    if (natLibDirField) {
                        jstring jNatLibDir = (jstring)(*env)->GetObjectField(env, appInfo, natLibDirField);
                        if (jNatLibDir) {
                            const char *natStr = (*env)->GetStringUTFChars(env, jNatLibDir, NULL);
                            if (natStr && natStr[0] == '/') {
                                strncpy(lib_dir, natStr, sizeof(lib_dir) - 1);
                            }
                            if (natStr) (*env)->ReleaseStringUTFChars(env, jNatLibDir, natStr);
                        }
                    }
                    (*env)->DeleteLocalRef(env, appInfo);
                }
            }
            (*vm)->DetachCurrentThread(vm);
        }
    }

    if (lib_dir[0] != '/') {
        Dl_info dlinfo;
        if (dladdr((void*)android_main, &dlinfo) && dlinfo.dli_fname && dlinfo.dli_fname[0] == '/') {
            char tmp[512];
            strncpy(tmp, dlinfo.dli_fname, sizeof(tmp));
            strncpy(lib_dir, dirname(tmp), sizeof(lib_dir) - 1);
        }
    }

    snprintf(ts->lib_proot, sizeof(ts->lib_proot), "%s/libproot.so", lib_dir);
    snprintf(ts->lib_loader, sizeof(ts->lib_loader), "%s/libloader.so", lib_dir);
    snprintf(ts->lib_busybox, sizeof(ts->lib_busybox), "%s/libbusybox.so", lib_dir);

    unlink(ts->proot_bin);
    symlink(ts->lib_proot, ts->proot_bin);

    unlink(ts->busybox_bin);
    symlink(ts->lib_busybox, ts->busybox_bin);

    char talloc_so2[512];
    char talloc_so[512];
    snprintf(talloc_so2, sizeof(talloc_so2), "%s/libtalloc.so.2", ts->bin_dir);
    snprintf(talloc_so, sizeof(talloc_so), "%s/libtalloc.so", lib_dir);
    unlink(talloc_so2);
    symlink(talloc_so, talloc_so2);

    const char *tools[] = { "sh", "wget", "tar", "xz", "gzip", "cat", "mkdir", "rm", "touch", "chmod", "echo", "nslookup", NULL };
    for (int i = 0; tools[i]; i++) {
        char link_path[512];
        snprintf(link_path, sizeof(link_path), "%s/%s", ts->bin_dir, tools[i]);
        unlink(link_path);
        symlink(ts->lib_busybox, link_path);
    }
}

// Recursively ensure all executables and directories in Debian rootfs have execute bits
static void ensure_subsystem_permissions(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child_path[1024];
        snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (lstat(child_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (strcmp(ent->d_name, "tmp") == 0 || strcmp(ent->d_name, "shm") == 0) {
                    chmod(child_path, 01777);
                } else {
                    chmod(child_path, 0755);
                }
                ensure_subsystem_permissions(child_path);
            } else if (S_ISREG(st.st_mode)) {
                if ((st.st_mode & 0111) != 0 ||
                    strstr(child_path, "/bin/") ||
                    strstr(child_path, "/sbin/") ||
                    strstr(child_path, "/lib/") ||
                    strstr(child_path, ".so")) {
                    chmod(child_path, 0755);
                } else {
                    chmod(child_path, 0644);
                }
            }
        }
    }
    closedir(d);
}

static void on_extract_progress(size_t bytes_decompressed, const char *cur_file, void *userdata) {
    TermState *ts = (TermState *)userdata;
    static size_t last_report = 0;
    if (bytes_decompressed - last_report >= 2 * 1024 * 1024) {
        last_report = bytes_decompressed;
        int pct = (int)((bytes_decompressed * 100) / (240 * 1024 * 1024));
        if (pct > 99) pct = 99;
        char msg[160];
        snprintf(msg, sizeof(msg), "\r\x1b[1;36m[ EXTRACT ]\x1b[0m %2d%% (%.1f MB) \x1b[90m%-30.30s\x1b[0m",
                 pct, (double)bytes_decompressed / (1024 * 1024), cur_file ? cur_file : "");
        term_write_string(ts, msg);
        refresh_display(ts);
    }
}

// Automated in-app bootstrap: download rootfs and unpack with live progress
static void perform_bootstrap_if_needed(TermState *ts) {
    char bash_check[512], bash_check2[512];
    snprintf(bash_check, sizeof(bash_check), "%s/bin/bash", ts->rootfs_dir);
    snprintf(bash_check2, sizeof(bash_check2), "%s/usr/bin/bash", ts->rootfs_dir);

    if (access(bash_check, F_OK) == 0 || access(bash_check2, F_OK) == 0) {
        return; // Debian is already fully installed
    }

    term_write_string(ts, "\x1b[1;33m[  0.002000]\x1b[0m First launch: Starting automated bootstrap...\r\n");
    refresh_display(ts);

    char tarball_path[512];
    snprintf(tarball_path, sizeof(tarball_path), "%s/rootfs.tar.xz", ts->tmp_dir);

    // Step 1: Download rootfs if not already present
    struct stat st;
    if (stat(tarball_path, &st) != 0 || st.st_size < 35 * 1024 * 1024) {
        term_write_string(ts, "\x1b[1;36m[  0.003000]\x1b[0m Downloading Debian Bookworm rootfs...\r\n");
        refresh_display(ts);

        const char *url = "https://github.com/termux/proot-distro/releases/download/v4.17.3/debian-bookworm-aarch64-pd-v4.17.3.tar.xz";
        bool ok = download_rootfs_in_app(ts, url, tarball_path);
        if (!ok) {
            term_write_string(ts, "\r\n\x1b[1;31m[ ERROR ]\x1b[0m Download failed. Retrying...\r\n");
            refresh_display(ts);
            download_rootfs_in_app(ts, url, tarball_path);
        }
        term_write_string(ts, "\r\n");
    }

    // Step 2: Unpack root filesystem using 100% encapsulated in-process C decompressor
    term_write_string(ts, "\x1b[1;36m[  0.004000]\x1b[0m Extracting Debian ARM64 root filesystem...\r\n");
    refresh_display(ts);

    bool ok_ext = extract_tar_xz(tarball_path, ts->debian_dir, on_extract_progress, ts);
    term_write_string(ts, "\r\x1b[1;36m[ EXTRACT ]\x1b[0m 100% (240.0 MB) Extraction complete!        \r\n");
    refresh_display(ts);

    if (!ok_ext || (access(bash_check, F_OK) != 0 && access(bash_check2, F_OK) != 0)) {
        term_write_string(ts, "\r\n\x1b[1;31m[ ERROR ]\x1b[0m Extraction failed: bash not found in rootfs\r\n");
        refresh_display(ts);
        return;
    }

    // Step 3: Configure DNS and hosts inside Debian rootfs
    char deb_resolv[512], deb_hosts[512], deb_etc[512];
    snprintf(deb_etc, sizeof(deb_etc), "%s/etc", ts->rootfs_dir);
    snprintf(deb_resolv, sizeof(deb_resolv), "%s/etc/resolv.conf", ts->rootfs_dir);
    snprintf(deb_hosts, sizeof(deb_hosts), "%s/etc/hosts", ts->rootfs_dir);

    mkdir(deb_etc, 0755);
    FILE *rf = fopen(deb_resolv, "w");
    if (rf) {
        fprintf(rf, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
        fclose(rf);
    }
    FILE *hf = fopen(deb_hosts, "w");
    if (hf) {
        fprintf(hf, "127.0.0.1 localhost shellp\n");
        fclose(hf);
    }

    // Step 4: Cleanup archive on successful extraction
    unlink(tarball_path);

    char installed_flag[512];
    snprintf(installed_flag, sizeof(installed_flag), "%s/.installed", ts->debian_dir);
    FILE *inst_f = fopen(installed_flag, "w");
    if (inst_f) fclose(inst_f);

    term_write_string(ts, "\r\n\x1b[1;32m[  0.005000]\x1b[0m \x1b[1mBootstrap complete! Launching Debian Bookworm...\x1b[0m\r\n\r\n");
    refresh_display(ts);
}

static void launch_subsystem(TermState *ts) {
    int master, slave;
    struct winsize ws = {
        .ws_row = ts->rows,
        .ws_col = ts->cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    struct termios term;
    memset(&term, 0, sizeof(term));
    cfmakeraw(&term);
    term.c_lflag |= (ECHO | ISIG | ICANON);
    term.c_iflag |= (ICRNL | IXON);
    term.c_oflag |= (OPOST | ONLCR);

    if (openpty(&master, &slave, NULL, &term, &ws) < 0) {
        term_write_string(ts, "\x1b[1;31m[ ERROR ]\x1b[0m Failed to open PTY\r\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        term_write_string(ts, "\x1b[1;31m[ ERROR ]\x1b[0m Fork failed\r\n");
        close(master);
        close(slave);
        return;
    }

    if (pid == 0) {
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);

        chdir(ts->files_dir);

        char bash_check[512], bash_check2[512];
        snprintf(bash_check, sizeof(bash_check), "%s/bin/bash", ts->rootfs_dir);
        snprintf(bash_check2, sizeof(bash_check2), "%s/usr/bin/bash", ts->rootfs_dir);

        if (access(bash_check, F_OK) == 0 || access(bash_check2, F_OK) == 0) {
            // Ensure all binaries, libraries, and directories have valid execute/read bits
            ensure_subsystem_permissions(ts->rootfs_dir);

            // Set PROOT required environment variables
            char ld_path[1024];
            // Compute lib_dir by taking dirname of ts->lib_proot
            char tmp_path[512];
            strncpy(tmp_path, ts->lib_proot, sizeof(tmp_path) - 1);
            tmp_path[sizeof(tmp_path) - 1] = '\0';
            snprintf(ld_path, sizeof(ld_path), "%s:%s", dirname(tmp_path), ts->bin_dir);
            setenv("LD_LIBRARY_PATH", ld_path, 1);

            setenv("PROOT_TMP_DIR", ts->tmp_dir, 1);
            setenv("TMPDIR", "/tmp", 1);
            if (ts->lib_loader[0]) {
                setenv("PROOT_LOADER", ts->lib_loader, 1);
            }
            setenv("PROOT_IGNORE_MISSING_BINDINGS", "1", 1);
            setenv("HOME", "/root", 1);
            setenv("USER", "root", 1);
            setenv("LOGNAME", "root", 1);
            setenv("TERM", "xterm-256color", 1);
            setenv("LANG", "C.UTF-8", 1);
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
            setenv("SHELL", "/bin/bash", 1);

            // Ensure guest /tmp and /dev/shm exist with standard sticky bit permissions (01777)
            char deb_tmp[512], deb_shm[512], deb_bashrc[512];
            snprintf(deb_tmp, sizeof(deb_tmp), "%s/tmp", ts->rootfs_dir);
            mkdir(deb_tmp, 01777);
            chmod(deb_tmp, 01777);

            snprintf(deb_shm, sizeof(deb_shm), "%s/dev/shm", ts->rootfs_dir);
            mkdir(deb_shm, 01777);
            chmod(deb_shm, 01777);

            // Ensure TMPDIR=/tmp is persisted in /root/.bashrc for all subshells
            snprintf(deb_bashrc, sizeof(deb_bashrc), "%s/root/.bashrc", ts->rootfs_dir);
            FILE *brc = fopen(deb_bashrc, "a");
            if (brc) {
                fprintf(brc, "\nexport TMPDIR=/tmp\n");
                fclose(brc);
            }

            char bind_resolv[512];
            snprintf(bind_resolv, sizeof(bind_resolv), "%s:/etc/resolv.conf", ts->resolv_conf);

            char rootfs_resolv[512];
            snprintf(rootfs_resolv, sizeof(rootfs_resolv), "%s/etc/resolv.conf", ts->rootfs_dir);
            unlink(rootfs_resolv);
            FILE *rrf = fopen(rootfs_resolv, "w");
            if (rrf) {
                fprintf(rrf, "nameserver 8.8.8.8\nnameserver 1.1.1.1\n");
                fclose(rrf);
            }

            char *proot_argv[] = {
                "proot",
                "--link2symlink",
                "--sysvipc",
                "-0",
                "-r", ts->rootfs_dir,
                "-b", "/dev",
                "-b", "/proc",
                "-b", "/sys",
                "-b", bind_resolv,
                "-w", "/root",
                "/bin/bash", "--login",
                NULL
            };
            execv(ts->lib_proot, proot_argv);
            proot_argv[0] = ts->proot_bin;
            execv(ts->proot_bin, proot_argv);

            fprintf(stderr, "\r\n\x1b[1;31m[ ERROR ]\x1b[0m PRoot exec failed: %s\r\n", strerror(errno));
        } else {
            fprintf(stderr, "\r\n\x1b[1;31m[ ERROR ]\x1b[0m Debian bash not found at %s\r\n", bash_check);
        }

        // Fallback to BusyBox shell
        char *fallback_argv[] = { "busybox", "sh", "-i", NULL };
        execv(ts->lib_busybox, fallback_argv);
        fallback_argv[0] = ts->busybox_bin;
        execv(ts->busybox_bin, fallback_argv);
        _exit(127);
    }

    close(slave);

    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);

    ts->pty_master = master;
    ts->child_pid = pid;
}

static void send_to_pty(TermState *ts, const char *seq, int len) {
    if (ts->pty_master >= 0 && seq && len > 0) {
        write(ts->pty_master, seq, len);
    }
}

static int32_t engine_handle_input(struct android_app *app, AInputEvent *event) {
    TermState *ts = (TermState *)app->userData;

    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        int action = AKeyEvent_getAction(event);
        int keycode = AKeyEvent_getKeyCode(event);

        // Always intercept and consume BACK key on BOTH DOWN and UP!
        // Never allow Android framework to invoke default onBackPressed() -> finish()!
        if (keycode == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                hide_keyboard(app);
            }
            return 1;
        }

        if (action == AKEY_EVENT_ACTION_DOWN) {
            // Typing any key immediately snaps viewport back to live prompt (Termux behavior)
            if (ts->scroll_offset > 0) {
                ts->scroll_offset = 0;
                ts->fling_velocity_y = 0.0f;
                refresh_display(ts);
            }

            int meta = AKeyEvent_getMetaState(event);

            // Special directional keys
            if (keycode == AKEYCODE_DPAD_UP) { send_to_pty(ts, "\x1b[A", 3); return 1; }
            if (keycode == AKEYCODE_DPAD_DOWN) { send_to_pty(ts, "\x1b[B", 3); return 1; }
            if (keycode == AKEYCODE_DPAD_RIGHT) { send_to_pty(ts, "\x1b[C", 3); return 1; }
            if (keycode == AKEYCODE_DPAD_LEFT) { send_to_pty(ts, "\x1b[D", 3); return 1; }

            char c = keycode_to_ascii(keycode, meta);

            if (c != 0) {
                if (ts->ctrl_active) {
                    if (c >= 'a' && c <= 'z') c = (c - 'a' + 1);
                    else if (c >= 'A' && c <= 'Z') c = (c - 'A' + 1);
                    ts->ctrl_active = false;
                }
                if (ts->alt_active) {
                    char esc_seq[2] = { 0x1B, c };
                    send_to_pty(ts, esc_seq, 2);
                    ts->alt_active = false;
                    return 1;
                }
                send_to_pty(ts, &c, 1);
                return 1;
            }
        }
        return 1; // Always consume key events to prevent unexpected exit
    } else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        float touch_x = AMotionEvent_getX(event, 0);
        float touch_y = AMotionEvent_getY(event, 0);

        int eff_visible_y = ts->visible_h > 300 ? ts->visible_h : ts->screen_h;

        // Support external mouse wheel & trackpad scrolling (Samsung DeX style)
        if (action == AMOTION_EVENT_ACTION_SCROLL) {
            float vscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
            if (vscroll != 0.0f) {
                int lines = (int)(vscroll * 3.0f);
                if (lines == 0) lines = (vscroll > 0.0f ? 1 : -1);
                ts->scroll_offset += lines;
                if (ts->scroll_offset < 0) ts->scroll_offset = 0;
                if (ts->scroll_offset > ts->history_count) ts->scroll_offset = ts->history_count;
                refresh_display(ts);
                return 1;
            }
        }

        if (action == AMOTION_EVENT_ACTION_DOWN) {
            ts->fling_velocity_y = 0.0f; // Instantly halt any kinetic fling on touch

            // Check if touch is in the mobile keybar area pinned above keyboard
            if (touch_y >= eff_visible_y - ts->bottom_bar_h && touch_y <= eff_visible_y) {
                ts->touch_is_scrolling = false;
                for (int i = 0; i < BTN_COUNT; i++) {
                    TouchButton *btn = &ts->buttons[i];
                    if (touch_x >= btn->x1 && touch_x < btn->x2) {
                        ts->pressed_button_idx = i;
                        if (ts->scroll_offset > 0) {
                            ts->scroll_offset = 0;
                        }
                        trigger_haptic(app);
                        switch (btn->key_id) {
                            case BTN_ESC:
                                send_to_pty(ts, "\x1b", 1);
                                break;
                            case BTN_TAB:
                                send_to_pty(ts, "\t", 1);
                                break;
                            case BTN_CTRL:
                                ts->ctrl_active = !ts->ctrl_active;
                                break;
                            case BTN_ALT:
                                ts->alt_active = !ts->alt_active;
                                break;
                            case BTN_PIPE:
                                send_to_pty(ts, "|", 1);
                                break;
                            case BTN_SLASH:
                                send_to_pty(ts, "/", 1);
                                break;
                            case BTN_UP:
                                send_to_pty(ts, "\x1b[A", 3);
                                break;
                            case BTN_DOWN:
                                send_to_pty(ts, "\x1b[B", 3);
                                break;
                            case BTN_LEFT:
                                send_to_pty(ts, "\x1b[D", 3);
                                break;
                            case BTN_RIGHT:
                                send_to_pty(ts, "\x1b[C", 3);
                                break;
                        }
                        refresh_display(ts);
                        return 1;
                    }
                }
            } else if (touch_y < eff_visible_y - ts->bottom_bar_h) {
                // Touch down on terminal area: prepare for drag or tap
                ts->touch_down_x = touch_x;
                ts->touch_down_y = touch_y;
                ts->touch_last_x = touch_x;
                ts->touch_last_y = touch_y;
                ts->touch_down_time_ms = get_time_ms();
                ts->touch_last_time_ms = ts->touch_down_time_ms;
                ts->touch_accum_dy = 0.0f;
                ts->touch_is_scrolling = false;
                return 1;
            }
        } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            if (ts->pressed_button_idx >= 0) {
                return 1;
            }
            if (touch_y < eff_visible_y - ts->bottom_bar_h) {
                uint64_t now = get_time_ms();
                float dt = (now - ts->touch_last_time_ms) / 1000.0f;
                float dy = touch_y - ts->touch_last_y;
                float total_dx = fabsf(touch_x - ts->touch_down_x);
                float total_dy = fabsf(touch_y - ts->touch_down_y);

                if (!ts->touch_is_scrolling) {
                    if (total_dy > 12.0f && total_dy > total_dx) {
                        ts->touch_is_scrolling = true;
                    }
                }

                if (ts->touch_is_scrolling) {
                    if (dt > 0.001f) {
                        float inst_v = dy / dt;
                        ts->fling_velocity_y = 0.5f * ts->fling_velocity_y + 0.5f * inst_v;
                    }
                    ts->touch_accum_dy += dy;
                    int lines = (int)(ts->touch_accum_dy / (float)ts->font_h);
                    if (lines != 0) {
                        ts->scroll_offset += lines;
                        ts->touch_accum_dy -= (float)(lines * ts->font_h);
                        if (ts->scroll_offset < 0) ts->scroll_offset = 0;
                        if (ts->scroll_offset > ts->history_count) ts->scroll_offset = ts->history_count;
                        refresh_display(ts);
                    }
                }

                ts->touch_last_x = touch_x;
                ts->touch_last_y = touch_y;
                ts->touch_last_time_ms = now;
                return 1;
            }
        } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            if (ts->pressed_button_idx >= 0) {
                ts->pressed_button_idx = -1;
                refresh_display(ts);
                return 1;
            }

            if (action == AMOTION_EVENT_ACTION_UP) {
                if (ts->touch_is_scrolling) {
                    uint64_t elapsed_since_last_move = get_time_ms() - ts->touch_last_time_ms;
                    if (elapsed_since_last_move > 80) {
                        ts->fling_velocity_y = 0.0f;
                    } else {
                        // Clamp maximum fling velocity
                        if (ts->fling_velocity_y > 6000.0f) ts->fling_velocity_y = 6000.0f;
                        if (ts->fling_velocity_y < -6000.0f) ts->fling_velocity_y = -6000.0f;
                        ts->last_fling_time_ms = get_time_ms();
                    }
                    ts->touch_is_scrolling = false;
                } else {
                    // Tap detected! Show soft keyboard with subtle haptic feedback
                    ts->fling_velocity_y = 0.0f;
                    trigger_haptic(app);
                    show_keyboard(app);
                }
            } else {
                ts->touch_is_scrolling = false;
                ts->fling_velocity_y = 0.0f;
            }
            return 1;
        }
    }
    return 0;
}

static void engine_handle_cmd(struct android_app *app, int32_t cmd) {
    TermState *ts = (TermState *)app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != NULL) {
                ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
                int w = ANativeWindow_getWidth(app->window);
                int h = ANativeWindow_getHeight(app->window);
                int vh = get_visible_height(app, h);

                if (ts->screen_w == 0 || ts->screen_h == 0) {
                    term_init(ts, w, h);
                    setup_environment(ts, app, app->activity->internalDataPath);
                    refresh_display(ts);

                    // Perform bootstrap in app with live progress displayed on screen
                    perform_bootstrap_if_needed(ts);

                    // Clean screen completely before launching Debian bash
                    term_clear_screen(ts);

                    // Launch Debian PRoot subsystem
                    launch_subsystem(ts);
                } else {
                    term_resize(ts, w, h, vh);
                    refresh_display(ts);
                }

                show_keyboard(app);
            }
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (app->window != NULL) {
                ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
                int w = ANativeWindow_getWidth(app->window);
                int h = ANativeWindow_getHeight(app->window);
                int vh = get_visible_height(app, h);
                term_resize(ts, w, h, vh);
                refresh_display(ts);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // Do NOT close pty_master or kill Debian child process! Keep session 100% alive!
            break;
        case APP_CMD_DESTROY:
            if (ts->pty_master >= 0) {
                close(ts->pty_master);
                ts->pty_master = -1;
            }
            if (ts->child_pid > 0) {
                kill(ts->child_pid, SIGTERM);
                ts->child_pid = -1;
            }
            break;
    }
}

void android_main(struct android_app *state) {
    signal(SIGPIPE, SIG_IGN);

    memset(&g_state, 0, sizeof(g_state));
    g_state.app = state;
    g_state.pty_master = -1;
    g_state.child_pid = -1;
    g_state.pressed_button_idx = -1;

    state->userData = &g_state;
    state->onAppCmd = engine_handle_cmd;
    state->onInputEvent = engine_handle_input;

    char read_buf[2048];
    int tick_count = 0;

    while (1) {
        int ident;
        int events;
        struct android_poll_source *source;
        int timeout = (g_state.pty_master >= 0 || fabsf(g_state.fling_velocity_y) > 5.0f) ? 0 : -1;

        while ((ident = ALooper_pollOnce(timeout, NULL, &events, (void **)&source)) >= 0) {
            if (source != NULL) source->process(state, source);
            if (state->destroyRequested != 0) return;
            // Update timeout immediately so we don't block once pty_master is opened
            timeout = (g_state.pty_master >= 0 || fabsf(g_state.fling_velocity_y) > 5.0f) ? 0 : -1;
        }

        // Periodically update visible height to track soft keyboard slide-up/slide-down
        tick_count++;
        if (tick_count % 10 == 0 && state->window != NULL) {
            int cur_h = ANativeWindow_getHeight(state->window);
            if (cur_h > 0) {
                int vh = get_visible_height(state, cur_h);
                if (vh != g_state.visible_h) {
                    term_resize(&g_state, g_state.screen_w, g_state.screen_h, vh);
                    refresh_display(&g_state);
                }
            }
        }

        // Drain all available PTY output for instant, zero-lag terminal responsiveness
        if (g_state.pty_master >= 0) {
            while (1) {
                ssize_t bytes = read(g_state.pty_master, read_buf, sizeof(read_buf));
                if (bytes > 0) {
                    for (ssize_t i = 0; i < bytes; i++) {
                        term_putc(&g_state, read_buf[i]);
                    }
                } else {
                    break;
                }
            }
        }

        // Handle Termux-style inertial kinetic scrolling (fling momentum)
        if (fabsf(g_state.fling_velocity_y) > 5.0f) {
            uint64_t now = get_time_ms();
            float dt = (now - g_state.last_fling_time_ms) / 1000.0f;
            if (dt > 0.0f && dt < 0.1f) {
                float dy = g_state.fling_velocity_y * dt;
                g_state.touch_accum_dy += dy;
                int lines = (int)(g_state.touch_accum_dy / (float)g_state.font_h);
                if (lines != 0) {
                    g_state.scroll_offset += lines;
                    g_state.touch_accum_dy -= (float)(lines * g_state.font_h);
                    if (g_state.scroll_offset <= 0) {
                        g_state.scroll_offset = 0;
                        g_state.fling_velocity_y = 0.0f;
                    } else if (g_state.scroll_offset >= g_state.history_count) {
                        g_state.scroll_offset = g_state.history_count;
                        g_state.fling_velocity_y = 0.0f;
                    }
                }
                g_state.fling_velocity_y *= 0.92f; // Smooth viscous damping
                if (fabsf(g_state.fling_velocity_y) < 20.0f) {
                    g_state.fling_velocity_y = 0.0f;
                }
            }
            g_state.last_fling_time_ms = now;
        }

        // Check if child exited and reap zombie
        if (g_state.child_pid > 0) {
            int status;
            pid_t wp = waitpid(g_state.child_pid, &status, WNOHANG);
            if (wp > 0) {
                g_state.child_pid = -1;
            }
        }

        if (state->window != NULL) {
            ANativeWindow_Buffer buffer;
            if (ANativeWindow_lock(state->window, &buffer, NULL) == 0) {
                render_mobile_frame(&g_state, &buffer);
                ANativeWindow_unlockAndPost(state->window);
            }
        }

        // Wait on PTY or sleep at most 10ms to yield CPU when idle
        if (g_state.pty_master >= 0 && fabsf(g_state.fling_velocity_y) <= 5.0f) {
            struct pollfd pfd = { .fd = g_state.pty_master, .events = POLLIN };
            poll(&pfd, 1, 10);
        }
    }
}
