/* Заглушки железа для хост-превью UI: дисплей ST7789 как буфер кадра (с
 * аппаратным скроллом), BK4819/SI4732/плата — имитация сигнала. */
#include "at32f423.h"
#include "driver/bk4819.h"
#include "driver/si473x.h"
#include "driver/st7789.h"
#include "radio.h"
#include "modes/mode.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

gpio_type g_fake_gpio[8];
uint32_t SystemCoreClock = 150000000;
uint32_t _sheap, _eheap;

/* ── время ── */
uint32_t g_now_ms = 100000;
extern "C" uint32_t millis(void) { return g_now_ms; }
extern "C" void delay_us(uint32_t) {}
extern "C" void delay_ms(uint32_t) {}
volatile uint32_t tick_ms;
uint16_t g_bat_cv = 372;
extern "C" uint16_t battery_voltage_cv(void) { return g_bat_cv; }
extern "C" void audio_path_set(uint8_t) {}
bool g_led_rx;
void led_rx_set(bool on) { g_led_rx = on; }

/* ── дисплей: память кадра 240x320 + аппаратный скролл ── */
static uint16_t gram[ST7789_HEIGHT][ST7789_WIDTH];
static uint16_t win_x, win_y, win_w, win_h, cur_x, cur_y;
static uint16_t sc_tfa = 0, sc_vsa = ST7789_HEIGHT, sc_bfa = 0, sc_vsp = 0;
static int pix_half = -1; /* для побайтового потока: старший байт ждёт младший */
static uint8_t pix_hi;

static void put_px(uint16_t c) {
  if (cur_y < ST7789_HEIGHT && cur_x < ST7789_WIDTH)
    gram[cur_y][cur_x] = c;
  if (++cur_x >= win_x + win_w) {
    cur_x = win_x;
    cur_y++;
    if (cur_y >= win_y + win_h)
      cur_y = win_y;
  }
}
extern "C" {
void st7789_set_addr_window_raw(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  win_x = cur_x = x; win_y = cur_y = y; win_w = w; win_h = h;
}
void st7789_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  st7789_set_addr_window_raw(x, y, w, h);
}
void st7789_start_pixels(void) {}
void st7789_write_data16(uint16_t d) { put_px(d); }
void st7789_cs_low(void) {}
void st7789_cs_high(void) {}
void st7789_dma_start(const uint8_t *buf, uint16_t n) { /* RGB565 big-endian поток */
  for (uint16_t i = 0; i + 1 < n; i += 2)
    put_px((uint16_t)(buf[i] << 8 | buf[i + 1]));
}
void st7789_dma_wait(void) {}
void st7789_write_pixels_dma(const uint16_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    put_px(p[i]);
}
void st7789_fill_rect_dma(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) {
  for (uint16_t j = y; j < y + h && j < ST7789_HEIGHT; j++)
    for (uint16_t i = x; i < x + w && i < ST7789_WIDTH; i++)
      gram[j][i] = c;
}
void st7789_flush(uint16_t c) { st7789_fill_rect_dma(0, 0, ST7789_WIDTH, ST7789_HEIGHT, c); }
void st7789_set_vscroll_area(uint16_t t, uint16_t v, uint16_t b) { sc_tfa = t; sc_vsa = v; sc_bfa = b; }
void st7789_set_vscroll_start(uint16_t s) { sc_vsp = s; }
void st7789_scroll_reset(void) { sc_tfa = 0; sc_vsa = ST7789_HEIGHT; sc_bfa = 0; sc_vsp = 0; }
}

/* то, что видит человек: учёт скролла */
uint16_t host_display_px(int x, int y) {
  int row = y;
  if (y >= sc_tfa && y < sc_tfa + sc_vsa) {
    int off = ((int)sc_vsp - sc_tfa + (y - sc_tfa)) % sc_vsa;
    row = sc_tfa + off;
  }
  return gram[row][x];
}

/* ── имитация приёмника ── */
static uint32_t g_tuneF = 14550000;
static bool g_sql;
static uint32_t g_noiseSeed = 12345;
static uint32_t rnd() { g_noiseSeed = g_noiseSeed * 1664525u + 1013904223u; return g_noiseSeed >> 8; }
/* «эфир»: шумовой пол ~ -105 дБм + несколько сигналов (ед. 10 Гц) */
uint16_t host_signal_rssi(uint32_t f) {
  static const struct { uint32_t f; int dbm; } SIG[] = {
      {14550000, -62}, {14800000, -90}, {43312500, -58}, {43350000, -85},
      {43400000, -75}, {16309300, -70}, {16200000, -88}, {17025000, -80}};
  int dbm = -108 + (int)(rnd() % 6);
  for (auto &s : SIG) {
    int d = (int)(f > s.f ? f - s.f : s.f - f);
    if (d < 4000) { int v = s.dbm - d / 140; if (v > dbm) dbm = v; }
  }
  return (uint16_t)((dbm + 160) * 2);
}
extern "C++" {
void BK4819_Init(void) {}
void BK4819_SelectChip(uint8_t) {}
void BK4819_RX_TurnOn(void) {}
void BK4819_SetAFC(uint8_t) {}
void BK4819_SetAGC(bool, uint8_t) {}
void BK4819_ToggleAFDAC(bool) {}
void BK4819_ToggleFilter(Filter, bool) {}
void BK4819_SetModulation(ModulationType) {}
void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t) {}
void BK4819_SetupSquelch(SQL, uint8_t, uint8_t) {}
void BK4819_WriteRegister(uint8_t, uint16_t) {}
void BK4819_SelectFilterByFrequency(uint32_t) {}
void BK4819_TuneTo(uint32_t f, bool) { g_tuneF = f; }
uint16_t BK4819_GetRSSI(void) { return host_signal_rssi(g_tuneF); }
bool BK4819_IsSquelchOpen(void) { return host_signal_rssi(g_tuneF) > (uint16_t)((-95 + 160) * 2); }
void BK4819_EnableFrequencyScanEx2(FreqScanTime, uint16_t) {}
void BK4819_DisableFrequencyScan(void) {}
bool BK4819_GetFrequencyScanResult(uint32_t *f) { *f = 43312500; return true; }
uint16_t BK4819_GetFilter(void) { return 0; }
bool SI47XX_Acquire(SI47XX_MODE) { return true; }
void SI47XX_Init(void) {}
void SI47XX_SetAutomaticGainControl(uint8_t, uint8_t) {}
void SI47XX_SetVolume(uint8_t) {}
void SI47XX_TuneTo(uint32_t) {}
}

/* режим «Radio» в превью не нужен — заглушка для ссылки из vfo.cpp */
static Mode s_dummyRadio;
Mode &MODE_RADIO = s_dummyRadio;
