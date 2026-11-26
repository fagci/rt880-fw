/* stage-2 загрузчик RT-880: fast-прошивка приложения.
 *
 * Живёт в 0x08002800..0x080047FF, стоковый загрузчик (0x08000000) не
 * трогает и остаётся recovery. Стартует после стокового, проверяет
 * метаданные приложения и либо прыгает в него, либо ждёт прошивку.
 *
 * Протокол (921600 8N1):
 *   A5 5A cmd len_lo len_hi payload crc32(cmd,len,payload)
 *   CMD_ENTER(1)          -> ACK
 *   CMD_WRITE(2)  off u32 + 2048 байт -> ACK/NAK
 *   CMD_DONE(3)   len u32 + crc32     -> ACK, затем прыжок
 */

#include "at32f423.h"
#include "at32f423_crm.h"
#include "at32f423_flash.h"
#include "at32f423_gpio.h"
#include "at32f423_usart.h"
#include "boot_layout.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define B2_SYNC0        0xA5u
#define B2_SYNC1        0x5Au
#define B2_CMD_ENTER    1u
#define B2_CMD_WRITE    2u
#define B2_CMD_DONE     3u
#define B2_ACK          0x06u
#define B2_NAK          0x15u
#define B2_HDR_LEN      3u

typedef struct {
  uint32_t magic;
  uint32_t app_len;
  uint32_t app_crc;
  uint32_t meta_crc;
} b2_meta_t;

static uint8_t s_payload[4 + B2_BLOCK] __attribute__((aligned(4)));
static bool s_uart_ready;
static bool s_dirty;
static bool s_done;

/* ── startup glue ─────────────────────────────────────────────────── */

typedef void (*b2_init_fn)(void);
extern b2_init_fn __init_array_start[], __init_array_end[];

void RunInitArray(void) {
  for (b2_init_fn *p = __init_array_start; p < __init_array_end; ++p)
    (*p)();
}

/* ── утилиты ──────────────────────────────────────────────────────── */

static uint32_t s_crc_table[256];

static void crc32_init(void) {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j)
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    s_crc_table[i] = c;
  }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
  while (n--)
    crc = (crc >> 8) ^ s_crc_table[(crc ^ *p++) & 0xFFu];
  return crc;
}

static uint32_t crc32_buf(const uint8_t *p, uint32_t n) {
  return crc32_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

static uint32_t flash_crc32(uint32_t addr, uint32_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t buf[64];
  while (len) {
    uint32_t n = len < sizeof(buf) ? len : sizeof(buf);
    for (uint32_t i = 0; i < n; ++i)
      buf[i] = *(const volatile uint8_t *)(addr + i);
    crc = crc32_update(crc, buf, n);
    addr += n;
    len -= n;
  }
  return crc ^ 0xFFFFFFFFu;
}

/* ── аварийный обработчик ─────────────────────────────────────────── */

static void fault_blink(void) __attribute__((noreturn));
static void fault_blink(void) {
  __disable_irq();
  for (;;) {
    gpio_bits_reset(GPIOA, GPIO_PINS_14);
    for (volatile uint32_t i = 0; i < 1500000u; ++i)
      ;
    gpio_bits_set(GPIOA, GPIO_PINS_14);
    for (volatile uint32_t i = 0; i < 1500000u; ++i)
      ;
  }
}

static void boot2_fault(uint32_t *sp, uint32_t code)
    __attribute__((used, noinline, noreturn));
static void boot2_fault(uint32_t *sp, uint32_t code) {
  __disable_irq();
  if (s_uart_ready) {
    static const char hex[] = "0123456789ABCDEF";
    const char *msg = "BOOT2 FAULT ";
    while (*msg)
      USART6->dt = (uint8_t)*msg++;
    const char *cmds[4] = {"HARD", "MEM", "BUS", "USAGE"};
    const char *s = cmds[code < 4 ? code : 0];
    while (*s)
      USART6->dt = (uint8_t)*s++;
    USART6->dt = ' ';
    uint32_t pc = sp ? sp[6] : 0;
    for (int i = 28; i >= 0; i -= 4)
      USART6->dt = (uint8_t)hex[(pc >> i) & 0xF];
    USART6->dt = '\r';
    USART6->dt = '\n';
    while (!(USART6->sts & USART_TDC_FLAG))
      ;
  }
  fault_blink();
}

__attribute__((naked, noreturn)) void HardFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "movs r1, #0\n"
                 "b boot2_fault\n");
}

__attribute__((naked, noreturn)) void MemManage_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "movs r1, #1\n"
                 "b boot2_fault\n");
}

