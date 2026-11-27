#include "modes.h"

#include "../helper/keymap.h"
#include "../helper/measurements.h"
#include "../helper/settings.h"
#include "../helper/store.h"
#include "../helper/unit_fmt.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/menu_overlay.h"
#include "../ui/radio_menu.h"
#include "../ui/screen_layout.h"
#include "../ui/statusline.h"
#include "mode.h"

#include <stdio.h>
#include <string.h>

enum {
  PAGE_MAIN,
  PAGE_SCAN,
  PAGE_RADIO,
  PAGE_EQ,
  PAGE_DISPLAY,
  PAGE_BATTERY,
  PAGE_RECEIVER,
  PAGE_MULTI,
  PAGE_BUTTONS,
  PAGE_ACTION,
};

#define PAGE_COUNT PAGE_ACTION
#define SET_ITEM(id) ((uint16_t)(0x8000u | (uint16_t)(id)))
#define IS_SET_ITEM(id) (((id) & 0x8000u) != 0u)
#define SET_OF(id) ((Setting)((id) & 0x7FFFu))

static const char *const PAGE_TITLE[] = {
    "SETTINGS", "SCAN", "RADIO", "EQUAL", "DISPLAY",
    "BATTERY",  "RECEIVER", "MULTI", "BUTTONS", "ACTION",
};

static const char *settingValue(const MenuItem *item) {
  return Settings_ValueString(SET_OF(item->id));
}

static void settingChange(const MenuItem *item, int8_t dir) {
  Settings_IncDec(SET_OF(item->id), dir > 0);
}

static const MenuItem MAIN_ITEMS[] = {
    {"Scan", nullptr, nullptr, PAGE_SCAN},
    {"Radio", nullptr, nullptr, PAGE_RADIO},
    {"Equalizer", nullptr, nullptr, PAGE_EQ},
    {"Display", nullptr, nullptr, PAGE_DISPLAY},
    {"Battery", nullptr, nullptr, PAGE_BATTERY},
    {"Receiver", nullptr, nullptr, PAGE_RECEIVER},
    {"Multiwatch", nullptr, nullptr, PAGE_MULTI},
    {"Buttons", nullptr, nullptr, PAGE_BUTTONS},
    {"Beep", settingValue, settingChange, SET_ITEM(SET_BEEP)},
    {"Main app", settingValue, settingChange, SET_ITEM(SET_MAINAPP)},
    {"TX enable", settingValue, settingChange, SET_ITEM(SET_TX_ENABLE)},
    {"TX power", settingValue, settingChange, SET_ITEM(SET_TXPOWER)},
    {"Lock PTT", settingValue, settingChange, SET_ITEM(SET_PTT_LOCK)},
    {"Keylock", settingValue, settingChange, SET_ITEM(SET_KEYLOCK)},
    {"Active VFO", settingValue, settingChange, SET_ITEM(SET_ACTIVEVFO)},
};
#define MAIN_COUNT (sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0]))

static const MenuItem SCAN_ITEMS[] = {
    {"SQL open", settingValue, settingChange, SET_ITEM(SET_SQLOPENTIME)},
    {"SQL close", settingValue, settingChange, SET_ITEM(SET_SQLCLOSETIME)},
    {"FC time", settingValue, settingChange, SET_ITEM(SET_FCTIME)},
    {"Listen t/o", settingValue, settingChange, SET_ITEM(SET_SQOPENEDTIMEOUT)},
    {"Stay time", settingValue, settingChange, SET_ITEM(SET_SQCLOSEDTIMEOUT)},
    {"Skip X_X", settingValue, settingChange,
     SET_ITEM(SET_SKIPGARBAGEFREQUENCIES)},
    {"Battery save", settingValue, settingChange, SET_ITEM(SET_BATSAVE)},
    {"Scan mode", settingValue, settingChange, SET_ITEM(SET_SCANMODE)},
    {"Scanlist", settingValue, settingChange, SET_ITEM(SET_CURRENTSCANLIST)},
    {"Scan delay", settingValue, settingChange, SET_ITEM(SET_SCANDELAY)},
};
#define SCAN_COUNT (sizeof(SCAN_ITEMS) / sizeof(SCAN_ITEMS[0]))

