/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Abstract:

    This file contains functions for device logic of the driver.

--*/

#include "driver.h"
#include "device.tmh"


#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS
OnDeviceAdd(
    WDFDRIVER       Driver,
    PWDFDEVICE_INIT DeviceInit
)
/*++

Routine Description:

    OnDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry
    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure

Return Value:

    Status

--*/
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES   objectAttributes;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;
    DECLARE_UNICODE_STRING_SIZE(symbolicLinkName, 128);

    //
    // Set PnP callbacks.
    //

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    //
    // PWM only allows exclusive access.
    //

    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    //
    // Create device object.
    //

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&objectAttributes, DEVICE_CONTEXT);
    objectAttributes.EvtCleanupCallback = OnDeviceContextCleanup;
    status = WdfDeviceCreate(&DeviceInit, &objectAttributes, &device);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create device (0x%08x)", status);
        goto Exit;
    }

    deviceContext = GetContext(device);

    //
    // Prepare config spin lock.
    //

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    status = WdfSpinLockCreate(&objectAttributes, &deviceContext->pwmLock);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create config spin lock (0x%08x)", status);
        goto Exit;
    }

    //
    // Prepare notification list spin lock.
    //

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    status = WdfSpinLockCreate(&objectAttributes, &deviceContext->notificationListLock);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create notification list spin lock (0x%08x)", status);
        goto Exit;
    }

    //
    // Prepare interrupt object.
    //

    WDF_INTERRUPT_CONFIG interruptConfig;
    WDF_INTERRUPT_CONFIG_INIT(
        &interruptConfig,
        DmaIsr,
        DmaDpc
        );
    status = WdfInterruptCreate(
        device,
        &interruptConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &deviceContext->interruptObj
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create interrupt object (0x%08x)", status);
        goto Exit;
    }

    //
    // Create queues to handle IO.
    //

    WDF_IO_QUEUE_CONFIG queueConfig;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoDeviceControl = OnIoDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &deviceContext->queueObj
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create IO queue (0x%08x)", status);
        goto Exit;
    }

    //
    // Create a symbolic link.
    //

    status = RtlUnicodeStringInit(&symbolicLinkName, BCM_PWM_SYMBOLIC_NAME);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not process the symbolic name (0x%08x)", status);
        goto Exit;
    }

    status = WdfDeviceCreateSymbolicLink(
        device,
        &symbolicLinkName
        );

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Can not create a symbolic name (0x%08x)", status);
        goto Exit;
    }
    
Exit:
    return status;
}

