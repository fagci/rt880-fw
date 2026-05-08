#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "driver/bk4819.h"
#include "driver/si473x.h"
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

    /* BK4819 #2 */
    BK4819_SelectChip(1);
    BK4819_Init();
    uint16_t id2 = BK4819_ReadRegister(BK4819_REG_00);

    /* Si4732 */
    SI47XX_Init();
    SI47XX_PowerUp();
    uint16_t rev = SI47XX_ReadRegister(CMD_GET_REV);

    PrintfEx(5, 10, POS_L, C_WHITE, "BK1 REG00: 0x%04X", id1);
    PrintfEx(5, 25, POS_L, C_WHITE, "BK2 REG00: 0x%04X", id2);
    PrintfEx(5, 40, POS_L, C_WHITE, "Si4732 REV: 0x%04X", rev);

    if (id1 == 0x0000) PrintfEx(5, 55, POS_L, C_GREEN, "BK1: OK");
    else               PrintfEx(5, 55, POS_L, C_RED,   "BK1: FAIL 0x%04X", id1);

    if (id2 == 0x0000) PrintfEx(5, 70, POS_L, C_GREEN, "BK2: OK");
    else               PrintfEx(5, 70, POS_L, C_RED,   "BK2: FAIL 0x%04X", id2);

    if (rev != 0x0000) PrintfEx(5, 85, POS_L, C_GREEN, "Si4732: OK");
    else               PrintfEx(5, 85, POS_L, C_RED,   "Si4732: NO RESP");

    while (1)
    {
        __WFI();
    }
}