__attribute__((naked, noreturn)) void BusFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "movs r1, #2\n"
                 "b boot2_fault\n");
}

__attribute__((naked, noreturn)) void UsageFault_Handler(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "movs r1, #3\n"
                 "b boot2_fault\n");
}

/* ── тактирование / UART / DWT ────────────────────────────────────── */

static bool wait_flag(uint32_t flag, uint32_t n) {
  while (crm_flag_get(flag) == RESET)
    if (!--n)
      return false;
  return true;
}

static bool clock_init(void) {
  flash_psr_set(FLASH_WAIT_CYCLE_4);

  crm_pll_clock_source_type src = CRM_PLL_SOURCE_HEXT;
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
  if (!wait_flag(CRM_HEXT_STABLE_FLAG, 200000)) {
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, FALSE);
    src = CRM_PLL_SOURCE_HICK;
  }

  crm_pll_config(src, 135, 1, CRM_PLL_FR_4);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  if (!wait_flag(CRM_PLL_STABLE_FLAG, 200000)) {
    flash_psr_set(FLASH_WAIT_CYCLE_0);
    system_core_clock_update();
    return false;
  }

  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_4);
  crm_apb2_div_set(CRM_APB2_DIV_1);

  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  uint32_t t = 200000;
  while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL && --t)
    ;
  crm_auto_step_mode_enable(FALSE);
  system_core_clock_update();
  return t != 0;
}

static void dwt_init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void uart_init(uint32_t baud) {
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART6_PERIPH_CLOCK, TRUE);
  usart_reset(USART6);

  usart_init(USART6, baud, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_transmitter_enable(USART6, TRUE);
  usart_receiver_enable(USART6, TRUE);
  usart_enable(USART6, TRUE);

  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE11, GPIO_MUX_8);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE12, GPIO_MUX_8);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_MUX;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull = GPIO_PULL_UP;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_pins = (uint16_t)(GPIO_PINS_11 | GPIO_PINS_12);
  gpio_init(GPIOA, &g);

  s_uart_ready = true;
}

static void uart_putc(uint8_t c) {
  while (!(USART6->sts & USART_TDBE_FLAG))
    ;
  USART6->dt = c;
}

static void uart_tx_wait(void) {
  while (!(USART6->sts & USART_TDC_FLAG))
    ;
}

static int uart_getc(uint32_t timeout_ms) {
  uint32_t start = DWT->CYCCNT;
  uint32_t lim = timeout_ms * (SystemCoreClock / 1000u);
  while (!(USART6->sts & USART_RDBF_FLAG)) {
    if ((DWT->CYCCNT - start) > lim)
      return -1;
  }
  return (int)(USART6->dt & 0xFFu);
}

/* ── индикация ────────────────────────────────────────────────────── */

static void led_init(void) {
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_OUTPUT;
  g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull = GPIO_PULL_NONE;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  g.gpio_pins = (uint16_t)(GPIO_PINS_13 | GPIO_PINS_14);
  gpio_init(GPIOA, &g);
  gpio_bits_set(GPIOA, GPIO_PINS_13 | GPIO_PINS_14);
}

static void delay_ms(uint32_t ms) {
  uint32_t start = DWT->CYCCNT;
  uint32_t lim = ms * (SystemCoreClock / 1000u);
  while ((DWT->CYCCNT - start) < lim)
    ;
}

static void led_blink(uint16_t pin, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    gpio_bits_reset(GPIOA, pin);
    delay_ms(80);
    gpio_bits_set(GPIOA, pin);
    delay_ms(120);
  }
}

/* ── flash / метаданные ───────────────────────────────────────────── */