#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS
PrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourceList,
    WDFCMRESLIST ResourceListTranslated
)
/*++

Routine Description:

    PrepareHardware parses the device resource description and assigns default values
    in device context.

Arguments:

    Device - Pointer to the device object
    ResourceList - Pointer to the devices resource list
    ResourceListTranslated - Ponter to the translated resource list of the device

Return Value:

    Status

--*/
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);

    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG resourceCount;
    ULONG memoryResources = 0;
    ULONG interruptResources = 0;
    ULONG dmaResources = 0;

    deviceContext = GetContext(Device);

    resourceCount = WdfCmResourceListGetCount(ResourceListTranslated);
    for (ULONG i = 0; i < resourceCount && NT_SUCCESS(status); ++i)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR res;

        res = WdfCmResourceListGetDescriptor(ResourceListTranslated, i);

        switch (res->Type)
        {

            case CmResourceTypeMemory:
                if (memoryResources == 0)
                {
                    //
                    // DMA channel registers
                    //

                    if (res->u.Memory.Length < sizeof(DMA_CHANNEL_REGS))
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "DMA channel memory region too small (start:0x%llX, length:0x%X)",
                            res->u.Memory.Start.QuadPart,
                            res->u.Memory.Length
                            );
                        status = STATUS_DEVICE_CONFIGURATION_ERROR;
                        break;
                    }

                    deviceContext->dmaChannelRegs = (PDMA_CHANNEL_REGS)MmMapIoSpaceEx(
                        res->u.Memory.Start,
                        res->u.Memory.Length,
                        PAGE_READWRITE | PAGE_NOCACHE
                        );

                    if (deviceContext->dmaChannelRegs == NULL)
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Unable to map DMA channel registers.");
                        status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }
                    deviceContext->dmaChannelRegsPa = res->u.Memory.Start;
                }
                else if (memoryResources == 1)
                {
                    //
                    // PWM registers
                    //

                    if (res->u.Memory.Length < sizeof(PWM_REGS))
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "PWM control register memory region too small (start:0x%llX, length:0x%X)",
                            res->u.Memory.Start.QuadPart,
                            res->u.Memory.Length
                            );
                        status = STATUS_DEVICE_CONFIGURATION_ERROR;
                        break;
                    }

                    deviceContext->pwmRegs = (PPWM_REGS)MmMapIoSpaceEx(
                        res->u.Memory.Start,
                        res->u.Memory.Length,
                        PAGE_READWRITE | PAGE_NOCACHE
                        );

                    if (deviceContext->pwmRegs == NULL)
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Unable to map PWM control registers.");
                        status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }
                    deviceContext->pwmRegsPa = res->u.Memory.Start;
                }
                else if (memoryResources == 2)
                {
                    //
                    // PWM control registers bus address
                    //

                    deviceContext->pwmRegsBusPa = res->u.Memory.Start;
                }
                else if (memoryResources == 3)
                {
                    //
                    // PWM control registers uncached address
                    //

                    PHYSICAL_ADDRESS pa = res->u.Memory.Start;
                    deviceContext->memUncachedOffset = pa.LowPart - deviceContext->pwmRegsPa.LowPart;
                }
                else if (memoryResources == 4)
                {
                    //
                    // PWM clock registers
                    //

                    if (res->u.Memory.Length < sizeof(CM_PWM_REGS))
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "PWM clock register memory region too small (start:0x%llX, length:0x%X)",
                            res->u.Memory.Start.QuadPart,
                            res->u.Memory.Length
                            );
                        status = STATUS_DEVICE_CONFIGURATION_ERROR;
                        break;
                    }

                    deviceContext->cmPwmRegs = (PCM_PWM_REGS)MmMapIoSpaceEx(
                        res->u.Memory.Start,
                        res->u.Memory.Length,
                        PAGE_READWRITE | PAGE_NOCACHE
                        );

                    if (deviceContext->cmPwmRegs == NULL)
                    {
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Unable to map PWM clock registers.");
                        status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }
                    deviceContext->cmPwmRegsPa = res->u.Memory.Start;
                }
                else
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Too many ACPI memory entries. Only 5 memory entries are allowed. Please verify ACPI configuration.");
                    status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    break;
                }
                memoryResources++;
                break;

            case CmResourceTypeInterrupt:

                //
                // Interrupt for used DMA channel. No setup required.
                //

                interruptResources++;
                break;

            case CmResourceTypeDma:

                //
                // Get the used DMA channel.
                //

                deviceContext->dmaChannel = res->u.DmaV3.Channel;
                deviceContext->dmaDreq = res->u.DmaV3.RequestLine;
                deviceContext->dmaTransferWidth = (DMA_WIDTH)res->u.DmaV3.TransferWidth;

                //
                // Sanity check DREQ, transfer width.
                //

                if (deviceContext->dmaDreq != DMA_DREQ_PWM)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "PWM DREQ configuration invalid (DREQ:%d)", res->u.DmaV3.RequestLine);
                    status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    break;
                }
                if (deviceContext->dmaTransferWidth != Width32Bits)
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "PWM DMA transfer width setting (width setting:%d)", (ULONG)res->u.DmaV3.TransferWidth);
                    status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    break;
                }

                //
                // Allocate and initialize contiguous DMA buffer logic.
                //

                status = AllocateDmaBuffer(deviceContext);
                if (NT_ERROR(status))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Error allocating DMA buffer (0x%08x)", status);
                }

                dmaResources++;
                break;

            default:
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Resource type not allowed for PWM. Please verify ACPI configuration.");
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
        }
    }
    
    //
    // Sanity check ACPI resources.
    //
    if (memoryResources != 5)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Too less ACPI memory entries. 5 memory entries are required. Please verify ACPI configuration.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (interruptResources != 1)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Exactly 1 interrupt entry is required and allowed. Please verify ACPI configuration.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (dmaResources != 1)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INIT, "Exactly 1 DMA entry is required and allowed. Please verify ACPI configuration.");
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(status))
    {
        //
        // Initialize device context and set default values.
        //

        deviceContext->pwmClockConfig.ClockSource = BCM_PWM_CLOCKSOURCE_PLLC;
        deviceContext->pwmClockConfig.Divisor = CM_PWMCTL_DIVI_PLLC_1MHZ;

        deviceContext->pwmChannel1Config.Channel = BCM_PWM_CHANNEL_CHANNEL1;
        deviceContext->pwmChannel1Config.Range = 0x20;
        deviceContext->pwmChannel1Config.DutyMode = BCM_PWM_DUTYMODE_PWM;
        deviceContext->pwmChannel1Config.Mode = BCM_PWM_MODE_PWM;
        deviceContext->pwmChannel1Config.Polarity = BCM_PWM_POLARITY_NORMAL;
        deviceContext->pwmChannel1Config.Repeat = BCM_PWM_REPEATMODE_OFF;
        deviceContext->pwmChannel1Config.Silence = BCM_PWM_SILENCELEVEL_LOW;
        deviceContext->pwmDuty1 = 0;

        deviceContext->pwmChannel2Config.Channel = BCM_PWM_CHANNEL_CHANNEL2;
        deviceContext->pwmChannel2Config.Range = 0x20;
        deviceContext->pwmChannel2Config.DutyMode = BCM_PWM_DUTYMODE_PWM;
        deviceContext->pwmChannel2Config.Mode = BCM_PWM_MODE_PWM;
        deviceContext->pwmChannel2Config.Polarity = BCM_PWM_POLARITY_NORMAL;
        deviceContext->pwmChannel2Config.Repeat = BCM_PWM_REPEATMODE_OFF;
        deviceContext->pwmChannel2Config.Silence = BCM_PWM_SILENCELEVEL_LOW;
        deviceContext->pwmDuty2 = 0;
        deviceContext->pwmMode = PWM_MODE_REGISTER;
        InitializeListHead(&deviceContext->notificationList);
        deviceContext->dmaDpcForIsrErrorCount = 0;
        deviceContext->dmaUnderflowErrorCount = 0;
        deviceContext->dmaLastKnownCompletedPacket = NO_LAST_COMPLETED_PACKET;
        deviceContext->dmaPacketsInUse = 0;
        deviceContext->dmaPacketsToPrime = 0;
        deviceContext->dmaPacketsToPrimePreset = 0;
        deviceContext->dmaPacketsProcessed = 0;
        deviceContext->dmaAudioNotifcationCount = 0;
        deviceContext->dmaRestartRequired = FALSE;
    }
    else
    {
        ReleaseHardware(Device, ResourceListTranslated);
    }

    return status;
}


