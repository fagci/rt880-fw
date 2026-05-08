#include "at32f423.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "driver/bk4819.h"
#include "ui/graphics.h"

static void setup_bk(uint8_t chip, uint32_t freq)
{
    BK4819_SelectChip(chip);
    BK4819_Init();
    BK4819_SetModulation(MOD_FM);
    BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
    BK4819_SetAGC(true, 0);
    BK4819_TuneTo(freq, true);
    BK4819_RX_TurnOn();
    BK4819_ToggleAFDAC(true);
    BK4819_ToggleAFBit(true);
    BK4819_WriteRegister(0x31, BK4819_ReadRegister(0x31) | 3);
}

int main(void)
{
    rt880_led_init();
    rt880_delay_init();
    rt880_audio_init();

    st7789_init();
    UI_ClearScreen(C_BLACK);

    setup_bk(0, 17230000);
    setup_bk(1, 44600000);

    rt880_audio_path_set(0);
    AF_MUTE_PORT->scr = AF_MUTE_PIN;
    AUDIO_PWR_PORT->scr = AUDIO_PWR_PIN;
    FM_PWR_PORT->scr = FM_PWR_PIN;

    PrintfEx(5, 10, POS_L, C_WHITE, "BK1: 172.300 MHz");
    PrintfEx(5, 25, POS_L, C_WHITE, "BK2: 446.000 MHz");
    PrintfEx(5, 40, POS_L, C_CYAN, "Audio: BK1");

    while (1)
    {
        BK4819_SelectChip(0);
        uint16_t r1 = BK4819_GetRSSI();
        uint16_t s1 = BK4819_GetSNR();

        BK4819_SelectChip(1);
        uint16_t r2 = BK4819_GetRSSI();
        uint16_t s2 = BK4819_GetSNR();

        PrintfEx(230, 10, POS_R, C_YELLOW, "RSSI:%3u SNR:%3u", r1, s1);
        PrintfEx(230, 25, POS_R, C_YELLOW, "RSSI:%3u SNR:%3u", r2, s2);

        uint16_t bar1 = (r1 * 150) / 255;
        if (bar1 > 150) bar1 = 150;
        if (r1 > 5) FillRect(5, 55, bar1, 6, C_GREEN);

        uint16_t bar2 = (r2 * 150) / 255;
        if (bar2 > 150) bar2 = 150;
        if (r2 > 5) FillRect(5, 70, bar2, 6, C_GREEN);

        rt880_delay_ms(100);
    }
}
