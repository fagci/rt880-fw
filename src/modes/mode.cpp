#include "mode.h"
#include "../driver/st7789.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "lvgl.h"
#include "vfo.h"

static Mode *current = nullptr;
static Mode *next = nullptr;

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

void Mode_Key(KEY_Code_t key, Key_State_t state) {
  if (current)
    current->key(key, state);
}

Mode *Mode_Current() {
  return current;
}
