#include "clrGen.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "clrGen";

/* Adafruit 3.5-inch 480x320 HX8357D FeatherWing wiring.
 * ESP32-S3 GPIO mapping provided by the PCB connections.
 */
#define CLRGEN_TFT_HOST  SPI2_HOST
#define CLRGEN_TFT_SCK   ((gpio_num_t)36)
#define CLRGEN_TFT_MOSI  ((gpio_num_t)35)
#define CLRGEN_TFT_MISO  GPIO_NUM_NC
#define CLRGEN_TFT_CS    ((gpio_num_t)9)
#define CLRGEN_TFT_DC    ((gpio_num_t)10)
#define CLRGEN_TFT_RST   ((gpio_num_t)6)
#define CLRGEN_TFT_BL    ((gpio_num_t)5)

#define CLRGEN_WIDTH     480
#define CLRGEN_HEIGHT    320

#define HX8357_SWRESET   0x01
#define HX8357_SLPOUT    0x11
#define HX8357_INVON     0x21
#define HX8357_DISPON    0x29
#define HX8357_CASET     0x2A
#define HX8357_PASET     0x2B
#define HX8357_RAMWR     0x2C
#define HX8357_MADCTL    0x36
#define HX8357_COLMOD    0x3A

static spi_device_handle_t s_tft;

static void write_cmd(uint8_t cmd)
{
    gpio_set_level(CLRGEN_TFT_DC, 0);
    spi_transaction_t tx = { .length = 8, .tx_buffer = &cmd };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_tft, &tx));
}

static void write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(CLRGEN_TFT_DC, 1);
    spi_transaction_t tx = { .length = len * 8, .tx_buffer = data };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_tft, &tx));
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t pa[] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    write_cmd(HX8357_CASET); write_data(ca, sizeof(ca));
    write_cmd(HX8357_PASET); write_data(pa, sizeof(pa));
    write_cmd(HX8357_RAMWR);
}

static void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    set_window(x, y, x + w - 1, y + h - 1);

    uint8_t line[CLRGEN_WIDTH * 2];
    for (uint16_t i = 0; i < w; ++i) {
        line[i * 2] = (uint8_t)(color >> 8);
        line[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    gpio_set_level(CLRGEN_TFT_DC, 1);
    for (uint16_t row = 0; row < h; ++row) {
        spi_transaction_t tx = { .length = w * 16, .tx_buffer = line };
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_tft, &tx));
    }
}

static void tft_init(void)
{
    if (CLRGEN_TFT_BL >= 0) {
        gpio_reset_pin(CLRGEN_TFT_BL);
        gpio_set_direction(CLRGEN_TFT_BL, GPIO_MODE_OUTPUT);
        gpio_set_level(CLRGEN_TFT_BL, 1);
    }

    if (CLRGEN_TFT_RST >= 0) {
        gpio_reset_pin(CLRGEN_TFT_RST);
        gpio_set_direction(CLRGEN_TFT_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(CLRGEN_TFT_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CLRGEN_TFT_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    gpio_reset_pin(CLRGEN_TFT_DC);
    gpio_set_direction(CLRGEN_TFT_DC, GPIO_MODE_OUTPUT);

    spi_bus_config_t bus = {
        .mosi_io_num = CLRGEN_TFT_MOSI,
        .miso_io_num = CLRGEN_TFT_MISO,
        .sclk_io_num = CLRGEN_TFT_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CLRGEN_TFT_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = CLRGEN_TFT_CS,
        .queue_size = 4,
        .cs_ena_posttrans = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CLRGEN_TFT_HOST, &dev, &s_tft));

    ESP_LOGI(TAG, "TFT pins: SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
             (int)CLRGEN_TFT_SCK, (int)CLRGEN_TFT_MOSI,
             (int)CLRGEN_TFT_CS, (int)CLRGEN_TFT_DC,
             (int)CLRGEN_TFT_RST, (int)CLRGEN_TFT_BL);

    write_cmd(HX8357_SWRESET); vTaskDelay(pdMS_TO_TICKS(50));
    write_cmd(HX8357_SLPOUT); vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t madctl = 0xE0;
    write_cmd(HX8357_MADCTL); write_data(&madctl, 1);
    uint8_t colmod = 0x55;
    write_cmd(HX8357_COLMOD); write_data(&colmod, 1);
    write_cmd(HX8357_INVON);
    write_cmd(HX8357_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static uint8_t font_char(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a');
    if (c >= '0' && c <= '9') return (uint8_t)(26 + c - '0');
    if (c == '-') return 36;
    return 37;
}

static const uint8_t font_full[][5] = {
    {0x7C,0x12,0x11,0x12,0x7C}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x51,0x51,0x72}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x22,0x41,0x49,0x49,0x36}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x08,0x08,0x08,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00}  // fallback
};

static void draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    uint8_t i = font_char(c);
    if (i >= sizeof(font_full) / sizeof(font_full[0])) i = 37;
    for (uint8_t col = 0; col < 5; ++col) {
        for (uint8_t row = 0; row < 7; ++row) {
            bool on = (font_full[i][col] & (1u << row)) != 0;
            tft_fill_rect(x + col * scale, y + row * scale, scale, scale, on ? fg : bg);
        }
    }
}

static void draw_text(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*text) {
        draw_char(x, y, *text, fg, bg, scale);
        x += 6 * scale;
        text++;
    }
}

static const char *color_name(unsigned int value)
{
    switch (value) {
        case 0: return "BLACK";
        case 1: return "BROWN";
        case 2: return "RED";
        case 3: return "ORANGE";
        case 4: return "YELLOW";
        case 5: return "GREEN";
        case 6: return "BLUE";
        case 7: return "VIOLET";
        case 8: return "GRAY";
        case 9: return "WHITE";
        default: return "";
    }
}

static uint16_t color_rgb565(unsigned int value)
{
    switch (value) {
        case 0: return 0xFFFF; // black
        case 1: return 0x8A01; // brown
        case 2: return 0xFFE0; // red
        case 3: return 0xFC00; // orange
        case 4: return 0xF800; // yellow
        case 5: return 0xA81F; // green
        case 6: return 0x020C; // blue
        case 7: return 0x07E0; // violet
        case 8: return 0x8410; // gray
        case 9: return 0x0000; // white
        default: return 0x0000;
    }
}

static uint16_t text_color(int value)
{
    if (value == 0) {
        return 0x0000;
    }
    if (value == 9) {
        return 0xFFFF;
    }
    if (value == 1 || value == 6 || value == 7) {
        return 0xFFFF;
    }
    return 0x0000;
}

void clrGen_init(void)
{
    tft_init();

    unsigned int values[3];
    for (int i = 0; i < 3; ++i) {
        values[i] = (unsigned int)(esp_random() % 10);
    }

    ESP_LOGI(TAG, "Generated resistor bands: %u, %u, %u", values[0], values[1], values[2]);

    const uint16_t third = CLRGEN_WIDTH / 3;
    for (int i = 0; i < 3; ++i) {
        uint16_t x = i * third;
        uint16_t color = color_rgb565(values[i]);
        uint16_t fg = text_color(values[i]);
        tft_fill_rect(x, 0, third, CLRGEN_HEIGHT, color);

        char number[8];
        snprintf(number, sizeof(number), "%u", values[i]);
        draw_text(x + 48, 120, number, fg, color, 5);
        draw_text(x + 18, 190, color_name(values[i]), fg, color, 3);
    }
}
