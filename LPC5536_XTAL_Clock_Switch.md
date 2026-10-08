# LPC5536 EVK — Switching from FRO12M to XTAL Clock in `main()`

## Overview

By default, the LPC5536 boots using the internal FRO12M oscillator. This guide explains how to detach FRO12M and attach the on-board 16 MHz crystal oscillator (XTAL) as the MAIN_CLK source, using the MCUXpresso SDK clock driver APIs directly in `main()`.

---

## Clock Mux Architecture

The MAIN_CLK selection on the LPC55S36 is a two-level multiplexer (Select A → Select B). The relevant attach IDs defined in `fsl_clock.h` are:

| Attach ID | Clock Source |
|---|---|
| `kFRO12M_to_MAIN_CLK` | Internal FRO 12 MHz (reset default) |
| `kEXT_CLK_to_MAIN_CLK` | External crystal oscillator (XTAL) |
| `kFRO_HF_to_MAIN_CLK` | FRO high frequency (96 MHz) |

The LPC5536 EVK populates a **16 MHz** crystal on the XTALIN/XTALOUT pins.

---

## Required SDK APIs

| API | Purpose |
|---|---|
| `POWER_DisablePD(kPDRUNCFG_PD_FRO192M)` | Keep FRO powered on as a safe anchor |
| `CLOCK_SetupFROClocking(freq)` | Configure FRO output frequency |
| `CLOCK_AttachClk(id)` | Switch the clock mux |
| `POWER_SetVoltageForFreq(freq)` | Adjust core voltage for target frequency |
| `CLOCK_SetFLASHAccessCyclesForFreq(freq)` | Set FLASH wait states |
| `ANACTRL->XO32M_CTRL |= ANACTRL_XO32M_CTRL_ENABLE_SYSTEM_CLK_OUT_MASK` | Enable XTAL output to SYSCON |
| `CLOCK_SetupExtClocking(freq)` | Register XTAL frequency with the driver |
| `CLOCK_GetCoreSysClkFreq()` | Read the resulting core clock frequency |

---

## Step-by-Step Procedure

### 1. Anchor on FRO12M (safe intermediate state)

Before touching any clock source, switch to FRO12M to ensure a stable clock during the transition.

```c
POWER_DisablePD(kPDRUNCFG_PD_FRO192M);          // Keep FRO powered on as anchor clock
CLOCK_SetupFROClocking(12000000U);               // Configure FRO to 12 MHz
CLOCK_AttachClk(kFRO12M_to_MAIN_CLK);           // Switch MAIN_CLK to FRO 12 MHz
POWER_SetVoltageForFreq(12000000U);              // Set voltage for 12 MHz operation
CLOCK_SetFLASHAccessCyclesForFreq(12000000U);    // Set FLASH wait states
CLOCK_SetClkDiv(kCLOCK_DivAhbClk, 1U, false);  // AHB divider = 1
// NOTE: kPDRUNCFG_PD_XTAL32M does NOT exist on LPC55S36 - XTAL is enabled via ANACTRL only
```

### 2. Enable the XTAL Crystal Oscillator

The XTAL output must be explicitly enabled through the Analog Control (ANACTRL) block before the SYSCON mux can use it.

```c
ANACTRL->XO32M_CTRL |= ANACTRL_XO32M_CTRL_ENABLE_SYSTEM_CLK_OUT_MASK;
```

> **Note:** Without this step, `kEXT_CLK_to_MAIN_CLK` will not have a valid clock source.

### 3. Register the XTAL Frequency with the Driver

```c
CLOCK_SetupExtClocking(16000000U);   // 16 MHz XTAL on the LPC5536 EVK
```

### 4. Attach XTAL to MAIN_CLK

```c
CLOCK_AttachClk(kEXT_CLK_to_MAIN_CLK);   // Switch MAIN_CLK to XTAL
```

### 5. Update SystemCoreClock

```c
SystemCoreClock = CLOCK_GetCoreSysClkFreq();
```

---

## Complete `main()` Example

This is the working example based on the SDK `hello_world` project.

