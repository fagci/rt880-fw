#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "ui/graphics.h"

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    st7789_init();

    /* тест заливки */
    st7789_flush(C_RED);
    rt880_delay_ms(300);
    st7789_flush(C_GREEN);
    rt880_delay_ms(300);
    st7789_flush(C_BLUE);
    rt880_delay_ms(300);
    st7789_flush(C_YELLOW);
    rt880_delay_ms(300);
    st7789_flush(C_CYAN);
    rt880_delay_ms(300);
    st7789_flush(C_MAGENTA);
    rt880_delay_ms(300);
    st7789_flush(C_WHITE);
    rt880_delay_ms(300);
    st7789_flush(C_BLACK);
    rt880_delay_ms(300);

    /* тест геометрии */
    UI_ClearScreen(C_BLUE);

    DrawLine(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1, C_YELLOW);
    DrawLine(ST7789_WIDTH - 1, 0, 0, ST7789_HEIGHT - 1, C_CYAN);

    DrawRect(20, 20, 200, 280, C_RED);
    FillRect(30, 30, 180, 260, C_GREEN);

    DrawRect(50, 80, 140, 160, C_YELLOW);
    FillCircle(ST7789_WIDTH / 2, ST7789_HEIGHT / 2 - 20, 40, C_MAGENTA);

    DrawHLine(0, 300, ST7789_WIDTH, C_WHITE);
    DrawVLine(200, 0, ST7789_HEIGHT, C_WHITE);

    /* тест текста */
    PrintfEx(ST7789_WIDTH / 2, 280, POS_C, C_WHITE, "Hello, world!");
    PrintfEx(ST7789_WIDTH / 2, 290, POS_C, C_YELLOW, "RT880");
    PrintfEx(5, 5, POS_L, C_RED, "x=5 y=5");
    PrintfEx(ST7789_WIDTH - 5, 15, POS_R, C_CYAN, "right");
    PrintfEx(ST7789_WIDTH / 2, 310, POS_C, C_GREEN, "center");

    while (1)
    {
        __WFI();
    }
}
