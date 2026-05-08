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
    UI_ClearScreen(C_BLACK);

    /* BK4819 #1 */
    BK4819_SelectChip(0);
    BK4819_Init();
    uint16_t id1 = BK4819_ReadRegister(BK4819_REG_00);
    uint16_t rssi1 = BK4819_GetRSSI();

    PrintfEx(5, 10, POS_L, C_WHITE, "BK1 REG00: 0x%04X RSSI:%u", id1, rssi1);

    /* BK4819 #2 */
    BK4819_SelectChip(1);
    BK4819_Init();
    uint16_t id2 = BK4819_ReadRegister(BK4819_REG_00);
    uint16_t rssi2 = BK4819_GetRSSI();

    PrintfEx(5, 25, POS_L, C_WHITE, "BK2 REG00: 0x%04X RSSI:%u", id2, rssi2);

    if (id1 == 0x0000)
        PrintfEx(5, 40, POS_L, C_GREEN, "BK1: OK");
    else
        PrintfEx(5, 40, POS_L, C_GREEN, "BK1: 0x%04X", id1);

    if (id2 == 0x0000)
        PrintfEx(5, 55, POS_L, C_GREEN, "BK2: OK");
    else
        PrintfEx(5, 55, POS_L, C_GREEN, "BK2: 0x%04X", id2);

    while (1)
    {
        __WFI();
    }
}
