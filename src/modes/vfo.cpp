/* Экран VFO — нативный LVGL.
 *
 * Виджеты создаются в enter() и удаляются в exit(). Никаких прямых
 * DMA-заливок и пула лейблов: состояние живёт в виджетах, LVGL сам
 * перерисовывает изменившееся. Редактор частоты — NumVal в нативном
 * режиме (NumVal_BindNative): он обновляет только текст лейбла,
 * курсор и позиционирование — здесь.
 */
#include "modes.h"
#include "../driver/bk4819.h"
#include "../driver/board.h"
#include "../driver/keyboard.h"
#include "../driver/st7789.h"
#include "../helper/log.h"
#include "../helper/lootlist.h"
#include "../helper/measurements.h"
#include "../helper/names.h"
#include "../helper/store.h"
#include "../helper/tone.h"
#include "../helper/unit_fmt.h"
#include "../bands.h"
#include "../misc.h"
#include "../radio.h"
#include "../ui/band_colors.h"
#include "../ui/graphics.h"
#include "../ui/lv_helpers.h"
#include "../ui/menu_overlay.h"
#include "../ui/numval.h"
#include "../ui/radio_menu.h"
#include "../ui/statusline.h"
#include "mode.h"
#include "lvgl.h"
#include "printf.h"
#include <string.h>

#define VFO_H 98
#define STATUS_H 24

#define FREQ_RIGHT 204 /* правый край колонки частоты (цифры XL, 7 знаков) */
/* Крупные цифры сдвинуты влево на один символ среднего шрифта. */
#define FREQ_DIGITS_RIGHT (FREQ_RIGHT - FontW(F_M))
/* Выравнивание частоты в VFO (и при вводе): POS_R — по правому краю,
 * POS_L — по левому. Одна настройка на NumVal, лейбл и курсор. */
#define VFO_FREQ_POS POS_R
#define COL_RIGHT 232 /* правый край колонок справа */
#define MARGIN 8

#define VFO_INFO_Y 6   /* строка диапазон / модуляция (M) */
#define VFO_FREQ_Y 26  /* верх строки частоты (XL) */
#define VFO_NAME_Y 34  /* имя канала: базовая линия как у частоты (L) */
#define VFO_SUB_Y 64   /* строка чип / шаг — сразу под частотой (S) */
#define VFO_BADGE_W 44 /* «BK AUTO» / «MBK VHF» */
#define VFO_TONE_X 52  /* CTCSS/DCS/DTMF — сразу перед AFC */
#define VFO_TONE_W 92
#define VFO_AFC_X 144 /* поправка AFC — между субтонами и шагом */
#define VFO_AFC_W 54
#define VFO_STEP_X 198
#define VFO_STEP_W (COL_RIGHT - VFO_STEP_X)
#define VFO_SMETER_Y 84 /* S-метр: квадратики */
#define VFO_SMETER_H 8
#define VFO_CELLS 16
#define VFO_CELL_W 9
#define VFO_CELL_GAP 2
#define VFO_CELL_PITCH (VFO_CELL_W + VFO_CELL_GAP)
#define VFO_SMETER_W (VFO_CELLS * VFO_CELL_PITCH - VFO_CELL_GAP)
#define VFO_PEAK_H (VFO_SMETER_H + 2)
#define VFO_DBM_X (MARGIN + VFO_SMETER_W + 6)
#define VFO_DBM_W (COL_RIGHT - VFO_DBM_X)
#define VFO_DBM_Y 82
#define VFO_METER_MS 250 /* обновление S-метра (RSSI/дБм/peak) */

struct {
  NumVal_t numVal[DEVICE_VFO_COUNT];
  lv_obj_t *s_scr; /* корневой контейнер экрана */
  lv_obj_t *s_meter; /* общий S-метр активного ряда (рисуется в draw-cb) */
  lv_obj_t *s_freqLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_chNameLbl[DEVICE_VFO_COUNT]; /* имя закреплённого канала */
  lv_obj_t *s_extraLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_badgeLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_modLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_stepLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_bandLbl[DEVICE_VFO_COUNT];
  lv_obj_t *s_afcLbl; /* общий на активный ряд */
  lv_obj_t *s_toneLbl; /* CTCSS/DCS/DTMF активного ряда */
  const char *s_prevBand[DEVICE_VFO_COUNT];
  char s_bandText[DEVICE_VFO_COUNT][24];
  char s_prevChName[DEVICE_VFO_COUNT][VFO_CH_NAME_LEN];
  bool s_prevChValid[DEVICE_VFO_COUNT];
  bool s_prevChMode[DEVICE_VFO_COUNT];
  uint32_t s_prevFreq[DEVICE_VFO_COUNT];
  lv_obj_t *s_peak;
  lv_obj_t *s_dbmLbl;
  lv_obj_t *s_cursor;
  bool s_prevActive[DEVICE_VFO_COUNT];
  uint8_t s_prevMod[DEVICE_VFO_COUNT];
  uint8_t s_prevRadio[DEVICE_VFO_COUNT];
  bool s_prevMon[DEVICE_VFO_COUNT];
  int8_t s_prevFlt[DEVICE_VFO_COUNT];
  uint8_t s_prevStep = 0xFF;
  uint8_t s_meterW;
  uint8_t s_peakW[DEVICE_VFO_COUNT];
  uint32_t s_peakMs[DEVICE_VFO_COUNT];
  uint32_t s_lastMeterMs;
  Color s_prevDbmColor;
  int16_t s_prevDbm = 0x7FFF;
  uint8_t s_prevExtra[DEVICE_VFO_COUNT];
  int16_t s_prevAfc;
  char s_afcBuf[12];
  char s_toneShown[20];
  bool s_inited;
} vfoCtx;

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

