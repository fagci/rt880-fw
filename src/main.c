#include "at32f423.h"
#include "at32f423_flash.h"
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

// HEXT = 8 MHz → PLL → SCLK = 150 MHz
// Формула: SCLK = (HEXT / pllms) * pllns / pllfr / 2
//          150  = (8    /  1    ) *  150  /   4   / 2  ✓

void system_clock_config(void) {
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  /* Пробуем запустить HEXT с таймаутом */
  CRM->ctrl_bit.hexten = TRUE;
  uint32_t timeout = 50000;
  while (CRM->ctrl_bit.hextstbl == 0 && --timeout)
    ;

  if (timeout == 0) {
    /* HEXT не завёлся — работаем на HICK 8 МГц, моргаем быстро */
    /* Сюда попадём если нет кварца или он не стартует           */
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return; // SystemCoreClock останется HICK = 8 МГц
  }

  /* HEXT завёлся — настраиваем PLL */
  CRM->pllcfg_bit.pllrcs = 1;
  CRM->pllcfg_bit.pllms = 1;
  CRM->pllcfg_bit.pllns = 150;
  CRM->pllcfg_bit.pllfr = 2;

  CRM->ctrl_bit.pllen = TRUE;
  timeout = 50000;
  while (CRM->ctrl_bit.pllstbl == 0 && --timeout)
    ;

  if (timeout == 0) {
    /* PLL не запустился — остаёмся на HEXT 8 МГц */
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    return;
  }

  CRM->cfg_bit.ahbdiv = 0;
  CRM->cfg_bit.apb1div = 4;
  CRM->cfg_bit.apb2div = 0;

  CRM->cfg_bit.sclksel = CRM_SCLK_PLL;
  timeout = 50000;
  while (CRM->cfg_bit.sclksts != CRM_SCLK_PLL && --timeout)
    ;

  system_core_clock_update();
}

typedef struct {
  uint32_t frame_count;
  uint32_t last_ms;
  uint32_t fps; // последнее измеренное значение
} FpsCounter;

static inline void fps_init(FpsCounter *f) {
  f->frame_count = 0;
  f->last_ms = 0;
  f->fps = 0;
}

// Вызывать в конце каждого кадра. Возвращает новое fps если прошла секунда,
// иначе 0.
static inline uint32_t fps_tick(FpsCounter *f, uint32_t now_ms) {
  f->frame_count++;
  uint32_t elapsed = now_ms - f->last_ms;
  if (elapsed >= 1000) {
    f->fps = (f->frame_count * 1000) / elapsed;
    f->frame_count = 0;
    f->last_ms = now_ms;
    return f->fps;
  }
  return 0;
}
const uint32_t stp = 25 * KHZ;
const uint32_t s = 172 * MHZ;
const uint32_t e = s + stp * LCD_WIDTH / 2;

FRange range = {.start = s, .end = e};

// main.c

void testScan(void) {
  BK4819_SelectChip(0);
  BK4819_Init();
  BK4819_RX_TurnOn();
  BK4819_SetAFC(0);
  BK4819_SetAGC(true, 1);
  BK4819_SetFilterBandwidth(BK4819_FILTER_BW_12k);
  /* BK4819_ToggleAFBit(false);
  BK4819_ToggleAFDAC(false); */

  SP_Init(&range, stp, stp);
  UI_ClearScreen(C_BLACK);

  FpsCounter fps; // ← перенести сюда из main
  fps_init(&fps);

  uint32_t tick = 0;

  for (;;) {
    uint32_t peakRssi = 0;
    uint32_t peakFreq = s;

    SP_Begin();

    for (uint32_t f = s; f < e; f += stp) {
      uint16_t rssi = BK4819_TuneToAndWaitRSSI(f);
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
    WF_Render(WATERFALL_Y, tick > 0);

    static int16_t last_dbm = 0;
    static uint32_t last_freq = 0;
    static uint32_t last_fps = 0;

    fps_tick(&fps, rt880_dwt_ms());

    int16_t peakDBm = Rssi2DBm(peakRssi);
    uint32_t cur_fps = fps.fps;

    // Перерисовываем строку только если что-то изменилось
    if (peakDBm != last_dbm || peakFreq != last_freq || cur_fps != last_fps) {
      PrintfEx(ST7789_WIDTH - 1, 22, POS_L, C_BLUE, C_BLACK, "%ddBm %u.%03uM",
               peakDBm, peakFreq / MHZ, (peakFreq % MHZ) / KHZ);
      PrintfEx(ST7789_WIDTH - 1, 22, POS_R, C_BLUE, C_BLACK, "%ufps", cur_fps);
      last_dbm = peakDBm;
      last_freq = peakFreq;
      last_fps = cur_fps;
    }

    tick++;
  }
}

int main(void) {
  system_clock_config();
  rt880_led_init();
  rt880_delay_init();
  rt880_dwt_init();
  rt880_audio_init();
  st7789_init();
  st7789_backlight_on();
  AF_MUTE_PORT->scr = AF_MUTE_PIN;

  // FM_PWR_PORT->scr = FM_PWR_PIN;

  testScan(); // не возвращается
}