static const MenuItem RADIO_ITEMS[] = {
    {"DTMF decode", settingValue, settingChange, SET_ITEM(SET_DTMFDECODE)},
    {"Filter bound", settingValue, settingChange, SET_ITEM(SET_BOUND240_280)},
    {"SI power off", settingValue, settingChange, SET_ITEM(SET_SI4732POWEROFF)},
    {"STE", settingValue, settingChange, SET_ITEM(SET_STE)},
    {"Tone local", settingValue, settingChange, SET_ITEM(SET_TONELOCAL)},
    {"Mic", settingValue, settingChange, SET_ITEM(SET_MIC)},
    {"Upconv", settingValue, settingChange, SET_ITEM(SET_UPCONVERTER)},
    {"TX time", settingValue, settingChange, SET_ITEM(SET_TXTIME)},
    {"Busy TX", settingValue, settingChange, SET_ITEM(SET_BUSYCHANNELTXLOCK)},
    {"Rep STE", settingValue, settingChange, SET_ITEM(SET_REPEATERSTE)},
    {"Deviation", settingValue, settingChange, SET_ITEM(SET_DEVIATION)},
};
#define RADIO_COUNT (sizeof(RADIO_ITEMS) / sizeof(RADIO_ITEMS[0]))

static const MenuItem EQ_ITEMS[] = {
    {"RX 300Hz", settingValue, settingChange, SET_ITEM(SET_AF_RX_300)},
    {"RX 3kHz", settingValue, settingChange, SET_ITEM(SET_AF_RX_3K)},
    {"TX 300Hz", settingValue, settingChange, SET_ITEM(SET_AF_TX_300)},
    {"TX 3kHz", settingValue, settingChange, SET_ITEM(SET_AF_TX_3K)},
};
#define EQ_COUNT (sizeof(EQ_ITEMS) / sizeof(EQ_ITEMS[0]))

static const MenuItem DISPLAY_ITEMS[] = {
    {"Gamma", settingValue, settingChange, SET_ITEM(SET_GAMMA)},
    {"BL max", settingValue, settingChange, SET_ITEM(SET_BRIGHTNESS_H)},
    {"BL min", settingValue, settingChange, SET_ITEM(SET_BRIGHTNESS_L)},
    {"BL time", settingValue, settingChange, SET_ITEM(SET_BACKLIGHT)},
    {"BL SQL mode", settingValue, settingChange,
     SET_ITEM(SET_BACKLIGHTONSQUELCH)},
    {"CH display", settingValue, settingChange, SET_ITEM(SET_CHDISPLAYMODE)},
    {"Level in VFO", settingValue, settingChange,
     SET_ITEM(SET_SHOWLEVELINVFO)},
    {"Always RSSI", settingValue, settingChange, SET_ITEM(SET_ALWAYS_RSSI)},
};
#define DISPLAY_COUNT (sizeof(DISPLAY_ITEMS) / sizeof(DISPLAY_ITEMS[0]))

static const MenuItem BATTERY_ITEMS[] = {
    {"Bat type", settingValue, settingChange, SET_ITEM(SET_BATTERYTYPE)},
    {"Bat style", settingValue, settingChange, SET_ITEM(SET_BATTERYSTYLE)},
    {"BAT cal", settingValue, settingChange, SET_ITEM(SET_BATTERYCALIBRATION)},
};
#define BATTERY_COUNT (sizeof(BATTERY_ITEMS) / sizeof(BATTERY_ITEMS[0]))

static const MenuItem RECEIVER_ITEMS[] = {
    RADIO_MENU_ITEM_STEP,  RADIO_MENU_ITEM_FILTER, RADIO_MENU_ITEM_RADIO,
    RADIO_MENU_ITEM_MOD,   RADIO_MENU_ITEM_BW,     RADIO_MENU_ITEM_SQL,
    RADIO_MENU_ITEM_GAIN,  RADIO_MENU_ITEM_AFC,
};
#define RECEIVER_COUNT (sizeof(RECEIVER_ITEMS) / sizeof(RECEIVER_ITEMS[0]))

static uint8_t s_page;
static uint8_t s_stack[6];
static uint8_t s_depth;
static bool s_openMulti;

static uint8_t s_editKey;
static uint8_t s_editSlot;
static char s_multiBuf[DEVICE_VFO_COUNT][24];
static char s_multiText[28];

