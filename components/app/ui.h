#pragma once

// Conversation-state UI on the round LCD. Call ui_init() after bsp_lvgl_init().
// Text is ASCII for now (CJK font is a later polish item).

typedef enum {
    UI_BOOT = 0,
    UI_PROVISION,    // SoftAP web config
    UI_CONNECTING,   // joining WiFi / reconnecting WS
    UI_IDLE,         // online, waiting for wake word
    UI_LISTENING,    // streaming mic
    UI_THINKING,     // server processing / awaiting response
    UI_SPEAKING,     // playing reply
    UI_ERROR,
} ui_state_t;

void ui_init(void);
void ui_set_state(ui_state_t state);     // sets orb color + default state label
void ui_set_status(const char *text);    // bottom status line (e.g. AP/IP details)
