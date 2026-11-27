/* Заглушки железа для хост-превью UI: дисплей ST7789 как буфер кадра (с
 * аппаратным скроллом), BK4819/SI4732/плата — имитация сигнала. */
#include "at32f423.h"
#include "driver/bk4819.h"
#include "driver/si473x.h"
#include "driver/st7789.h"
#include "driver/w25q32.h"
#include "radio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

gpio_type g_fake_gpio[8];
uint32_t SystemCoreClock = 150000000;
uint32_t _sheap, _eheap;
bool isSi4732On = false;

/* ── время ── */
uint32_t g_now_ms = 100000;
extern "C" uint32_t millis(void) { return g_now_ms; }
extern "C" void delay_us(uint32_t) {}
extern "C" void delay_ms(uint32_t) {}
volatile uint32_t tick_ms;
uint16_t g_bat_cv = 744;
extern "C" uint16_t battery_voltage_cv(void) { return g_bat_cv; }
extern "C" void audio_path_set(uint8_t) {}
extern "C" void uart_printf(const char *, ...) {}
void st7789_set_mute(bool) {}
void st7789_set_gamma(uint8_t) {}

static bool g_ant_main;
extern "C" void ant_sw(bool hf) { g_ant_main = hf; }
extern "C" bool ant_is_main(void) { return g_ant_main; }
extern "C" uint32_t ant_last_switch_ms(void) { return 0; }
extern "C" void board_tx_gates(bool, bool) {}
bool g_led_rx;
void led_rx_set(bool on) { g_led_rx = on; }
void keyboard_backlight_set(bool) {}

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
void st7789_backlight_set(uint16_t) {}
void st7789_backlight_off(void) {}
void st7789_backlight_on(void) {}
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
static uint32_t g_siF = 958000;
static bool g_sql;

/* «Живой эфир»: шумовой пол с медленным наклоном и «травой» плюс сетка
 * станций — вещание, авиация, LPD/PMR, репитеры, APRS/DMR-пачки и спуры.
 * Частоты и ширины — в единицах 10 Гц (как rxF), ширина — полуширина.
 *   0 — узкополосная ЧМ/цифра: плоская вершина, параболический спад;
 *   1 — широковещательная ЧМ / КВ АМ: мягкие линейные скаты;
 *   2 — пакетная: мигает слотами, как DMR/APRS. */
enum { FK_NARROW, FK_WIDE, FK_BURST };
struct FakeSig { uint32_t f; uint16_t w; int8_t dbm; uint8_t kind; };
#define HZ10(hz) ((uint32_t)((hz) / 10))

