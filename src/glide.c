#include "wm.h"

double purp_glide_speed(double held_secs) {
    double t = purp_clamp_f64(held_secs / PURP_RESIZE_RAMP_SECS, 0.0, 1.0);
    return PURP_RESIZE_SPEED_MIN + (PURP_RESIZE_SPEED_MAX - PURP_RESIZE_SPEED_MIN) * t * t;
}

void purp_key_glide_init(purp_key_glide_t *glide, uint64_t now_us) {
    *glide = (purp_key_glide_t){ .last_tick_us = now_us };
}

void purp_key_glide_press(purp_key_glide_t *glide, bool has_keycode, uint32_t keycode,
                          int32_t dir_x, int32_t dir_y, uint64_t now_us) {
    if (glide->key_count == 0) glide->last_tick_us = now_us;
    for (size_t i = 0; i < glide->key_count; ++i) {
        if (has_keycode && glide->keys[i].held && glide->keys[i].has_keycode &&
            glide->keys[i].keycode == keycode) return;
    }
    if (glide->key_count == PURP_GLIDE_MAX_KEYS) return;
    glide->keys[glide->key_count++] = (purp_glide_key_t){
        .has_keycode = has_keycode, .keycode = keycode, .dir_x = dir_x, .dir_y = dir_y,
        .pressed_us = now_us, .held = has_keycode,
    };
}

void purp_key_glide_release(purp_key_glide_t *glide, uint32_t keycode) {
    for (size_t i = 0; i < glide->key_count; ++i)
        if (glide->keys[i].has_keycode && glide->keys[i].keycode == keycode)
            glide->keys[i].held = false;
}

void purp_key_glide_release_all(purp_key_glide_t *glide) {
    for (size_t i = 0; i < glide->key_count; ++i) glide->keys[i].held = false;
}

bool purp_key_glide_is_done(const purp_key_glide_t *glide) { return glide->key_count == 0; }

purp_point_t purp_key_glide_advance(purp_key_glide_t *glide, uint64_t now_us) {
    double dt = now_us >= glide->last_tick_us ? (double)(now_us - glide->last_tick_us) / 1e6 : 0.0;
    dt = fmin(dt, PURP_GLIDE_MAX_FRAME_SECS);
    glide->last_tick_us = now_us;
    double x = glide->carry_x, y = glide->carry_y;
    size_t retained = 0;
    for (size_t i = 0; i < glide->key_count; ++i) {
        purp_glide_key_t key = glide->keys[i];
        double held = now_us >= key.pressed_us ? (double)(now_us - key.pressed_us) / 1e6 : 0.0;
        double step = purp_glide_speed(held) * fmin(dt, held);
        if (!key.held) step = fmax(0.0, fmin(step, PURP_RESIZE_STEP - key.travelled));
        key.travelled += step;
        x += key.dir_x * step;
        y += key.dir_y * step;
        if (key.held || key.travelled < PURP_RESIZE_STEP - 1e-6)
            glide->keys[retained++] = key;
    }
    glide->key_count = retained;
    double whole_x = retained == 0 ? round(x) : trunc(x);
    double whole_y = retained == 0 ? round(y) : trunc(y);
    glide->carry_x = x - whole_x;
    glide->carry_y = y - whole_y;
    return (purp_point_t){ purp_round_i32(whole_x), purp_round_i32(whole_y) };
}
