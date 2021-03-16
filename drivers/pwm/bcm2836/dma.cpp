/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains functions for DMA processing.

--*/

#include "driver.h"
#include "dma.tmh"

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
AllocateDmaBuffer(
    PDEVICE_CONTEXT DeviceContext
)
/*++

Routine Description:

    Allocate a contiguous buffer for DMA and a noncached buffer of the control blocks.

Arguments:

    DeviceContext - pointer to the device context

Return Value:

    Status

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    NT_ASSERT(DeviceContext->dmaBuffer == NULL);

    //
    // Allocate buffer for the data.
    //

    PHYSICAL_ADDRESS lowAddress = { 0, 0 };
    PHYSICAL_ADDRESS highAddress = { 0, 0 };
    PHYSICAL_ADDRESS boundaryAddress = { 0, 0 };
    highAddress.LowPart = 0xffffffff;
    if (NULL == (DeviceContext->dmaBuffer = (PUINT8)MmAllocateContiguousNodeMemory(DMA_BUFFER_SIZE, lowAddress, highAddress, boundaryAddress, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK)))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Could not allocate contiguous memory buffer for DMA");
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(status))
    {
        //
        // Allocate non cached non paged memory for the DMA Control Blocks and for link information provided to the audio driver.
        // For each audio packet we need 2 CBs. The second (smaller) one is used to generate audio packet notifications and is used to pause
        // audio in case of an underflow condition. 
        // We use a full page for CBs, which defines the maximal supported number of packets.
        //

        DeviceContext->dmaControlDataSize = PAGE_SIZE;
        DeviceContext->dmaMaxPackets = DeviceContext->dmaControlDataSize / (2 * sizeof(DMA_CB) + sizeof(BCM_PWM_PACKET_LINK_INFO));
        if (NULL == (DeviceContext->dmaCb = (PDMA_CB)MmAllocateContiguousNodeMemory(DeviceContext->dmaControlDataSize, lowAddress, highAddress, boundaryAddress, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK)))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not allocate %d bytes of non paged memory for control blocks.", DeviceContext->dmaControlDataSize);
            status = STATUS_INSUFFICIENT_RESOURCES;
            MmFreeContiguousMemory(DeviceContext->dmaBuffer);
            DeviceContext->dmaBuffer = NULL;
        }
        else
        {
            //
            // Update the address info in the context. The actual setup of the CBs is done in AllocateAudioBufferConfig, when the packet size
            // is known.
            //

            DeviceContext->dmaCbPa = MmGetPhysicalAddress(DeviceContext->dmaCb);
            DeviceContext->dmaBufferPa = MmGetPhysicalAddress(DeviceContext->dmaBuffer);
            DeviceContext->dmaPacketLinkInfo = (PBCM_PWM_PACKET_LINK_INFO)(DeviceContext->dmaCb + 2 * DeviceContext->dmaMaxPackets);
        }
    }
    return status;
}

#pragma code_seg()
VOID
StopDma(
    PDEVICE_CONTEXT DeviceContext
)
/*++

Routine Description:

    Function to stop the DMA.

Arguments:

    DeviceContext - Pointer to the device context.

Return Value:

    None

--*/
{
    //
    // Stop the DMA.
    //

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Stop DMA.");
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS, READ_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS) & ~DMA_CS_ACTIVE);
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CONBLK_AD, 0);
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS, READ_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS) | DMA_CS_RESET);
}

#pragma code_seg()
_Use_decl_annotations_
VOID
StartDma(
    PDEVICE_CONTEXT DeviceContext,
    PHYSICAL_ADDRESS ControlBlockPa
)
/*++

Routine Description:

    Function to start the DMA with the given control block.

Arguments:

    DeviceContext - Pointer to the device context.
    ControlBlockPa - Physical address of the control block to start the DMA with.

Return Value:

    None

--*/
{
    //
    // Start the DMA.
    //

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Start DMA with CB phys @ 0x%08x", ControlBlockPa.LowPart + DeviceContext->memUncachedOffset);
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CONBLK_AD, ControlBlockPa.LowPart + DeviceContext->memUncachedOffset);
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS,
        DMA_CS_ACTIVE | DMA_CS_END | DMA_CS_INT | DMA_CS_PRIORITY_8 | DMA_CS_PANIC_PRIORITY_F | DMA_CS_WAIT_FOR_OUTSTANDING_WRITES | DMA_CS_DISDEBUG
        );
}


