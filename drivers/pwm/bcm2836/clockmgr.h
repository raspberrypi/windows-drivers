/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

Abstract:

This file contains definitions for the BCM2836 clock manager.

--*/

#pragma once

#define CM_APB_CLK_FREQ             250000000L  // 250Mhz core clock

//
// CM Audio Clock Divisors (PWMDIV)
//

#define CM_PWMDIV_DIVF_SHIFT    0
#define CM_PWMDIV_DIVF_MASK     (0xFFF<<CM_PWMDIV_DIVF_SHIFT)
#define CM_PWMDIV_DIVI_SHIFT    12
#define CM_PWMDIV_DIVI_MASK     (0xFFF<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMDIV_PASSWD_SHIFT  24
#define CM_PWMDIV_PASSWD_MASK   (0xff<<CM_PWMDIV_PASSWD_SHIFT)
#define CM_PWMDIV_PASSWD        (0x5A<<24)

//
// CM Audio Clocks Control
//

#define CM_PWMCTL_SRC_SHIFT         0
#define CM_PWMCTL_SRC_MASK          (0x0F<<CM_PWMCTL_SRC_SHIFT)
#define CM_PWMCTL_SRC_GND           (0<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_SRC_OSCILLATOR    (1<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_SRC_TESTDEBUG0    (2<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_SRC_TESTDEBUG1    (3<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_SRC_PLLA          (4<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_SRC_PLLC          (5<<CM_PWMCTL_SRC_SHIFT)  // 1 GHz
#define CM_PWMCTL_SRC_PLLD          (6<<CM_PWMCTL_SRC_SHIFT)  // 500 MHz
#define CM_PWMCTL_SRC_HDMI          (7<<CM_PWMCTL_SRC_SHIFT)  // not usable
#define CM_PWMCTL_ENAB              (1<<4)
#define CM_PWMCTL_KILL              (1<<5)
#define CM_PWMCTL_BUSY              (1<<7)
#define CM_PWMCTL_FLIP              (1<<8)
#define CM_PWMCTL_MASH_SHIFT        9
#define CM_PWMCTL_MASH_MASK         (0x3<<CM_PWMCTL_MASH_SHIFT)
#define CM_PWMCTL_MASH0             (0<<CM_PWMCTL_MASH_SHIFT)
#define CM_PWMCTL_MASH1             (1<<CM_PWMCTL_MASH_SHIFT)
#define CM_PWMCTL_MASH2             (2<<CM_PWMCTL_MASH_SHIFT)
#define CM_PWMCTL_MASH3             (3<<CM_PWMCTL_MASH_SHIFT)
#define CM_PWMCTL_PASSWD_SHIFT      24
#define CM_PWMCTL_PASSWD_MASK       (0xff<<CM_PWMCTL_PASSWD_SHIFT)
#define CM_PWMCTL_PASSWD            (0x5a<<24)

//
// DIVI values
//

//
// PLLC frequency: 1 GHz
//

#define CM_PWMCTL_DIVI_PLLC_1MHZ              (1000<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLC_10MHZ             (100<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLC_100MHZ            (10<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLC_25MHZ             (40<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLC_250MHZ            (4<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLC_500MHZ            (2<<CM_PWMDIV_DIVI_SHIFT)

//
// PLLD frequency: 500 MHz
//

#define CM_PWMCTL_DIVI_PLLD_1MHZ              (500<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLD_10MHZ             (50<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLD_25MHZ             (20<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLD_50MHZ             (10<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLD_100MHZ            (5<<CM_PWMDIV_DIVI_SHIFT)
#define CM_PWMCTL_DIVI_PLLD_250MHZ            (2<<CM_PWMDIV_DIVI_SHIFT)


//
// CM PCM Audio Clocks Control Registers
//

typedef struct _CM_PCM_REGS
{
    ULONG PCMCTL;
    ULONG PCMDIV;
} CM_PCM_REGS, *PCM_PCM_REGS;

//
// CM PWM Audio Clocks Control  Registers
//

typedef struct _CM_PWM_REGS
{
    ULONG PWMCTL;
    ULONG PWMDIV;
} CM_PWM_REGS, *PCM_PWM_REGS;




