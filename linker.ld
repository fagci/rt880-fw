/* Radtel RT-880 — bare metal linker script
 * Flash total: 256K at 0x08000000
 * Bootloader:  first 0x2800 bytes (10KB)
 * stage-2:     0x08002800..0x080047FF (8KB)
 * Our code:    starts at 0x08004800
 * RAM:         48K at 0x20000000
 *
 * Для запуска без стокового bootloader — ORIGIN = 0x08000000, LENGTH = 256K
 */

_estack = 0x2000A000;   /* вершина стека. 0x2000C000 (реальный топ SRAM 48K)
                            не даёт устройству стартовать — похоже, верхушка
                            RAM резервирована стоковым бутлоадером.
                            Старое рабочее значение стока: 0x20009A68. */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08004800, LENGTH = 256K - 0x4800
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 48K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Таблица векторов — самое начало Flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Код и константы */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* C++ static constructors (SysTick_Handler and other ISR names live in C linkage) */
    .init_array :
    {
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
        . = ALIGN(4);
    } >FLASH

    /* Инициализированные глобальные переменные (хранятся во Flash, копируются в RAM) */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT >FLASH

    /* Неинициализированные переменные (обнуляются при старте) */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } >RAM

    /* Куча (heap) — для sprintf/при необходимости */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        _sheap = .;
        . = . + 0x100;   /* 256 Б — LVGL живёт в своём пуле, malloc не используется */
        _eheap = .;
        end = .;
    } >RAM

    /* Сбросить секции, не используемые в проекте */
    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note.*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}