> **Key lesson learned:** `BOARD_InitHardware()` calls the board's default boot clock function
> (`BOARD_BootClockPLL150M`), which overrides any clock setup placed before it and switches
> MAIN_CLK to PLL0 at 150 MHz. However, it also enables the XTAL internally as the PLL0
> reference — so EXT_CLK is valid after `BOARD_InitHardware()` returns. The correct approach
> is to let `BOARD_InitHardware()` run first, then re-attach MAIN_CLK to EXT_CLK.
>
> **Why the UART still works after switching:** The debug UART (FlexComm) is clocked from its
> own source (FRO12M), independent of MAIN_CLK. Switching MAIN_CLK to 16 MHz after board init
> does not break serial output.

```c
#include "fsl_clock.h"
#include "fsl_power.h"
#include "fsl_debug_console.h"
#include "board.h"
#include "pin_mux.h"

int main(void)
{
    /* --- Step 1: Let board init run first ---
     * BOARD_InitHardware() enables the XTAL (as PLL0 reference) and sets
     * MAIN_CLK to PLL0 at 150 MHz. EXT_CLK is valid after this returns. */
    BOARD_InitHardware();

    /* --- Step 2: Re-attach MAIN_CLK directly to EXT_CLK (XTAL 16 MHz),
     * bypassing PLL0. kPDRUNCFG_PD_XTAL32M does NOT exist on LPC55S36 —
     * the XTAL is already enabled by BOARD_InitHardware() above. */
    CLOCK_AttachClk(kEXT_CLK_to_MAIN_CLK);
    SystemCoreClock = CLOCK_GetCoreSysClkFreq();

    /* --- Step 3: Verify and print --- */
    clock_attach_id_t actual = CLOCK_GetClockAttachId(kEXT_CLK_to_MAIN_CLK);
    PRINTF("--- LPC5536 EVK Clock Verification ---\r\n");
    PRINTF("MAIN_CLK mux    : %s\r\n",
        (actual == kEXT_CLK_to_MAIN_CLK) ? "EXT_CLK (XTAL) OK" : "MISMATCH");
    PRINTF("SystemCoreClock : %lu Hz\r\n", SystemCoreClock);
    PRINTF("Expected        : 16000000 Hz\r\n");
    PRINTF("Result          : %s\r\n",
        (SystemCoreClock == 16000000U) ? "PASS" : "FAIL");
    PRINTF("--------------------------------------\r\n");

    PRINTF("\r\nhello world.\r\n");

    while (1) { }
    return 0;
}
```

Expected serial output:

```
--- LPC5536 EVK Clock Verification ---
MAIN_CLK mux    : EXT_CLK (XTAL) OK
SystemCoreClock : 16000000 Hz
Expected        : 16000000 Hz
Result          : PASS
--------------------------------------

hello world.
```

---

## FRO192M Active-Mode Power-Down

### Background

The FRO192M supplies the 12 MHz, 48 MHz, and 96 MHz clock outputs. Powering it down in Active mode is not documented as a supported operation by NXP — the reference manual and AN13530 only describe FRO192M shutdown as part of Power-down mode.

However, through systematic dependency elimination and a RAM-execution strategy, FRO192M **can** be powered down while the CPU remains active. The key findings from testing on the LPCXpresso55S36 EVK:

- `POWER_EnablePD()` executes from Flash — after FRO outputs are disabled, Flash fetches corrupt and cause a reset. Use `PMC->PDRUNCFGSET0` direct register writes instead.
- `PRINTF()` / SDK debug console calls `CLOCK_XtalHfCapabankTrim()` internally, which asserts when FRO outputs are disabled. Use direct USART TX FIFO writes after FRO disable.
- Reading any PMC register after FRO outputs are disabled causes an MCU reset regardless of `SELCLOCK` setting. Snapshot all PMC register values before disabling FRO outputs.
- `TXEMPTY` polling hangs after FRO outputs are disabled. Use `TXNOTFULL` per-character polling instead.
- Flash instruction fetches and string literal reads corrupt after FRO disable even with prefetch/cache disabled. Place all post-FRO functions in `.ramfunc` section and all strings in `static` RAM buffers.
- CSS (Crypto Security Subsystem) AHB clock must be disabled before FRO outputs are removed, or it stalls the AHB bus.
- Jira MCUCM-12448 documents that the PMC must be switched from FRO12M to FRO1M via `PMC->CTRL &= ~PMC_CTRL_SELCLOCK_MASK` before any FRO disable attempt.

### Required Sequence

The following steps must all be completed **in order** before FRO192M can be powered down:

| Step | Action | API / Register |
|---|---|---|
| 1 | MAIN_CLK → XTAL | `CLOCK_AttachClk(kEXT_CLK_to_MAIN_CLK)` |
| 2 | FLEXCOMM0 → MAIN_CLK/XTAL | `CLOCK_AttachClk(kMAIN_CLK_to_FLEXCOMM0)` |
| 3 | Reinitialize UART for 16 MHz | `DbgConsole_Deinit()` / `DbgConsole_Init()` |
| 4 | PMC → FRO1M | `PMC->CTRL &= ~PMC_CTRL_SELCLOCK_MASK` |
| 5 | SYSCON FRO1M mux disabled | `SYSCON->CLOCK_CTRL &= ~SYSCON_CLOCK_CTRL_FRO1MHZ_CLK_ENA_MASK` |
| 6 | Flash prefetch/cache disabled | `SYSCON->FMCCR &= ~(FETCHCFG_MASK \| DATACFG_MASK)` |
| 7 | CSS AHB clock disabled | `SYSCON->AHBCLKCTRL2 &= ~(1UL << 18U)` |
| 8 | Snapshot all PMC/SYSCON registers | Read before FRO disable |
| 9 | FRO12M output disabled | `ANACTRL->FRO192M_CTRL &= ~ENA_12MHZCLK_MASK` |
| 10 | FRO96M output disabled | `ANACTRL->FRO192M_CTRL &= ~ENA_96MHZCLK_MASK` |
| 11 | FRO48M output disabled | `ANACTRL->FRO192M_CTRL &= ~(1UL << 15U)` |
| 12 | PLL0 / PLL0_SSCG powered down | `PMC->PDRUNCFGSET0 = PLL0_MASK \| PLL0_SSCG_MASK` |
| 13 | FRO192M powered down | `PMC->PDRUNCFGSET0 = PMC_PDRUNCFG0_PDEN_FRO192M_MASK` |

### RAM Execution Requirements

All code that executes after Step 9 (FRO12M output disabled) must run from RAM:

```c
/* Place functions in RAM */
__attribute__((section(".ramfunc"))) int main(void)
__attribute__((section(".ramfunc"))) static void DirectPrint(const char *str)
__attribute__((section(".ramfunc"))) static void DirectPrintHex(uint32_t val)

/* Place strings in static RAM */
static char strExample[] = "Example string\r\n";

/* Use direct USART TX FIFO writes — not PRINTF() */
#define DIRECT_PUTCHAR(c) \
    do { \
        while ((USART0->FIFOSTAT & USART_FIFOSTAT_TXNOTFULL_MASK) == 0U) {} \
        USART0->FIFOWR = (uint8_t)(c); \
    } while (0)

/* Use PDRUNCFGSET0 for power-down — not POWER_EnablePD() */
PMC->PDRUNCFGSET0 = PMC_PDRUNCFG0_PDEN_FRO192M_MASK;
```

### Confirmed Output

After the complete sequence, the verified terminal output on the LPCXpresso55S36 EVK is:

```text
--- LPC5536 EVK Clock Verification ---
MAIN_CLK mux    : EXT_CLK (XTAL) OK
SystemCoreClock : 16000000 Hz
Expected        : 16000000 Hz
FLEXCOMM0 mux   : MAIN_CLK / XTAL OK
FLEXCOMM0 freq  : 16000000 Hz
PMC clock source : FRO1M
SYSCON FRO1M mux : DISABLED
Flash prefetch  : DISABLED
CSS clock       : DISABLED
Registers snapshot captured.
FRO12M output   : OFF
FRO96M output   : OFF
FRO48M output   : OFF
FRO192M_CTRL    : 0x00801217

--- Final Register Status ---
FRO192M_CTRL    : 0x00801217
PDRUNCFG0 (snap): 0xFE4EFCC0
AHBCLKCTRL0     : 0x0000E1FB
AHBCLKCTRL1     : 0x00000800
AHBCLKCTRL2     : 0x08000000
hello Emerson.
FRO192M         : POWERING DOWN...
FRO192M         : POWERED DOWN
PDRUNCFG0 after : 0xFECEFEE0
FRO192M bit     : SET (PASS)
```

`PDRUNCFG0 after = 0xFECEFEE0` confirms:

| Domain | Bit | Status |
|---|---|---|
| FRO192M | 5 | OFF |
| PLL0 | 9 | OFF |
| PLL0_SSCG | 23 | OFF |
| FRO1M | 4 | ON (PMC clock) |
| XTAL32M | 8 | ON (MAIN_CLK source) |

---

## Verifying the XTAL is Attached (in `hello_world`)

After switching the clock in `main()`, use `CLOCK_GetClockAttachId()` to read back the actual hardware mux register and confirm XTAL is truly selected — not just what the driver was asked to set.

Add the following after `BOARD_InitDebugConsole()` in your modified `hello_world` `main()`:

```c
/* Read back the actual mux register state */
clock_attach_id_t actual = CLOCK_GetClockAttachId(kEXT_CLK_to_MAIN_CLK);
SystemCoreClock = CLOCK_GetCoreSysClkFreq();

PRINTF("--- Clock Verification ---\r\n");
PRINTF("MAIN_CLK mux : %s\r\n",
    (actual == kEXT_CLK_to_MAIN_CLK) ? "EXT_CLK (XTAL) OK" : "MISMATCH");
PRINTF("SystemCoreClock : %lu Hz\r\n", SystemCoreClock);
PRINTF("Expected        : 16000000 Hz\r\n");
PRINTF("Result          : %s\r\n",
    (SystemCoreClock == 16000000U) ? "PASS" : "FAIL");
```

`CLOCK_GetClockAttachId()` reads the actual SYSCON mux register and returns the attach ID that is currently active. This validates the hardware state, not just what the driver requested. If the XTAL failed to start or the `ANACTRL->XO32M_CTRL` enable bit was missed, the returned ID will not match `kEXT_CLK_to_MAIN_CLK` and the result will print `MISMATCH`.

Expected terminal output when XTAL is correctly attached:

```
--- Clock Verification ---
MAIN_CLK mux : EXT_CLK (XTAL) OK
SystemCoreClock : 16000000 Hz
Expected        : 16000000 Hz
Result          : PASS
```

---

## Important Notes

### Call `BOARD_InitDebugConsole()` After the Clock Switch

The debug UART baud rate divider is calculated from `SystemCoreClock` at initialization time. If `BOARD_InitDebugConsole()` is called before the clock switch, the baud rate will be wrong and output will be garbled.

### Do Not Skip the FRO12M Anchor Step

Switching directly from an unknown reset state to XTAL without anchoring on FRO12M first can cause the system to be momentarily clocked from an unstable or unready source during the XTAL startup transient.

### ANACTRL `XO32M_CTRL` is the Gate

The `ANACTRL_XO32M_CTRL_ENABLE_SYSTEM_CLK_OUT_MASK` bit in `ANACTRL->XO32M_CTRL` is the hardware gate that connects the running XTAL to the SYSCON clock mux. This is distinct from just powering the oscillator — you must explicitly set this bit.

### Skipping `clock_config.c` Functions

If the project calls `BOARD_BootClockFRO12M()` or a similar function from `clock_config.c`, you can either:
* Let it run as-is (it anchors on FRO12M), then add your XTAL switch code immediately after, or
* Remove the call entirely and do all clock setup in `main()` as shown above.

### Optional: Verify with FREQME

Use the Frequency Measurement Engine (FREQME) peripheral to validate the XTAL is running at 16 MHz by comparing it against the internal FRO 1 MHz reference clock.

---

## Sources

* [fsl_clock.h — MCXW236 (clock attach IDs reference)](https://nxp1-my.sharepoint.com/personal/maria_gonzalezbocanegra_nxp_com/Documents/Desktop/MM%20-%20Mexico%20Region%20-%20Projects/mcuxsdk/mcuxsdk/devices/MCX/MCXW/MCXW236/drivers/fsl_clock.h?web=1)
* [clock_config.c — LPCXpresso55S36 XTAL enable pattern](https://nxp1-my.sharepoint.com/personal/maria_gonzalezbocanegra_nxp_com/Documents/Desktop/MM%20-%20Mexico%20Region%20-%20Projects/mcuxsdk/mcuxsdk/examples/_boards/lpcxpresso55s36/demo_apps/mc_pmsm/pmsm_enc_iopamp/clock_config.c?web=1)
* [Getting Started with the LPC5536/LPC55S36 Evaluation Board](https://www.nxp.com/document/guide/getting-started-with-the-lpc5536-lpc55s36-evaluation-board:GS-LPC55S36-EVK)
