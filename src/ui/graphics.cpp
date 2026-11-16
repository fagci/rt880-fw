/* Слой совместимости: прежний immediate-mode API (graphics.h) поверх LVGL.
 *
 * Примитивы (прямоугольники, линии, круги) идут напрямую в ST7789 через DMA —
 * как раньше: они дёшевы и LVGL про них знать не должен.
 *
 * Текст (Printf/PrintfEx/PrintfT) маппится на пул LVGL-лейблов: метка
 * создаётся один раз на (x, y, шрифт, выравнивание) и дальше только
 * обновляется. LVGL сам перерисует только изменившееся, а spi не мусорит
 * каждый кадр. Пул сбрасывается в UI_ClearScreen (переход режима).
 *
 * Координатная семантика прежняя: y — базовая линия, фон заливается
 * по bbox (теперь это bg-стиль лейбла).
 */
#include "graphics.h"
#include "driver/st7789.h"
#include "gfxfont.h"
#include "lvgl_fonts/lv_font_mono12.h"
#include "lvgl_fonts/lv_font_mono18.h"
#include "lvgl_fonts/lv_font_mono9.h"
#include "lvgl_fonts/lv_font_tomthumb.h"
#include "lvgl.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>

bool gInverted;
bool gThemeLight;

#define FB_W ST7789_WIDTH
#define FB_H ST7789_HEIGHT

static bool s_lvgl_ready;

#define LABEL_POOL 40

typedef struct {
  lv_obj_t *label;
  int16_t x, y;
  Color fg, bg;
  uint32_t text_hash; /* FNV-1a: не дергаем LVGL, если строка не изменилась */
  uint16_t text_len;
  int8_t font_idx; /* в FONT_MAP — указатели GFXfont между TU несопоставимы */
  TextPos align;
  bool styled;
  bool used;
} LabelSlot;

static LabelSlot s_pool[LABEL_POOL];

void Graphics_MarkLvglReady(void) { s_lvgl_ready = true; }

/* Прямая DMA-заливка обходит буферы LVGL: если в области был лейбл,
   его пиксели стёрты, а хеш-слой не узнает об этом. Инвалидируем только
   сами лейблы, пересекающиеся с областью — голую область трогать нельзя:
   LVGL залил бы её фоном экрана и стёр прямую отрисовку (бары и т.п.). */
static void lv_invalidate(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!s_lvgl_ready || w <= 0 || h <= 0) return;
  for (uint8_t i = 0; i < LABEL_POOL; i++) {
    if (!s_pool[i].used) continue;
    lv_obj_t *l = s_pool[i].label;
    lv_coord_t lx = lv_obj_get_x(l), ly = lv_obj_get_y(l);
    lv_coord_t lw = lv_obj_get_width(l), lh = lv_obj_get_height(l);
    if (x < lx + lw && lx < x + w && y < ly + lh && ly < y + h)
      lv_obj_invalidate(l);
  }
}

static int16_t clip_x(int16_t x) {
  if (x < 0) return 0;
  if (x >= FB_W) return FB_W - 1;
  return x;
}
static int16_t clip_y(int16_t y) {
  if (y < 0) return 0;
  if (y >= FB_H) return FB_H - 1;
  return y;
}

void PutPixel(int16_t x, int16_t y, Color c) {
  x = clip_x(x);
  y = clip_y(y);
  st7789_set_addr_window(x, y, 1, 1);
  st7789_cs_low();
  st7789_start_pixels();
  st7789_write_data16(c);
  st7789_cs_high();
}

void DrawVLine(int16_t x, int16_t y, int16_t h, Color c) {
  if (x < 0 || x >= FB_W || h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > FB_H) h = FB_H - y;
  if (h <= 0) return;
  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, 1, (uint16_t)h, c);
  lv_invalidate(x, y, 1, h);
}

void DrawHLine(int16_t x, int16_t y, int16_t w, Color c) {
  if (y < 0 || y >= FB_H || w <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > FB_W) w = FB_W - x;
  if (w <= 0) return;
  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, (uint16_t)w, 1, c);
  lv_invalidate(x, y, w, 1);
}

void FillRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_W) w = FB_W - x;
  if (y + h > FB_H) h = FB_H - y;
  if (w <= 0 || h <= 0) return;
  st7789_fill_rect_dma((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, c);
  lv_invalidate(x, y, w, h);
}

