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

    BK4819_SelectChip(0);
    BK4819_Init();

    uint16_t id0 = BK4819_ReadRegister(0);
    uint16_t id1 = BK4819_ReadRegister(1);

    BK4819_WriteRegister(0x02, 0x5AA5);
    uint16_t rb02 = BK4819_ReadRegister(0x02);

    uint32_t wanted = 17230000;
    BK4819_WriteRegister(0x38, wanted & 0xFFFF);
    BK4819_WriteRegister(0x39, (wanted >> 16) & 0xFFFF);

    uint32_t rbl = BK4819_ReadRegister(0x38);
    uint32_t rbh = BK4819_ReadRegister(0x39);
    uint32_t rb_freq = (rbh << 16) | rbl;

    uint16_t rb38b = BK4819_ReadRegister(0x38);
    uint16_t rb39b = BK4819_ReadRegister(0x39);

    PrintfEx(5, 10, POS_L, C_WHITE, "REG00: 0x%04X  REG01: 0x%04X", id0, id1);
    PrintfEx(5, 25, POS_L, C_WHITE, "Wrote 0x5AA5 to REG02");
    PrintfEx(5, 35, POS_L, C_YELLOW, "Read REG02: 0x%04X", rb02);

    PrintfEx(5, 50, POS_L, C_CYAN, "Wrote 17230000 to 38/39");
    PrintfEx(5, 60, POS_L, C_CYAN, "Read  38:0x%04X 39:0x%04X", rb38b, rb39b);
    PrintfEx(5, 70, POS_L, C_GREEN, "Readback: %u", rb_freq);

    while (1)
    {
        rt880_delay_ms(1000);
    }
}
