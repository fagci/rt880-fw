/* Экран VFO — нативный LVGL.
 *
 * Виджеты создаются в enter() и удаляются в exit(). Никаких прямых
 * DMA-заливок и пула лейблов: состояние живёт в виджетах, LVGL сам
 * перерисовывает изменившееся. Редактор частоты — NumVal в нативном
 * режиме (NumVal_BindNative): он обновляет только текст лейбла,
 * курсор и позиционирование — здесь.
 */
#include "vfo.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/numval.h"
#include "../ui/statusline.h"
#include "loot_screen.h"
#include "mode.h"
#include "radio.h"
#include "scanner.h"
#include "fc.h"
#include "analyser.h"
#include "lvgl.h"
#include "printf.h"
#include <string.h>

#define VFO_H 98
#define STATUS_H 24

#define FREQ_RIGHT 204 /* правый край частоты (цифры XL, 7 знаков) */
/* Выравнивание частоты в VFO (и при вводе): POS_R — по правому краю,
 * POS_L — по левому. Одна настройка на NumVal, лейбл и курсор. */
#define VFO_FREQ_POS POS_R
#define COL_RIGHT 232 /* правый край колонок справа */
#define MARGIN 8

#define VFO_INFO_Y 6   /* строка диапазон / модуляция (M) */
#define VFO_FREQ_Y 26  /* верх строки частоты (XL) */
#define VFO_SUB_Y 68   /* строка чип / шаг (S) */
#define VFO_RSSI_Y 88  /* тонкая полоса уровня */
#define VFO_RSSI_W (LCD_WIDTH - 2 * MARGIN)
#define VFO_RSSI_H 3

static const char *MOD_NAMES[] = {
    [MOD_FM] = "FM",   [MOD_AM] = "AM",   [MOD_LSB] = "LSB", [MOD_USB] = "USB",
    [MOD_BYP] = "BYP", [MOD_RAW] = "RAW", [MOD_WFM] = "WFM",
};

struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
} vfoCtx;

/* ─── Виджеты ─── */
static lv_obj_t *s_scr; /* корневой контейнер экрана */
static lv_obj_t *s_track[DEVICE_VFO_COUNT]; /* 1px подложка полосы */
static lv_obj_t *s_freqLbl[DEVICE_VFO_COUNT];
static lv_obj_t *s_extraLbl[DEVICE_VFO_COUNT];
static lv_obj_t *s_badgeLbl[DEVICE_VFO_COUNT];
static lv_obj_t *s_modLbl[DEVICE_VFO_COUNT];
static lv_obj_t *s_stepLbl[DEVICE_VFO_COUNT];
static lv_obj_t *s_bandLbl[DEVICE_VFO_COUNT];
static const char *s_prevBand[DEVICE_VFO_COUNT];
static lv_obj_t *s_rssiBar[DEVICE_VFO_COUNT];
static lv_obj_t *s_cursor;

/* кэши предыдущего состояния */
static bool s_prevActive[DEVICE_VFO_COUNT];
static uint8_t s_prevMod[DEVICE_VFO_COUNT];
static uint8_t s_prevRadio[DEVICE_VFO_COUNT];
static bool s_prevMon[DEVICE_VFO_COUNT];
static uint8_t s_prevStep = 0xFF;
static uint8_t s_prevRssi[DEVICE_VFO_COUNT];
static uint8_t s_prevExtra[DEVICE_VFO_COUNT];
static bool s_inited;

static inline lv_color_t lvc(Color c) {
  return lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                       (uint8_t)(c << 3));
}

/* lv_obj_add_flag/clear_flag(HIDDEN) инвалидируют область даже если флаг
 * не менялся; вызываемые каждый кадр они забивают буфер dirty-областей
 * LVGL (32 шт.) — при переполнении перерисовывается весь экран (~600 мс).
 * Поэтому трогаем флаг только при реальной смене. */
static void setHidden(lv_obj_t *o, bool hidden) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Цвет с учётом глобальной темы */
static Color fgAd(Color c) { return TC(c); }