/* Ручная настройка выводит VFO из канального режима; закреплённый канал
 * остаётся, чтобы в него можно было вернуться тем же длинным нажатием. */
static void leaveChannelMode(uint8_t idx) {
  if (!VFO_ChannelMode(idx))
    return;
  VFO_SetChannelMode(idx, false);
  Store_MarkDirty();
}

static void onFreqEntered(uint32_t f) {
  leaveChannelMode(currentVfo);
  Radio_TuneToAuto(f);
}

/* ── Меню VFO: параметры приёмника (не на весь экран) ── */

static const MenuItem VFO_MENU[] = {
    RADIO_MENU_ITEM_STEP, RADIO_MENU_ITEM_FILTER, RADIO_MENU_ITEM_RADIO,
    RADIO_MENU_ITEM_MOD,  RADIO_MENU_ITEM_BW,     RADIO_MENU_ITEM_SQL,
    RADIO_MENU_ITEM_GAIN, RADIO_MENU_ITEM_AFC,
};
#define VFO_MENU_COUNT (uint8_t)(sizeof(VFO_MENU) / sizeof(VFO_MENU[0]))

/* Шаг и фильтр RadioMenu меняет сам; остальные параметры применяем полной
 * настройкой VFO (модуляция/полоса/усиление). */
static void vfoMenuApply(uint8_t id) {
  switch (id) {
  case RADIO_MENU_STEP:
  case RADIO_MENU_FILTER:
  case RADIO_MENU_RADIO:
  case RADIO_MENU_SQL:
  case RADIO_MENU_AFC:
    break;
  default:
    Radio_TuneTo(vfos[currentVfo].rxF, true);
    break;
  }
}

static void openVfoMenu(void) {
  MenuOverlay_Open(VFO_MENU, VFO_MENU_COUNT, 0, STATUS_H, LCD_WIDTH / 2,
                   MENU_OVERLAY_BOX_H, false);
}

static void initNumVals() {
  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;
    NumVal_Init(&vfoCtx.numVal[i], FREQ_DIGITS_RIGHT,
                sy + VFO_FREQ_Y + FontAscent(F_XL), F_XL, 0, 0, UNIT_MHZ,
                vfos[i].rxF, 0, 1340 * MHZ, onFreqEntered, VFO_FREQ_POS, F_M,
                0, 0);
  }
}

/* ─── Построение экрана ─── */

/* S-метр — светодиодная линейка: у каждого квадратика свой цвет зоны.
 * Потухшие квадратики остаются приглушённого цвета своей зоны. Ячейка —
 * виртуальный светодиод: горит всегда целиком, а яркость отражает долю её
 * перекрытия баром (50% ширины — 50% яркости). */
static uint16_t meterZoneColor(uint8_t c) {
  if (c < 8) /* S1..S7 */
    return C_GREEN;
  if (c < 12) /* S8..S9+10 */
    return C_YELLOW;
  return C_RED; /* +20..+30 */
}

static void meterDrawCb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = 0;
  for (uint8_t c = 0; c < VFO_CELLS; c++) {
    lv_coord_t x1 = (lv_coord_t)(a.x1 + (int16_t)c * VFO_CELL_PITCH);
    lv_area_t cell = {x1, a.y1, (lv_coord_t)(x1 + VFO_CELL_W - 1),
                      (lv_coord_t)(a.y1 + VFO_SMETER_H - 1)};
    uint16_t zc = meterZoneColor(c);
    d.bg_color = lvColor(zc);
    d.bg_opa = LV_OPA_30; /* светодиод не горит */
    lv_draw_rect(dc, &d, &cell);

    int16_t fw = (int16_t)vfoCtx.s_meterW - (int16_t)(c * VFO_CELL_PITCH);
    if (fw <= 0)
      continue;
    if (fw > VFO_CELL_W)
      fw = VFO_CELL_W;
    /* Светодиод зажигается целиком, яркость — доля его перекрытия. */
    d.bg_opa = (lv_opa_t)((fw * 255 + VFO_CELL_W / 2) / VFO_CELL_W);
    lv_draw_rect(dc, &d, &cell);
  }
}