static void flash_clear(void) {
  flash_flag_clear(FLASH_ODF_FLAG | FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
}

static bool meta_read(b2_meta_t *m) {
  const volatile uint32_t *p = (const volatile uint32_t *)META_ADDR;
  m->magic = p[0];
  m->app_len = p[1];
  m->app_crc = p[2];
  m->meta_crc = p[3];
  if (m->magic != META_MAGIC)
    return false;
  if (m->meta_crc != crc32_buf((const uint8_t *)&m->magic, 12))
    return false;
  if (m->app_len == 0 || m->app_len > APP_MAX_LEN || (m->app_len % B2_BLOCK))
    return false;
  return true;
}

static bool meta_erase(void) {
  flash_unlock();
  flash_clear();
  flash_status_type st = flash_sector_erase(META_ADDR);
  flash_lock();
  return st == FLASH_OPERATE_DONE;
}

static bool meta_write(uint32_t app_len, uint32_t app_crc) {
  b2_meta_t m;
  m.magic = META_MAGIC;
  m.app_len = app_len;
  m.app_crc = app_crc;
  m.meta_crc = crc32_buf((const uint8_t *)&m.magic, 12);

  flash_unlock();
  flash_clear();
  flash_status_type st = flash_sector_erase(META_ADDR);
  if (st == FLASH_OPERATE_DONE) {
    const uint32_t *w = (const uint32_t *)&m;
    for (uint32_t i = 0; i < 4; ++i) {
      st = flash_word_program(META_ADDR + i * 4u, w[i]);
      if (st != FLASH_OPERATE_DONE)
        break;
    }
  }
  flash_lock();
  if (st != FLASH_OPERATE_DONE)
    return false;

  b2_meta_t chk;
  return meta_read(&chk) && chk.app_len == app_len && chk.app_crc == app_crc;
}

static bool write_block(uint32_t off, const uint8_t *data) {
  uint32_t addr = APP_BASE + off;
  if ((off % B2_BLOCK) || addr < APP_BASE || addr >= META_ADDR)
    return false;

  flash_unlock();
  flash_clear();
  flash_status_type st = flash_sector_erase(addr);
  if (st == FLASH_OPERATE_DONE) {
    const uint32_t *w = (const uint32_t *)data;
    for (uint32_t i = 0; i < B2_BLOCK / 4u; ++i) {
      st = flash_word_program(addr + i * 4u, w[i]);
      if (st != FLASH_OPERATE_DONE)
        break;
    }
  }
  flash_lock();
  if (st != FLASH_OPERATE_DONE)
    return false;

  const volatile uint32_t *f = (const volatile uint32_t *)addr;
  const uint32_t *w = (const uint32_t *)data;
  for (uint32_t i = 0; i < B2_BLOCK / 4u; ++i)
    if (f[i] != w[i])
      return false;
  return true;
}

/* ── приложение ───────────────────────────────────────────────────── */

static bool app_vector_ok(uint32_t app_len) {
  if (app_len < 8 || app_len > APP_MAX_LEN)
    return false;
  uint32_t sp = *(const volatile uint32_t *)APP_BASE;
  uint32_t pc = *(const volatile uint32_t *)(APP_BASE + 4u);
  if (sp < 0x20000000u || sp > 0x2000C000u || (sp & 3u))
    return false;
  if (pc < APP_BASE + 8u || pc >= APP_BASE + app_len)
    return false;
  if ((pc & 1u) == 0)
    return false;
  return true;
}

static void jump_app(void) __attribute__((noreturn));
static void jump_app(void) {
  uint32_t sp = *(const volatile uint32_t *)APP_BASE;
  uint32_t pc = *(const volatile uint32_t *)(APP_BASE + 4u);
  __disable_irq();
  for (uint32_t i = 0; i < 8; ++i) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;
  SCB->VTOR = APP_BASE;
  __set_MSP(sp);
  __enable_irq();
  ((void (*)(void))pc)();
  for (;;)
    ;
}

/* ── протокол ─────────────────────────────────────────────────────── */

static int read_frame(uint32_t sync_ms) {
  int c = uart_getc(sync_ms);
  if (c < 0)
    return 0;

  for (;;) {
    if (c != (int)B2_SYNC0) {
      c = uart_getc(sync_ms);
      if (c < 0)
        return 0;
      continue;
    }
    int c2 = uart_getc(20);
    if (c2 == (int)B2_SYNC1)
      break;
    if (c2 < 0)
      return 0;
    c = c2;
  }

  int cmd = uart_getc(100);
  int l0 = uart_getc(100);
  int l1 = uart_getc(100);
  if (cmd < 0 || l0 < 0 || l1 < 0)
    return 1;
  uint32_t len = (uint32_t)l0 | ((uint32_t)l1 << 8);
  if (len > sizeof(s_payload)) {
    uart_putc(B2_NAK);
    return 1;
  }
  for (uint32_t i = 0; i < len; ++i) {
    int b = uart_getc(100);
    if (b < 0) {
      uart_putc(B2_NAK);
      return 1;
    }
    s_payload[i] = (uint8_t)b;
  }
  uint32_t rcv = 0;
  for (int i = 0; i < 4; ++i) {
    int b = uart_getc(100);
    if (b < 0) {
      uart_putc(B2_NAK);
      return 1;
    }
    rcv |= (uint32_t)b << (i * 8);
  }

  uint32_t crc = 0xFFFFFFFFu;
  uint8_t hdr[B2_HDR_LEN] = {(uint8_t)cmd, (uint8_t)l0, (uint8_t)l1};
  crc = crc32_update(crc, hdr, sizeof(hdr));
  crc = crc32_update(crc, s_payload, len);
  crc ^= 0xFFFFFFFFu;
  if (crc != rcv) {
    uart_putc(B2_NAK);
    return 1;
  }

  if (cmd == (int)B2_CMD_ENTER) {
    uart_putc(B2_ACK);
    return 1;
  }

  if (cmd == (int)B2_CMD_WRITE && len == 4u + B2_BLOCK) {
    uint32_t off = (uint32_t)s_payload[0] | ((uint32_t)s_payload[1] << 8) |
                   ((uint32_t)s_payload[2] << 16) | ((uint32_t)s_payload[3] << 24);
    if (off > APP_MAX_LEN - B2_BLOCK) {
      uart_putc(B2_NAK);
      return 1;
    }
    if (!s_dirty) {
      if (!meta_erase()) {
        uart_putc(B2_NAK);
        return 1;
      }
      s_dirty = true;
    }
    bool ok = write_block(off, s_payload + 4);
    uart_putc(ok ? B2_ACK : B2_NAK);
    return 1;
  }

  if (cmd == (int)B2_CMD_DONE && len == 8u) {
    uint32_t app_len = (uint32_t)s_payload[0] | ((uint32_t)s_payload[1] << 8) |
                       ((uint32_t)s_payload[2] << 16) | ((uint32_t)s_payload[3] << 24);
    uint32_t app_crc = (uint32_t)s_payload[4] | ((uint32_t)s_payload[5] << 8) |
                       ((uint32_t)s_payload[6] << 16) | ((uint32_t)s_payload[7] << 24);
    if (app_len == 0 || app_len > APP_MAX_LEN || (app_len % B2_BLOCK) ||
        !app_vector_ok(app_len) || flash_crc32(APP_BASE, app_len) != app_crc) {
      uart_putc(B2_NAK);
      return 1;
    }
    if (!meta_write(app_len, app_crc)) {
      uart_putc(B2_NAK);
      return 1;
    }
    uart_putc(B2_ACK);
    s_done = true;
    return 1;
  }

  uart_putc(B2_NAK);
  return 1;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(void) {
  __disable_irq();
  for (uint32_t i = 0; i < 8; ++i) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  SCB->VTOR = BOOT2_BASE;
  flash_psr_set(FLASH_WAIT_CYCLE_0); /* HICK 8 МГц: без wait states */
  crc32_init();

  volatile uint32_t *magic = (volatile uint32_t *)BOOT2_MAGIC_ADDR;
  bool want_fast = (*magic == BOOT2_MAGIC);
  if (want_fast)
    *magic = 0;

  b2_meta_t m;
  bool have_meta = meta_read(&m);
  /* Полный CRC проверяется при записи (CMD_DONE + readback), мета
   * стирается перед первой записью. На загрузке достаточно меты и
   * вектора — так cold boot не платит за чтение всего приложения. */
  bool app_ok = have_meta && app_vector_ok(m.app_len);

  if (app_ok && !want_fast)
    jump_app();

  led_init();
  bool clk_ok = clock_init();
  dwt_init();

  if (!clk_ok) {
    /* Без 135 МГц UART на 921600 не совпадёт с хостом. */
    for (;;) {
      gpio_bits_reset(GPIOA, GPIO_PINS_14);
      for (volatile uint32_t i = 0; i < 2000000u; ++i)
        ;
      gpio_bits_set(GPIOA, GPIO_PINS_14);
      for (volatile uint32_t i = 0; i < 2000000u; ++i)
        ;
    }
  }

  /* Диагностика на 115200, затем рабочий 921600. */
  uart_init(115200);
  const char *msg = have_meta ? "BOOT2 META OK\r\n" : "BOOT2 NO META\r\n";
  while (*msg)
    uart_putc((uint8_t)*msg++);
  uart_tx_wait();
  uart_init(B2_BAUD);

  led_blink(GPIO_PINS_14, have_meta ? 1u : 3u);

  uint32_t idle = 0;
  for (;;) {
    int r = read_frame(100);
    if (r == 0) {
      if (app_ok && !s_dirty && have_meta && ++idle >= 300u)
        jump_app();
      if ((idle % 10u) == 0)
        gpio_bits_reset(GPIOA, GPIO_PINS_14);
      else
        gpio_bits_set(GPIOA, GPIO_PINS_14);
      continue;
    }
    idle = 0;
    if (s_done) {
      uart_tx_wait();
      led_blink(GPIO_PINS_13, 2u);
      jump_app();
    }
  }
}