#pragma code_seg()
_Use_decl_annotations_
VOID
PauseDma(
    PDEVICE_CONTEXT DeviceContext
)
/*++

Routine Description:

    Function to pause DMA.

Arguments:

    DeviceContext - Pointer to the device context.

Return Value:

    None

--*/
{
    //
    // Start the DMA.
    //

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Pause DMA.");
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS,
        DMA_CS_END | DMA_CS_INT | DMA_CS_PRIORITY_8 | DMA_CS_PANIC_PRIORITY_F | DMA_CS_WAIT_FOR_OUTSTANDING_WRITES | DMA_CS_DISDEBUG
        );
}


#pragma code_seg()
_Use_decl_annotations_
VOID
ResumeDma(
    PDEVICE_CONTEXT DeviceContext
)
/*++

Routine Description:

    Function to resume DMA.

Arguments:

    DeviceContext - Pointer to the device context.

Return Value:

    None

--*/
{
    //
    // Resume the DMA.
    //

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Resume DMA.");
    WRITE_REGISTER_ULONG(&DeviceContext->dmaChannelRegs->CS,
        DMA_CS_ACTIVE | DMA_CS_END | DMA_CS_INT | DMA_CS_PRIORITY_8 | DMA_CS_PANIC_PRIORITY_F | DMA_CS_WAIT_FOR_OUTSTANDING_WRITES | DMA_CS_DISDEBUG
        );
}