static const FakeSig SIG[] = {
    /* ДВ/СВ, кГц */
    {HZ10(153000), 500, -72, FK_NARROW},  {HZ10(171000), 500, -80, FK_NARROW},
    {HZ10(198000), 500, -78, FK_NARROW},  {HZ10(225000), 500, -74, FK_NARROW},
    {HZ10(252000), 500, -82, FK_NARROW},  {HZ10(531000), 500, -70, FK_WIDE},
    {HZ10(594000), 500, -66, FK_WIDE},    {HZ10(639000), 500, -72, FK_WIDE},
    {HZ10(693000), 500, -68, FK_WIDE},    {HZ10(747000), 500, -75, FK_WIDE},
    {HZ10(828000), 500, -64, FK_WIDE},    {HZ10(873000), 500, -78, FK_WIDE},
    {HZ10(936000), 500, -70, FK_WIDE},    {HZ10(999000), 500, -73, FK_WIDE},
    {HZ10(1071000), 500, -76, FK_WIDE},   {HZ10(1152000), 500, -68, FK_WIDE},
    {HZ10(1251000), 500, -72, FK_WIDE},   {HZ10(1359000), 500, -80, FK_WIDE},
    {HZ10(1431000), 500, -74, FK_WIDE},   {HZ10(1521000), 500, -78, FK_WIDE},
    {HZ10(1602000), 500, -71, FK_WIDE},
    /* КВ, кГц: 90m..11m */
    {HZ10(3200000), 500, -78, FK_WIDE},   {HZ10(3950000), 500, -74, FK_WIDE},
    {HZ10(4885000), 500, -80, FK_WIDE},   {HZ10(5900000), 500, -66, FK_WIDE},
    {HZ10(5950000), 500, -72, FK_WIDE},   {HZ10(6000000), 500, -60, FK_WIDE},
    {HZ10(6070000), 500, -70, FK_WIDE},   {HZ10(6110000), 500, -75, FK_WIDE},
    {HZ10(6155000), 500, -68, FK_WIDE},   {HZ10(6180000), 500, -77, FK_WIDE},
    {HZ10(7200000), 500, -62, FK_WIDE},   {HZ10(7230000), 500, -74, FK_WIDE},
    {HZ10(7280000), 500, -68, FK_WIDE},   {HZ10(7300000), 500, -76, FK_WIDE},
    {HZ10(7350000), 500, -72, FK_WIDE},   {HZ10(7415000), 500, -79, FK_WIDE},
    {HZ10(9420000), 500, -64, FK_WIDE},   {HZ10(9500000), 500, -70, FK_WIDE},
    {HZ10(9580000), 500, -58, FK_WIDE},   {HZ10(9650000), 500, -74, FK_WIDE},
    {HZ10(9720000), 500, -68, FK_WIDE},   {HZ10(9835000), 500, -72, FK_WIDE},
    {HZ10(11600000), 500, -66, FK_WIDE},  {HZ10(11730000), 500, -60, FK_WIDE},
    {HZ10(11810000), 500, -74, FK_WIDE},  {HZ10(11945000), 500, -70, FK_WIDE},
    {HZ10(12050000), 500, -76, FK_WIDE},  {HZ10(13600000), 500, -65, FK_WIDE},
    {HZ10(13750000), 500, -73, FK_WIDE},  {HZ10(15100000), 500, -62, FK_WIDE},
    {HZ10(15300000), 500, -70, FK_WIDE},  {HZ10(15500000), 500, -74, FK_WIDE},
    {HZ10(15700000), 500, -79, FK_WIDE},  {HZ10(17480000), 500, -68, FK_WIDE},
    {HZ10(17650000), 500, -72, FK_WIDE},  {HZ10(17800000), 500, -66, FK_WIDE},
    {HZ10(21450000), 500, -70, FK_WIDE},  {HZ10(21550000), 500, -74, FK_WIDE},
    {HZ10(21700000), 500, -77, FK_WIDE},  {HZ10(25600000), 500, -72, FK_WIDE},
    {HZ10(25800000), 500, -78, FK_WIDE},
    /* CB 27 МГц */
    {HZ10(26965000), 450, -72, FK_NARROW}, {HZ10(27135000), 450, -80, FK_NARROW},
    {HZ10(27185000), 450, -68, FK_NARROW}, {HZ10(27245000), 450, -85, FK_NARROW},
    {HZ10(27405000), 450, -78, FK_NARROW},
    /* OIRT 64–74 и FM 87.5–108 МГц */
    {HZ10(65900000), 9000, -62, FK_WIDE},  {HZ10(66800000), 9000, -74, FK_WIDE},
    {HZ10(68000000), 9000, -58, FK_WIDE},  {HZ10(70100000), 9000, -79, FK_WIDE},
    {HZ10(71300000), 9000, -68, FK_WIDE},  {HZ10(73400000), 9000, -82, FK_WIDE},
    {HZ10(87500000), 9000, -57, FK_WIDE},  {HZ10(88600000), 9000, -69, FK_WIDE},
    {HZ10(90300000), 9000, -61, FK_WIDE},  {HZ10(92400000), 9000, -76, FK_WIDE},
    {HZ10(94500000), 9000, -66, FK_WIDE},  {HZ10(96800000), 9000, -80, FK_WIDE},
    {HZ10(98000000), 9000, -55, FK_WIDE},  {HZ10(100700000), 9000, -72, FK_WIDE},
    {HZ10(102400000), 9000, -64, FK_WIDE}, {HZ10(104600000), 9000, -83, FK_WIDE},
    {HZ10(106800000), 9000, -70, FK_WIDE}, {HZ10(107900000), 9000, -78, FK_WIDE},
    /* авиация 118–136 МГц, АМ */
    {HZ10(118100000), 600, -80, FK_NARROW}, {HZ10(118300000), 600, -86, FK_NARROW},
    {HZ10(119900000), 600, -75, FK_NARROW}, {HZ10(120500000), 600, -82, FK_NARROW},
    {HZ10(121500000), 600, -70, FK_NARROW}, {HZ10(123450000), 600, -84, FK_NARROW},
    {HZ10(124700000), 600, -78, FK_NARROW}, {HZ10(125100000), 600, -73, FK_NARROW},
    {HZ10(126200000), 600, -85, FK_NARROW}, {HZ10(127600000), 600, -79, FK_NARROW},
    {HZ10(128300000), 600, -74, FK_NARROW}, {HZ10(129900000), 600, -83, FK_NARROW},
    {HZ10(131500000), 600, -77, FK_NARROW}, {HZ10(132000000), 600, -86, FK_NARROW},
    {HZ10(133250000), 600, -80, FK_NARROW}, {HZ10(134500000), 600, -75, FK_NARROW},
    /* 2 м */
    {HZ10(144800000), 700, -82, FK_BURST},  {HZ10(145050000), 700, -88, FK_NARROW},
    {HZ10(145175000), 700, -76, FK_BURST},  {HZ10(145300000), 700, -84, FK_NARROW},
    {HZ10(145375000), 700, -90, FK_NARROW}, {HZ10(145500000), 700, -62, FK_NARROW},
    {HZ10(145600000), 700, -70, FK_NARROW}, {HZ10(145725000), 700, -79, FK_NARROW},
    {HZ10(145750000), 700, -85, FK_NARROW}, {HZ10(145775000), 700, -80, FK_NARROW},
    {HZ10(146000000), 700, -92, FK_NARROW}, {HZ10(147500000), 700, -88, FK_NARROW},
    /* 70 см: репитеры, DMR, ISM 433.92 (разная плотность и уровень) */
    {HZ10(430025000), 700, -92, FK_NARROW}, {HZ10(432000000), 700, -88, FK_NARROW},
    {HZ10(433050000), 700, -77, FK_NARROW}, {HZ10(433125000), 700, -63, FK_NARROW},
    {HZ10(433500000), 700, -82, FK_NARROW}, {HZ10(433920000), 1200, -59, FK_NARROW},
    {HZ10(434500000), 700, -84, FK_NARROW}, {HZ10(435000000), 1500, -74, FK_NARROW},
    {HZ10(437000000), 700, -71, FK_NARROW}, {HZ10(437500000), 300, -93, FK_NARROW},
    {HZ10(438000000), 700, -86, FK_NARROW}, {HZ10(438775000), 1300, -67, FK_BURST},
    {HZ10(439000000), 700, -83, FK_NARROW}, {HZ10(439500000), 700, -91, FK_NARROW},
    /* PMR/LPD 446 и деловые 450–470 МГц */
    {HZ10(446006250), 700, -78, FK_NARROW}, {HZ10(446018750), 700, -84, FK_NARROW},
    {HZ10(446031250), 700, -80, FK_BURST},  {HZ10(446043750), 700, -88, FK_NARROW},
    {HZ10(446056250), 700, -76, FK_NARROW}, {HZ10(446068750), 700, -90, FK_NARROW},
    {HZ10(446081250), 700, -82, FK_NARROW}, {HZ10(446093750), 700, -86, FK_NARROW},
    {HZ10(450050000), 700, -80, FK_NARROW}, {HZ10(452500000), 700, -85, FK_NARROW},
    {HZ10(456700000), 700, -78, FK_NARROW}, {HZ10(460100000), 700, -83, FK_NARROW},
    {HZ10(462562500), 700, -70, FK_NARROW}, {HZ10(462587500), 700, -78, FK_NARROW},
    {HZ10(462612500), 700, -82, FK_BURST},  {HZ10(464500000), 700, -75, FK_NARROW},
    {HZ10(466000000), 700, -88, FK_NARROW}, {HZ10(467562500), 700, -75, FK_NARROW},
    {HZ10(467587500), 700, -84, FK_NARROW}, {HZ10(469500000), 700, -82, FK_NARROW},
};