#pragma code_seg("PAGE")
_Use_decl_annotations_
NTSTATUS
ReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
/*++

Routine Description:

    ReleaseHardware releases resources allocated by PrepareHardware.

Arguments:

    Device - Pointer to the device object
    ResourceListTranslated - Ponter to the translated resource list of the device

Return Value:

    NTSTATUS

--*/
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PDEVICE_CONTEXT deviceContext;

    deviceContext = GetContext(Device);

    if (deviceContext->dmaChannelRegs)
    {
        MmUnmapIoSpace(deviceContext->dmaChannelRegs, sizeof(DMA_CHANNEL_REGS));
    }
    if (deviceContext->pwmRegs)
    {
        MmUnmapIoSpace(deviceContext->pwmRegs, sizeof(PWM_REGS));
    }
    if (deviceContext->cmPwmRegs)
    {
        MmUnmapIoSpace(deviceContext->cmPwmRegs, sizeof(CM_PWM_REGS));
    }
    return STATUS_SUCCESS;
}


#pragma code_seg()
_Use_decl_annotations_
VOID
OnIoDeviceControl(
    WDFQUEUE    Queue,
    WDFREQUEST  Request,
    size_t      OutputBufferLength,
    size_t      InputBufferLength,
    ULONG       IoControlCode
)
/*++

Routine Description:

    This event handled device control calls.

Arguments:

    Queue - Handle of the queue object associated with the request
    Request - Handle of the request object
    OutputBufferLength - length of the request's output buffer
    InputBufferLength - length of the request's input buffer
    IoControlCode - the driver-defined or system-defined I/O control code

Return Value:

    None

--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    device = WdfIoQueueGetDevice(Queue);
    deviceContext = GetContext(device);

    //
    // Validate the IO code and exexute on it.
    //

    switch (IoControlCode)
    {
    case IOCTL_BCM_PWM_SET_CLOCKCONFIG:
        status = ValidateAndSetClockConfig(device, Request);
        break;

    case IOCTL_BCM_PWM_GET_CLOCKCONFIG:
        status = GetClockConfig(device, Request);
        break;

    case IOCTL_BCM_PWM_SET_CHANNELCONFIG:
        status = ValidateAndSetChannelConfig(device, Request);
        break;

    case IOCTL_BCM_PWM_GET_CHANNELCONFIG:
        status = GetChannelConfig(device, Request);
        break;

    case IOCTL_BCM_PWM_SET_DUTY_REGISTER:
        status = ValidateAndSetDutyRegister(device, Request);
        break;

    case IOCTL_BCM_PWM_GET_DUTY_REGISTER:
        status = GetDutyRegister(device, Request);
        break;

    case IOCTL_BCM_PWM_START:
        status = ValidateAndStartChannel(device, Request);
        break;

    case IOCTL_BCM_PWM_STOP:
        status = ValidateAndStopChannel(device, Request);
        break;

    case IOCTL_BCM_PWM_AQUIRE_AUDIO:
        status = AquireAudio(device);
        break;

    case IOCTL_BCM_PWM_RELEASE_AUDIO:
        status = ReleaseAudio(device);
        break;

    case IOCTL_BCM_PWM_INITIALIZE_AUDIO:
        status = InitializeAudio(device, Request);
        break;

    case IOCTL_BCM_PWM_REGISTER_AUDIO_NOTIFICATION:
        status = RegisterAudioNotification(device, Request);
        break;

    case IOCTL_BCM_PWM_UNREGISTER_AUDIO_NOTIFICATION:
        status = UnregisterAudioNotification(device, Request);
        break;

    case IOCTL_BCM_PWM_START_AUDIO:
        status = StartAudio(device);
        break;

    case IOCTL_BCM_PWM_PAUSE_AUDIO:
        status = PauseAudio(device);
        break;

    case IOCTL_BCM_PWM_RESUME_AUDIO:
        status = ResumeAudio(device);
        break;

    case IOCTL_BCM_PWM_STOP_AUDIO:
        status = StopAudio(device);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IOCTL, "Unexpected IO code in request. Request: 0x%p, Code: 0x%08x", Request, IoControlCode);
        break;
    }

    WdfRequestComplete(Request, status);
}

#pragma code_seg()
_Use_decl_annotations_
VOID
OnDeviceContextCleanup(
    WDFOBJECT Object
)
/*++

Routine Description:

    This event handles cleanup of the device object.

Arguments:

    Object - Pointer to the object to cleanup

Return Value:

    None

--*/
{
    PDEVICE_CONTEXT deviceContext;

    deviceContext = GetContext(Object);


    //
    // release all resources from the device object (there is only one)
    //

    if (deviceContext->dmaBuffer)
    {
        MmFreeContiguousMemorySpecifyCache(deviceContext->dmaBuffer, DMA_BUFFER_SIZE, MmNonCached);
    }
    if (deviceContext->dmaCb)
    {
        // For cleanup functions of WDFDEVICE objects the IRQL is IRQL_PASSIVE. So we are save to call.
        #pragma warning(push)
        #pragma warning(disable:28118)
        MmFreeContiguousMemorySpecifyCache(deviceContext->dmaCb, deviceContext->dmaControlDataSize, MmNonCached);
        #pragma warning(pop)
    }
}