#include "console.h"

#include "../driver/board.h"
#include "../driver/board_pins.h"
#include "../driver/bk4819.h"
#include "../driver/gpio.h"
#include "../driver/uart.h"
#include "../driver/w25q32.h"
#include "../misc.h"
#include "store.h"
#include "at32f423.h"
#include "bootprof.h"

#include <string.h>

/* Загрузчик RT-880: 10 КиБ перед приложением. */
#define BOOT_ADDR 0x08000000u
#define BOOT_LEN 0x2800u

#define CMD_MAX 40
#define POLL_BYTES 32

static char s_cmd[CMD_MAX];
static uint8_t s_len;

static uint32_t sum32(const uint8_t *p, size_t n) {
  uint32_t s = 0;
  while (n--)
    s += *p++;
  return s;
}

static void dump_boot(void) {
  const uint8_t *p = (const uint8_t *)BOOT_ADDR;

  uart_printf("RT880BOOT %08lx %04x %08lx\r\n", (unsigned long)BOOT_ADDR,
              (unsigned)BOOT_LEN, (unsigned long)sum32(p, BOOT_LEN));
  uart_write(p, BOOT_LEN);
  uart_puts("\r\n");
}

/* WPR-последовательность AT32: блокирует CFGR/OMODE/ODRV/PULL выбранных ног
 * до сброса. */
static void gpio_lock_pin(gpio_type *port, uint16_t pins) {
  uint32_t seq = 0x00010000u | pins;

  port->wpr = seq;
  port->wpr = pins;
  port->wpr = seq;
  (void)port->wpr;
  (void)port->wpr;
}

/* Загрузчик пускает дальше только при PB6 == 0 (верхняя PTT). Держим PB6
 * выходом в ноль и запрещаем загрузчику переконфигурировать его в input
 * pull-up. */
