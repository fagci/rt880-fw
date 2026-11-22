#include "mode.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "vfo.h"

static Mode *current = nullptr;
static Mode *next = nullptr;
static bool s_displayOff;
static bool s_keyLocked;
static KEY_Code_t s_wakeKey;
static bool s_wakeSuppress;

void Mode_Switch(Mode &mode) {
  next = &mode;
}

void Mode_Update() {
  if (next) {
    if (current)
      current->exit();
    /* Лейблы пула (Printf*) живут между режимами: их LVGL перерисует поверх
     * чужого экрана и даже в память зоны аппаратного скролла (чёрные полосы
     * в водопаде по x-диапазонам надписей). Освобождаем при каждой смене. */
    UI_ReleaseLabels();
    /* Тема глобальная: корневой фон LVGL задаём на каждой смене режима.
     * При смене темы — сразу перерисовываем экран ДО enter(), иначе
     * отложенная перерисовка LVGL затёрла бы то, что режим нарисует. */
    static bool appliedLight;
    bool themeChanged = (gThemeLight != appliedLight);
    appliedLight = gThemeLight;
    UI_ClearScreen(BG());
    if (themeChanged) {
      lv_refr_now(NULL);
      STATUSLINE_ApplyTheme();
    }
    current = next;
    next = nullptr;
    if (current)
      current->enter();
    /* Аппаратный скролл сбрасывается при каждой смене режима — вместе с
     * ним сбрасываем и программное состояние водопада (wfScrollInited).
     * Иначе второй вход в режим с водопадом пропускает инициализацию
     * области скролла, а WF_Render продолжает слать смещение — и едет
     * весь экран. */
    WF_Reset();
  }
  if (current)
    current->update();
}

void Mode_Render() {
  if (current)
    current->render();
}

uint16_t Mode_RenderInterval() {
  return current ? current->renderIntervalMs() : 50;
}

bool Mode_KeyLocked(void) { return s_keyLocked; }

void Mode_Key(KEY_Code_t key, Key_State_t state) {
  /* Дисплей выключен длинным EXIT: любое нажатие только включает
   * подсветку и гасится до отпускания, чтобы не сработало действие. */
  if (s_displayOff) {
    if (state == KEY_EVT_PRESS) {
      st7789_backlight_on();
      s_displayOff = false;
      s_wakeKey = key;
      s_wakeSuppress = true;
      return;
    }
    if (s_wakeSuppress && key == s_wakeKey) {
      if (state == KEY_EVT_RELEASE)
        s_wakeSuppress = false;
      return;
    }
    return;
  }

  /* Долгая STAR — блокировка клавиатуры в любом режиме. */
  if (state == KEY_LONG_PRESSED && key == KEY_STAR) {
    s_keyLocked = !s_keyLocked;
    return;
  }
  if (s_keyLocked)
    return;

  /* Долгий EXIT — выключить дисплей (подсветку). */
  if (state == KEY_LONG_PRESSED && key == KEY_EXIT) {
    s_displayOff = true;
    st7789_backlight_off();
    return;
  }

  /* Тема переключается глобально длинным ALARM: не зависит от режима,
   * включая редакторы частоты. Долгое нажатие приходит один раз. */
  if (current && state == KEY_LONG_PRESSED && key == KEY_ALARM) {
    gThemeLight = !gThemeLight;
    Mode_Switch(*current);
    return;
  }
  /* Короткий ALARM — разъём антенны (A1/A2), глобально как и тема.
   * Реле механическое: не дёргаем его чаще 300 мс. */
  if (current && state == KEY_EVT_RELEASE && key == KEY_ALARM) {
    static uint32_t lastMs;
    uint32_t now = millis();
    if (now - lastMs < 300)
      return;
    lastMs = now;
    ant_sw(!ant_is_main());
    return;
  }
  if (current)
    current->key(key, state);
}

Mode *Mode_Current() {
  return current;
}
