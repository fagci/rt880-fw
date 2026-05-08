#include "at32f423.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "ui/graphics.h"

static void setup_bk(uint8_t chip, uint32_t freq) {
  BK4819_SelectChip(chip);
  BK4819_Init();
  BK4819_SetModulation(MOD_FM);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_SetAGC(true, 0);
  BK4819_TuneTo(freq, true);
  BK4819_RX_TurnOn();
  BK4819_ToggleAFDAC(true);
  BK4819_ToggleAFBit(true);
}

int main(void) {
  rt880_led_init();
  rt880_delay_init();
  rt880_audio_init();

  st7789_init();
  UI_ClearScreen(C_BLACK);

  // AUDIO_PWR_PORT->scr = AUDIO_PWR_PIN;
  // FM_PWR_PORT->scr = FM_PWR_PIN;

  setup_bk(0, 17230000);

  rt880_audio_path_set(1);
  AF_MUTE_PORT->scr = AF_MUTE_PIN;

  uint8_t y = 0;
  PrintfEx(5, (y += 12), POS_L, C_WHITE, "BK1 SPICFG");

  uint16_t chipid = BK4819_ReadRegister(0);
  uint16_t reg36 = BK4819_ReadRegister(0x36);
  uint16_t reg30 = BK4819_ReadRegister(0x30);
  uint16_t reg31 = BK4819_ReadRegister(0x31);
  uint16_t reg33 = BK4819_ReadRegister(0x33);
  uint16_t reg47 = BK4819_ReadRegister(0x47);
  uint16_t rssi = BK4819_GetRSSI();
  uint16_t snr = BK4819_GetSNR();

  PrintfEx(5, (y += 12), POS_L, C_GREEN, "CHIP:0x%04X", chipid);
  PrintfEx(5, (y += 12), POS_L, C_WHITE, "REG30:0x%04X REG31:0x%04X", reg30,
           reg31);
  PrintfEx(5, (y += 12), POS_L, C_WHITE, "REG33:0x%04X REG36:0x%04X", reg33,
           reg36);
  PrintfEx(5, (y += 12), POS_L, C_WHITE, "REG47:0x%04X", reg47);
  PrintfEx(5, (y += 12), POS_L, C_YELLOW, "RSSI:%u SNR:%u", rssi, snr);

  PrintfEx(5, (y += 16), POS_L, C_WHITE, "GPIO STATE:");
  PrintfEx(5, (y += 12), POS_L, C_CYAN, "PC4(AUD):%d PC5(FM):%d",
           (GPIOC->odt >> 4) & 1, (GPIOC->odt >> 5) & 1);
  PrintfEx(5, (y += 12), POS_L, C_CYAN, "PF8(MUT):%d PC3(ANC):%d",
           (GPIOF->odt >> 8) & 1, (GPIOC->odt >> 3) & 1);

  while (1) {
    rt880_delay_ms(500);
  }
}
