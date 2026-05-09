#include "at32f423.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "helper/measurements.h"
#include "ui/graphics.h"
#include "ui/spectrum.h"

#define KHZ 100
#define MHZ 100000

#define SPECTRUM_BAR_Y 20
#define SPECTRUM_BAR_H 120
#define WATERFALL_Y (SPECTRUM_BAR_Y + SPECTRUM_BAR_H)

void testScan(void) {
  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);

  uint32_t stp = 25 * KHZ;
  uint32_t s = 172 * MHZ;
  uint32_t e = s + stp * SP_MAX_POINTS;

  FRange range = {.start = s, .end = e};
  SP_Init(&range, stp, stp);

  UI_ClearScreen(C_BLACK);

  uint32_t tick = 0;

  for (;;) {
    uint32_t peakRssi = 0;
    uint32_t peakFreq = s;

    SP_Begin();

    for (uint32_t i = 0; i < SP_MAX_POINTS; i++) {
      uint32_t f = s + i * stp;

      BK4819_TuneTo(f, false);
      rt880_delay_ms(1);

      uint16_t rssi = BK4819_GetRSSI();
      if (rssi > peakRssi) {
        peakRssi = rssi;
        peakFreq = f;
      }

      Loot msm;
      msm.f = f;
      msm.rssi = rssi;
      msm.noise = 0;
      msm.open = false;
      SP_AddPoint(&msm);
    }

    SP_Render(&range, SPECTRUM_BAR_Y, SPECTRUM_BAR_H);

    st7789_fill_rect_dma(0, WATERFALL_Y, ST7789_WIDTH, WF_YN + 1, C_BLACK);
    WF_Render(WATERFALL_Y, tick > 0);

    if (tick > 0) {
      int16_t peakDBm = Rssi2DBm(peakRssi);
      FillRect(120, 0, 120, 16, C_BLACK);
      PrintfEx(ST7789_WIDTH - 1, 14, POS_R, C_BLUE, "%ddBm %u.%03uM", peakDBm,
               peakFreq / MHZ, (peakFreq % MHZ) / KHZ);
    }

    tick++;
  }
}

int main(void) {
  rt880_led_init();
  rt880_delay_init();
  rt880_audio_init();
  st7789_init();

  st7789_backlight_on();
  rt880_audio_path_set(1);
  AF_MUTE_PORT->scr = AF_MUTE_PIN;

  testScan();
}
