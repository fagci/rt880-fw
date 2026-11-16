/* Хост-превью экранов: гоняет настоящие режимы и сохраняет кадры в PPM. */
#include "helper/lootlist.h"
#include "lvgl.h"
#include "modes/analyser.h"
#include "modes/fc.h"
#include "modes/loot_screen.h"
#include "modes/mode.h"
#include "modes/scanner.h"
#include "modes/vfo.h"
#include "radio.h"
#include "ui/graphics.h"
#include "ui/lv_port.h"
#include <stdio.h>
#include <string.h>

extern uint32_t g_now_ms;
extern uint16_t g_bat_cv;
uint16_t host_display_px(int x, int y);

static const char *outdir = ".";

static void step(int n) {
  for (int i = 0; i < n; i++) {
    g_now_ms += 5;
    lv_tick_inc(5);
    Mode_Update();
    Mode_Render();
    lv_timer_handler();
  }
}

static void dump(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n240 320\n255\n");
  for (int y = 0; y < 320; y++)
    for (int x = 0; x < 240; x++) {
      uint16_t c = host_display_px(x, y);
      uint8_t rgb[3] = {(uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                        (uint8_t)((c << 3) & 0xF8)};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

static void key(KEY_Code_t k, Key_State_t s = KEY_RELEASED) { Mode_Key(k, s); step(4); }

static void screens(const char *tag) {
  char n[64];
  Mode_Switch(MODE_VFO); step(60);
  snprintf(n, sizeof(n), "%s_vfo", tag); dump(n);
  Mode_Switch(MODE_SCANNER); step(500);
  snprintf(n, sizeof(n), "%s_scanner", tag); dump(n);
  Mode_Switch(MODE_ANALYSER); step(500);
  snprintf(n, sizeof(n), "%s_analyser", tag); dump(n);
  Mode_Switch(MODE_FC); step(80);
  snprintf(n, sizeof(n), "%s_fc", tag); dump(n);
  Mode_Switch(MODE_LOOTLIST); step(40);
  snprintf(n, sizeof(n), "%s_loot", tag); dump(n);
}

int main(int argc, char **argv) {
  if (argc > 1) outdir = argv[1];
  lv_port_init();
  { lv_mem_monitor_t m; lv_mem_monitor(&m); fprintf(stderr,"mem total=%u free=%u\n",(unsigned)m.total_size,(unsigned)m.free_size); }
  Radio_Init();
  g_bat_cv = 372;
  /* немного находок для экрана списка */
  uint32_t fr[] = {43312500, 14550000, 16309300, 43350000, 14800000, 17025000, 12712500};
  for (int i = 0; i < 7; i++) {
    g_now_ms += 7000;
    for (int k = 0; k < 6 + i * 3; k++) { g_now_ms += 250; LOOT_Hit(fr[i], 150 + i * 7, i % 3 ? 0 : 1, 1, 0); }
    LOOT_Standby();
  }
  LOOT_ToggleBlacklist(LOOT_Item(2));
  LOOT_ToggleWhitelist(LOOT_Item(4));
  gThemeLight = false;
  screens("dark");
  gThemeLight = true;
  screens("light");
  return 0;
}
