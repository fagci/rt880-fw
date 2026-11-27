/* Диагностика отказов без отладчика: HardFault/Default рисуют
 * PC (или номер вектора) на экране и мигают красным LED.
 *
 * После входа в fault обработчик: читаем PC из фрейма исключения
 * (смещение 6 слов от MSP/PSP по биту LR[2]), выводим 8 hex-цифр
 * шрифтом 5x7 через прямой SPI (поллинг, прерывания не нужны).
 */
#include "driver/board.h"
#include "driver/gpio.h"
#include "driver/st7789.h"
#include "driver/uart.h"
#include <stdint.h>

/* 5x7, столбцы битами, MSB = верхний пиксель */
static const uint8_t HEXFONT[16][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x41, 0x41, 0x36}, /* A */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
};

/* Печать 8 hex-цифр значения в строку y0 белым на чёрном */
static void show_hex(uint32_t v, uint16_t y0) {
  st7789_cs_low();
  st7789_set_addr_window_raw(0, y0, 240, 10);
  for (uint16_t row = 0; row < 10; row++) {
    for (uint16_t cx = 0; cx < 8; cx++) {
      uint8_t nib = (v >> ((7 - cx) * 4)) & 0xF;
      uint8_t bits = 0;
      int8_t gy = row - 1; /* 1px верхний отступ */
      if (gy >= 0 && gy < 7)
        bits = HEXFONT[nib][gy] >> 1; /* центрируем 5px в 6px ячейке */
      for (int8_t px = 5; px >= 0; px--) {
        uint16_t c = (bits >> px) & 1 ? 0xFFFF : 0x0000;
        st7789_write_data16(c);
      }
    }
  }
  st7789_cs_high();
}

/* PC/xPSR в UART: экран паники на ST7789 может быть нечитаем (чужое
 * окно/прокрутка), а порт 115200 всегда под рукой. Без stdio/стека. */
static void fault_uart_hex(uint32_t v) {
  static const char h[] = "0123456789ABCDEF";
  for (int8_t i = 28; i >= 0; i -= 4)
    uart_putc(h[(v >> i) & 0xF]);
}

static void fault_uart(uint32_t pc, uint32_t marker, uint32_t lr) {
  uart_puts("FAULT pc=");
  fault_uart_hex(pc);
  uart_puts(" lr=");
  fault_uart_hex(lr);
  uart_puts(" xpsr=");
  fault_uart_hex(marker);
  uart_puts("\r\n");
  uart_flush();
}

static void fault_show(uint32_t pc, uint32_t marker, uint32_t lr) {
  __disable_irq();
  fault_uart(pc, marker, lr);
  show_hex(pc, 0);
  show_hex(marker, 12); /* вторая строка — маркер (xPSR/причина) */
  while (1) {
    gpio_pin_clr(&PIN_LED_RED);
    for (volatile uint32_t i = 0; i < 800000; i++)
      ;
    gpio_pin_set(&PIN_LED_RED);
    for (volatile uint32_t i = 0; i < 800000; i++)
      ;
  }
}

void hard_fault_handler_c(uint32_t *sp) {
  uint32_t pc = sp[6];
  uint32_t xpsr = sp[7];
  uint32_t lr = sp[5];
  fault_show(pc, xpsr, lr);
}

__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b hard_fault_handler_c");
}

__attribute__((naked)) void MemManage_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b hard_fault_handler_c");
}

__attribute__((naked)) void BusFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b hard_fault_handler_c");
}

__attribute__((naked)) void UsageFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b hard_fault_handler_c");
}