static lv_obj_t *mkBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, Color bg, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  if (!o) return NULL; /* пул LVGL исчерпан */
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lvc(TC(bg)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
  return o;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *font, Color fg,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w,
                         lv_text_align_t align) {
  lv_obj_t *l = lv_label_create(parent);
  if (!l) return NULL;
  lv_obj_set_pos(l, x, y);
  if (w) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  }
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lvc(TC(fg)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
  return l;
}

static void onFreqEntered(uint32_t f) {
  Radio_TuneToAuto(f);
}

static void initNumVals() {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&vfoCtx.numVal[i], FREQ_RIGHT, sy + VFO_FREQ_Y + FontAscent(F_XL), F_XL, 0,
                0, UNIT_MHZ, vfos[i].rxF, 0, 1340 * MHZ, onFreqEntered,
                VFO_FREQ_POS, F_S, 0, 0);
  }
}

/* ─── Построение экрана ─── */

static void buildScreen(void) {
  s_scr = mkBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK,
                LV_OPA_TRANSP);

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;

    s_track[i] = mkBox(s_scr, MARGIN, sy + VFO_RSSI_Y + 1, VFO_RSSI_W, 1,
                       C_GRAY, LV_OPA_COVER);
    s_rssiBar[i] =
        mkBox(s_scr, MARGIN, sy + VFO_RSSI_Y, 0, VFO_RSSI_H, C_GREEN,
              LV_OPA_COVER);

    s_bandLbl[i] = mkLabel(s_scr, F_M, C_CYAN, MARGIN, sy + VFO_INFO_Y, 120,
                           LV_TEXT_ALIGN_LEFT);
    s_modLbl[i] = mkLabel(s_scr, F_M, C_GREEN, COL_RIGHT - 60,
                          sy + VFO_INFO_Y, 60, LV_TEXT_ALIGN_RIGHT);

    s_freqLbl[i] = mkLabel(s_scr, F_XL, C_WHITE, 0, sy + VFO_FREQ_Y,
                           FREQ_RIGHT,
                           VFO_FREQ_POS == POS_L ? LV_TEXT_ALIGN_LEFT
                                                 : LV_TEXT_ALIGN_RIGHT);
    s_extraLbl[i] = mkLabel(s_scr, F_S, C_GRAY, FREQ_RIGHT + 4,
                            sy + VFO_FREQ_Y + FontAscent(F_XL) -
                                FontAscent(F_S),
                            COL_RIGHT - FREQ_RIGHT - 4, LV_TEXT_ALIGN_LEFT);

    s_badgeLbl[i] = mkLabel(s_scr, F_S, C_GRAY, MARGIN, sy + VFO_SUB_Y, 60,
                            LV_TEXT_ALIGN_LEFT);
    s_stepLbl[i] = mkLabel(s_scr, F_S, C_YELLOW, COL_RIGHT - 80,
                           sy + VFO_SUB_Y, 80, LV_TEXT_ALIGN_RIGHT);
  }

  s_cursor = mkBox(s_scr, 0, 0, 2, 30, C_YELLOW, LV_OPA_COVER);
  if (s_cursor)
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
}

/* Активный ряд: яркая частота и акценты; неактивный — приглушён */
static void setRowActive(uint8_t i, bool active) {
  if (s_freqLbl[i])
    lv_obj_set_style_text_color(s_freqLbl[i],
                                lvc(TC(active ? C_WHITE : C_GRAY)),
                                LV_PART_MAIN);
  if (s_bandLbl[i])
    lv_obj_set_style_text_color(s_bandLbl[i],
                                lvc(TC(active ? C_CYAN : C_GRAY)),
                                LV_PART_MAIN);
  if (s_modLbl[i])
    lv_obj_set_style_text_color(s_modLbl[i],
                                lvc(TC(active ? C_GREEN : C_GRAY)),
                                LV_PART_MAIN);
  setHidden(s_track[i], !active);
}

/* ─── Режим ─── */

class VfoMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("VFO");
    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();
    buildScreen();
    initNumVals();
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
      NumVal_BindNative(&vfoCtx.numVal[i], s_freqLbl[i]);
    memset(s_prevActive, 0xFF, sizeof(s_prevActive));
    memset(s_prevMod, 0xFF, sizeof(s_prevMod));
    memset(s_prevRadio, 0xFF, sizeof(s_prevRadio));
    memset(s_prevMon, 0xFF, sizeof(s_prevMon));
    memset(s_prevRssi, 0xFF, sizeof(s_prevRssi));
    memset(s_prevExtra, 0xFF, sizeof(s_prevExtra));
    memset(s_prevBand, 0, sizeof(s_prevBand));
    s_prevStep = 0xFF;
    s_inited = false;
  }

  void exit() override {
    if (s_scr) {
      lv_obj_del(s_scr);
      s_scr = NULL;
    }
  }

  void update() override {
    Radio_Update();
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
      NumVal_Update(&vfoCtx.numVal[i]);
      if (vfos[i].rxF != NumVal_GetValue(&vfoCtx.numVal[i]))
        NumVal_SetValue(&vfoCtx.numVal[i], vfos[i].rxF);
    }
  }

  void render() override {
    bool first = !s_inited;
    STATUSLINE_render();

    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
      uint16_t sy = STATUS_H + i * VFO_H;
      bool active = (i == currentVfo);
      if (!s_freqLbl[i]) continue; /* пул кончился — пропускаем ряд */

      if (first || active != s_prevActive[i]) {
        setRowActive(i, active);
        s_prevActive[i] = active;
      }

      NumVal_t *nv = &vfoCtx.numVal[i];
      bool editing = NumVal_IsEditing(nv);
      NumVal_Render(nv);

      /* Редактор: выравнивание то же, что и в обычном виде (VFO_FREQ_POS) —
         значение не «уезжает» при вводе. Курсор — сразу после последней
         введённой цифры: у правого края (POS_R) или правее набранного
         (POS_L). На выходе курсор спрятан. */
      if (editing) {
        if (s_cursor) {
          lv_coord_t cx = (VFO_FREQ_POS == POS_L)
                              ? 4 + nv->cursor * nv->charW
                              : FREQ_RIGHT + 2;
          lv_obj_set_pos(s_cursor, cx, sy + VFO_FREQ_Y + 4);
          setHidden(s_cursor, !nv->blinkState);
        }
      } else if (s_cursor && i == currentVfo) {
        setHidden(s_cursor, true);
      }

      if (s_badgeLbl[i] &&
          (first || vfos[i].modulation != s_prevMod[i] ||
           vfos[i].monitor != s_prevMon[i] ||
           vfos[i].radio != s_prevRadio[i])) {
        const char *bdg = "  ";
        if (vfos[i].monitor)
          bdg = vfos[i].radio == RADIO_SI4732 ? "MSI" : "MBK";
        else
          bdg = vfos[i].radio == RADIO_SI4732 ? "SI" : "BK";
        lv_label_set_text_static(s_badgeLbl[i], bdg);
        lv_label_set_text_static(s_modLbl[i], MOD_NAMES[vfos[i].modulation]);
        s_prevMod[i] = vfos[i].modulation;
        s_prevMon[i] = vfos[i].monitor;
        s_prevRadio[i] = vfos[i].radio;
      }

      /* название диапазона плана (40m, AIR, FM BC...) */
      if (s_bandLbl[i]) {
        const char *bn = Radio_BandName(vfos[i].rxF);
        if (first || bn != s_prevBand[i]) {
          lv_label_set_text_static(s_bandLbl[i], bn);
          s_prevBand[i] = bn;
        }
      }

      if (s_stepLbl[i]) {
        if (active) {
          setHidden(s_stepLbl[i], false);
          if (first || vfos[i].step != s_prevStep) {
            static char stepBuf[10];
            uint32_t step = StepFrequencyTable[vfos[i].step];
            snprintf(stepBuf, sizeof(stepBuf), "%u.%02uk", step / 100,
                     step % 100);
            lv_label_set_text_static(s_stepLbl[i], stepBuf);
            s_prevStep = vfos[i].step;
          }
        } else {
          setHidden(s_stepLbl[i], true);
        }
      }

      /* RSSI активного VFO (у SI своего индикатора пока нет) */
      uint8_t w = 0;
      if (active && vfos[i].radio != RADIO_SI4732) {
        BK4819_SelectChip(vfos[i].radio == RADIO_BK4819B ? 1 : 0);
        w = ConvertDomain(BK4819_GetRSSI(), 0, 255, 0, VFO_RSSI_W);
        if (w > VFO_RSSI_W)
          w = VFO_RSSI_W; /* ConvertDomain не клампит сверху */
      }
      if (s_rssiBar[i] && w != s_prevRssi[i]) {
        lv_obj_set_size(s_rssiBar[i], w, VFO_RSSI_H);
        s_prevRssi[i] = w;
      }

      /* Доп. точность: только при изменении значения */
      if (s_extraLbl[i] && nv->extraPrecision) {
        if (!NumVal_IsEditing(nv)) {
          uint8_t extra = (vfos[i].rxF % 100000) % 100;
          if (extra != s_prevExtra[i]) {
            static char extraBuf[3];
            s_prevExtra[i] = extra;
            snprintf(extraBuf, sizeof(extraBuf), "%02u", extra);
            lv_label_set_text_static(s_extraLbl[i], extraBuf);
          }
        } else if (s_prevExtra[i] != 0xFF) {
          s_prevExtra[i] = 0xFF;
          lv_label_set_text_static(s_extraLbl[i], "");
        }
      }
    }

    s_inited = true;

    /* Сначала сбросить нарисованное на экран, потом — тяжёлое
       радио-применение (патч SSB): переключение VFO отзывается
       мгновенно, загрузка патча идёт уже после кадра. */
    lv_timer_handler();
    Radio_ApplyPending();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (NumVal_Key(&vfoCtx.numVal[currentVfo], k, state))
      return true;

    if (state == KEY_LONG_PRESSED_CONT) {
      switch (k) {
      case KEY_UP:
        Radio_TuneStep(+1);
        return true;
      case KEY_DOWN:
        Radio_TuneStep(-1);
      default:
        break;
      }
    }

    if (state == KEY_EVT_LONG_PRESS) {
      switch (k) {
      case KEY_4:
        Mode_Switch(MODE_FC);
        return true;
      case KEY_5:
        Mode_Switch(MODE_ANALYSER);
        return true;
      case KEY_0:
        Mode_Switch(MODE_RADIO);
        return true;
      case KEY_SIDE1: /* группа диапазонов: вещание → SSB → службы */
        Radio_NextGroup();
        return true;
      case KEY_SIDE2: /* светлая/тёмная тема (глобально) */
        gThemeLight = !gThemeLight;
        Mode_Switch(*Mode_Current()); /* пересбор экрана с новой темой */
        return true;
      case KEY_8: /* список находок */
        LOOTLIST_OpenFrom(MODE_VFO);
        return true;
      case KEY_9: { /* текущая частота — в белый список находок */
        VFO *v = &vfos[currentVfo];
        Loot *it = LOOT_Add(v->rxF);
        if (it) {
          it->modulation = v->modulation;
          it->bw = v->bw;
          it->radio = v->radio;
          it->blacklist = false;
          it->whitelist = true;
        }
        return true;
      }
      case KEY_1:
        Radio_ToggleMonitor(0);
        return true;
      case KEY_2:
        Radio_ToggleMonitor(1);
        return true;
      case KEY_7:
        Radio_ToggleMonitor(2);
        return true;
      default:
        break;
      }
    }

    if (state != KEY_EVT_RELEASE)
      return false;

    switch (k) {
    case KEY_MENU:
      Mode_Switch(MODE_SCANNER);
      return true;
    case KEY_EXIT:
      Radio_NextVfo();
      initNumVals();
      return true;
    case KEY_UP:
      Radio_TuneStep(+1);
      return true;
    case KEY_DOWN:
      Radio_TuneStep(-1);
      return true;
    case KEY_SIDE1: /* следующий диапазон */
      Radio_NextBand(+1);
      return true;
    case KEY_SIDE2: /* предыдущий диапазон */
      Radio_NextBand(-1);
      return true;
    case KEY_STAR: /* шаг перестройки */
      Radio_NextStep();
      return true;
    case KEY_HASH: /* модуляция */
      vfos[currentVfo].modulation =
          static_cast<ModulationType>((vfos[currentVfo].modulation + 1) % (MOD_WFM + 1));
      Radio_TuneTo(vfos[currentVfo].rxF, true);
      return true;
    default:
      break;
    }
    return false;
  }
};

VfoMode vfoModeInstance;
Mode &MODE_VFO = vfoModeInstance;