#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
InitializeAudio(
    WDFDEVICE Device,
    WDFREQUEST Request
)
/*++

Routine Description:

    PWM is initialized for audio playback. This includes setting up the PWM clock and channel configuration as well as
    initializing the DMA control blocks for the DMA operation.

Arguments:

    Device - Pointer to the WDF device object.
    Request - Pointer to the request object.

Return Value:

    Status

--*/
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;
    PDMA_CB currentCb;
    PHYSICAL_ADDRESS nextCbPa;
    ULONG packetIndex;
    PBCM_PWM_AUDIO_CONFIG bufferConfigIn;
    PBCM_PWM_AUDIO_CONFIG bufferConfigOut;

    //
    // Validate the request parameter.
    //

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(*bufferConfigIn),
        (PVOID *)&bufferConfigIn,
        NULL
        );

    if (NT_SUCCESS(status))
    {

        //
        // Validate requested buffer size.
        //

        if (bufferConfigIn->RequestedBufferSize > DMA_BUFFER_SIZE)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "Requested audio buffer size (%d) is too large. Maximum size allowed is: %d",
                bufferConfigIn->RequestedBufferSize, DMA_BUFFER_SIZE
                );
            return STATUS_UNSUCCESSFUL;
        }

        deviceContext = GetContext(Device);

        NT_ASSERT(bufferConfigIn->NotificationsPerBuffer > 0);
        ULONG packetSize = bufferConfigIn->RequestedBufferSize / bufferConfigIn->NotificationsPerBuffer;
        NT_ASSERT(packetSize > AUDIO_PACKET_LAST_CHUNK_SIZE);
        ULONG packetFirstChunkSize = packetSize - AUDIO_PACKET_LAST_CHUNK_SIZE;
        deviceContext->dmaNumPackets = DMA_BUFFER_SIZE / packetSize;
        if (deviceContext->dmaMaxPackets < deviceContext->dmaNumPackets)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "Too less memory for packet management allocated (%d byte, required %d byte). Increase memory for packet management or packet size.",
                PAGE_SIZE, deviceContext->dmaNumPackets * sizeof(DMA_CB) * 2
                );
            return STATUS_UNSUCCESSFUL;
        }

        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(*bufferConfigOut),
            (PVOID *)&bufferConfigOut,
            NULL
            );

        if (NT_SUCCESS(status))
        {
            WdfSpinLockAcquire(deviceContext->pwmLock);

            //
            // Only allow change if no PWM channel is running.
            //
            if (PWM_CHANNEL1_IS_RUNNING(deviceContext) || PWM_CHANNEL2_IS_RUNNING(deviceContext))
            {
                status = STATUS_OPERATION_IN_PROGRESS;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_IOCTL, "Device is running. Could not initialize PWM for audio.");
            }

            if (NT_SUCCESS(status))
            {
                //
                // Set audio clock configuration to use a 100 MHz PWM clock.
                //

                deviceContext->pwmClockConfig.ClockSource = BCM_PWM_CLOCKSOURCE_PLLC;
                deviceContext->pwmClockConfig.Divisor = 10;
                SetClockConfig(deviceContext);

                //
                // Set audio channel configuration. The audio generation is based on a PWM range
                // of 2268 to generate an 44,1kHz audio stream with 11 valid audio bits.
                //
                
                NT_ASSERT(bufferConfigIn->PwmRange == 2268);
                deviceContext->pwmChannel1Config.Range = bufferConfigIn->PwmRange;
                deviceContext->pwmChannel1Config.DutyMode = BCM_PWM_DUTYMODE_MARKSPACE;
                deviceContext->pwmChannel1Config.Mode = BCM_PWM_MODE_PWM;
                deviceContext->pwmChannel1Config.Polarity = BCM_PWM_POLARITY_NORMAL;
                deviceContext->pwmChannel1Config.Repeat = BCM_PWM_REPEATMODE_OFF;
                deviceContext->pwmChannel1Config.Silence = BCM_PWM_SILENCELEVEL_LOW;

                deviceContext->pwmChannel2Config.Range = bufferConfigIn->PwmRange;
                deviceContext->pwmChannel2Config.DutyMode = BCM_PWM_DUTYMODE_MARKSPACE;
                deviceContext->pwmChannel2Config.Mode = BCM_PWM_MODE_PWM;
                deviceContext->pwmChannel2Config.Polarity = BCM_PWM_POLARITY_NORMAL;
                deviceContext->pwmChannel2Config.Repeat = BCM_PWM_REPEATMODE_OFF;
                deviceContext->pwmChannel2Config.Silence = BCM_PWM_SILENCELEVEL_LOW;

                SetChannelConfig(deviceContext);
            }

            WdfSpinLockRelease(deviceContext->pwmLock);
            
            if (NT_SUCCESS(status))
            {
                //
                // PWM output rate does not match the audio sample rate precisely. To keep the playback
                // time correct we by skipping samples. 
                // With a range of 2268 at 100MHz clock rate, each sample takes 2.268 * 10E-5 seconds.
                // At 44.1 kZHz each sample should take 2.26757... * 10E-5 seconds.
                // The difference cummulates over 5320 samples to drift one sample, which means we should
                // skip each 5320 samples one sample. The buffer size and packet data we receive are
                // already taking into account that a PWM stereo sample is 8 byte in size.
                // Note: We do not calculate at runtime to prevent usage of floating point operations.
                //

                NT_ASSERT(deviceContext->pwmClockConfig.ClockSource == BCM_PWM_CLOCKSOURCE_PLLC);
                NT_ASSERT(deviceContext->pwmClockConfig.Divisor == 10);
                ULONG correctionDropSampleCount = 5320;
                ULONG bytesPerPwmSample = 8;
                ULONG samplesPerPacket = packetSize / bytesPerPwmSample;
                ULONG correctionDropPacketCount = correctionDropSampleCount / samplesPerPacket;

                //
                // If our DMA buffer is too small we overcorrect by dropping a sample in the last packet.
                //

                ULONG correctionDropPacketIndex = correctionDropPacketCount > deviceContext->dmaNumPackets ? deviceContext->dmaNumPackets - 1 : correctionDropPacketCount - 1;

                //
                // Create control blocks for DMA operation.
                //

                deviceContext->dmaPacketsToPrimePreset = deviceContext->dmaNumPackets / 4;
                deviceContext->dmaPacketsToPrime = deviceContext->dmaPacketsToPrimePreset;
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INIT, "Preset for packet prime: %d packets", deviceContext->dmaPacketsToPrimePreset);

                //
                // Create for each packet 2 CBs and link them. 
                //

                ULONG ti = DMA_TI_SRC_INC | DMA_TI_SRC_DREQ | (deviceContext->dmaDreq << DMA_TI_PERMAP_SHIFT) | DMA_TI_BURST_LENGTH_0;

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INIT, "Build CBs for %d audio packets. CBs @ 0x%p (phys: 0x%08x) Packet size: %d, First chunk size: %d",
                    deviceContext->dmaNumPackets, deviceContext->dmaCb, deviceContext->dmaCbPa.LowPart, packetSize, packetFirstChunkSize
                    );

                for (currentCb = deviceContext->dmaCb, packetIndex = 0; packetIndex < deviceContext->dmaNumPackets; packetIndex++, currentCb++)
                {
                    //
                    // The first CB of a buffer does not generate an interrupt.
                    //

                    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(currentCb);
                    currentCb->TI = ti;
                    currentCb->SOURCE_AD = deviceContext->dmaBufferPa.LowPart + packetIndex * packetSize + deviceContext->memUncachedOffset;
                    currentCb->DEST_AD = deviceContext->pwmRegsBusPa.LowPart + FIELD_OFFSET(PWM_REGS, FIF1);

                    // 
                    // Apply drift correction.
                    //

                    if (packetIndex == correctionDropPacketIndex)
                    {
                        currentCb->TXFR_LEN = packetFirstChunkSize - bytesPerPwmSample;
                    }
                    else
                    {
                        currentCb->TXFR_LEN = packetFirstChunkSize;
                    }
                    currentCb->STRIDE = 0;
                    nextCbPa = MmGetPhysicalAddress(currentCb + 1);
                    currentCb->NEXTCONBK = nextCbPa.LowPart + deviceContext->memUncachedOffset;
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INIT, "First CB packet %d @ 0x%p (phys: 0x%08x) - NEXTCONBK: 0x%08x, TI: 0x%08x, SOURCE_AD: 0x%08x, DEST_AD: 0x%08x, TXFR_LEN: 0x%08x (%d)",
                        packetIndex, currentCb, pa.LowPart, currentCb->NEXTCONBK, currentCb->TI, currentCb->SOURCE_AD, currentCb->DEST_AD, currentCb->TXFR_LEN, currentCb->TXFR_LEN
                        );
                    currentCb++;

                    //
                    // The last CB of a buffer generates an interrupt.
                    //

                    pa = MmGetPhysicalAddress(currentCb);
                    currentCb->TI = ti | DMA_TI_INTEN;
                    currentCb->SOURCE_AD = deviceContext->dmaBufferPa.LowPart + packetIndex * packetSize + packetFirstChunkSize + deviceContext->memUncachedOffset;
                    currentCb->DEST_AD = deviceContext->pwmRegsBusPa.LowPart + FIELD_OFFSET(PWM_REGS, FIF1);
                    currentCb->TXFR_LEN = AUDIO_PACKET_LAST_CHUNK_SIZE;
                    currentCb->STRIDE = 0;
                    currentCb->NEXTCONBK = 0;
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INIT, "Second CB packet %d @ 0x%p (phys: 0x%08x) - NEXTCONBK: 0x%08x, TI: 0x%08x, SOURCE_AD: 0x%08x, DEST_AD: 0x%08x, TXFR_LEN: 0x%08x (%d)",
                        packetIndex, currentCb, pa.LowPart, currentCb->NEXTCONBK, currentCb->TI, currentCb->SOURCE_AD, currentCb->DEST_AD, currentCb->TXFR_LEN, currentCb->TXFR_LEN
                        );

                    //
                    // Set up the link info for the packets. This information is use by the audio driver to link in 
                    // an audio packet when the data is ready for sending. The LinkValue is the physical address of the 
                    // first control block of the packet. The LinkPtr is the address of the NEXTCONBK value in the second
                    // control block of the preceeding packet. Packet 0 needs special handling to establish a cyclic list.
                    // Finally we set all NEXTCONBK values in the packet list to 0.
                    //

                    deviceContext->dmaPacketLinkInfo[packetIndex].LinkValue = (UINT_PTR)((PCHAR)(deviceContext->dmaCbPa.LowPart) + packetIndex * 2 * sizeof(DMA_CB));;
                    if (packetIndex == 0)
                    {
                        deviceContext->dmaPacketLinkInfo[packetIndex].LinkPtr = &(deviceContext->dmaCb[2 * (deviceContext->dmaNumPackets - 1) + 1].NEXTCONBK);;
                    }
                    else
                    {
                        deviceContext->dmaPacketLinkInfo[packetIndex].LinkPtr = &(deviceContext->dmaCb[2 * (packetIndex - 1) + 1].NEXTCONBK);
                    }
                }
                bufferConfigOut->DmaNumPackets = deviceContext->dmaNumPackets;
                bufferConfigOut->DmaPacketLinkInfo = deviceContext->dmaPacketLinkInfo;
                bufferConfigOut->DmaPacketsInUse = &deviceContext->dmaPacketsInUse;
                bufferConfigOut->DmaPacketsToPrime = &deviceContext->dmaPacketsToPrime;
                bufferConfigOut->DmaBuffer = deviceContext->dmaBuffer;
                bufferConfigOut->DmaRestartRequired = &deviceContext->dmaRestartRequired;
                bufferConfigOut->DmaPacketsProcessed = &deviceContext->dmaPacketsProcessed;
                bufferConfigOut->DmaLastProcessedPacketTime = &deviceContext->dmaLastProcessedPacketTime;

                WdfRequestSetInformation(Request, sizeof(BCM_PWM_AUDIO_CONFIG));
            }
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Error retrieving audio buffer configuration output buffer. (0x%08x)", status);
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Error retrieving audio buffer configuration input buffer. (0x%08x)", status);
    }

    return status;
}

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
RegisterAudioNotification(
    WDFDEVICE Device,
    WDFREQUEST Request
)
/*++

Routine Description:

    Function to register an audio notification.

Arguments:

    Device - a pointer to the WDFDEVICE object
    Request - a pointer to the WDFREQUEST object

Return Value:

    Status

--*/
{
    PKEVENT *notificationEvent;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(*notificationEvent),
        (PVOID*)&notificationEvent,
        0);

    if (NT_SUCCESS(status))
    {
        deviceContext = GetContext(Device);

        NOTIFICATION_LIST_ENTRY *notification = (PNOTIFICATION_LIST_ENTRY)ExAllocatePoolWithTag(
            NonPagedPoolNx,
            sizeof(NOTIFICATION_LIST_ENTRY),
            BCM_PWM_POOLTAG);
        if (NULL == notification)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        notification->NotificationEvent = *notificationEvent;

        WdfSpinLockAcquire(deviceContext->notificationListLock);

        if (!IsListEmpty(&deviceContext->notificationList))
        {
            PLIST_ENTRY currentNotificationListEntry = deviceContext->notificationList.Flink;
            while (currentNotificationListEntry != &deviceContext->notificationList)
            {
                PNOTIFICATION_LIST_ENTRY currentNotification = CONTAINING_RECORD(currentNotificationListEntry, NOTIFICATION_LIST_ENTRY, ListEntry);
                if (currentNotification->NotificationEvent == *notificationEvent)
                {
                    RemoveEntryList(currentNotificationListEntry);
                    ExFreePoolWithTag(notification, BCM_PWM_POOLTAG);
                    WdfSpinLockRelease(deviceContext->notificationListLock);
                    return STATUS_UNSUCCESSFUL;
                }
                currentNotificationListEntry = currentNotificationListEntry->Flink;
            }
        }
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Notification event registered: 0x%p, Current process: 0x%p", *notificationEvent, IoGetCurrentProcess());
        InsertTailList(&deviceContext->notificationList, &(notification->ListEntry));

        WdfSpinLockRelease(deviceContext->notificationListLock);

    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Error retrieving register notification event. (0x%08x)", status);
    }
    return status;
}

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
UnregisterAudioNotification(
    WDFDEVICE Device,
    WDFREQUEST Request
)
/*++

Routine Description:

    Function to unregister an audio notification.

Arguments:

    Device - a pointer to the WDFDEVICE object
    Request - a pointer to the WDFREQUEST object

Return Value:

Status

--*/
{
    PKEVENT *notificationEvent;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(*notificationEvent),
        (PVOID*)&notificationEvent,
        0);

    if (NT_SUCCESS(status))
    {
        deviceContext = GetContext(Device);

        WdfSpinLockAcquire(deviceContext->notificationListLock);

        if (!IsListEmpty(&deviceContext->notificationList))
        {
            PLIST_ENTRY currentNotificationListEntry = deviceContext->notificationList.Flink;
            PLIST_ENTRY nextNotificationListEntry = NULL;
            while (currentNotificationListEntry != &deviceContext->notificationList)
            {
                PNOTIFICATION_LIST_ENTRY currentNotfication = CONTAINING_RECORD(currentNotificationListEntry, NOTIFICATION_LIST_ENTRY, ListEntry);
                if (currentNotfication->NotificationEvent == *notificationEvent)
                {
                    nextNotificationListEntry = currentNotificationListEntry->Flink;
                    RemoveEntryList(currentNotificationListEntry);
                    ExFreePoolWithTag(currentNotfication, BCM_PWM_POOLTAG);
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Notification event unregistered: 0x%p", *notificationEvent);
                    currentNotificationListEntry = nextNotificationListEntry;
                }
                else
                {
                    currentNotificationListEntry = currentNotificationListEntry->Flink;
                }
            }
        }

        WdfSpinLockRelease(deviceContext->notificationListLock);

    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Error retrieving unregister notification event. (0x%08x)", status);
    }
    return status;
}


