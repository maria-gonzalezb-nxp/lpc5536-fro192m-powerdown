/*
 * Copyright (c) 2013 - 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017, 2024 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "fsl_clock.h"
#include "fsl_power.h"
#include "fsl_common.h"
#include "board.h"
#include "app.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define XTAL_FREQ_HZ    16000000U   /* 16 MHz crystal on LPC5536 EVK */

/*
 * DIRECT_PUTCHAR: Write one byte directly to the USART0 TX FIFO.
 *
 * Used after FRO outputs are disabled. The SDK debug console lite
 * internally calls CLOCK_XtalHfCapabankTrim() which asserts when
 * FRO outputs are disabled, causing an MCU reset. Direct USART
 * register writes bypass the SDK clock driver entirely.
 *
 * FIFOSTAT TXNOTFULL (bit 4) = 1 when TX FIFO can accept data.
 * FIFOWR bits [7:0] = data byte to transmit.
 * Reference: LPC553xRM Chapter 24, USART FIFOSTAT and FIFOWR.
 */
#define DIRECT_PUTCHAR(c) \
    do { \
        while ((USART0->FIFOSTAT & \
                USART_FIFOSTAT_TXNOTFULL_MASK) == 0U) {} \
        USART0->FIFOWR = (uint8_t)(c); \
    } while (0)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void DirectPrint(const char *str)
    __attribute__((section(".ramfunc")));
static void DirectPrintHex(uint32_t val)
    __attribute__((section(".ramfunc")));

/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/

/*!
 * @brief Print a null-terminated string directly to USART0 TX FIFO.
 *
 * Placed in RAM via .ramfunc section so the function body executes
 * from RAM after FRO outputs are disabled. Flash instruction fetches
 * corrupt after FRO disable even with prefetch/cache disabled.
 * Reference: NXP SDK linker script .ramfunc section.
 */
__attribute__((section(".ramfunc")))
static void DirectPrint(const char *str)
{
    while (*str != '\0')
    {
        DIRECT_PUTCHAR(*str);
        str++;
    }
}

/*!
 * @brief Print a 32-bit hex value directly to USART0 TX FIFO.
 *
 * Placed in RAM via .ramfunc section so the function body executes
 * from RAM after FRO outputs are disabled. Uses arithmetic instead
 * of a lookup table to avoid any Flash data reads inside the
 * function.
 * Reference: NXP SDK linker script .ramfunc section.
 */
__attribute__((section(".ramfunc")))
static void DirectPrintHex(uint32_t val)
{
    uint32_t nibble;
    int i;

    DIRECT_PUTCHAR('0');
    DIRECT_PUTCHAR('x');

    for (i = 28; i >= 0; i -= 4)
    {
        nibble = (val >> (uint32_t)i) & 0xFU;
        if (nibble < 10U)
        {
            DIRECT_PUTCHAR((char)('0' + nibble));
        }
        else
        {
            DIRECT_PUTCHAR((char)('A' + (nibble - 10U)));
        }
    }
}

/*!
 * @brief Main function
 *
 * Clock-switch sequence:
 *   1. Run BOARD_InitHardware() first.
 *      BOARD_BootClockPLL150M (called internally) sets MAIN_CLK to PLL0 at
 *      150 MHz and enables the XTAL as the PLL0 reference, so EXT_CLK is
 *      valid and stable when BOARD_InitHardware() returns.
 *      Note: kPDRUNCFG_PD_XTAL32M does NOT exist on LPC55S36 -- the XTAL
 *      is enabled entirely by the board boot clock function via ANACTRL.
 *   2. Re-attach MAIN_CLK directly to EXT_CLK (XTAL, 16 MHz), bypassing PLL0.
 *      Reconfigure Flash wait states for 16 MHz after the switch.
 *   3. Move FLEXCOMM0 to MAIN_CLK/XTAL before disabling any FRO output.
 *   4. Move PMC clock from FRO12M to FRO1M (Jira MCUCM-12448).
 *   5. Disable SYSCON FRO1M mux routing and Flash prefetch/cache.
 *   6. Disable CSS AHB clock before disabling FRO outputs.
 *   7. Snapshot all registers before disabling FRO outputs.
 *      Reading any PMC register after FRO outputs are disabled
 *      causes an MCU reset regardless of SELCLOCK setting.
 *   8. Disable FRO12M, FRO96M, FRO48M outputs.
 *      All output after FRO disable uses RAM-resident DirectPrint/
 *      DirectPrintHex with static RAM string buffers.
 *   9. Power down PLL0 and PLL0_SSCG using direct PMC register
 *      writes from RAM -- POWER_EnablePD() executes from Flash
 *      and causes reset after FRO outputs are disabled.
 *  10. Power down FRO192M.
 *  11. main() placed in .ramfunc so all instruction fetches are
 *      from RAM after FRO outputs are disabled.
 */
