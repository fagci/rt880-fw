#include "at32f423.h"
#include "driver/bk4819.h"
#include "driver/board.h"
#include "driver/st7789.h"
#include "ui/graphics.h"

static void setup_bk(uint8_t chip, uint32_t freq) {
  BK4819_SelectChip(chip);
  BK4819_Init();
  BK4819_RX_TurnOn();

  BK4819_SetAFC(0);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  BK4819_TuneTo(freq, true);
}

void Spectrum_DrawBars(int x0, int y_bottom, int bar_w, int gap, int count,
                       const uint8_t *curr, uint8_t *prev, Color fg, Color bg) {
  for (int i = 0; i < count; i++) {
    int x = x0 + i * (bar_w + gap);
    int old_h = prev[i];
    int new_h = curr[i];

    if (new_h > old_h) {
      FillRect(x, y_bottom - new_h, bar_w, new_h - old_h, fg);
    } else if (new_h < old_h) {
      FillRect(x, y_bottom - old_h, bar_w, old_h - new_h, bg);
    }

    prev[i] = new_h;
  }
}

#define KHZ 100
#define MHZ 100000

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define START_Y 16
#define END_Y LCD_HEIGHT / 4

void testScan(void) {
  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();

  BK4819_SetAFC(0);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);

  uint32_t stp = 25 * KHZ;
  uint32_t s = 172 * MHZ;
  uint32_t e = s + stp * LCD_WIDTH;

  uint16_t v[LCD_WIDTH];
  uint16_t peakRssi;
  uint8_t peakX;

  static const int8_t kGrid[] = {-60, -80, -100};

  UI_ClearScreen(C_BLACK);

  for (uint8_t g = 0; g < ARRAY_SIZE(kGrid); g++) {
    int16_t gy = ConvertDomain(kGrid[g], -140, -50, END_Y, START_Y);
    Printf(0, gy, "%d", kGrid[g]);
  }

  for (;;) {
    peakRssi = 0;
    peakX = 0;

    uint8_t i = 0;
    for (uint32_t f = s; f < e; f += stp) {
      BK4819_TuneTo(f, false);
      rt880_delay_ms(2);
      v[i] = BK4819_GetRSSI();
      if (v[i] > peakRssi) {
        peakRssi = v[i];
        peakX = i;
      }
      i++;
    }

    FillRect(0, 0, LCD_WIDTH, END_Y, C_BLACK);
    for (uint8_t g = 0; g < ARRAY_SIZE(kGrid); g++) {
      int16_t gy = ConvertDomain(kGrid[g], -140, -50, END_Y, START_Y);
      Printf(0, gy, "%d", kGrid[g]);
    }

    for (uint8_t x = 0; x < LCD_WIDTH; x++) {
      int16_t dBm = Rssi2DBm(v[x]);
      uint32_t y = ConvertDomain(dBm, -140, -50, END_Y, START_Y);
      DrawVLine(x, y, END_Y - y, C_YELLOW);
    }

    FillRect(120, 0, 120, 16, C_BLACK);
    int16_t peakDBm = Rssi2DBm(peakRssi);
    uint32_t peakFreq = s + (uint32_t)peakX * stp;
    PrintfEx(LCD_WIDTH - 1, 14, POS_R, C_BLUE, "%ddBm %u.%03uM", peakDBm,
             peakFreq / MHZ, (peakFreq % MHZ) / KHZ);
    setup_bk(1, peakFreq);
    BK4819_SelectChip(0);
  }
}

int main(void) {
  rt880_led_init();
  rt880_delay_init();
  rt880_audio_init();
  st7789_init();

  UI_ClearScreen(C_BLACK);
  st7789_backlight_on();

  // AUDIO_PWR_PORT->scr = AUDIO_PWR_PIN;
  // FM_PWR_PORT->scr = FM_PWR_PIN;

  // setup_bk(0, 17230000);

  rt880_audio_path_set(1);
  AF_MUTE_PORT->scr = AF_MUTE_PIN;

  // rt880_ant_sw(true);

  testScan();
}
