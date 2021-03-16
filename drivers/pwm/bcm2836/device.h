/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains definitions for the device context.

--*/

#pragma once

#define NO_LAST_COMPLETED_PACKET    0xFFFFFFFF

typedef struct _DEVICE_CONTEXT
{
    //
    // Resource information.
    //

    PHYSICAL_ADDRESS            pwmRegsPa;
    PHYSICAL_ADDRESS            pwmRegsBusPa;
    PHYSICAL_ADDRESS            cmPwmRegsPa;
    PHYSICAL_ADDRESS            dmaChannelRegsPa;   
    PHYSICAL_ADDRESS            dmaBufferPa;
    PHYSICAL_ADDRESS            dmaCbPa;
    ULONG                       memUncachedOffset;
    PDMA_CHANNEL_REGS           dmaChannelRegs;
    PPWM_REGS                   pwmRegs;
    PCM_PWM_REGS                cmPwmRegs;
    PDMA_CB		                dmaCb;
    ULONG                       dmaControlDataSize;
    PUINT8		                dmaBuffer;
    LIST_ENTRY                  notificationList;
    WDFSPINLOCK                 notificationListLock;
    WDFINTERRUPT                interruptObj;
    WDFQUEUE                    queueObj;
    ULONG                       dmaChannel;
    ULONG                       dmaDreq;
    DMA_WIDTH                   dmaTransferWidth;

    //
    // DMA processing.
    //

    PBCM_PWM_PACKET_LINK_INFO   dmaPacketLinkInfo;
    ULONG                       dmaMaxPackets;
    ULONG                       dmaNumPackets;
    ULONG                       dmaPacketsInUse;
    ULONG                       dmaPacketsToPrime;
    ULONG                       dmaPacketsToPrimePreset;
    ULONG                       dmaPacketsProcessed;
    LARGE_INTEGER               dmaLastProcessedPacketTime;
    ULONG                       dmaLastKnownCompletedPacket;
    ULONG                       dmaAudioNotifcationCount;
    ULONG                       dmaDpcForIsrErrorCount;
    ULONG                       dmaUnderflowErrorCount;
    BOOLEAN                     dmaRestartRequired;

    //
    // PWM configuration.
    //

    BCM_PWM_CLOCK_CONFIG        pwmClockConfig;
    BCM_PWM_CHANNEL_CONFIG      pwmChannel1Config;
    BCM_PWM_CHANNEL_CONFIG      pwmChannel2Config;
    BCM_PWM_CLOCK_CONFIG        pwmSavedClockConfig;
    BCM_PWM_CHANNEL_CONFIG      pwmSavedChannel1Config;
    BCM_PWM_CHANNEL_CONFIG      pwmSavedChannel2Config;
    ULONG                       pwmDuty1;
    ULONG                       pwmDuty2;
    PWM_MODE                    pwmMode;

    // Protects PWM configuration.
    WDFSPINLOCK                 pwmLock;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

EVT_WDF_DRIVER_DEVICE_ADD           OnDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE     PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     ReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  OnIoDeviceControl;
EVT_WDF_DEVICE_CONTEXT_CLEANUP      OnDeviceContextCleanup;