__attribute__((section(".ramfunc"))) int main(void)
{
    clock_attach_id_t mainActual;
    clock_attach_id_t flexcomm0Actual;
    uint32_t flexcomm0Freq;
    uint32_t pmcCtrlAfter;
    uint32_t froCtrlAfter;
    uint32_t pdruncfg0Snapshot;
    uint32_t ahbclkctrl0Snapshot;
    uint32_t ahbclkctrl1Snapshot;
    uint32_t ahbclkctrl2Snapshot;

    /*
     * Static RAM string buffers for use after FRO outputs are
     * disabled.
     *
     * Using 'static' places these in the .data section in RAM at
     * link time rather than on the stack. This eliminates stack
     * pressure and guarantees the strings are in RAM before FRO
     * outputs are disabled.
     */
    static char strFro12Off[]   = "FRO12M output   : OFF\r\n";
    static char strFro96Off[]   = "FRO96M output   : OFF\r\n";
    static char strFro48Off[]   = "FRO48M output   : OFF\r\n";
    static char strFroCtrl[]    = "FRO192M_CTRL    : ";
    static char strFinalHdr[]   = "\r\n--- Final Register Status ---\r\n";
    static char strFroCtrl2[]   = "FRO192M_CTRL    : ";
    static char strPdrun[]      = "PDRUNCFG0 (snap): ";
    static char strAhb0[]       = "AHBCLKCTRL0     : ";
    static char strAhb1[]       = "AHBCLKCTRL1     : ";
    static char strAhb2[]       = "AHBCLKCTRL2     : ";
    static char strCrlf[]       = "\r\n";
    static char strHello[]      = "hello Emerson.\r\n";
    static char strFroPdn[]     = "FRO192M         : POWERING DOWN...\r\n";
    static char strFroPdDone[]  = "FRO192M         : POWERED DOWN\r\n";
    static char strPdrun2[]     = "PDRUNCFG0 after : ";
    static char strPass[]       = "FRO192M bit     : SET (PASS)\r\n";
    static char strFail[]       = "FRO192M bit     : NOT SET (FAIL)\r\n";

    /* 1. Initialize the board and debug console. */
    BOARD_InitHardware();

    /* 2. MAIN_CLK = 16 MHz external XTAL. */
    CLOCK_AttachClk(kEXT_CLK_to_MAIN_CLK);
    SystemCoreClock = CLOCK_GetCoreSysClkFreq();

    /*
     * Reconfigure Flash wait states for 16 MHz operation.
     * BOARD_BootClockPLL150M() set Flash wait states for 150 MHz.
     * Reference: LPC553xRM Chapter 6, Flash controller timing.
     */
    CLOCK_SetFLASHAccessCyclesForFreq(16000000U);

    /* 3. FLEXCOMM0/debug UART = MAIN_CLK/XTAL. */
    CLOCK_AttachClk(kMAIN_CLK_to_FLEXCOMM0);
    flexcomm0Freq = CLOCK_GetFlexCommClkFreq(0U);

    /* 4. Reinitialize UART for its 16 MHz clock. */
    DbgConsole_Deinit();

    DbgConsole_Init(BOARD_DEBUG_UART_INSTANCE,
                    BOARD_DEBUG_UART_BAUDRATE,
                    BOARD_DEBUG_UART_TYPE,
                    flexcomm0Freq);

    /* 5. Verify clock routing. */
    mainActual = CLOCK_GetClockAttachId(kEXT_CLK_to_MAIN_CLK);
    flexcomm0Actual = CLOCK_GetClockAttachId(kMAIN_CLK_to_FLEXCOMM0);

    PRINTF("\r\n--- LPC5536 EVK Clock Verification ---\r\n");
    PRINTF("MAIN_CLK mux    : %s\r\n",
           (mainActual == kEXT_CLK_to_MAIN_CLK)
               ? "EXT_CLK (XTAL) OK"
               : "MISMATCH");
    PRINTF("SystemCoreClock : %u Hz\r\n", SystemCoreClock);
    PRINTF("Expected        : %u Hz\r\n", XTAL_FREQ_HZ);
    PRINTF("FLEXCOMM0 mux   : %s\r\n",
           (flexcomm0Actual == kMAIN_CLK_to_FLEXCOMM0)
               ? "MAIN_CLK / XTAL OK"
               : "MISMATCH");
    PRINTF("FLEXCOMM0 freq  : %u Hz\r\n", flexcomm0Freq);

    /*
     * 6. Move PMC from FRO12M to FRO1M.
     *
     * POWER_PowerInit() sets PMC->CTRL SELCLOCK = 1 (FRO12M).
     * Must change to 0 (FRO1M) before any FRO disable attempt.
     * Reference: LPC553xRM Chapter 36, PMC->CTRL SELCLOCK field.
     *            Jira MCUCM-12448: PMC clocking issue LPC553x.
     */
    POWER_DisablePD(kPDRUNCFG_PD_FRO1M);
    PMC->CTRL &= ~PMC_CTRL_SELCLOCK_MASK;

    __DSB();
    __ISB();

    pmcCtrlAfter = PMC->CTRL;

    PRINTF("PMC clock source : %s\r\n",
           ((pmcCtrlAfter & PMC_CTRL_SELCLOCK_MASK) == 0U)
               ? "FRO1M"
               : "FRO12M");

    (void)pmcCtrlAfter;

    /*
     * 7. Remove FRO1M from SYSCON clock mux routing.
     *
     * Reference: Jira MCUCM-12448, LPC553x PMC clocking issue.
     */
    SYSCON->CLOCK_CTRL &= ~SYSCON_CLOCK_CTRL_FRO1MHZ_CLK_ENA_MASK;

    __DSB();
    __ISB();

    PRINTF("SYSCON FRO1M mux : DISABLED\r\n");

    /*
     * 8. Disable the Flash prefetch and data cache.
     *
     * Reference: LPC553xRM Chapter 6, SYSCON->FMCCR register,
     *            FETCHCFG and DATACFG fields.
     */
    SYSCON->FMCCR &= ~(SYSCON_FMCCR_FETCHCFG_MASK |
                       SYSCON_FMCCR_DATACFG_MASK);

    __DSB();
    __ISB();

    PRINTF("Flash prefetch  : DISABLED\r\n");

    /*
     * 9. Disable CSS AHB clock before disabling FRO outputs.
     *
     * CSS AHB clock enable is bit 18 of AHBCLKCTRL2.
     * CSS may use FRO-derived clocks internally and can stall
     * the AHB bus after FRO outputs are disabled.
     * Reference: LPC553xRM Chapter 7, SYSCON->AHBCLKCTRL2 bit 18.
     *            fsl_power.c low-power entry sequence.
     */
    SYSCON->AHBCLKCTRL2 &= ~(1UL << 18U);

    __DSB();
    __ISB();

    PRINTF("CSS clock       : DISABLED\r\n");

    /*
     * 10. Snapshot all registers before disabling FRO outputs.
     *
     * Reading any PMC register after FRO outputs are disabled
     * causes an MCU reset. All values captured here while FRO
     * outputs are still active.
     * Reference: LPC553xRM Chapter 36, PMC->PDRUNCFG0.
     *            LPC553xRM Chapter 7, SYSCON->AHBCLKCTRLn.
     */
    pdruncfg0Snapshot   = PMC->PDRUNCFG0;
    ahbclkctrl0Snapshot = SYSCON->AHBCLKCTRL0;
    ahbclkctrl1Snapshot = SYSCON->AHBCLKCTRL1;
    ahbclkctrl2Snapshot = SYSCON->AHBCLKCTRL2;

    PRINTF("Registers snapshot captured.\r\n");

    /*
     * 11. Disable FRO12M output.
     *
     * Drain TX FIFO BEFORE each FRO register write only.
     * Do NOT poll TXEMPTY after FRO outputs are disabled.
     * After this point all output uses RAM-resident functions
     * and static RAM string buffers.
     * Reference: LPC553xRM Chapter 35, ANACTRL->FRO192M_CTRL.
     *            LPC553xRM Chapter 24, USART FIFOSTAT.
     */
    while ((USART0->FIFOSTAT & USART_FIFOSTAT_TXEMPTY_MASK) == 0U) {}

    ANACTRL->FRO192M_CTRL &=
        ~ANACTRL_FRO192M_CTRL_ENA_12MHZCLK_MASK;

    __DSB();
    __ISB();

    DirectPrint(strFro12Off);

    /*
     * 12. Disable FRO 96 MHz output.
     *
     * Reference: LPC553xRM Chapter 35, ANACTRL->FRO192M_CTRL
     *            ENA_96MHZCLK bit field (bit 21).
     */
    while ((USART0->FIFOSTAT & USART_FIFOSTAT_TXEMPTY_MASK) == 0U) {}

    ANACTRL->FRO192M_CTRL &=
        ~ANACTRL_FRO192M_CTRL_ENA_96MHZCLK_MASK;

    __DSB();
    __ISB();

    DirectPrint(strFro96Off);

    /*
     * 13. Disable FRO 48 MHz output.
     *
     * ENA_48MHZCLK is bit 15 of ANACTRL->FRO192M_CTRL.
     * Not defined in SDK header so bit position used directly.
     * Reference: LPC553xRM Chapter 35, ANACTRL->FRO192M_CTRL
     *            ENA_48MHZCLK bit field (bit 15).
     */
    while ((USART0->FIFOSTAT & USART_FIFOSTAT_TXEMPTY_MASK) == 0U) {}

    ANACTRL->FRO192M_CTRL &= ~(1UL << 15U);

    __DSB();
    __ISB();

    froCtrlAfter = ANACTRL->FRO192M_CTRL;

    DirectPrint(strFro48Off);
    DirectPrint(strFroCtrl);
    DirectPrintHex(froCtrlAfter);
    DirectPrint(strCrlf);

    (void)froCtrlAfter;

    /*
     * 14. Print final status from static RAM snapshots.
     *
     * All values captured before FRO outputs were disabled.
     * PMC registers not re-read -- causes MCU reset after FRO
     * disable regardless of SELCLOCK setting.
     */
    DirectPrint(strFinalHdr);

    DirectPrint(strFroCtrl2);
    DirectPrintHex((uint32_t)ANACTRL->FRO192M_CTRL);
    DirectPrint(strCrlf);

    DirectPrint(strPdrun);
    DirectPrintHex(pdruncfg0Snapshot);
    DirectPrint(strCrlf);

    DirectPrint(strAhb0);
    DirectPrintHex(ahbclkctrl0Snapshot);
    DirectPrint(strCrlf);

    DirectPrint(strAhb1);
    DirectPrintHex(ahbclkctrl1Snapshot);
    DirectPrint(strCrlf);

    DirectPrint(strAhb2);
    DirectPrintHex(ahbclkctrl2Snapshot);
    DirectPrint(strCrlf);

    DirectPrint(strHello);

    /*
     * 15. Power down PLL0 and PLL0_SSCG using direct register writes.
     *
     * POWER_EnablePD() executes from Flash -- after FRO outputs are
     * disabled Flash instruction fetches corrupt and cause a reset.
     * PMC->PDRUNCFGSET0 is the set-only alias of PDRUNCFG0.
     * Writing a 1 to any bit powers down that domain atomically.
     * Reference: LPC553xRM Chapter 36, PMC->PDRUNCFGSET0.
     */
    PMC->PDRUNCFGSET0 = PMC_PDRUNCFG0_PDEN_PLL0_MASK |
                        PMC_PDRUNCFG0_PDEN_PLL0_SSCG_MASK;

    __DSB();
    __ISB();

    /*
     * 16. Power down FRO192M.
     *
     * All FRO outputs disabled, all dependencies removed.
     * PMC->PDRUNCFG0 PDEN_FRO192M (bit 5):
     *   0 = FRO192M powered ON  (default after reset)
     *   1 = FRO192M powered OFF
     *
     * Prerequisites confirmed:
     *   1.  MAIN_CLK       = XTAL (EXT_CLK).
     *   2.  FLEXCOMM0      = MAIN_CLK/XTAL.
     *   3.  PMC            = FRO1M (SELCLOCK = 0).
     *   4.  SYSCON FRO1M mux routing = disabled.
     *   5.  Flash prefetch and data cache = disabled.
     *   6.  CSS AHB clock = disabled.
     *   7.  FRO12M output  = disabled (ENA_12MHZCLK = 0).
     *   8.  FRO96M output  = disabled (ENA_96MHZCLK = 0).
     *   9.  FRO48M output  = disabled (bit 15 = 0).
     *   10. PLL0 and PLL0_SSCG = powered down.
     *   11. main() and helper functions execute from RAM.
     *   12. All strings in static RAM buffers.
     *   13. PLL0/PLL0_SSCG powered down via direct register write.
     *
     * Reference: LPC553xRM Chapter 36, PMC->PDRUNCFGSET0.
     */
    DirectPrint(strFroPdn);

    PMC->PDRUNCFGSET0 = PMC_PDRUNCFG0_PDEN_FRO192M_MASK;

    __DSB();
    __ISB();

    /*
     * Snapshot PDRUNCFG0 immediately after power-down.
     * Use RAM variable -- do not pass PMC reads to PRINTF.
     * Expected: bit 5 set = (pdrunAfter & 0x20U) == 0x20U
     */
    volatile uint32_t pdrunAfter = PMC->PDRUNCFG0;

    DirectPrint(strFroPdDone);
    DirectPrint(strPdrun2);
    DirectPrintHex(pdrunAfter);
    DirectPrint(strCrlf);

    if ((pdrunAfter & 0x20U) != 0U)
    {
        DirectPrint(strPass);
    }
    else
    {
        DirectPrint(strFail);
    }

    while (1)
    {
        __NOP();
    }
}