static void draw_aline(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) {
  int16_t s = abs(y1 - y0) > abs(x1 - x0);
  if (s) {
    int16_t t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
  }
  if (x0 > x1) {
    int16_t t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  int16_t dx = x1 - x0, dy = abs(y1 - y0), e = dx >> 1;
  int16_t ys = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++, e -= dy) {
    PutPixel(s ? y0 : x0, s ? x0 : y0, c);
    if (e < 0) { y0 += ys; e += dx; }
  }
}

void DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, Color c) {
  if (x0 == x1) {
    if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
    DrawVLine(x0, y0, y1 - y0 + 1, c);
  } else if (y0 == y1) {
    if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
    DrawHLine(x0, y0, x1 - x0 + 1, c);
  } else {
    draw_aline(x0, y0, x1, y1, c);
    /* диагональ рисуем попиксельно — инвалидируем весь bbox */
    int16_t xa = x0 < x1 ? x0 : x1, xb = x0 < x1 ? x1 : x0;
    int16_t ya = y0 < y1 ? y0 : y1, yb = y0 < y1 ? y1 : y0;
    lv_invalidate(xa, ya, xb - xa + 1, yb - ya + 1);
  }
}

void DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, Color c) {
  DrawHLine(x, y, w, c);
  DrawHLine(x, y + h - 1, w, c);
  DrawVLine(x, y, h, c);
  DrawVLine(x + w - 1, y, h, c);
}

void FillCircle(int16_t x0, int16_t y0, int16_t r, Color c) {
  int16_t x = 0, y = r, d = 3 - 2 * r;
  while (y >= x) {
    int16_t w = 2 * x + 1;
    DrawHLine(x0 - x, y0 - y, w, c);
    DrawHLine(x0 - x, y0 + y, w, c);
    w = 2 * y + 1;
    DrawHLine(x0 - y, y0 - x, w, c);
    DrawHLine(x0 - y, y0 + x, w, c);
    x++;
    if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
    else d = d + 4 * x + 6;
  }
}

/* ─── Текст: пул лейблов поверх LVGL ─── */

typedef struct {
  const GFXfont *gfx;
  const lv_font_t *lv;
  uint8_t ascent; /* = line_height - base_line сконвертированного шрифта */
} FontMap;

static const FontMap FONT_MAP[] = {
    {F_SS, &lv_font_tomthumb, 4},
    {F_SM, &lv_font_mono9, 11},
    {F_NORM, &lv_font_mono12, 15},
    {F_LG, &lv_font_mono18, 22},
};

/* GFXfont объявлены static в заголовках — у каждого .cpp своя копия,
   указатели между модулями несопоставимы. Идентифицируем по метрикам. */
static int8_t font_map(const GFXfont *f) {
  for (uint8_t i = 0; i < sizeof(FONT_MAP) / sizeof(FONT_MAP[0]); i++)
    if (f->first == FONT_MAP[i].gfx->first &&
        f->last == FONT_MAP[i].gfx->last &&
        f->yAdvance == FONT_MAP[i].gfx->yAdvance)
      return (int8_t)i;
  return 2; /* F_NORM как fallback */
}

static int16_t text_width(const char *s, const GFXfont *f) {
  int16_t x = 0;
  for (; *s; s++) {
    if (*s < f->first || *s > f->last) continue;
    x += f->glyph[*s - f->first].xAdvance;
  }
  return x;
}

int16_t TextWidth(const char *s, const GFXfont *f) { return text_width(s, f); }

/* LVGL хранит RGB565 байтами переключёнными (LV_COLOR_16_SWAP), поэтому
   собираем цвет через lv_color_make из компонентов, а не .full. */
