/* startup.s — Radtel RT-880 / AT32F423 (Artery Cortex-M4F)
 * Flash: 0x08000000, 256KB
 * SRAM:  0x20000000, 48KB (0x2000C000)
 * SP:    0x20009290 (как в оригинале)
 * IRQ:   76 внешних каналов
 */

    .syntax unified
    .cpu cortex-m4
    .fpu softvfp
    .thumb

    .global _estack
    .global _sidata, _sdata, _edata
    .global _sbss, _ebss

/* ─── Таблица векторов ─────────────────────────────────────────────── */
    .section .isr_vector, "a", %progbits
    .type vector_table, %object

vector_table:
    /* Cortex-M4 core */
    .word   _estack
    .word   Reset_Handler
    .word   NMI_Handler
    .word   HardFault_Handler
    .word   MemManage_Handler
    .word   BusFault_Handler
    .word   UsageFault_Handler
    .word   0
    .word   0
    .word   0
    .word   0
    .word   SVC_Handler
    .word   DebugMon_Handler
    .word   0
    .word   PendSV_Handler
    .word   SysTick_Handler

    /* AT32F423 внешние IRQ (из официального HAL/startup_at32f423.s) */
    .word   WWDT_IRQHandler             /* IRQ0:  Window watchdog */
    .word   PVM_IRQHandler              /* IRQ1:  PVM via EXINT */
    .word   TAMP_STAMP_IRQHandler       /* IRQ2:  Tamper/timestamp */
    .word   ERTC_WKUP_IRQHandler        /* IRQ3:  RTC wakeup */
    .word   FLASH_IRQHandler            /* IRQ4:  Flash */
    .word   CRM_IRQHandler              /* IRQ5:  CRM clock */
    .word   EXINT0_IRQHandler           /* IRQ6:  EXINT Line 0 */
    .word   EXINT1_IRQHandler           /* IRQ7:  EXINT Line 1 */
    .word   EXINT2_IRQHandler           /* IRQ8:  EXINT Line 2 */
    .word   EXINT3_IRQHandler           /* IRQ9:  EXINT Line 3 */
    .word   EXINT4_IRQHandler           /* IRQ10: EXINT Line 4 */
    .word   DMA1_Channel1_IRQHandler    /* IRQ11 */
    .word   DMA1_Channel2_IRQHandler    /* IRQ12 */
    .word   DMA1_Channel3_IRQHandler    /* IRQ13 */
    .word   DMA1_Channel4_IRQHandler    /* IRQ14 */
    .word   DMA1_Channel5_IRQHandler    /* IRQ15 */
    .word   DMA1_Channel6_IRQHandler    /* IRQ16 */
    .word   DMA1_Channel7_IRQHandler    /* IRQ17 */
    .word   ADC1_IRQHandler             /* IRQ18 */
    .word   CAN1_TX_IRQHandler          /* IRQ19 */
    .word   CAN1_RX0_IRQHandler         /* IRQ20 */
    .word   CAN1_RX1_IRQHandler         /* IRQ21 */
    .word   CAN1_SE_IRQHandler          /* IRQ22 */
    .word   EXINT9_5_IRQHandler         /* IRQ23: EXINT Lines 5-9 */
    .word   TMR1_BRK_TMR9_IRQHandler    /* IRQ24 */
    .word   TMR1_OVF_TMR10_IRQHandler   /* IRQ25 */
    .word   TMR1_TRG_HALL_TMR11_IRQHandler /* IRQ26 */
    .word   TMR1_CH_IRQHandler          /* IRQ27 */
    .word   TMR2_GLOBAL_IRQHandler      /* IRQ28 */
    .word   TMR3_GLOBAL_IRQHandler      /* IRQ29 */
    .word   TMR4_GLOBAL_IRQHandler      /* IRQ30 */
    .word   I2C1_EVT_IRQHandler         /* IRQ31 */
    .word   I2C1_ERR_IRQHandler         /* IRQ32 */
    .word   I2C2_EVT_IRQHandler         /* IRQ33 */
    .word   I2C2_ERR_IRQHandler         /* IRQ34 */
    .word   SPI1_IRQHandler             /* IRQ35 */
    .word   SPI2_IRQHandler             /* IRQ36 */
    .word   USART1_IRQHandler           /* IRQ37 */
    .word   USART2_IRQHandler           /* IRQ38 */
    .word   USART3_IRQHandler           /* IRQ39 */
    .word   EXINT15_10_IRQHandler       /* IRQ40: EXINT Lines 10-15 */
    .word   ERTCAlarm_IRQHandler        /* IRQ41 */
    .word   OTGFS1_WKUP_IRQHandler      /* IRQ42 */
    .word   TMR8_BRK_TMR12_IRQHandler   /* IRQ43 */
    .word   TMR8_OVF_TMR13_IRQHandler   /* IRQ44 */
    .word   TMR8_TRG_HALL_TMR14_IRQHandler /* IRQ45 */
    .word   TMR8_CH_IRQHandler          /* IRQ46 */
    .word   0                           /* IRQ47: Reserved */
    .word   XMC_IRQHandler              /* IRQ48 */
    .word   0                           /* IRQ49: Reserved */
    .word   TMR5_GLOBAL_IRQHandler      /* IRQ50 */
    .word   SPI3_IRQHandler             /* IRQ51 */
    .word   UART4_IRQHandler            /* IRQ52 */
    .word   UART5_IRQHandler            /* IRQ53 */
    .word   TMR6_DAC_GLOBAL_IRQHandler  /* IRQ54 */
    .word   TMR7_GLOBAL_IRQHandler      /* IRQ55 */
    .word   DMA2_Channel1_IRQHandler    /* IRQ56 */
    .word   DMA2_Channel2_IRQHandler    /* IRQ57 */
    .word   DMA2_Channel3_IRQHandler    /* IRQ58 */
    .word   DMA2_Channel4_IRQHandler    /* IRQ59 */
    .word   DMA2_Channel5_IRQHandler    /* IRQ60 */
    .word   0                           /* IRQ61: Reserved */
    .word   0                           /* IRQ62: Reserved */
    .word   CAN2_TX_IRQHandler          /* IRQ63 */
    .word   CAN2_RX0_IRQHandler         /* IRQ64 */
    .word   CAN2_RX1_IRQHandler         /* IRQ65 */
    .word   CAN2_SE_IRQHandler          /* IRQ66 */
    .word   OTGFS1_IRQHandler           /* IRQ67 */
    .word   DMA2_Channel6_IRQHandler    /* IRQ68 */
    .word   DMA2_Channel7_IRQHandler    /* IRQ69 */
    .word   USART6_IRQHandler           /* IRQ70 */
    .word   I2C3_EVT_IRQHandler         /* IRQ71 */
    .word   I2C3_ERR_IRQHandler         /* IRQ72 */
    .word   0                           /* IRQ73: Reserved */
    .word   0                           /* IRQ74: Reserved */
    .word   0                           /* IRQ75: Reserved */

    .size vector_table, .-vector_table

