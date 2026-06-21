#include "bsp_lvgl.h"
#include "st77916_panel.h"   // panel_handle, EXAMPLE_LCD_*
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#define LCD_W 360
#define LCD_H 360
#define BUF_LINES 10          // partial buffer height (px) -> W*10 per buffer
                              // Kept small so both buffers fit in internal DMA RAM
                              // (see bsp_lvgl_init): SPI then DMAs straight from
                              // them with no per-transfer bounce buffer, which was
                              // exhausting internal RAM during streaming.
#define TICK_MS 2

static const char *TAG = "bsp_lvgl";
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_flush_done;   /* given from SPI ISR on flush completion */
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

/* Fired from the SPI ISR when a flush transaction finishes. */
static bool color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hpw);
    return hpw == pdTRUE;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
    /* Block (yielding the CPU) until the DMA finishes, so LVGL never reuses the
       buffer mid-transfer. Timeout guards against a dropped completion so a lost
       transaction can't deadlock the LVGL task. */
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100));
    lv_disp_flush_ready(drv);
}

static void tick_cb(void *arg) { lv_tick_inc(TICK_MS); }

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        uint32_t delay = lv_timer_handler();
        bsp_lvgl_unlock();
        if (delay > 20) delay = 20;
        if (delay < 2) delay = 2;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

esp_err_t bsp_lvgl_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) return ESP_ERR_NO_MEM;

    lv_init();

    /* DMA-capable internal RAM so the SPI LCD transmits directly from these
       buffers. PSRAM buffers would force spi_master to allocate a same-sized
       internal bounce buffer per transfer, which fails under memory pressure
       during audio streaming ("Failed to allocate priv TX buffer"). */
    size_t px = LCD_W * BUF_LINES;
    lv_color_t *b1 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *b2 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_disp_draw_buf_init(&s_draw_buf, b1, b2, px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_W;
    s_disp_drv.ver_res = LCD_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    /* Get flush-completion callbacks from the panel IO so LVGL is back-pressured
       by actual SPI completion instead of free-running. */
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = color_trans_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(lcd_io_handle, &io_cbs, &s_disp_drv));

    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lvgl_tick" };
    esp_timer_handle_t th = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &th));
    ESP_ERROR_CHECK(esp_timer_start_periodic(th, TICK_MS * 1000));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6144, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "LVGL ready");
    return ESP_OK;
}