#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
StartAudio(
    WDFDEVICE Device
)
/*++

Routine Description:

    This function starts DMA for audio.

Arguments:

    Device - a pointer to the WDFDEVICE object

Return Value:

Status

--*/
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    deviceContext = GetContext(Device);

    //
    // If both channels are configured for register usage fail the call.
    //

    WdfSpinLockAcquire(deviceContext->pwmLock);

    if (deviceContext->pwmMode != PWM_MODE_AUDIO)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "PWM is not configured for audio.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(status))
    {

        //
        // Start all stream channels, if they do not run.
        //

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Start all channels.");

        deviceContext->dmaLastKnownCompletedPacket = NO_LAST_COMPLETED_PACKET;
        deviceContext->dmaRestartRequired = FALSE;
        deviceContext->dmaLastProcessedPacketTime.QuadPart = 0;
        deviceContext->dmaDpcForIsrErrorCount = 0;

        StartDma(deviceContext, deviceContext->dmaCbPa);
        StartChannel(deviceContext, BCM_PWM_CHANNEL_ALLCHANNELS);

    }

    WdfSpinLockRelease(deviceContext->pwmLock);

    return status;
}

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
StopAudio(
    WDFDEVICE Device
)
/*++

Routine Description:

    This function stops DMA for audio.

Arguments:

    Device - a pointer to the WDFDEVICE object

Return Value:

    Status

--*/
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    deviceContext = GetContext(Device);

    //
    // If PWM is not in audio mode,  no need to stop audio.
    //

    WdfSpinLockAcquire(deviceContext->pwmLock);

    if (deviceContext->pwmMode != PWM_MODE_AUDIO)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "PWM is not in audio mode.");
    }
    else
    {
        //
        // Stop all channels.
        //

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Stop all channels.");

        StopChannel(deviceContext, BCM_PWM_CHANNEL_ALLCHANNELS);
        StopDma(deviceContext);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "DMA notification count at stop: %d, packets processed: %d", deviceContext->dmaAudioNotifcationCount, deviceContext->dmaPacketsProcessed);

        deviceContext->dmaDpcForIsrErrorCount = 0;
        deviceContext->dmaUnderflowErrorCount = 0;
        deviceContext->dmaLastKnownCompletedPacket = NO_LAST_COMPLETED_PACKET;
        deviceContext->dmaPacketsInUse = 0;
        deviceContext->dmaPacketsToPrime = deviceContext->dmaPacketsToPrimePreset;
        deviceContext->dmaPacketsProcessed = 0;
        deviceContext->dmaRestartRequired = FALSE;
        deviceContext->dmaAudioNotifcationCount = 0;
        deviceContext->dmaLastProcessedPacketTime.QuadPart = 0;
    }

    WdfSpinLockRelease(deviceContext->pwmLock);

    return status;
}

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
PauseAudio(
    WDFDEVICE Device
)
/*++

Routine Description:

    This function pauses DMA.

Arguments:

    Device - a pointer to the WDFDEVICE object

Return Value:

    Status

--*/
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    deviceContext = GetContext(Device);

    WdfSpinLockAcquire(deviceContext->pwmLock);

    //
    // If PWM is not in audio mode, fail the call.
    //

    if (deviceContext->pwmMode != PWM_MODE_AUDIO)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "PWM is not in audio mode.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(status))
    {

        //
        // Pause DMA, if they run.
        //
        
        ULONG cs = READ_REGISTER_ULONG(&deviceContext->dmaChannelRegs->CS);
        if (cs & DMA_CS_ACTIVE)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Pause DMA.");
            PauseDma(deviceContext);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Pause requested, but DMA not running.");
        }
    }

    WdfSpinLockRelease(deviceContext->pwmLock);

    return status;
}

#pragma code_seg()
_Use_decl_annotations_
NTSTATUS
ResumeAudio(
WDFDEVICE Device
)
/*++

Routine Description:

    This function resumes DMA.

Arguments:

    Device - a pointer to the WDFDEVICE object

Return Value:

    Status

--*/
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    deviceContext = GetContext(Device);

    WdfSpinLockAcquire(deviceContext->pwmLock);

    //
    // If PWM is not in audio mode, fail the call.
    //

    if (deviceContext->pwmMode != PWM_MODE_AUDIO)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO, "PWM is not in audio mode.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(status))
    {

        //
        // Resume DMA, if they do not run.
        //

        ULONG cs = READ_REGISTER_ULONG(&deviceContext->dmaChannelRegs->CS);
        if (cs & DMA_CS_ACTIVE)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Resume requested, but DMA is already running.");
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "Resume DMA.");
            ResumeDma(deviceContext);
        }
    }

    WdfSpinLockRelease(deviceContext->pwmLock);

    return status;
}

