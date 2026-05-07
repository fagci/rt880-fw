typedef unsigned long uint32_t;

#define GPIOA_BASE 0x40020000

static void dumb_delay(void)
{
    volatile uint32_t i;
    for (i = 0; i < 500000; ++i) {
        __asm volatile ("nop");
    }
}

int main(void)
{
    volatile uint32_t *gpio = (uint32_t *)GPIOA_BASE;
    volatile uint32_t *rcc = (uint32_t *)0x40021014;

    *rcc |= (1 << 17);

    gpio[0] &= ~((0b11 << 8) | (0b11 << 26) | (0b11 << 28));
    gpio[0] |=  ((0b01 << 8) | (0b01 << 26) | (0b01 << 28));

    gpio[1] &= ~((1 << 4) | (1 << 13) | (1 << 14));

    gpio[3] &= ~((0b11 << 8) | (0b11 << 26) | (0b11 << 28));

    while (1)
    {
        gpio[6] = (1 << 4) | (1 << 13) | (1 << 14);
        dumb_delay();
        gpio[7] = (1 << 4) | (1 << 13) | (1 << 14);
        dumb_delay();
    }
}
