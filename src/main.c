#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "driver/bk4819.h"
#include "ui/graphics.h"

int main(void)
{
    rt880_led_init();
    rt880_delay_init();

    st7789_init();
    UI_ClearScreen(C_BLUE);

    BK4819_SelectChip(0);
    BK4819_Init();

    uint16_t id = BK4819_ReadRegister(BK4819_REG_00);

    UI_ClearScreen(C_BLACK);

    PrintfEx(5, 10, POS_L, C_WHITE, "BK4819 #1 Init");
    PrintfEx(5, 20, POS_L, C_CYAN, "REG00: 0x%04X", id);

    if (id == 0x0000) {
        PrintfEx(5, 35, POS_L, C_GREEN, "ID OK (reset state)");
    } else {
        PrintfEx(5, 35, POS_L, C_YELLOW, "ID: 0x%04X", id);
    }

    uint16_t rssi = BK4819_GetRSSI();
    PrintfEx(5, 50, POS_L, C_RED, "RSSI: %u", rssi);

    while (1)
    {
        __WFI();
    }
}