static void settingsRadioApply(uint8_t id) {
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

static const char *multiValue(const MenuItem *item) {
  uint8_t i = (uint8_t)item->id;
  if (i >= DEVICE_VFO_COUNT)
    return "";
  fmt_mhz3(s_multiBuf[i], sizeof(s_multiBuf[i]), vfos[i].rxF);
  snprintf(s_multiText, sizeof(s_multiText), "%s %s", s_multiBuf[i],
           vfos[i].monitor ? "MON" : "---");
  return s_multiText;
}

static void multiToggle(uint8_t i) {
  if (i >= DEVICE_VFO_COUNT)
    return;
  vfos[i].monitor = !vfos[i].monitor;
  Store_MarkDirty();
}

static void multiChange(const MenuItem *item, int8_t dir) {
  (void)dir;
  multiToggle((uint8_t)item->id);
}

static const MenuItem MULTI_ITEMS[DEVICE_VFO_COUNT] = {
    {"VFO 1", multiValue, multiChange, 0},
    {"VFO 2", multiValue, multiChange, 1},
    {"VFO 3", multiValue, multiChange, 2},
};

static bool keyMapLocked(uint8_t idx) {
  uint8_t key = (uint8_t)(idx / 2);
  uint8_t slot = (uint8_t)(idx & 1);
  if (key == KEY_MENU && slot == 1)
    return true; /* long MENU всегда открывает меню */
  if (key == KEY_STAR && slot == 1)
    return true; /* глобальная блокировка */
  if (key == KEY_EXIT && slot == 1)
    return true; /* глобальное выключение дисплея */
  if (key == KEY_ALARM)
    return true; /* глобальные A1/A2 и тема */
  return false;
}

static const char *buttonValue(const MenuItem *item) {
  uint8_t idx = (uint8_t)item->id;
  if (keyMapLocked(idx))
    return "GLOBAL";
  const KeyBinding *b = &gKeymap[idx / 2][idx & 1];
  if (!b->bound)
    return "app";
  return Keymap_ActionName((KeyAction)b->action);
}

static void buttonChange(const MenuItem *item, int8_t dir) {
  uint8_t idx = (uint8_t)item->id;
  if (keyMapLocked(idx))
    return;
  KeyBinding *b = &gKeymap[idx / 2][idx & 1];
  int a = b->bound ? (int)b->action : 0;
  a += dir > 0 ? 1 : -1;
  if (a < 0)
    a = KA_COUNT - 1;
  if (a >= KA_COUNT)
    a = 0;
  b->action = (uint8_t)a;
  b->bound = 1;
  Store_MarkDirty();
}

#define BTN(i, n) {n, buttonValue, buttonChange, i}

static const MenuItem BUTTON_ITEMS[KEY_COUNT * 2] = {
    BTN(0, "- short"),     BTN(1, "- long"),      BTN(2, "0 short"),
    BTN(3, "0 long"),      BTN(4, "1 short"),     BTN(5, "1 long"),
    BTN(6, "2 short"),     BTN(7, "2 long"),      BTN(8, "3 short"),
    BTN(9, "3 long"),      BTN(10, "4 short"),    BTN(11, "4 long"),
    BTN(12, "5 short"),    BTN(13, "5 long"),     BTN(14, "6 short"),
    BTN(15, "6 long"),     BTN(16, "7 short"),    BTN(17, "7 long"),
    BTN(18, "8 short"),    BTN(19, "8 long"),     BTN(20, "9 short"),
    BTN(21, "9 long"),     BTN(22, "MENU short"), BTN(23, "MENU long"),
    BTN(24, "UP short"),   BTN(25, "UP long"),    BTN(26, "DOWN short"),
    BTN(27, "DOWN long"),  BTN(28, "EXIT short"), BTN(29, "EXIT long"),
    BTN(30, "STAR short"), BTN(31, "STAR long"),  BTN(32, "# short"),
    BTN(33, "# long"),     BTN(34, "PTT short"),  BTN(35, "PTT long"),
    BTN(36, "S1 short"),   BTN(37, "S1 long"),    BTN(38, "S2 short"),
    BTN(39, "S2 long"),    BTN(40, "ALM short"),  BTN(41, "ALM long"),
};
#define BUTTON_COUNT (sizeof(BUTTON_ITEMS) / sizeof(BUTTON_ITEMS[0]))

static const MenuItem *pageItems(uint8_t page, uint8_t *count) {
  switch (page) {
  case PAGE_MAIN:
    *count = MAIN_COUNT;
    return MAIN_ITEMS;
  case PAGE_SCAN:
    *count = SCAN_COUNT;
    return SCAN_ITEMS;
  case PAGE_RADIO:
    *count = RADIO_COUNT;
    return RADIO_ITEMS;
  case PAGE_EQ:
    *count = EQ_COUNT;
    return EQ_ITEMS;
  case PAGE_DISPLAY:
    *count = DISPLAY_COUNT;
    return DISPLAY_ITEMS;
  case PAGE_BATTERY:
    *count = BATTERY_COUNT;
    return BATTERY_ITEMS;
  case PAGE_RECEIVER:
    *count = RECEIVER_COUNT;
    return RECEIVER_ITEMS;
  case PAGE_MULTI:
    *count = DEVICE_VFO_COUNT;
    return MULTI_ITEMS;
  case PAGE_BUTTONS:
    *count = BUTTON_COUNT;
    return BUTTON_ITEMS;
  case PAGE_ACTION:
    *count = (uint8_t)Keymap_ActionItemCount();
    return Keymap_ActionItems();
  default:
    *count = 0;
    return nullptr;
  }
}

static void renderPage(void) {
  uint8_t count = 0;
  const MenuItem *items = pageItems(s_page, &count);
  STATUSLINE_SetTitle(PAGE_TITLE[s_page]);
  MenuOverlay_Open(items, count, 0, UI_STATUS_H, LCD_WIDTH,
                   LCD_HEIGHT - UI_STATUS_H, false);
}

static void openPage(uint8_t page) {
  s_page = page;
  renderPage();
}

static void openMain(void) {
  s_depth = 0;
  s_stack[0] = PAGE_MAIN;
  openPage(PAGE_MAIN);
}

static void pushPage(uint8_t page) {
  if (s_depth + 1 >= sizeof(s_stack) / sizeof(s_stack[0]))
    return;
  s_stack[++s_depth] = page;
  openPage(page);
}

static void popPage(void) {
  if (s_depth == 0) {
    MenuOverlay_Close();
    Mode_Switch(MODE_VFO);
    return;
  }
  s_depth--;
  openPage(s_stack[s_depth]);
}

static void openActionPicker(void) {
  pushPage(PAGE_ACTION);
  const KeyBinding *b = &gKeymap[s_editKey][s_editSlot];
  MenuOverlay_SetSelected(b->bound ? (uint8_t)(b->action + 1) : 0);
}

static void enterSelected(void) {
  uint8_t sel = MenuOverlay_Selected();
  switch (s_page) {
  case PAGE_MAIN: {
    uint16_t id = MAIN_ITEMS[sel].id;
    if (!IS_SET_ITEM(id) && id < PAGE_COUNT)
      pushPage((uint8_t)id);
    break;
  }
  case PAGE_BUTTONS: {
    uint8_t idx = (uint8_t)sel;
    if (keyMapLocked(idx))
      break;
    s_editKey = (uint8_t)(idx / 2);
    s_editSlot = (uint8_t)(idx & 1);
    openActionPicker();
    break;
  }
  case PAGE_ACTION: {
    KeyBinding *b = &gKeymap[s_editKey][s_editSlot];
    if (sel == 0) {
      b->action = KA_NONE;
      b->bound = 0;
    } else {
      b->action = (uint8_t)(sel - 1);
      b->bound = 1;
    }
    Store_MarkDirty();
    s_depth--;
    openPage(PAGE_BUTTONS);
    MenuOverlay_SetSelected((uint8_t)(s_editKey * 2 + s_editSlot));
    break;
  }
  default:
    break;
  }
}

class SettingsMode : public Mode {
public:
  void enter() override {
    STATUSLINE_SetTitle(PAGE_TITLE[PAGE_MAIN]);
    UI_ReleaseLabels();
    UI_ClearScreen(BG());
    Radio_Init();
    RadioMenu_SetApply(settingsRadioApply);
    if (s_openMulti) {
      s_openMulti = false;
      s_depth = 0;
      s_stack[0] = PAGE_MAIN;
      pushPage(PAGE_MULTI);
    } else {
      openMain();
    }
  }

  void exit() override {
    MenuOverlay_Close();
    RadioMenu_SetApply(nullptr);
    s_depth = 0;
  }

  void update() override { Radio_Update(); }

  void render() override {
    STATUSLINE_render();
    MenuOverlay_Render();
    Radio_ApplyPending();
  }

  bool key(KEY_Code_t k, Key_State_t state) override {
    if (!MenuOverlay_IsOpen()) {
      openMain();
      return true;
    }

    if (s_page == PAGE_MULTI && state == KEY_RELEASED && k >= KEY_0 &&
        k <= KEY_2) {
      multiToggle((uint8_t)(k - KEY_0));
      return true;
    }

    if (state == KEY_RELEASED && k == KEY_MENU) {
      enterSelected();
      return true;
    }
    if (state == KEY_RELEASED && k == KEY_EXIT) {
      popPage();
      return true;
    }
    return MenuOverlay_Key(k, state);
  }
};

static SettingsMode settingsModeInstance;
Mode &MODE_SETTINGS = settingsModeInstance;

void SETTINGS_OpenMultiwatch(void) {
  s_openMulti = true;
  Mode_Switch(MODE_SETTINGS);
}
