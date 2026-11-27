#include "mode.h"
#include "../driver/board.h"
#include "../driver/st7789.h"
#include "../helper/settings.h"
#include "../helper/store.h"
#include "../helper/tone.h"
#include "../radio.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "modes.h"

static Mode *current = nullptr;
static Mode *next = nullptr;
static bool s_displayOff;
static bool s_paintPaused;
static bool s_displayRxGate;

/* Автозатемнение подсветки LCD в простое: яркость/задержка из настроек
 * Display, любой ключ — полная яркость. 200 Гц ШИМ не лезет в 400–470 МГц
 * (там SPI дисплея). */
static uint32_t s_lastKeyMs;
static bool s_dimmed;

void Mode_Switch(Mode &mode) {
  next = &mode;
}

void Mode_Update() {
  if (next) {
    if (Radio_TxActive())
      Radio_TxStop();
    Tone_Reset();
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
    if (current) {
      current->enter();
      /* Пауза отрисовки (замеры): даём один кадр, чтобы после смены режима
       * не остался пустой экран, дальше дисплей молчит. */
      if (s_paintPaused)
        current->render();
    }
    /* Аппаратный скролл сбрасывается при каждой смене режима — вместе с
     * ним сбрасываем и программное состояние водопада (wfScrollInited).
     * Иначе второй вход в режим с водопадом пропускает инициализацию
     * области скролла, а WF_Render продолжает слать смещение — и едет
     * весь экран. */
    WF_Reset();
  }
  /* BL SQL mode: On — открытие шумодава снимает автозатемнение и сбрасывает
   * таймер; Open — при закрытии сразу возвращает затемнение (или LowDuty,
   * если BL time = Off). Пока дисплей погашен, подсветку не трогаем. */
  static bool s_prevSq;
  bool sq = Radio_SquelchOpen();
  if (!s_displayOff) {
    int32_t sqlMode = Settings_Get(SET_BACKLIGHTONSQUELCH);
    if (sq && !s_prevSq && sqlMode) {
      s_lastKeyMs = millis();
      if (s_dimmed) {
        s_dimmed = false;
        st7789_backlight_set(Settings_BacklightHighDuty());
      }
    } else if (!sq && s_prevSq && sqlMode == 2) {
      uint32_t dimMs = Settings_BacklightDimMs();
      if (dimMs) {
        /* Следующий тик Mode_Update затемнит по таймеру. */
        s_lastKeyMs = millis() - dimMs;
      } else {
        s_dimmed = true;
        st7789_backlight_set(Settings_BacklightLowDuty());
      }
    }
  }
  s_prevSq = sq;

  /* Автозатемнение в простое: таймер сбрасывает любая клавиша. */
  uint32_t dimMs = Settings_BacklightDimMs();
  if (!s_displayOff && !s_dimmed && dimMs &&
      (uint32_t)(millis() - s_lastKeyMs) >= dimMs) {
    s_dimmed = true;
    st7789_backlight_set(Settings_BacklightLowDuty());
  }
  if (current)
    current->update();
}

void Mode_Render() {
  if (current && !s_paintPaused)
    current->render();
}

void Mode_SetPaint(bool on) { s_paintPaused = !on; }
bool Mode_PaintPaused(void) { return s_paintPaused; }
void Mode_SetDisplayRxGate(bool on) { s_displayRxGate = on; }
bool Mode_DisplayRxGate(void) { return s_displayRxGate; }

uint16_t Mode_RenderInterval() {
  return current ? current->renderIntervalMs() : 50;
}

bool Mode_KeyLocked(void) { return Settings_Get(SET_KEYLOCK) != 0; }

void Mode_ToggleKeyLock(void) { Settings_Set(SET_KEYLOCK, !Mode_KeyLocked()); }

void Mode_Key(KEY_Code_t key, Key_State_t state) {
  /* Любая клавиша могла изменить VFO/настройки — сохраним после паузы.
   * PTT состояние не меняет, лишнюю запись во флеш не планируем. */
  if (key != KEY_PTT)
    Store_MarkDirty();

  /* Активность: снимаем автозатемнение и сбрасываем таймер простоя.
   * Пока дисплей погашен долгим EXIT, не трогаем — иначе короткое
   * нажатие включало бы подсветку. */
  if (!s_displayOff) {
    s_lastKeyMs = millis();
    if (s_dimmed) {
      s_dimmed = false;
      st7789_backlight_set(Settings_BacklightHighDuty());
    }
  }

  /* Долгий EXIT гасит дисплей и подсветку клавиатуры. Пока они погашены,
   * остальные клавиши игнорируются; включает обратно только повторный
   * долгий EXIT. */
  if (s_displayOff) {
    if (state == KEY_LONG_PRESSED && key == KEY_EXIT) {
      s_displayOff = false;
      s_lastKeyMs = millis();
      s_dimmed = false;
      st7789_backlight_set(Settings_BacklightHighDuty());
      keyboard_backlight_set(true);
    }
    return;
  }

  /* Клавишный бипер: до проверки keylock, чтобы заблокированные клавиши
   * тоже озвучивались; PTT и погашенный дисплей молчат. */
  if (state == KEY_EVT_PRESS && key != KEY_PTT)
    Radio_Beep();

  /* PTT — глобально, чтобы клавиша не уходила в редакторы/меню. PTT
   * работает и при keylock (как special key в k1-fw); отдельный запрет —
   * Settings → Lock PTT. Передачу разрешаем только из VFO; Radio_TxStart
   * сам проверит Settings, чип, калибровку и частоту. Отпускание
   * обязательно даже после TOT. */
  if (key == KEY_PTT) {
    if (state == KEY_EVT_PRESS) {
      if (current == &MODE_VFO)
        Radio_TxStart();
      return;
    }
    if (state == KEY_EVT_RELEASE) {
      Radio_TxStop();
      Radio_TxPttReleased();
      return;
    }
    return;
  }
  /* Пока идёт передача, остальные клавиши не трогают VFO/меню. */
  if (Radio_TxActive())
    return;

  /* Долгая STAR — блокировка клавиатуры в любом режиме. */
  if (state == KEY_LONG_PRESSED && key == KEY_STAR) {
    Mode_ToggleKeyLock();
    return;
  }

  /* Отпускание цифры после длинного нажатия драйвер клавиатуры не шлёт
   * (long_sent в keyboard.cpp), поэтому здесь его гасить не нужно. */

  if (Mode_KeyLocked())
    return;

  /* Долгий EXIT — выключить дисплей и подсветку клавиатуры. */
  if (state == KEY_LONG_PRESSED && key == KEY_EXIT) {
    s_displayOff = true;
    s_dimmed = false;
    st7789_backlight_off();
    keyboard_backlight_set(false);
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
