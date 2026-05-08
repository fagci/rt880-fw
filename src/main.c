#include "at32f423.h"
#include "driver/board.h"

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    while (1)
    {
        rt880_led_toggle();
        rt880_delay_ms(500);
    }
}
