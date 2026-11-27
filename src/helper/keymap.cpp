#include "keymap.h"

#include "lootlist.h"
#include "settings.h"
#include "store.h"

#include "../driver/board.h"
#include "../modes/mode.h"
#include "../modes/modes.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/menu_overlay.h"
#include "../ui/radio_menu.h"

KeyBinding gKeymap[KEY_COUNT][2];

static const char *const ACTION_NAMES[KA_COUNT] = {
    [KA_NONE] = "NONE",
    [KA_BAND] = "BAND",
    [KA_FC] = "FC",
    [KA_ANALYSER] = "ANALYSER",
    [KA_BC_SCANNER] = "BC SCAN",
    [KA_CHANNELS] = "CHANNELS",
    [KA_BANDS] = "BANDS",
    [KA_SCANNER] = "SCANNER",
    [KA_SETTINGS] = "SETTINGS",
    [KA_LOOTLIST] = "LOOT LIST",
    [KA_SAVE_LOOT] = "SAVE LOOT",
    [KA_CLEAR_LOOT] = "CLEAR LOOT",

    [KA_NEXT_VFO] = "NEXT VFO",
    [KA_VFO_MODE] = "VFO MODE",
    [KA_GROUP_NEXT] = "GROUP",
    [KA_FILTER_NEXT] = "FILTER",
    [KA_RADIO_NEXT] = "RECEIVER",
    [KA_BAND_UP] = "BAND +",
    [KA_BAND_DOWN] = "BAND -",

    [KA_STEP_UP] = "STEP +",
    [KA_STEP_DOWN] = "STEP -",
    [KA_BW_UP] = "BW +",
    [KA_BW_DOWN] = "BW -",
    [KA_MOD_UP] = "MOD +",
    [KA_MOD_DOWN] = "MOD -",
    [KA_SQL_UP] = "SQL +",
    [KA_SQL_DOWN] = "SQL -",
    [KA_GAIN_UP] = "GAIN +",
    [KA_GAIN_DOWN] = "GAIN -",
    [KA_AFC_UP] = "AFC +",
    [KA_AFC_DOWN] = "AFC -",
    [KA_SQUELCH_FORCE] = "SQL OPEN",

    [KA_THEME] = "THEME",
    [KA_ANTENNA] = "ANTENNA",
    [KA_KEYLOCK] = "KEYLOCK",
    [KA_LEVEL_VFO] = "LEVEL VFO",
    [KA_ALWAYS_RSSI] = "ALWAYS RSSI",
    [KA_BRIGHT_UP] = "BRIGHT +",
    [KA_BRIGHT_DOWN] = "BRIGHT -",
    [KA_MULTIWATCH] = "MULTIWATCH",
};

#define AI0 \
  {"Default (app)", nullptr, nullptr, 0}
#define AI(id) [id + 1] = {ACTION_NAMES[id], nullptr, nullptr, (uint16_t)(id + 1)}

static const MenuItem s_actionItems[KA_COUNT + 1] = {
    AI0,
    AI(KA_NONE),
    AI(KA_BAND),
    AI(KA_FC),
    AI(KA_ANALYSER),
    AI(KA_BC_SCANNER),
    AI(KA_CHANNELS),
    AI(KA_BANDS),
    AI(KA_SCANNER),
    AI(KA_SETTINGS),
    AI(KA_LOOTLIST),
    AI(KA_SAVE_LOOT),
    AI(KA_CLEAR_LOOT),
    AI(KA_NEXT_VFO),
    AI(KA_VFO_MODE),
    AI(KA_GROUP_NEXT),
    AI(KA_FILTER_NEXT),
    AI(KA_RADIO_NEXT),
    AI(KA_BAND_UP),
    AI(KA_BAND_DOWN),
    AI(KA_STEP_UP),
    AI(KA_STEP_DOWN),
    AI(KA_BW_UP),
    AI(KA_BW_DOWN),
    AI(KA_MOD_UP),
    AI(KA_MOD_DOWN),
    AI(KA_SQL_UP),
    AI(KA_SQL_DOWN),
    AI(KA_GAIN_UP),
    AI(KA_GAIN_DOWN),
    AI(KA_AFC_UP),
    AI(KA_AFC_DOWN),
    AI(KA_SQUELCH_FORCE),
    AI(KA_THEME),
    AI(KA_ANTENNA),
    AI(KA_KEYLOCK),
    AI(KA_LEVEL_VFO),
    AI(KA_ALWAYS_RSSI),
    AI(KA_BRIGHT_UP),
    AI(KA_BRIGHT_DOWN),
    AI(KA_MULTIWATCH),
};

