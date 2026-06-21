#include "ui.h"
#include "bsp_lvgl.h"
#include "lvgl.h"

static lv_obj_t *s_orb;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_status_lbl;

static lv_color_t color_for(ui_state_t st)
{
    switch (st) {
        case UI_PROVISION:  return lv_color_hex(0xE08200); // orange
        case UI_CONNECTING: return lv_color_hex(0xC0A000); // amber-yellow
        case UI_IDLE:       return lv_color_hex(0x1E7060); // teal
        case UI_LISTENING:  return lv_color_hex(0x1565C0); // blue
        case UI_THINKING:   return lv_color_hex(0xB06000); // amber
        case UI_SPEAKING:   return lv_color_hex(0x2E9E4F); // green
        case UI_ERROR:      return lv_color_hex(0xC02020); // red
        case UI_BOOT:
        default:            return lv_color_hex(0x444444); // gray
    }
}

static const char *label_for(ui_state_t st)
{
    switch (st) {
        case UI_PROVISION:  return "WiFi Setup";
        case UI_CONNECTING: return "Connecting";
        case UI_IDLE:       return "Idle";
        case UI_LISTENING:  return "Listening";
        case UI_THINKING:   return "Thinking";
        case UI_SPEAKING:   return "Speaking";
        case UI_ERROR:      return "Error";
        case UI_BOOT:
        default:            return "Starting";
    }
}

static void orb_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *) obj, (lv_opa_t) v, 0);
}

void ui_init(void)
{
    bsp_lvgl_lock();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "XiaoPuTao");
    lv_obj_set_style_text_color(title, lv_color_hex(0x707070), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    s_orb = lv_obj_create(scr);
    lv_obj_remove_style_all(s_orb);
    lv_obj_set_size(s_orb, 150, 150);
    lv_obj_align(s_orb, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(s_orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_orb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_orb, color_for(UI_BOOT), 0);
    lv_obj_clear_flag(s_orb, LV_OBJ_FLAG_SCROLLABLE);

    s_state_lbl = lv_label_create(scr);
    lv_label_set_text(s_state_lbl, label_for(UI_BOOT));
    lv_obj_set_style_text_color(s_state_lbl, lv_color_white(), 0);
    lv_obj_align_to(s_state_lbl, s_orb, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    s_status_lbl = lv_label_create(scr);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status_lbl, lv_pct(82));
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x9A9A9A), 0);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -40);

    // Breathing pulse on the orb opacity (always running; color encodes state).
    static lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, s_orb);
    lv_anim_set_exec_cb(&pulse, orb_opa_cb);
    lv_anim_set_values(&pulse, 110, 255);
    lv_anim_set_time(&pulse, 1000);
    lv_anim_set_playback_time(&pulse, 1000);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&pulse);

    bsp_lvgl_unlock();
}

void ui_set_state(ui_state_t st)
{
    if (!s_orb) return;
    bsp_lvgl_lock();
    lv_obj_set_style_bg_color(s_orb, color_for(st), 0);
    lv_label_set_text(s_state_lbl, label_for(st));
    bsp_lvgl_unlock();
}

void ui_set_status(const char *text)
{
    if (!s_status_lbl) return;
    bsp_lvgl_lock();
    lv_label_set_text(s_status_lbl, text ? text : "");
    bsp_lvgl_unlock();
}