static void buildScreen(void) {
  vfoCtx.s_scr = lvBox(lv_scr_act(), 0, 0, LCD_WIDTH, LCD_HEIGHT, C_BLACK,
                LV_OPA_TRANSP);

  for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
    uint16_t sy = STATUS_H + i * VFO_H;

    vfoCtx.s_bandLbl[i] = lvLabel(vfoCtx.s_scr, F_M, C_CYAN, MARGIN, sy + VFO_INFO_Y, 120,
                           LV_TEXT_ALIGN_LEFT);
    /* Модуляция — у верхнего края больших цифр, на 2 px ниже. */
    vfoCtx.s_modLbl[i] = lvLabel(vfoCtx.s_scr, F_M, C_GREEN, FREQ_RIGHT + 4,
                          sy + VFO_FREQ_Y + 2, COL_RIGHT - FREQ_RIGHT - 4,
                          LV_TEXT_ALIGN_LEFT);

    vfoCtx.s_freqLbl[i] = lvLabel(vfoCtx.s_scr, F_XL, C_WHITE, 0, sy + VFO_FREQ_Y,
                           FREQ_DIGITS_RIGHT,
                           VFO_FREQ_POS == POS_L ? LV_TEXT_ALIGN_LEFT
                                                 : LV_TEXT_ALIGN_RIGHT);
    /* Имя закреплённого канала — на месте крупной частоты. */
    vfoCtx.s_chNameLbl[i] = lvLabel(vfoCtx.s_scr, F_L, C_WHITE, MARGIN,
                                    sy + VFO_NAME_Y, FREQ_RIGHT - MARGIN,
                                    LV_TEXT_ALIGN_LEFT);
    if (vfoCtx.s_chNameLbl[i])
      lv_obj_add_flag(vfoCtx.s_chNameLbl[i], LV_OBJ_FLAG_HIDDEN);
    vfoCtx.s_extraLbl[i] = lvLabel(vfoCtx.s_scr, F_M, C_WHITE, FREQ_RIGHT + 4,
                            sy + VFO_FREQ_Y + FontAscent(F_XL) -
                                FontAscent(F_M),
                            COL_RIGHT - FREQ_RIGHT - 4, LV_TEXT_ALIGN_LEFT);

    vfoCtx.s_badgeLbl[i] = lvLabel(vfoCtx.s_scr, F_S, C_YELLOW, MARGIN, sy + VFO_SUB_Y,
                            VFO_BADGE_W, LV_TEXT_ALIGN_LEFT);
    vfoCtx.s_stepLbl[i] = lvLabel(vfoCtx.s_scr, F_S, C_YELLOW, VFO_STEP_X,
                           sy + VFO_SUB_Y, VFO_STEP_W, LV_TEXT_ALIGN_RIGHT);
  }

  vfoCtx.s_toneLbl = lvLabel(vfoCtx.s_scr, F_S, C_CYAN, VFO_TONE_X,
                              STATUS_H + VFO_SUB_Y, VFO_TONE_W,
                              LV_TEXT_ALIGN_RIGHT);
  if (vfoCtx.s_toneLbl) {
    lv_obj_add_flag(vfoCtx.s_toneLbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_static(vfoCtx.s_toneLbl, "");
  }
  vfoCtx.s_afcLbl = lvLabel(vfoCtx.s_scr, F_S, C_CYAN, VFO_AFC_X,
                             STATUS_H + VFO_SUB_Y, VFO_AFC_W,
                             LV_TEXT_ALIGN_RIGHT);
  if (vfoCtx.s_afcLbl)
    lv_obj_add_flag(vfoCtx.s_afcLbl, LV_OBJ_FLAG_HIDDEN);

  /* S-метр: один объект с квадратиками, шкала, peak hold и дБм. */
  vfoCtx.s_meter = lvBox(vfoCtx.s_scr, 0, 0, VFO_SMETER_W, VFO_SMETER_H,
                         C_BLACK, LV_OPA_TRANSP);
  if (vfoCtx.s_meter) {
    lv_obj_add_event_cb(vfoCtx.s_meter, meterDrawCb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(vfoCtx.s_meter, LV_OBJ_FLAG_HIDDEN);
  }
  vfoCtx.s_peak = lvBox(vfoCtx.s_scr, 0, 0, 2, VFO_PEAK_H, C_YELLOW,
                        LV_OPA_COVER);
  if (vfoCtx.s_peak)
    lv_obj_add_flag(vfoCtx.s_peak, LV_OBJ_FLAG_HIDDEN);
  vfoCtx.s_dbmLbl = lvLabel(vfoCtx.s_scr, F_M, C_CYAN, VFO_DBM_X, 0, VFO_DBM_W,
                            LV_TEXT_ALIGN_RIGHT);
  if (vfoCtx.s_dbmLbl)
    lv_obj_add_flag(vfoCtx.s_dbmLbl, LV_OBJ_FLAG_HIDDEN);

  vfoCtx.s_cursor = lvBox(vfoCtx.s_scr, 0, 0, 2, 30, C_YELLOW, LV_OPA_COVER);
  if (vfoCtx.s_cursor)
    lv_obj_add_flag(vfoCtx.s_cursor, LV_OBJ_FLAG_HIDDEN);
}

static void layoutMeter(uint8_t i) {
  uint16_t sy = STATUS_H + i * VFO_H;
  if (vfoCtx.s_meter) {
    lv_obj_set_pos(vfoCtx.s_meter, MARGIN, sy + VFO_SMETER_Y);
    setHidden(vfoCtx.s_meter, false);
    lv_obj_invalidate(vfoCtx.s_meter);
  }
  vfoCtx.s_meterW = 0;
  if (vfoCtx.s_toneLbl)
    lv_obj_set_pos(vfoCtx.s_toneLbl, VFO_TONE_X, sy + VFO_SUB_Y);
  if (vfoCtx.s_afcLbl)
    lv_obj_set_pos(vfoCtx.s_afcLbl, VFO_AFC_X, sy + VFO_SUB_Y);
  if (vfoCtx.s_dbmLbl)
    lv_obj_set_y(vfoCtx.s_dbmLbl, sy + VFO_DBM_Y);
  if (vfoCtx.s_peak)
    lv_obj_set_y(vfoCtx.s_peak, sy + VFO_SMETER_Y - 1);
  vfoCtx.s_prevDbm = 0x7FFF;
  vfoCtx.s_prevDbmColor = 0;
  vfoCtx.s_prevAfc = 0x7FFF;
  vfoCtx.s_lastMeterMs = millis() - VFO_METER_MS;
}

static void updateMeter(uint8_t wR) {
  if (!vfoCtx.s_meter || wR == vfoCtx.s_meterW)
    return;
  vfoCtx.s_meterW = wR;
  lv_obj_invalidate(vfoCtx.s_meter);
}

/* Активный ряд: белая частота; неактивные — серым все тексты. */
static void setRowActive(uint8_t i, bool active) {
  Color fg = active ? C_WHITE : C_GRAY;
  if (vfoCtx.s_freqLbl[i])
    lv_obj_set_style_text_color(vfoCtx.s_freqLbl[i], lvColor(fg),
                                LV_PART_MAIN);
  if (vfoCtx.s_chNameLbl[i])
    lv_obj_set_style_text_color(vfoCtx.s_chNameLbl[i], lvColor(fg),
                                LV_PART_MAIN);
  if (vfoCtx.s_extraLbl[i])
    lv_obj_set_style_text_color(vfoCtx.s_extraLbl[i], lvColor(fg),
                                LV_PART_MAIN);
  if (vfoCtx.s_bandLbl[i])
    lv_obj_set_style_text_color(
        vfoCtx.s_bandLbl[i],
        lvColor(active ? Band_Palette565(Band_Color(vfos[i].rxF)) : C_GRAY),
        LV_PART_MAIN);
  if (vfoCtx.s_modLbl[i])
    lv_obj_set_style_text_color(vfoCtx.s_modLbl[i],
                                lvColor(active ? C_GREEN : C_GRAY),
                                LV_PART_MAIN);
  if (vfoCtx.s_badgeLbl[i])
    lv_obj_set_style_text_color(vfoCtx.s_badgeLbl[i],
                                lvColor(active ? C_YELLOW : C_GRAY),
                                LV_PART_MAIN);
  if (active)
    layoutMeter(i);
}

/* ─── Режим ─── */

class VfoMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle("VFO");
    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();
    RadioMenu_SetApply(vfoMenuApply);
    buildScreen();
    initNumVals();
    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++)
      NumVal_BindNative(&vfoCtx.numVal[i], vfoCtx.s_freqLbl[i]);
    memset(vfoCtx.s_prevActive, 0xFF, sizeof(vfoCtx.s_prevActive));
    memset(vfoCtx.s_prevMod, 0xFF, sizeof(vfoCtx.s_prevMod));
    memset(vfoCtx.s_prevRadio, 0xFF, sizeof(vfoCtx.s_prevRadio));
    memset(vfoCtx.s_prevMon, 0xFF, sizeof(vfoCtx.s_prevMon));
    vfoCtx.s_meterW = 0;
    memset(vfoCtx.s_peakW, 0, sizeof(vfoCtx.s_peakW));
    memset(vfoCtx.s_peakMs, 0, sizeof(vfoCtx.s_peakMs));
    vfoCtx.s_lastMeterMs = millis() - VFO_METER_MS;
    vfoCtx.s_prevDbm = 0x7FFF;
    vfoCtx.s_prevDbmColor = 0;
    vfoCtx.s_prevAfc = 0x7FFF;
    vfoCtx.s_toneShown[0] = 0;
    memset(vfoCtx.s_prevExtra, 0xFF, sizeof(vfoCtx.s_prevExtra));
    memset(vfoCtx.s_prevBand, 0, sizeof(vfoCtx.s_prevBand));
    memset(vfoCtx.s_prevChName, 0, sizeof(vfoCtx.s_prevChName));
    memset(vfoCtx.s_prevChValid, 0, sizeof(vfoCtx.s_prevChValid));
    memset(vfoCtx.s_prevChMode, 0, sizeof(vfoCtx.s_prevChMode));
    memset(vfoCtx.s_prevFreq, 0, sizeof(vfoCtx.s_prevFreq));
    vfoCtx.s_prevStep = 0xFF;
    vfoCtx.s_inited = false;
  }

  void exit() override {
    MenuOverlay_Close();
    RadioMenu_SetApply(nullptr);
    if (vfoCtx.s_scr) {
      lv_obj_del(vfoCtx.s_scr);
      vfoCtx.s_scr = NULL;
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
    bool first = !vfoCtx.s_inited;
    STATUSLINE_render();

    for (uint8_t i = 0; i < DEVICE_VFO_COUNT; i++) {
      uint16_t sy = STATUS_H + i * VFO_H;
      bool active = (i == currentVfo);
      if (!vfoCtx.s_freqLbl[i]) continue; /* пул кончился — пропускаем ряд */

      if (first || active != vfoCtx.s_prevActive[i]) {
        setRowActive(i, active);
        vfoCtx.s_prevActive[i] = active;
      }

      NumVal_t *nv = &vfoCtx.numVal[i];
      bool editing = NumVal_IsEditing(nv);
      NumVal_Render(nv);

      /* Редактор: выравнивание то же, что и в обычном виде (VFO_FREQ_POS) —
         значение не «уезжает» при вводе. Курсор — сразу после последней
         введённой цифры: у правого края (POS_R) или правее набранного
         (POS_L). На выходе курсор спрятан. */
      if (editing) {
        if (vfoCtx.s_cursor) {
          lv_coord_t cx = (VFO_FREQ_POS == POS_L)
                              ? 4 + nv->cursor * nv->charW
                              : FREQ_DIGITS_RIGHT + 2;
          lv_obj_set_pos(vfoCtx.s_cursor, cx, sy + VFO_FREQ_Y + 4);
          setHidden(vfoCtx.s_cursor, !nv->blinkState);
        }
      } else if (vfoCtx.s_cursor && i == currentVfo) {
        setHidden(vfoCtx.s_cursor, true);
      }

      const VFOChannel *vc = VFO_GetChannel(i);
      const char *cn = vc ? vc->name : "";
      bool chActive = vc != nullptr && VFO_ChannelMode(i);
      bool chChanged = (vc != nullptr) != vfoCtx.s_prevChValid[i] ||
                       (vc && strcmp(cn, vfoCtx.s_prevChName[i]) != 0) ||
                       chActive != vfoCtx.s_prevChMode[i];
      bool showName = chActive && !editing;

      if (vfoCtx.s_badgeLbl[i] &&
          (first || vfos[i].modulation != vfoCtx.s_prevMod[i] ||
           vfos[i].monitor != vfoCtx.s_prevMon[i] ||
           vfos[i].radio != vfoCtx.s_prevRadio[i] ||
           filterIndex != vfoCtx.s_prevFlt[i] || chChanged)) {
        static const char *const BADGE[2][5] = {
            {"BK AUTO", "BK HF", "BK VHF", "BK UHF", "BK 800"},
            {"MBK AUTO", "MBK HF", "MBK VHF", "MBK UHF", "MBK 800"},
        };
        bool mon = vfos[i].monitor || VFO_ChannelMode(i);
        const char *bdg;
        if (vfos[i].radio == RADIO_SI4732)
          bdg = mon ? "MSI" : "SI";
        else
          bdg = BADGE[mon ? 1 : 0][filterIndex + 1];
        lv_label_set_text_static(vfoCtx.s_badgeLbl[i], bdg);
        lv_label_set_text_static(vfoCtx.s_modLbl[i], MOD_NAMES[vfos[i].modulation]);
        vfoCtx.s_prevMod[i] = vfos[i].modulation;
        vfoCtx.s_prevMon[i] = vfos[i].monitor;
        vfoCtx.s_prevRadio[i] = vfos[i].radio;
        vfoCtx.s_prevFlt[i] = filterIndex;
      }

      /* Диапазон: с закреплённым каналом крупную частоту заменяет имя, а
       * собственная частота VFO остаётся мелким текстом в этой строке. */
      if (vfoCtx.s_bandLbl[i]) {
        const char *bn = Band_Name(vfos[i].rxF);
        if (first || bn != vfoCtx.s_prevBand[i] || chChanged ||
            vfos[i].rxF != vfoCtx.s_prevFreq[i]) {
          if (chActive) {
            char fbuf[12];
            fmt_mhz3(fbuf, sizeof(fbuf), vfos[i].rxF);
            snprintf(vfoCtx.s_bandText[i], sizeof(vfoCtx.s_bandText[i]),
                     "%s %s", bn, fbuf);
          } else if (vc) {
            snprintf(vfoCtx.s_bandText[i], sizeof(vfoCtx.s_bandText[i]),
                     "%s | %s", bn, vc->name[0] ? vc->name : "CH");
          } else {
            snprintf(vfoCtx.s_bandText[i], sizeof(vfoCtx.s_bandText[i]), "%s",
                     bn);
          }
          lv_label_set_text_static(vfoCtx.s_bandLbl[i], vfoCtx.s_bandText[i]);
          if (active)
            lv_obj_set_style_text_color(
                vfoCtx.s_bandLbl[i],
                lvColor(Band_Palette565(Band_Color(vfos[i].rxF))),
                LV_PART_MAIN);
          vfoCtx.s_prevBand[i] = bn;
          vfoCtx.s_prevFreq[i] = vfos[i].rxF;
        }
      }

      if (vfoCtx.s_chNameLbl[i]) {
        if (chChanged)
          lv_label_set_text_static(vfoCtx.s_chNameLbl[i],
                                   vc && vc->name[0] ? vc->name : "CH");
        setHidden(vfoCtx.s_chNameLbl[i], !showName);
      }
      setHidden(vfoCtx.s_freqLbl[i], showName);
      if (vfoCtx.s_extraLbl[i])
        setHidden(vfoCtx.s_extraLbl[i], showName);

      vfoCtx.s_prevChValid[i] = vc != nullptr;
      vfoCtx.s_prevChMode[i] = chActive;
      if (vc)
        memcpy(vfoCtx.s_prevChName[i], cn, VFO_CH_NAME_LEN);
      else
        memset(vfoCtx.s_prevChName[i], 0, VFO_CH_NAME_LEN);

      if (vfoCtx.s_stepLbl[i]) {
        if (active) {
          setHidden(vfoCtx.s_stepLbl[i], false);
          if (first || vfos[i].step != vfoCtx.s_prevStep) {
            static char stepBuf[10];
            uint32_t step = StepFrequencyTable[vfos[i].step];
            fmt_step(stepBuf, sizeof(stepBuf), step);
            lv_label_set_text_static(vfoCtx.s_stepLbl[i], stepBuf);
            vfoCtx.s_prevStep = vfos[i].step;
          }
        } else {
          setHidden(vfoCtx.s_stepLbl[i], true);
        }
      }

      /* CTCSS/DCS/DTMF активного приёмника — рядом с AFC. */
      if (active) {
        char tone[20];
        Tone_GetStatus(tone, sizeof(tone));
        if (strcmp(tone, vfoCtx.s_toneShown)) {
          strcpy(vfoCtx.s_toneShown, tone);
          if (vfoCtx.s_toneLbl)
            lv_label_set_text_static(vfoCtx.s_toneLbl, vfoCtx.s_toneShown);
        }
        setHidden(vfoCtx.s_toneLbl, tone[0] == 0);
      }

      /* S-метр активного VFO: квадратики по dBm, шкала S1..S9+, peak hold,
       * рядом — значение в дБм без подписи. У SI своих измерений пока нет;
       * обновляем раз в 250 мс. */
      if (active) {
        bool haveMeter = vfos[i].radio != RADIO_SI4732;
        uint32_t now = millis();
        bool due = (uint32_t)(now - vfoCtx.s_lastMeterMs) >= VFO_METER_MS;
        if (due) {
          vfoCtx.s_lastMeterMs = now;
          uint8_t wR = 0;
          Color cR = C_GREEN;
          int16_t dbm = 0;
          int16_t afc = 0;
          bool rx = false;
          if (haveMeter) {
            BK4819_SelectChip(vfos[i].radio == RADIO_BK4819B ? 1 : 0);
            uint16_t rssi = BK4819_GetRSSI();
            afc = BK4819_GetAFCValue();
            rx = (squelchLevel == 0) || BK4819_IsSquelchOpen();
            dbm = Rssi2DBm(rssi);
            const uint8_t *r2 = rssi2s[vfos[i].rxF >= 30 * MHZ ? 1 : 0];
            int lo = -(int)r2[0];
            int hi = -(int)r2[11];
            wR = (uint8_t)ConvertDomain(dbm, lo, hi, 0, VFO_SMETER_W);
            uint8_t cell = (uint8_t)(wR / VFO_CELL_PITCH);
            if (cell >= VFO_CELLS)
              cell = VFO_CELLS - 1;
            cR = meterZoneColor(cell);
          }
          updateMeter(wR);

          if (haveMeter) {
            if (vfoCtx.s_dbmLbl && cR != vfoCtx.s_prevDbmColor) {
              lv_obj_set_style_text_color(vfoCtx.s_dbmLbl, lvColor(cR),
                                          LV_PART_MAIN);
              vfoCtx.s_prevDbmColor = cR;
            }
            if (wR >= vfoCtx.s_peakW[i]) {
              vfoCtx.s_peakW[i] = wR;
              vfoCtx.s_peakMs[i] = now;
            } else if (now - vfoCtx.s_peakMs[i] > 600 &&
                       vfoCtx.s_peakW[i] > wR) {
              vfoCtx.s_peakW[i]--;
              vfoCtx.s_peakMs[i] = now;
            }
            if (vfoCtx.s_peak && vfoCtx.s_peakW[i] > 0) {
              lv_obj_set_x(vfoCtx.s_peak, MARGIN + vfoCtx.s_peakW[i] - 1);
              setHidden(vfoCtx.s_peak, false);
            } else if (vfoCtx.s_peak) {
              setHidden(vfoCtx.s_peak, true);
            }
            if (vfoCtx.s_dbmLbl && dbm != vfoCtx.s_prevDbm) {
              static char dbmBuf[8];
              snprintf(dbmBuf, sizeof(dbmBuf), "%+d", dbm);
              lv_label_set_text_static(vfoCtx.s_dbmLbl, dbmBuf);
              vfoCtx.s_prevDbm = dbm;
            }
            if (vfoCtx.s_dbmLbl)
              setHidden(vfoCtx.s_dbmLbl, false);
            if (vfoCtx.s_afcLbl) {
              if (rx) {
                if (afc != vfoCtx.s_prevAfc) {
                  vfoCtx.s_prevAfc = afc;
                  if (afc)
                    snprintf(vfoCtx.s_afcBuf, sizeof(vfoCtx.s_afcBuf), "AFC %+d",
                             (int)afc);
                  else
                    snprintf(vfoCtx.s_afcBuf, sizeof(vfoCtx.s_afcBuf), "AFC 0");
                  lv_label_set_text_static(vfoCtx.s_afcLbl, vfoCtx.s_afcBuf);
                }
                setHidden(vfoCtx.s_afcLbl, false);
              } else {
                /* Без приёма AFC не информативен. */
                setHidden(vfoCtx.s_afcLbl, true);
              }
            }
          } else {
            if (vfoCtx.s_peak)
              setHidden(vfoCtx.s_peak, true);
            if (vfoCtx.s_dbmLbl)
              setHidden(vfoCtx.s_dbmLbl, true);
            if (vfoCtx.s_afcLbl)
              setHidden(vfoCtx.s_afcLbl, true);
          }
        } else if (!haveMeter) {
          if (vfoCtx.s_peak)
            setHidden(vfoCtx.s_peak, true);
          if (vfoCtx.s_dbmLbl)
            setHidden(vfoCtx.s_dbmLbl, true);
          if (vfoCtx.s_afcLbl)
            setHidden(vfoCtx.s_afcLbl, true);
        }
      }

      /* Доп. точность: только при изменении значения */
      if (vfoCtx.s_extraLbl[i] && nv->extraPrecision) {
        if (!NumVal_IsEditing(nv)) {
          uint8_t extra = (vfos[i].rxF % 100000) % 100;
          if (extra != vfoCtx.s_prevExtra[i]) {
            static char extraBuf[3];
            vfoCtx.s_prevExtra[i] = extra;
            snprintf(extraBuf, sizeof(extraBuf), "%02u", extra);
            lv_label_set_text_static(vfoCtx.s_extraLbl[i], extraBuf);
          }
        } else if (vfoCtx.s_prevExtra[i] != 0xFF) {
          vfoCtx.s_prevExtra[i] = 0xFF;
          lv_label_set_text_static(vfoCtx.s_extraLbl[i], "");
        }
      }
    }

    vfoCtx.s_inited = true;

    /* Сначала сбросить нарисованное на экран, потом — тяжёлое
       радио-применение (патч SSB): переключение VFO отзывается
       мгновенно, загрузка патча идёт уже после кадра. */
    MenuOverlay_Render();
    lv_timer_handler();
    Radio_ApplyPending();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (MenuOverlay_IsOpen()) {
      if (MenuOverlay_Key(k, state))
        return true;
      LOOTLIST_OpenFrom(MODE_VFO); /* STAR закрыл меню */
      return true;
    }

    /* Ввод частоты/шаг/смена диапазона — ручная настройка, канал снимаем. */
    if (state == KEY_EVT_RELEASE && k >= KEY_0 && k <= KEY_9 &&
        !NumVal_IsEditing(&vfoCtx.numVal[currentVfo]))
      leaveChannelMode(currentVfo);

    if (NumVal_Key(&vfoCtx.numVal[currentVfo], k, state))
      return true;

    if (state == KEY_LONG_PRESSED_CONT) {
      switch (k) {
      case KEY_UP:
        leaveChannelMode(currentVfo);
        Radio_TuneStep(+1);
        return true;
      case KEY_DOWN:
        leaveChannelMode(currentVfo);
        Radio_TuneStep(-1);
        return true;
      default:
        break;
      }
    }

    if (state == KEY_EVT_LONG_PRESS) {
      switch (k) {
      case KEY_MENU: /* меню-оверлей: параметры приёмника */
        if (!NumVal_IsEditing(&vfoCtx.numVal[currentVfo]))
          openVfoMenu();
        return true;
      case KEY_4:
        Mode_Switch(MODE_FC);
        return true;
      case KEY_5:
        Mode_Switch(MODE_ANALYSER);
        return true;
      case KEY_6:
        Mode_Switch(MODE_BC_SCANNER);
        return true;
      case KEY_7: /* сохранённые каналы и скан по ним */
        Mode_Switch(MODE_CHANNELS);
        return true;
      case KEY_0:
        Mode_Switch(MODE_RADIO);
        return true;
      case KEY_SIDE1: /* группа диапазонов: вещание → SSB → службы */
        leaveChannelMode(currentVfo);
        Radio_NextGroup();
        return true;
      case KEY_SIDE2: /* фильтр: AUTO → HF → VHF → UHF → 800 */
        Radio_NextFilter();
        return true;
      case KEY_8: /* список диапазонов: правка плана во флеше */
        Mode_Switch(MODE_BANDLIST);
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
      case KEY_HASH: /* приёмник: AUTO ↔ BK/SI, если чип поддерживает */
        Radio_NextReceiver();
        return true;
      case KEY_1: /* канальный VFO — выключить; активный — включить;
                     остальные — multiwatch */
      case KEY_2:
      case KEY_3: {
        uint8_t idx = (uint8_t)(k - KEY_1);
        if (VFO_ChannelMode(idx)) {
          VFO_SetChannelMode(idx, false);
          Store_MarkDirty();
        } else if (idx == currentVfo) {
          if (VFO_SetChannelMode(idx, true))
            Store_MarkDirty();
        } else {
          Radio_ToggleMonitor(idx);
        }
        return true;
      }
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
      leaveChannelMode(currentVfo);
      Radio_TuneStep(+1);
      return true;
    case KEY_DOWN:
      leaveChannelMode(currentVfo);
      Radio_TuneStep(-1);
      return true;
    case KEY_SIDE1: /* BK: принудительно открыть/закрыть шумодав; SI: диапазон */
      if (vfos[currentVfo].radio != RADIO_SI4732) {
        Radio_ToggleSquelchForce();
        return true;
      }
      leaveChannelMode(currentVfo);
      Radio_NextBand(+1);
      return true;
    case KEY_SIDE2: /* на BK диапазоны не листаем; SI — предыдущий */
      if (vfos[currentVfo].radio != RADIO_SI4732)
        return true;
      leaveChannelMode(currentVfo);
      Radio_NextBand(-1);
      return true;
    case KEY_STAR: /* список находок (шаг — в длинном MENU) */
      LOOTLIST_OpenFrom(MODE_VFO);
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