const MenuItem *Keymap_ActionItems(void) { return s_actionItems; }

uint16_t Keymap_ActionItemCount(void) {
  return (uint16_t)(sizeof(s_actionItems) / sizeof(s_actionItems[0]));
}

void Keymap_LoadDefaults(void) {
  for (uint8_t k = 0; k < KEY_COUNT; ++k) {
    gKeymap[k][0].action = KA_NONE;
    gKeymap[k][0].bound = 0;
    gKeymap[k][1].action = KA_NONE;
    gKeymap[k][1].bound = 0;
  }

  /* Длинные клавиши приложений RT-880. long 1/2/3 не занимаем: multiwatch
   * теперь открывается из меню. */
  gKeymap[KEY_0][1] = {KA_BAND, 1};
  gKeymap[KEY_3][1] = {KA_VFO_MODE, 1};
  gKeymap[KEY_4][1] = {KA_FC, 1};
  gKeymap[KEY_5][1] = {KA_ANALYSER, 1};
  gKeymap[KEY_6][1] = {KA_BC_SCANNER, 1};
  gKeymap[KEY_7][1] = {KA_CHANNELS, 1};
  gKeymap[KEY_8][1] = {KA_BANDS, 1};
  gKeymap[KEY_9][1] = {KA_SAVE_LOOT, 1};
  gKeymap[KEY_HASH][1] = {KA_RADIO_NEXT, 1};
  gKeymap[KEY_SIDE1][1] = {KA_GROUP_NEXT, 1};
  gKeymap[KEY_SIDE2][1] = {KA_FILTER_NEXT, 1};

  /* Короткие клавиши, совпадающие с текущим поведением VFO. */
  gKeymap[KEY_MENU][0] = {KA_SCANNER, 1};
  gKeymap[KEY_EXIT][0] = {KA_NEXT_VFO, 1};
  gKeymap[KEY_STAR][0] = {KA_LOOTLIST, 1};
}

const char *Keymap_ActionName(KeyAction a) {
  if (a < 0 || a >= KA_COUNT)
    return "?";
  return ACTION_NAMES[a];
}

static void leaveChannel(void) {
  if (VFO_ChannelMode(currentVfo)) {
    VFO_SetChannelMode(currentVfo, false);
    Store_MarkDirty();
  }
}

static void saveCurrentToLoot(void) {
  VFO *v = &vfos[currentVfo];
  Loot *it = LOOT_Add(v->rxF);
  if (!it)
    return;
  it->modulation = v->modulation;
  it->bw = v->bw;
  it->radio = v->radio;
  it->blacklist = false;
  it->whitelist = true;
  Store_MarkDirty();
}

static void radioParam(uint16_t id, int8_t dir) {
  MenuItem item = {"", nullptr, nullptr, id};
  RadioMenu_Change(&item, dir);
  if (id == RADIO_MENU_BW || id == RADIO_MENU_MOD || id == RADIO_MENU_GAIN)
    Radio_TuneTo(vfos[currentVfo].rxF, true);
}