static uint32_t h32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

/* Медленное замирание: у каждой станции свой период и фаза. */
static int fadeDb(uint32_t key, uint32_t t) {
  uint32_t h = h32(key);
  float w = 0.0008f + (float)((h >> 9) & 7u) * 0.00022f;
  float ph = (float)(h & 1023u) * 0.00613f;
  return (int)(5.0f * sinf(t * w + ph));
}

uint16_t host_signal_rssi(uint32_t f) {
  uint32_t t = g_now_ms;
  /* Шумовой пол: на КВ выше (атмосферный/городской), на УКВ ниже. */
  int dbm = f < 3000000 ? -104 : (f < 30000000 ? -106 : -109);
  dbm += (int)(2.0f * sinf((float)f * 0.00000027f)); /* медленный наклон */
  dbm += (int)(h32((f * 7u) ^ (t / 37u)) & 7u) - 3;  /* «трава», ±3.5 дБ */

  for (const FakeSig &s : SIG) {
    int d = (int)f - (int)s.f;
    if (d < 0)
      d = -d;
    if (d > s.w)
      continue;
    int v = s.dbm;
    if (s.kind == FK_BURST) {
      /* 5 слотов по 90 мс: станция «дышит» пачками */
      if (((t / 90u + (h32(s.f) & 7u)) % 5u) > 1u)
        continue;
      v -= 4;
    }
    if (s.kind == FK_WIDE)
      v -= (d * 7) / (s.w ? s.w : 1);
    else if (s.w)
      v -= (d * d * 8) / (s.w * s.w);
    v += fadeDb(s.f, t);
    if (v > dbm)
      dbm = v;
  }
  return (uint16_t)((dbm + 160) * 2);
}
extern "C++" {
void BK4819_Init(void) {}
void BK4819_SelectChip(uint8_t) {}
void BK4819_RX_TurnOn(void) {}
void BK4819_RxEnable(void) {}
void BK4819_SetAFC(uint8_t) {}
void BK4819_SetAGC(bool, uint8_t) {}
uint8_t BK4819_GetGainDb(uint8_t) { return 0; }
void BK4819_ToggleAFDAC(bool) {}
void BK4819_ToggleFilter(Filter, bool) {}
void BK4819_SelectFilter(Filter) {}
void BK4819_SetFilterOverride(Filter, bool) {}
void BK4819_InvalidateFilterCache(void) {}
void BK4819_SetModulation(ModulationType) {}
void BK4819_SetFilterBandwidth(BK4819_FilterBandwidth_t) {}
void BK4819_SetupSquelch(SQL, uint8_t, uint8_t) {}
void BK4819_SetDeviation(uint16_t) {}
void BK4819_SetTailDetection(uint32_t) {}
void BK4819_PrepareTransmit(void) {}
void BK4819_WriteRegister(uint8_t, uint16_t) {}
uint16_t BK4819_ReadRegister(uint8_t) { return 0; }
uint16_t BK4819_ReadRegisterRaw(uint8_t) { return 0; }
static uint16_t g_bk_gpio_out;
uint16_t BK4819_GetGpioOutState(void) { return g_bk_gpio_out; }
void BK4819_SetGpioOutState(uint16_t s) { g_bk_gpio_out = s; }
void BK4819_InvalidateFrequencyCache(void) {}
uint8_t BK4819_GetAFC(void) { return 0; }
int16_t BK4819_GetAFCValue(void) { return 0; }
void BK4819_EnableCxCSSScan(void) {}
void BK4819_DisableCxCSSScan(void) {}
BK4819_CssScanResult_t BK4819_GetCxCSSScanResult(uint32_t *cdcss,
                                                 uint16_t *ctcss) {
  if (cdcss)
    *cdcss = 0;
  if (ctcss)
    *ctcss = 0;
  return BK4819_CSS_RESULT_NOT_FOUND;
}
void BK4819_EnableDTMF(void) {}
void BK4819_DisableDTMF(void) {}
uint8_t BK4819_GetDTMF_5TONE_Code(void) { return 0; }
void BK4819_SelectFilterByFrequency(uint32_t) {}
void BK4819_TuneTo(uint32_t f, bool) { g_tuneF = f; }
void BK4819_RxSuspend(void) {}
void BK4819_RxResume(void) {}
uint16_t BK4819_GetRSSI(void) { return host_signal_rssi(g_tuneF); }
bool BK4819_IsSquelchOpen(void) { return host_signal_rssi(g_tuneF) > (uint16_t)((-95 + 160) * 2); }
void BK4819_EnableFrequencyScanEx2(FreqScanTime, uint16_t) {}
void BK4819_DisableFrequencyScan(void) {}
bool BK4819_GetFrequencyScanResult(uint32_t *f) { *f = 43312500; return true; }
uint16_t BK4819_GetFilter(void) { return 0; }
bool SI47XX_Acquire(SI47XX_MODE) { return true; }
void SI47XX_Init(void) {}
void SI47XX_PowerDown() {}
void SI47XX_SetAutomaticGainControl(uint8_t, uint8_t) {}
void SI47XX_SetVolume(uint8_t) {}
void SI47XX_SetBandwidth(SI47XX_FilterBW, bool) {}
static uint8_t host_si_rssi_at(uint32_t f) {
  int dbm = (int)host_signal_rssi(f) / 2 - 160;
  int uV = dbm + 107; /* dBm → дБмкВ (50 Ом) */
  return uV < 0 ? 0 : (uint8_t)(uV > 127 ? 127 : uV);
}

void SI47XX_TuneTo(uint32_t f) { g_siF = f; }
uint8_t SI47XX_GetRssi(void) { return host_si_rssi_at(g_siF); }
void SI47XX_GetRssiSnr(uint8_t *rssi, uint8_t *snr) {
  if (rssi)
    *rssi = host_si_rssi_at(g_siF);
  if (snr) {
    /* SNR: 0 на шумовом полу (−108 дБм), до 30 у сильных станций */
    int dbm = (int)host_signal_rssi(g_siF) / 2 - 160;
    int v = dbm + 108;
    *snr = v < 0 ? 0 : (uint8_t)(v > 30 ? 30 : v);
  }
}
void SI47XX_SetBFO(int16_t) {}
bool SI47XX_IsSSB(void) { return true; }
}

/* В превью нет внешней W25Q32 — сохранение состояния не нужно. */
bool W25Q_Init(void) { return false; }
bool W25Q_Present(void) { return false; }
uint32_t W25Q_JedecId(void) { return 0; }
void W25Q_Read(uint32_t, void *dst, uint32_t len) {
  memset(dst, 0xFF, len);
}
bool W25Q_SectorErase(uint32_t) { return false; }
bool W25Q_PageProgram(uint32_t, const void *, uint32_t) { return false; }

void Store_MarkDirty(void) {}
void Store_PinRadio(void) {}
void Store_UnpinRadio(void) {}
bool Store_ApplyRadio(void) { return false; }