static inline lv_color_t to_lv(Color c) {
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

static uint32_t fnv1a(const char *s, uint16_t *len_out) {
  uint32_t h = 2166136261u;
  uint16_t len = 0;
  for (; *s; s++, len++) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  *len_out = len;
  return h;
}

/* Удалить все лейблы пула (Printf*): нативные LVGL-экраны (VFO, список
 * находок) не должны получать «призраков» чужого режима поверх себя.
 * Следующий Printf* пересоздаст нужные лейблы лениво. */
void UI_ReleaseLabels(void) {
  if (!s_lvgl_ready)
    return;
  for (uint8_t i = 0; i < LABEL_POOL; i++) {
    if (!s_pool[i].used)
      continue;
    if (s_pool[i].label)
      lv_obj_del(s_pool[i].label);
    s_pool[i] = LabelSlot{};
  }
}

void UI_ClearScreen(Color bg) {
  if (s_lvgl_ready) {
    /* Не удаляем лейблы — сбрасываем хеши: при следующем PrintfEx текст
       перепечатается (с учётом инверсии). Прямой st7789_flush здесь не
       делаем: если на экране нативные виджеты, LVGL об этом не знает и
       картинка расходится. Полная инвалидация — LVGL сам всё перерисует
       (и фон, и нативные виджеты, и пул-лейблы). */
    for (uint8_t i = 0; i < LABEL_POOL; i++) {
      s_pool[i].text_hash = 0;
      s_pool[i].text_len = 0xFFFF;
    }
    lv_obj_set_style_bg_color(lv_scr_act(), to_lv(bg), LV_PART_MAIN);
    lv_obj_invalidate(lv_scr_act());
  } else {
    st7789_flush(bg); /* LVGL ещё не инициализирован (board_init) */
  }
}

static void label_style(lv_obj_t *label, Color fg, Color bg, const FontMap *fm) {
  lv_obj_set_style_text_font(label, fm->lv, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, to_lv(fg), LV_PART_MAIN);
  lv_obj_set_style_bg_color(label, to_lv(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(label, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(label, 0, LV_PART_MAIN);
}

static void printfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
                     const GFXfont *f, const char *fmt, va_list a) {
  if (!s_lvgl_ready) return;

  if (gInverted) { Color tmp = col; col = bg; bg = tmp; }

  char s[64];
  vsnprintf(s, sizeof(s), fmt, a);
  if (s[0] == '\0') return;

  const int8_t fidx = font_map(f);
  const FontMap *fm = &FONT_MAP[fidx];
  int16_t tw = text_width(s, f);

  int16_t sx = x;
  if (align == POS_C) sx = x - (tw >> 1);
  else if (align == POS_R) sx = x - tw;

  uint16_t len;
  uint32_t hash = fnv1a(s, &len);

  /* Ищем слот с тем же якорем — переиспользуем, не пересоздаём */
  LabelSlot *slot = NULL;
  for (uint8_t i = 0; i < LABEL_POOL; i++) {
    if (s_pool[i].used && s_pool[i].font_idx == fidx && s_pool[i].x == x &&
        s_pool[i].y == y && s_pool[i].align == align) {
      slot = &s_pool[i];
      break;
    }
  }

  if (slot && slot->styled && slot->text_hash == hash &&
      slot->text_len == len && slot->fg == col && slot->bg == bg) {
    return; /* текст и стиль не изменились — LVGL не трогаем */
  }

  if (!slot) {
    for (uint8_t i = 0; i < LABEL_POOL; i++) {
      if (!s_pool[i].used) { slot = &s_pool[i]; break; }
    }
    if (!slot) { /* не должно случиться: перезаписываем первый слот */
      slot = &s_pool[0];
      lv_obj_del(slot->label);
    }
    slot->used = true;
    slot->styled = false;
    slot->label = lv_label_create(lv_scr_act());
    if (slot->label == NULL) { /* пул LVGL исчерпан — пропускаем текст */
      slot->used = false;
      return;
    }
  }

  slot->x = x;
  slot->y = y;
  slot->font_idx = fidx;
  slot->align = align;

  /* Позиция зависит от ширины текста — выставляем всегда */
  lv_obj_set_pos(slot->label, sx, y - fm->ascent);

  if (!slot->styled || slot->fg != col || slot->bg != bg) {
    slot->fg = col;
    slot->bg = bg;
    slot->styled = true;
    label_style(slot->label, col, bg, fm);
  }

  slot->text_hash = hash;
  slot->text_len = len;
  lv_label_set_text(slot->label, s);
}

void Printf(uint8_t x, uint16_t y, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, POS_L, C_WHITE, C_BLACK, F_NORM, fmt, a);
  va_end(a);
}

void PrintfEx(uint8_t x, uint16_t y, TextPos align, Color col, Color bg,
              const GFXfont *f, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, align, col, bg, f, fmt, a);
  va_end(a);
}

void PrintfT(uint8_t x, uint16_t y, TextStyle st, const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  printfEx(x, y, st.align, st.fg, st.bg, st.font, fmt, a);
  va_end(a);
}