static bool runAction(KeyAction a) {
  if (a == KA_NONE)
    return true;

  switch (a) {
  case KA_BAND:
    Mode_Switch(MODE_RADIO);
    return true;
  case KA_FC:
    Mode_Switch(MODE_FC);
    return true;
  case KA_ANALYSER:
    Mode_Switch(MODE_ANALYSER);
    return true;
  case KA_BC_SCANNER:
    Mode_Switch(MODE_BC_SCANNER);
    return true;
  case KA_CHANNELS:
    Mode_Switch(MODE_CHANNELS);
    return true;
  case KA_BANDS:
    Mode_Switch(MODE_BANDLIST);
    return true;
  case KA_SCANNER:
    Mode_Switch(MODE_SCANNER);
    return true;
  case KA_SETTINGS:
    Mode_Switch(MODE_SETTINGS);
    return true;
  case KA_LOOTLIST:
    if (Mode_Current())
      LOOTLIST_OpenFrom(*Mode_Current());
    return true;
  case KA_SAVE_LOOT:
    saveCurrentToLoot();
    return true;
  case KA_CLEAR_LOOT:
    LOOT_Clear();
    Store_MarkDirty();
    return true;

  case KA_NEXT_VFO:
    Radio_NextVfo();
    return true;
  case KA_VFO_MODE:
    if (VFO_ChannelMode(currentVfo)) {
      VFO_SetChannelMode(currentVfo, false);
      Store_MarkDirty();
    } else if (VFO_SetChannelMode(currentVfo, true)) {
      Store_MarkDirty();
    }
    return true;
  case KA_GROUP_NEXT:
    leaveChannel();
    Radio_NextGroup();
    return true;
  case KA_FILTER_NEXT:
    Radio_NextFilter();
    return true;
  case KA_RADIO_NEXT:
    Radio_NextReceiver();
    return true;
  case KA_BAND_UP:
    leaveChannel();
    Radio_NextBand(+1);
    return true;
  case KA_BAND_DOWN:
    leaveChannel();
    Radio_NextBand(-1);
    return true;

  case KA_STEP_UP:
    radioParam(RADIO_MENU_STEP, +1);
    return true;
  case KA_STEP_DOWN:
    radioParam(RADIO_MENU_STEP, -1);
    return true;
  case KA_BW_UP:
    radioParam(RADIO_MENU_BW, +1);
    return true;
  case KA_BW_DOWN:
    radioParam(RADIO_MENU_BW, -1);
    return true;
  case KA_MOD_UP:
    radioParam(RADIO_MENU_MOD, +1);
    return true;
  case KA_MOD_DOWN:
    radioParam(RADIO_MENU_MOD, -1);
    return true;
  case KA_SQL_UP:
    radioParam(RADIO_MENU_SQL, +1);
    return true;
  case KA_SQL_DOWN:
    radioParam(RADIO_MENU_SQL, -1);
    return true;
  case KA_GAIN_UP:
    radioParam(RADIO_MENU_GAIN, +1);
    return true;
  case KA_GAIN_DOWN:
    radioParam(RADIO_MENU_GAIN, -1);
    return true;
  case KA_AFC_UP:
    radioParam(RADIO_MENU_AFC, +1);
    return true;
  case KA_AFC_DOWN:
    radioParam(RADIO_MENU_AFC, -1);
    return true;
  case KA_SQUELCH_FORCE:
    Radio_ToggleSquelchForce();
    return true;

  case KA_THEME:
    gThemeLight = !gThemeLight;
    if (Mode_Current())
      Mode_Switch(*Mode_Current());
    return true;
  case KA_ANTENNA:
    ant_sw(!ant_is_main());
    return true;
  case KA_KEYLOCK:
    Mode_ToggleKeyLock();
    return true;
  case KA_LEVEL_VFO:
    Settings_Set(SET_SHOWLEVELINVFO, !Settings_Get(SET_SHOWLEVELINVFO));
    return true;
  case KA_ALWAYS_RSSI:
    Settings_Set(SET_ALWAYS_RSSI, !Settings_Get(SET_ALWAYS_RSSI));
    return true;
  case KA_BRIGHT_UP:
    Settings_IncDec(SET_BRIGHTNESS_H, true);
    return true;
  case KA_BRIGHT_DOWN:
    Settings_IncDec(SET_BRIGHTNESS_H, false);
    return true;
  case KA_MULTIWATCH:
    SETTINGS_OpenMultiwatch();
    return true;
  default:
    return false;
  }
}

bool Keymap_Handle(KEY_Code_t key, Key_State_t state) {
  if (key >= KEY_COUNT)
    return false;
  uint8_t slot;
  if (state == KEY_RELEASED)
    slot = 0;
  else if (state == KEY_LONG_PRESSED)
    slot = 1;
  else
    return false;

  /* Долгий MENU зарезервирован под системное меню: иначе можно
   * назначить действие и потерять единственный вход в настройки. */
  if (key == KEY_MENU && state == KEY_LONG_PRESSED)
    return false;

  const KeyBinding *b = &gKeymap[key][slot];
  if (!b->bound)
    return false;
  runAction((KeyAction)b->action);
  return true;
}
