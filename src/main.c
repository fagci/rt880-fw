#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "ui/graphics.h"

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    st7789_init();
    UI_ClearScreen(C_BLUE);

    PrintfEx(ST7789_WIDTH / 2, ST7789_HEIGHT / 2 - 10, POS_C, C_YELLOW, "Hello, world!");

    while (1)
    {
        __WFI();
    }
}
