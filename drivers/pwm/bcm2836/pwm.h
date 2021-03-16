/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains definitions for the BCM2836 PWM controller.

--*/

#pragma once

typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

//
// PWM Control (CTL)
//

#define PWM_CTL_PWEN1   (1<<0)
#define PWM_CTL_MODE1   (1<<1)
#define PWM_CTL_RPTL1   (1<<2)
#define PWM_CTL_SBIT1   (1<<3)
#define PWM_CTL_POLA1   (1<<4)
#define PWM_CTL_USEF1   (1<<5)
#define PWM_CTL_CLRF1   (1<<6)
#define PWM_CTL_MSEN1   (1<<7)
#define PWM_CTL_PWEN2   (1<<8)
#define PWM_CTL_MODE2   (1<<9)
#define PWM_CTL_RPTL2   (1<<10)
#define PWM_CTL_SBIT2   (1<<11)
#define PWM_CTL_POLA2   (1<<12)
#define PWM_CTL_USEF2   (1<<13)
#define PWM_CTL_MSEN2   (1<<15)

//
// PWM Status (STA)
//

#define PWM_STA_FULL1   (1<<0)
#define PWM_STA_EMPT1   (1<<1)
#define PWM_STA_WERR1   (1<<2)
#define PWM_STA_RERR1   (1<<3)
#define PWM_STA_GAPO1   (1<<4)
#define PWM_STA_GAPO2   (1<<5)
#define PWM_STA_GAPO3   (1<<6)
#define PWM_STA_GAPO4   (1<<7)
#define PWM_STA_BERR    (1<<8)
#define PWM_STA_STA1    (1<<9)
#define PWM_STA_STA2    (1<<10)
#define PWM_STA_STA3    (1<<11)
#define PWM_STA_STA4    (1<<12)

//
// PWM DMA Configuration (DMAC)
//

#define PWM_DMAC_DREQ_SHIFT     0
#define PWM_DMAC_DREQ_MASK      (0xFF<<PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_PANIC_SHIFT    8
#define PWM_DMAC_PANIC_MASK     (0xFF<<PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_ENAB           (1<<31)

#define PWM_DMAC_DREQ_0         (0 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_1         (1 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_2         (2 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_3         (3 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_4         (4 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_5         (5 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_6         (6 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_7         (7 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_8         (8 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_9         (9 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_10        (10 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_11        (11 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_12        (12 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_13        (13 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_14        (14 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_15        (15 << PWM_DMAC_DREQ_SHIFT)
#define PWM_DMAC_DREQ_16        (16 << PWM_DMAC_DREQ_SHIFT)

#define PWM_DMAC_PANIC_0         (0 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_1         (1 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_2         (2 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_3         (3 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_4         (4 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_5         (5 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_6         (6 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_7         (7 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_8         (8 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_9         (9 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_10        (10 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_11        (11 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_12        (12 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_13        (13 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_14        (14 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_15        (15 << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_PANIC_16        (16 << PWM_DMAC_PANIC_SHIFT)

//
// PWM duty register (DAT)
//

#define PWM_DUTY_REGISTER_DEFAULT   0

//
// PWM Control and Status Registers
//

typedef struct _PWM_REGS
{
    ULONG CTL;
    ULONG STA;
    ULONG DMAC;
    ULONG RSVD0;
    ULONG RNG1;
    ULONG DAT1;
    ULONG FIF1;
    ULONG RSVD1;
    ULONG RNG2;
    ULONG DAT2;
} PWM_REGS, *PPWM_REGS;

//
// PWM modes
//

typedef enum _PWM_MODE
{
    PWM_MODE_REGISTER,
    PWM_MODE_AUDIO
} PWM_MODE;

//
// Macros
//

#define PWM_CHANNEL1_IS_RUNNING(DeviceContext)  ((READ_REGISTER_ULONG(&DeviceContext->pwmRegs->STA) & PWM_STA_STA1) == PWM_STA_STA1)
#define PWM_CHANNEL2_IS_RUNNING(DeviceContext)  ((READ_REGISTER_ULONG(&DeviceContext->pwmRegs->STA) & PWM_STA_STA2) == PWM_STA_STA2)

#define IS_INVALID_CHANNEL(Channel)             (Channel != BCM_PWM_CHANNEL_CHANNEL1 && Channel != BCM_PWM_CHANNEL_CHANNEL2 && Channel != BCM_PWM_CHANNEL_ALLCHANNELS)
#define IS_CHANNEL_1(Channel)                   (Channel == BCM_PWM_CHANNEL_CHANNEL1)
#define IS_CHANNEL_2(Channel)                   (Channel == BCM_PWM_CHANNEL_CHANNEL2)
#define IS_CHANNEL_ALL(Channel)                 (Channel == BCM_PWM_CHANNEL_ALLCHANNELS)
#define IS_CHANNEL_1_OR_ALL(Channel)            (IS_CHANNEL_1(Channel) || IS_CHANNEL_ALL(Channel))
#define IS_CHANNEL_2_OR_ALL(Channel)            (IS_CHANNEL_2(Channel) || IS_CHANNEL_ALL(Channel))

//
// Exports
//

NTSTATUS
ValidateAndSetClockConfig(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
GetClockConfig(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
ValidateAndSetChannelConfig(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
GetChannelConfig(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
ValidateAndSetDutyRegister(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
GetDutyRegister(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
ValidateAndStartChannel(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
ValidateAndStopChannel(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

VOID
StartChannel(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BCM_PWM_CHANNEL Channel
);

VOID
StopChannel(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BCM_PWM_CHANNEL Channel
);

VOID
SetClockConfig(
    _In_ PDEVICE_CONTEXT DeviceContext
);

VOID
SetChannelConfig(
    _In_ PDEVICE_CONTEXT DeviceContext
);

NTSTATUS
AquireAudio(
    _In_ WDFDEVICE Device
);

NTSTATUS
ReleaseAudio(
    _In_ WDFDEVICE Device
);