static void enter_loader(void) __attribute__((noreturn));
static void enter_loader(void) {
  uart_puts("OK\r\n");
  uart_flush();

  dma_channel_enable(DMA1_CHANNEL1, FALSE);
  spi_i2s_dma_transmitter_enable(SPI1, FALSE);

  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  gpio_pin_init_output(&PIN_KEY_PTT);
  gpio_pin_clr(&PIN_KEY_PTT);
  gpio_lock_pin(PIN_KEY_PTT.port, PIN_KEY_PTT.pin);

  __disable_irq();
  for (uint32_t i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
  /* Все NVIC-линии выключены; PRIMASK загрузчик не снимает сам. */
  __enable_irq();

  uint32_t sp = *(volatile uint32_t *)BOOT_ADDR;
  uint32_t pc = *(volatile uint32_t *)(BOOT_ADDR + 4u);
  __set_MSP(sp);
  ((void (*)(void))pc)();

  for (;;)
    ;
}

/* Сколько байт в каждом 64-КиБ блоке отличается от 0xFF, плюс JEDEC ID.
 * По карте видно, занята ли внешняя флеш стоковыми данными. */
static void flash_map(void) {
  if (!W25Q_Init()) {
    uart_puts("FLASH NONE\r\n");
    return;
  }
  uart_printf("FLASH ID %06lx\r\n", (unsigned long)W25Q_JedecId());

  uint8_t buf[32];
  for (uint32_t blk = 0; blk < W25Q_SIZE_BYTES / 65536u; ++blk) {
    uint32_t used = 0;
    for (uint32_t off = 0; off < 65536u; off += sizeof(buf)) {
      W25Q_Read(blk * 65536u + off, buf, sizeof(buf));
      for (uint32_t i = 0; i < sizeof(buf); ++i)
        if (buf[i] != 0xFF)
          ++used;
    }
    uart_printf("%02lx:%05lx ", (unsigned long)blk, (unsigned long)used);
    if ((blk & 7) == 7)
      uart_puts("\r\n");
  }
  uart_puts("\r\n");
}

static const char *parse_hex(const char *s, uint32_t *v) {
  while (*s == ' ')
    ++s;
  uint32_t x = 0;
  bool any = false;
  for (;;) {
    char c = *s;
    int d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    else
      break;
    x = (x << 4) | (uint32_t)d;
    any = true;
    ++s;
  }
  if (!any)
    return nullptr;
  *v = x;
  return s;
}

/* flashdump <addr> <len> — hex-дамп внешней флеш (адреса и длина hex). */
static void flash_dump(const char *args) {
  uint32_t addr, len;
  const char *p = parse_hex(args, &addr);
  if (p)
    p = parse_hex(p, &len);
  if (!p || !W25Q_Init()) {
    uart_puts("ERR\r\n");
    return;
  }
  uint8_t row[16];
  for (uint32_t off = 0; off < len; off += sizeof(row)) {
    uint32_t n = len - off;
    if (n > sizeof(row))
      n = sizeof(row);
    W25Q_Read(addr + off, row, n);
    uart_printf("%08lx:", (unsigned long)(addr + off));
    for (uint32_t i = 0; i < n; ++i)
      uart_printf(" %02x", row[i]);
    uart_puts("\r\n");
  }
}

static const Filter BK_FILTERS[] = {FILTER_HF, FILTER_VHF, FILTER_UHF,
                                    FILTER_800};

/* Результат счётчика: REG_0D<10:0> — старшие, REG_0E — младшие 16 бит,
 * единицы 10 Гц. */
static uint32_t bk_scan_result(uint16_t *hiOut, uint16_t *loOut) {
  uint16_t hi = BK4819_ReadRegisterRaw(BK4819_REG_0D);
  uint16_t lo = BK4819_ReadRegisterRaw(BK4819_REG_0E);
  if (hiOut)
    *hiOut = hi;
  if (loOut)
    *loOut = lo;
  return ((uint32_t)(hi & 0x7FF) << 16) | lo;
}

/* Один проход частотомера с блокировкой главного цикла: выбрать фильтр
 * платы, включить скан REG_32 и ждать результат до 2 с. */
static void bk_fc(const char *args) {
  uint32_t flt = 1, time = 0, thr = 0x244;
  const char *p = parse_hex(args, &flt);
  if (p)
    p = parse_hex(p, &time);
  if (p)
    parse_hex(p, &thr);
  if (flt >= ARRAY_SIZE(BK_FILTERS))
    flt = 1;
  if (time > 3)
    time = 0;

  BK4819_SelectChip(0);
  BK4819_SelectFilter(BK_FILTERS[flt]);
  BK4819_ToggleAFDAC(false);
  BK4819_EnableFrequencyScanEx2((FreqScanTime)time, (uint16_t)thr);

  uint32_t start = millis(), freq = 0;
  bool found = false;
  while (millis() - start < 2000) {
    if (BK4819_GetFrequencyScanResult(&freq)) {
      found = true;
      break;
    }
  }

  uint16_t hi, lo;
  uint32_t raw = bk_scan_result(&hi, &lo);

  BK4819_DisableFrequencyScan();
  BK4819_RX_TurnOn();
  BK4819_ToggleAFDAC(false);
  BK4819_InvalidateFilterCache();

  uart_printf("BKFC flt=%lu t=%lu thr=%03lx %s f=%lu.%03lu raw=%lu "
              "0D=%04x 0E=%04x dt=%lums\r\n",
              (unsigned long)flt, (unsigned long)time, (unsigned long)thr,
              found ? "HIT" : "TIMEOUT", (unsigned long)(freq / MHZ),
              (unsigned long)((freq % MHZ) / KHZ), (unsigned long)raw, hi, lo,
              (unsigned long)(millis() - start));
}

static const char *parse_dec(const char *s, uint32_t *v) {
  while (*s == ' ')
    ++s;
  uint32_t x = 0;
  bool any = false;
  while (*s >= '0' && *s <= '9') {
    x = x * 10 + (uint32_t)(*s - '0');
    any = true;
    ++s;
  }
  if (!any)
    return nullptr;
  *v = x;
  return s;
}

/* bk — отладка BK4819 по UART. Регистры и значения — в hex. */
static void bk_cmd(const char *args) {
  while (*args == ' ')
    ++args;
  uint32_t a, b;

  if (strncmp(args, "r ", 2) == 0) {
    const char *p = parse_hex(args + 2, &a);
    if (!p || a > 0x7F) {
      uart_puts("ERR\r\n");
      return;
    }
    uart_printf("BK R %02lx = %04x\r\n", (unsigned long)a,
                BK4819_ReadRegisterRaw((uint8_t)a));
  } else if (strncmp(args, "w ", 2) == 0) {
    const char *p = parse_hex(args + 2, &a);
    if (p)
      p = parse_hex(p, &b);
    if (!p || a > 0x7F || b > 0xFFFF) {
      uart_puts("ERR\r\n");
      return;
    }
    BK4819_WriteRegister((uint8_t)a, (uint16_t)b);
    uart_printf("BK W %02lx = %04x\r\n", (unsigned long)a,
                BK4819_ReadRegisterRaw((uint8_t)a));
  } else if (strcmp(args, "chip 0") == 0) {
    BK4819_SelectChip(0);
    uart_puts("BK CHIP 0\r\n");
  } else if (strcmp(args, "chip 1") == 0) {
    BK4819_SelectChip(1);
    uart_puts("BK CHIP 1\r\n");
  } else if (strncmp(args, "filter ", 7) == 0) {
    const char *p = parse_hex(args + 7, &a);
    if (!p || a >= ARRAY_SIZE(BK_FILTERS)) {
      uart_puts("ERR\r\n");
      return;
    }
    BK4819_SelectFilter(BK_FILTERS[a]);
    uart_printf("BK FILTER %lu reg33=%04x\r\n", (unsigned long)a,
                BK4819_ReadRegisterRaw(BK4819_REG_33));
  } else if (strncmp(args, "scan", 4) == 0) {
    a = 0;
    b = 0x244;
    const char *p = parse_hex(args + 4, &a);
    if (p)
      parse_hex(p, &b);
    if (a > 3 || b > 0x3FFF) {
      uart_puts("ERR\r\n");
      return;
    }
    BK4819_EnableFrequencyScanEx2((FreqScanTime)a, (uint16_t)b);
    uart_printf("BK SCAN t=%lu thr=%03lx reg32=%04x\r\n", (unsigned long)a,
                (unsigned long)b, BK4819_ReadRegisterRaw(BK4819_REG_32));
  } else if (strcmp(args, "stop") == 0) {
    BK4819_DisableFrequencyScan();
    uart_printf("BK STOP reg32=%04x\r\n", BK4819_ReadRegisterRaw(BK4819_REG_32));
  } else if (strncmp(args, "tune", 4) == 0) {
    const char *p = parse_dec(args + 4, &a);
    if (!p || a < 100000 || a > 1300000000) {
      uart_puts("ERR\r\n");
      return;
    }
    a = (a + 5) / 10; /* Гц → единицы 10 Гц */
    BK4819_TuneTo(a, true);
    uart_printf("BK TUNE %lu.%03lu rssi=%u reg39=%04x reg38=%04x\r\n",
                (unsigned long)(a / MHZ), (unsigned long)((a % MHZ) / KHZ),
                BK4819_GetRSSI(), BK4819_ReadRegisterRaw(BK4819_REG_39),
                BK4819_ReadRegisterRaw(BK4819_REG_38));
  } else if (strcmp(args, "res") == 0) {
    uint16_t hi, lo;
    uint32_t raw = bk_scan_result(&hi, &lo);
    uart_printf("BK RES fin=%u raw=%lu f=%lu.%03lu 0D=%04x 0E=%04x\r\n",
                (hi & 0x8000) == 0 ? 1u : 0u, (unsigned long)raw,
                (unsigned long)(raw / MHZ),
                (unsigned long)((raw % MHZ) / KHZ), hi, lo);
  } else if (strncmp(args, "rssi", 4) == 0) {
    const char *p = parse_dec(args + 4, &a);
    if (!p) {
      uart_printf("BK RSSI=%u SNR=%u\r\n", BK4819_GetRSSI(),
                  BK4819_GetSNR());
      return;
    }
    const char *q = parse_dec(p, &b);
    uint32_t step = 10000;
    if (q)
      parse_dec(q, &step);
    if (!q || a > b || step == 0 || b > 1300000) {
      uart_puts("ERR\r\n");
      return;
    }
    a *= 100; /* кГц → единицы 10 Гц */
    b *= 100;
    step *= 100;
    for (uint32_t f = a; f <= b; f += step) {
      BK4819_TuneTo(f, false);
      delay_ms(3);
      uart_printf("%lu.%03lu %u\r\n", (unsigned long)(f / MHZ),
                  (unsigned long)((f % MHZ) / KHZ), BK4819_GetRSSI());
    }
  } else if (strncmp(args, "ant", 3) == 0) {
    const char *p = parse_dec(args + 3, &a);
    if (p && a <= 1)
      ant_sw(a != 0);
    uart_printf("BK ANT %u %s\r\n", ant_is_main() ? 1u : 0u,
                ant_is_main() ? "A2" : "A1");
  } else if (strncmp(args, "fc", 2) == 0) {
    bk_fc(args + 2);
  } else {
    uart_puts("bk r/w/chip/filter/ant/tune/scan/stop/res/rssi[kHz]/fc\r\n");
  }
}

static void run(const char *cmd) {
  if (strcmp(cmd, "dumpboot") == 0)
    dump_boot();
  else if (strcmp(cmd, "boot") == 0)
    enter_loader();
  else if (strcmp(cmd, "reboot") == 0) {
    uart_puts("REBOOT\r\n");
    uart_flush();
    NVIC_SystemReset();
  } else if (strcmp(cmd, "prof") == 0)
    Prof_Print();
  else if (strcmp(cmd, "flashmap") == 0)
    flash_map();
  else if (strncmp(cmd, "flashdump", 9) == 0)
    flash_dump(cmd + 9);
  else if (strcmp(cmd, "store") == 0)
    Store_DebugPrint();
  else if (strcmp(cmd, "storewipe") == 0)
    uart_printf("STOREWIPE %u\r\n", Store_Wipe() ? 1u : 0u);
  else if (strcmp(cmd, "bk") == 0 || strncmp(cmd, "bk ", 3) == 0)
    bk_cmd(cmd + 2);
  else if (strcmp(cmd, "help") == 0)
    uart_puts("boot\r\nreboot\r\nprof\r\ndumpboot\r\nflashmap\r\n"
              "flashdump <addr> <len>\r\nstore\r\nstorewipe\r\n"
              "bk r|w|chip|filter|ant|tune|scan|stop|res|rssi[kHz]|fc\r\n");
  else
    uart_puts("ERR\r\n");
}

void Console_Poll(void) {
  for (int i = 0; i < POLL_BYTES; i++) {
    int c = uart_getc();
    if (c < 0)
      break;

    if (c == '\r' || c == '\n') {
      s_cmd[s_len] = 0;
      if (s_len)
        run(s_cmd);
      s_len = 0;
    } else if (s_len < CMD_MAX - 1) {
      s_cmd[s_len++] = (char)c;
    }
  }
}
