#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "ui/graphics.h"

static void color_swatch(uint16_t x, uint16_t y, uint16_t w, uint16_t h, Color c, const char *label)
{
    FillRect(x, y, w, h, c);
    DrawRect(x, y, w, h, C_WHITE);
    PrintfEx(x + w / 2, y + h / 2 - 2, POS_C, C_WHITE, label);
}

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    st7789_init();
    UI_ClearScreen(C_BLACK);

    /* 2 ряда цветов по 4 штуки, 60x40 каждый */
    int sw = 55, sh = 40, gap = 5, x0 = 5, y0 = 10;

    color_swatch(x0, y0, sw, sh, C_RED,     "RED");
    color_swatch(x0 + sw + gap, y0, sw, sh, C_GREEN,   "GREEN");
    color_swatch(x0 + (sw + gap) * 2, y0, sw, sh, C_BLUE,    "BLUE");
    color_swatch(x0 + (sw + gap) * 3, y0, sw, sh, C_YELLOW,  "YELLOW");

    color_swatch(x0, y0 + sh + gap, sw, sh, C_CYAN,    "CYAN");
    color_swatch(x0 + sw + gap, y0 + sh + gap, sw, sh, C_MAGENTA, "MAGENTA");
    color_swatch(x0 + (sw + gap) * 2, y0 + sh + gap, sw, sh, C_WHITE,   "WHITE");
    color_swatch(x0 + (sw + gap) * 3, y0 + sh + gap, sw, sh, C_GRAY,    "GRAY");

    /* подписи RGB565 ожидаемые */
    PrintfEx(ST7789_WIDTH / 2, y0 + (sh + gap) * 2 + 10, POS_C, C_WHITE, "RGB565: R=0xF800 G=0x07E0 B=0x001F");
    PrintfEx(ST7789_WIDTH / 2, y0 + (sh + gap) * 2 + 20, POS_C, C_WHITE, "Y=0xFFE0 C=0x07FF M=0xF81F W=0xFFFF");

    /* под экраном тест оттенков серого */
    int bar_w = 28, bar_gap = 3;
    for (int i = 0; i < 8; i++) {
        uint16_t gray = (i * 36) << 5 | (i * 36) << 10 | (i * 36);
        FillRect(x0 + i * (bar_w + bar_gap), 160, bar_w, 30, gray);
        DrawRect(x0 + i * (bar_w + bar_gap), 160, bar_w, 30, C_WHITE);
    }

    /* тест RGB565 констант */
    PrintfEx(5, 200, POS_L, RGB565(255, 0, 0), "RGB(255,0,0)");
    PrintfEx(5, 210, POS_L, RGB565(0, 255, 0), "RGB(0,255,0)");
    PrintfEx(5, 220, POS_L, RGB565(0, 0, 255), "RGB(0,0,255)");
    PrintfEx(5, 230, POS_L, RGB565(128, 128, 128), "RGB(128,128,128)");
    PrintfEx(5, 240, POS_L, RGB565(255, 255, 0), "RGB(255,255,0)");
    PrintfEx(5, 250, POS_L, RGB565(0, 255, 255), "RGB(0,255,255)");
    PrintfEx(5, 260, POS_L, RGB565(255, 0, 255), "RGB(255,0,255)");

    while (1)
    {
        __WFI();
    }
}