/* ─── Reset Handler ────────────────────────────────────────────────── */
    .section .text.Reset_Handler, "ax", %progbits
    .type Reset_Handler, %function
    .global Reset_Handler
    .thumb_func

Reset_Handler:
    /* Копируем .data: Flash → RAM */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
    b       .Lcopy_check
.Lcopy_loop:
    ldr     r3, [r2], #4
    str     r3, [r0], #4
.Lcopy_check:
    cmp     r0, r1
    blo     .Lcopy_loop

    /* Обнуляем .bss */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    mov     r2, #0
    b       .Lzero_check
.Lzero_loop:
    str     r2, [r0], #4
.Lzero_check:
    cmp     r0, r1
    blo     .Lzero_loop

    bl      SystemInit
    bl      main
    b       .

    .size Reset_Handler, .-Reset_Handler

/* ─── Default Handler ──────────────────────────────────────────────── */
    .section .text.Default_Handler, "ax", %progbits
    .type Default_Handler, %function
    .global Default_Handler
    .thumb_func
Default_Handler:
    b       .
    .size Default_Handler, .-Default_Handler

/* ─── Weak aliases ─────────────────────────────────────────────────── */
    .macro WEAK_IRQ name
    .weak \name
    .thumb_set \name, Default_Handler
    .endm

    WEAK_IRQ NMI_Handler
    WEAK_IRQ HardFault_Handler
    WEAK_IRQ MemManage_Handler
    WEAK_IRQ BusFault_Handler
    WEAK_IRQ UsageFault_Handler
    WEAK_IRQ SVC_Handler
    WEAK_IRQ DebugMon_Handler
    WEAK_IRQ PendSV_Handler
    WEAK_IRQ SysTick_Handler

    WEAK_IRQ WWDT_IRQHandler
    WEAK_IRQ PVM_IRQHandler
    WEAK_IRQ TAMP_STAMP_IRQHandler
    WEAK_IRQ ERTC_WKUP_IRQHandler
    WEAK_IRQ FLASH_IRQHandler
    WEAK_IRQ CRM_IRQHandler
    WEAK_IRQ EXINT0_IRQHandler
    WEAK_IRQ EXINT1_IRQHandler
    WEAK_IRQ EXINT2_IRQHandler
    WEAK_IRQ EXINT3_IRQHandler
    WEAK_IRQ EXINT4_IRQHandler
    WEAK_IRQ DMA1_Channel1_IRQHandler
    WEAK_IRQ DMA1_Channel2_IRQHandler
    WEAK_IRQ DMA1_Channel3_IRQHandler
    WEAK_IRQ DMA1_Channel4_IRQHandler
    WEAK_IRQ DMA1_Channel5_IRQHandler
    WEAK_IRQ DMA1_Channel6_IRQHandler
    WEAK_IRQ DMA1_Channel7_IRQHandler
    WEAK_IRQ ADC1_IRQHandler
    WEAK_IRQ CAN1_TX_IRQHandler
    WEAK_IRQ CAN1_RX0_IRQHandler
    WEAK_IRQ CAN1_RX1_IRQHandler
    WEAK_IRQ CAN1_SE_IRQHandler
    WEAK_IRQ EXINT9_5_IRQHandler
    WEAK_IRQ TMR1_BRK_TMR9_IRQHandler
    WEAK_IRQ TMR1_OVF_TMR10_IRQHandler
    WEAK_IRQ TMR1_TRG_HALL_TMR11_IRQHandler
    WEAK_IRQ TMR1_CH_IRQHandler
    WEAK_IRQ TMR2_GLOBAL_IRQHandler
    WEAK_IRQ TMR3_GLOBAL_IRQHandler
    WEAK_IRQ TMR4_GLOBAL_IRQHandler
    WEAK_IRQ I2C1_EVT_IRQHandler
    WEAK_IRQ I2C1_ERR_IRQHandler
    WEAK_IRQ I2C2_EVT_IRQHandler
    WEAK_IRQ I2C2_ERR_IRQHandler
    WEAK_IRQ SPI1_IRQHandler
    WEAK_IRQ SPI2_IRQHandler
    WEAK_IRQ USART1_IRQHandler
    WEAK_IRQ USART2_IRQHandler
    WEAK_IRQ USART3_IRQHandler
    WEAK_IRQ EXINT15_10_IRQHandler
    WEAK_IRQ ERTCAlarm_IRQHandler
    WEAK_IRQ OTGFS1_WKUP_IRQHandler
    WEAK_IRQ TMR8_BRK_TMR12_IRQHandler
    WEAK_IRQ TMR8_OVF_TMR13_IRQHandler
    WEAK_IRQ TMR8_TRG_HALL_TMR14_IRQHandler
    WEAK_IRQ TMR8_CH_IRQHandler
    WEAK_IRQ XMC_IRQHandler
    WEAK_IRQ TMR5_GLOBAL_IRQHandler
    WEAK_IRQ SPI3_IRQHandler
    WEAK_IRQ UART4_IRQHandler
    WEAK_IRQ UART5_IRQHandler
    WEAK_IRQ TMR6_DAC_GLOBAL_IRQHandler
    WEAK_IRQ TMR7_GLOBAL_IRQHandler
    WEAK_IRQ DMA2_Channel1_IRQHandler
    WEAK_IRQ DMA2_Channel2_IRQHandler
    WEAK_IRQ DMA2_Channel3_IRQHandler
    WEAK_IRQ DMA2_Channel4_IRQHandler
    WEAK_IRQ DMA2_Channel5_IRQHandler
    WEAK_IRQ CAN2_TX_IRQHandler
    WEAK_IRQ CAN2_RX0_IRQHandler
    WEAK_IRQ CAN2_RX1_IRQHandler
    WEAK_IRQ CAN2_SE_IRQHandler
    WEAK_IRQ OTGFS1_IRQHandler
    WEAK_IRQ DMA2_Channel6_IRQHandler
    WEAK_IRQ DMA2_Channel7_IRQHandler
    WEAK_IRQ USART6_IRQHandler
    WEAK_IRQ I2C3_EVT_IRQHandler
    WEAK_IRQ I2C3_ERR_IRQHandler

    .end
