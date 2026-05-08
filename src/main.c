#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    st7789_init();

    st7789_flush(0xF800); /* красный */

    while (1)
    {
        __WFI();
    }
}
