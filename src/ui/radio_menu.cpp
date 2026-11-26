#include "radio_menu.h"

#include "../driver/bk4819.h"
#include "../helper/names.h"
#include "../radio.h"
#include <stdio.h>

static RadioMenuApply s_apply;

static const char *const FILTER_NAMES[] = {"AUTO", "HF", "VHF", "UHF", "800"};

static char s_buf[8];

static uint8_t activeChip(void) {
  return vfos[currentVfo].radio == RADIO_BK4819B ? 1 : 0;
}

static const char *filterName(void) {
  int i = filterIndex + 1;
  if (i < 0 || i > 4)
    i = 0;
  return FILTER_NAMES[i];
}

static const char *radioName(void) {
  const VFO *v = &vfos[currentVfo];
  if (!v->radioManual)
    return "AUTO";
  if (v->radio == RADIO_SI4732)
    return "SI";
  return v->radio == RADIO_BK4819B ? "BK-B" : "BK-A";
}

const char *RadioMenu_Value(const MenuItem *item) {
  VFO *v = &vfos[currentVfo];
  switch (item->id) {
  case RADIO_MENU_STEP:
    return STEP_NAMES[v->step];
  case RADIO_MENU_FILTER:
    return filterName();
  case RADIO_MENU_RADIO:
    return radioName();
  case RADIO_MENU_MOD:
    return MOD_NAMES[v->modulation & 7];
  case RADIO_MENU_BW:
    return BW_NAMES[v->bw % 10];
  case RADIO_MENU_SQL:
    snprintf(s_buf, sizeof(s_buf), "%u", (unsigned)squelchLevel);
    return s_buf;
  case RADIO_MENU_GAIN:
    if (v->gainIndex == AUTO_GAIN_INDEX)
      return "AUTO";
    snprintf(s_buf, sizeof(s_buf), "%udB",
             (unsigned)BK4819_GetGainDb(v->gainIndex));
    return s_buf;
  case RADIO_MENU_AFC: {
    BK4819_SelectChip(activeChip());
    uint8_t afc = BK4819_GetAFC();
    if (!afc)
      return "OFF";
    snprintf(s_buf, sizeof(s_buf), "%u", (unsigned)afc);
    return s_buf;
  }
  default:
    return "";
  }
}

void RadioMenu_Change(const MenuItem *item, int8_t dir) {
  VFO *v = &vfos[currentVfo];
  switch (item->id) {
  case RADIO_MENU_STEP: {
    int s = (int)v->step + dir;
    if (s < 0)
      s = STEP_COUNT - 1;
    else if (s >= STEP_COUNT)
      s = 0;
    v->step = static_cast<Step>(s);
    break;
  }
  case RADIO_MENU_FILTER:
    if (dir < 0) { /* Radio_NextFilter листает только вперёд */
      for (uint8_t i = 0; i < 4; ++i)
        Radio_NextFilter();
    } else {
      Radio_NextFilter();
    }
    break;
  case RADIO_MENU_RADIO:
    Radio_NextReceiver();
    break;
  case RADIO_MENU_MOD: {
    int m = (int)v->modulation + dir;
    if (m < 0)
      m = MOD_WFM;
    else if (m > MOD_WFM)
      m = 0;
    v->modulation = (uint8_t)m;
    break;
  }
  case RADIO_MENU_BW: {
    int b = (int)v->bw + dir;
    if (b < 0)
      b = 9;
    else if (b > 9)
      b = 0;
    v->bw = (uint8_t)b;
    break;
  }
  case RADIO_MENU_SQL: {
    int s = (int)squelchLevel + dir;
    if (s < 0)
      s = 10;
    else if (s > 10)
      s = 0;
    squelchLevel = (uint8_t)s;
    Radio_SetSquelch(squelchLevel);
    break;
  }
  case RADIO_MENU_GAIN: {
    int g = (int)v->gainIndex + dir;
    if (g < 0)
      g = 31;
    else if (g > 31)
      g = 0;
    v->gainIndex = (uint8_t)g;
    break;
  }
  case RADIO_MENU_AFC: {
    BK4819_SelectChip(activeChip());
    int a = (int)BK4819_GetAFC() + dir;
    if (a < 0)
      a = 8;
    else if (a > 8)
      a = 0;
    BK4819_SetAFC((uint8_t)a);
    break;
  }
  default:
    break;
  }

  if (s_apply)
    s_apply((uint8_t)item->id);
}

void RadioMenu_SetApply(RadioMenuApply fn) { s_apply = fn; }
