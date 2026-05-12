#include "log.h"
#include "../driver/st7789.h"
#include "../ui/graphics.h"
#include <stdarg.h>
#include <stdio.h>

#define LOG_LINES 8
#define LOG_LINE_W 32

static char logBuf[LOG_LINES][LOG_LINE_W];
static uint8_t logHead = 0;

void Log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(logBuf[logHead % LOG_LINES], LOG_LINE_W, fmt, args);
  va_end(args);
  logHead++;
}

void Log_Render(uint16_t x, uint16_t y, uint8_t lineH) {
  for (uint8_t i = 0; i < LOG_LINES; i++) {
    uint8_t idx = (logHead + i) % LOG_LINES;
    PrintfEx(x, y + i * lineH, POS_L, C_YELLOW, C_BLACK, F_SS, "%s",
             logBuf[idx]);
  }
}
