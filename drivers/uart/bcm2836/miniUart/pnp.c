// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
// pnp.c
//
// Abstract:
//
//     This module contains the code that handles the plug and play
//    IRPs for the serial driver.


#include "precomp.h"
#include <stdlib.h>
#include "pnp.tmh"

static const PHYSICAL_ADDRESS SerialPhysicalZero = {0};
static const SUPPORTED_BAUD_RATES SupportedBaudRates[] = {
        {1200, SERIAL_BAUD_1200},
        {1800, SERIAL_BAUD_1800},
        {2400, SERIAL_BAUD_2400},
        {4800, SERIAL_BAUD_4800},
        {7200, SERIAL_BAUD_7200},
        {9600, SERIAL_BAUD_9600},
        {14400, SERIAL_BAUD_14400},
        {19200, SERIAL_BAUD_19200},
        {38400, SERIAL_BAUD_38400},
        {56000, SERIAL_BAUD_56K},
        {57600, SERIAL_BAUD_57600},
        {115200, SERIAL_BAUD_115200},
        {230400, SERIAL_BAUD_230400},
        {460800, SERIAL_BAUD_460800},
        {921600, SERIAL_BAUD_921600},
        {SERIAL_BAUD_INVALID, SERIAL_BAUD_USER}
    };


#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGESRP0, SerialEvtDeviceAdd)
#pragma alloc_text(PAGESRP0, SerialEvtPrepareHardware)
#pragma alloc_text(PAGESRP0, SerialEvtReleaseHardware)
#pragma alloc_text(PAGESRP0, SerialEvtDeviceD0ExitPreInterruptsDisabled)
#pragma alloc_text(PAGESRP0, SerialMapHWResources)
#pragma alloc_text(PAGESRP0, SerialUnmapHWResources)
#pragma alloc_text(PAGESRP0, SerialEvtDeviceContextCleanup)
#pragma alloc_text(PAGESRP0, SerialDoExternalNaming)
#pragma alloc_text(PAGESRP0, SerialReportMaxBaudRate)
#pragma alloc_text(PAGESRP0, SerialUndoExternalNaming)
#pragma alloc_text(PAGESRP0, SerialInitController)
#pragma alloc_text(PAGESRP0, SerialGetMappedAddress)
#pragma alloc_text(PAGESRP0, SerialSetPowerPolicy)
#pragma alloc_text(PAGESRP0, SerialReadSymName)
#pragma alloc_text(PAGESRP0, SerialSetPortNameDevInterfProp)
#endif

_Use_decl_annotations_
PVOID LocalMmMapIoSpace(
    PHYSICAL_ADDRESS PhysicalAddress,
    SIZE_T NumberOfBytes
    )
{
    typedef
    PVOID
    (*PFN_MM_MAP_IO_SPACE_EX) (
        _In_ PHYSICAL_ADDRESS PhysicalAddress,
        _In_ SIZE_T NumberOfBytes,
        _In_ ULONG Protect
        );

    UNICODE_STRING name;
    PFN_MM_MAP_IO_SPACE_EX pMmMapIoSpaceEx;
    PVOID pvReturn=NULL;

    // MmMapIoSpaceEx is Windows 10 specific API 
    // check if it is available

    RtlInitUnicodeString(&name, L"MmMapIoSpaceEx");
    pMmMapIoSpaceEx = (PFN_MM_MAP_IO_SPACE_EX) (ULONG_PTR)MmGetSystemRoutineAddress(&name);

    if(pMmMapIoSpaceEx != NULL)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                    "LocalMmMapIoSpace() - using Win10 API\r\n");

        pvReturn=pMmMapIoSpaceEx(PhysicalAddress,
                               NumberOfBytes,
                               PAGE_READWRITE | PAGE_NOCACHE); 
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                    "LocalMmMapIoSpace() - using std API\r\n");

        pvReturn=MmMapIoSpace(PhysicalAddress, NumberOfBytes, MmNonCached); 
    };
    return pvReturn;
}

/*++

Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager.


Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SerialEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    PSERIAL_DEVICE_EXTENSION pDevExt;
    static ULONG currentInstance = 0;
    WDF_FILEOBJECT_CONFIG fileobjectConfig;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE defaultqueue;
    PULONG countSoFar;
    WDF_INTERRUPT_CONFIG interruptConfig;
    PSERIAL_INTERRUPT_CONTEXT interruptContext;
    ULONG relinquishPowerPolicy;
    WDF_DEVICE_PNP_CAPABILITIES pnpCapab;

    DECLARE_UNICODE_STRING_SIZE(deviceName, DEVICE_OBJECT_NAME_LENGTH);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "++SerialEvtDeviceAdd()\r\n");

    status = RtlUnicodeStringPrintf(&deviceName,
                                    L"%ws%u",
                                    L"\\Device\\Serial",
                                    currentInstance++);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceInitAssignName(DeviceInit,& deviceName);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_SERIAL_PORT);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REQUEST_CONTEXT);

    WdfDeviceInitSetRequestAttributes(DeviceInit, &attributes);

    // Zero out the PnpPowerCallbacks structure.

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    // Set Callbacks for any of the functions we are interested in.
    // If no callback is set, Framework will take the default action
    // by itself.  These next two callbacks set up and tear down hardware state,
    // specifically that which only has to be done once.

    pnpPowerCallbacks.EvtDevicePrepareHardware = SerialEvtPrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = SerialEvtReleaseHardware;

    // These two callbacks set up and tear down hardware state that must be
    // done every time the device moves in and out of the D0-working state.

    pnpPowerCallbacks.EvtDeviceD0Entry = SerialEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = SerialEvtDeviceD0Exit;

    // Specify the callback for monitoring when the device's interrupt are
    // enabled or about to be disabled.

    pnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = 
            SerialEvtDeviceD0EntryPostInterruptsEnabled;

    pnpPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled  = 
            SerialEvtDeviceD0ExitPreInterruptsDisabled;

    // Register the PnP and power callbacks.

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    if ( !NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "WdfDeviceInitSetPnpPowerEventCallbacks failed %Xh\r\n",
                    status);
        return status;
    }

    // Find out if we own power policy

    SerialGetFdoRegistryKeyValue( DeviceInit,
                                  L"SerialRelinquishPowerPolicy",
                                  &relinquishPowerPolicy );

    if(relinquishPowerPolicy) {

        // FDO's are assumed to be power policy owner by default. So tell
        // the framework explicitly to relinquish the power policy ownership.

        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                    "RelinquishPowerPolicy due to registry settings\r\n");

        WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);
    }

    // For Windows XP and below, we will register for the WDM Preprocess callback
    // for IRP_MJ_CREATE. This is done because, the Serenum filter doesn't handle
    // creates that are marked pending. Since framework always marks the IRP pending,
    // we are registering this WDM preprocess handler so that we can bypass the
    // framework and handle the create and close ourself. This workaround is need
    // only if you intend to install the Serenum as an upper filter.

    if (RtlIsNtDdiVersionAvailable(NTDDI_VISTA) == FALSE) {

        status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                            SerialWdmDeviceFileCreate,
                                                            IRP_MJ_CREATE,
                                                            NULL,
                                                            0);

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                        "WdfDeviceInitAssignWdmIrpPreprocessCallback failed %Xh\r\n",
                        status);
            return status;
        }

        status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                            SerialWdmFileClose,
                                                            IRP_MJ_CLOSE,
                                                            NULL,
                                                            0);

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                        "WdfDeviceInitAssignWdmIrpPreprocessCallback failed %Xh\r\n",
                        status);
            return status;
        }

    } else {

        // FileEvents can opt for Device level synchronization only if the ExecutionLevel
        // of the Device is passive. Since we can't choose passive execution-level for
        // device because we have chose to synchronize timers & dpcs with the device,
        // we will opt out of synchonization with the device for fileobjects.
        // Note: If the driver has to synchronize Create with the other I/O events,
        // it can create a queue and configure-dispatch create requests to the queue.

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.SynchronizationScope = WdfSynchronizationScopeNone;

        // Set Entry points for Create and Close..

        WDF_FILEOBJECT_CONFIG_INIT(&fileobjectConfig,
                                    SerialEvtDeviceFileCreate,
                                    SerialEvtFileClose,
                                    WDF_NO_EVENT_CALLBACK);

        WdfDeviceInitSetFileObjectConfig(DeviceInit,
                                        &fileobjectConfig,
                                        &attributes);
    }

    // Since framework queues doesn't handle IRP_MJ_FLUSH_BUFFERS,
    // IRP_MJ_QUERY_INFORMATION and IRP_MJ_SET_INFORMATION requests,
    // we will register a preprocess callback to handle them.

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                        SerialFlush,
                                                        IRP_MJ_FLUSH_BUFFERS,
                                                        NULL,
                                                        0);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "WdfDeviceInitAssignWdmIrpPreprocessCallback failed %Xh\r\n",
                    status);
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                        SerialQueryInformationFile,
                                                        IRP_MJ_QUERY_INFORMATION,
                                                        NULL,
                                                        0);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "WdfDeviceInitAssignWdmIrpPreprocessCallback failed %Xh\r\n",
                    status);
        return status;
    }
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit,
                                                        SerialSetInformationFile,
                                                        IRP_MJ_SET_INFORMATION,
                                                        NULL,
                                                        0);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "WdfDeviceInitAssignWdmIrpPreprocessCallback failed %Xh\r\n",
                    status);
        return status;
    }

    // Create mini Uart device

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&attributes,
                                            SERIAL_DEVICE_EXTENSION);

    // Provide a callback to cleanup the context. This will be called
    // when the device is removed.

    attributes.EvtCleanupCallback = SerialEvtDeviceContextCleanup;

    // By opting for SynchronizationScopeDevice, we tell the framework to
    // synchronize callbacks events of all the objects directly associated
    // with the device. In this driver, we will associate queues, dpcs,
    // and timers. By doing that we don't have to worrry about synchronizing
    // access to device-context by Io Events, cancel-routine, timer and dpc
    // callbacks.

    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialAddDevice - WdfDeviceCreate failed %Xh\r\n",
                    status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                     "Created device (%p) %wZ\r\n", device, &deviceName);

    pDevExt = SerialGetDeviceExtension (device);

    pDevExt->DriverObject = WdfDriverWdmGetDriverObject(Driver);

    // on IoT platform serial port created by mini Uart driver must be marked
    // as Removable to avoid being placed into system container

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCapab);
    pnpCapab.Removable=TRUE;
    WdfDeviceSetPnpCapabilities(device,&pnpCapab);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "Marked device (%p) %wZ as Removable in PnP capabilities\r\n", 
                device,
                &deviceName);

    // Set up mini Uart device extension.

    pDevExt = SerialGetDeviceExtension (device);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "AddDevice PDO(0x%p) FDO(0x%p), Lower(0x%p) DevExt (0x%p)\r\n",
                WdfDeviceWdmGetPhysicalDevice (device),
                WdfDeviceWdmGetDeviceObject (device),
                WdfDeviceWdmGetAttachedDevice(device),
                pDevExt);

    pDevExt->DeviceIsOpened = FALSE;
    pDevExt->DeviceObject = WdfDeviceWdmGetDeviceObject(device);
    pDevExt->WdfDevice = device;

    pDevExt->TxFifoAmount = DriverDefaults.TxFIFODefault;
    pDevExt->UartRemovalDetect = DriverDefaults.UartRemovalDetect;
    pDevExt->CreatedSymbolicLink = FALSE;
    pDevExt->IsDeviceInterfaceEnabled = FALSE;
    pDevExt->OwnsPowerPolicy = relinquishPowerPolicy ? FALSE : TRUE;

    status = SerialSetPowerPolicy(pDevExt);
    if(!NT_SUCCESS(status)){
        return status;
    }

    // We create four(4) manual queues below.
    // Since requests jump from queue to queue, we cannot configure the queues to receive a
    // particular type of request. For example, some of the IOCTLs end up
    // in read and write queue.

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             WdfIoQueueDispatchManual);

    queueConfig.EvtIoStop = SerialEvtIoStop;
    queueConfig.EvtIoResume = SerialEvtIoResume;
    queueConfig.EvtIoCanceledOnQueue = SerialEvtCanceledOnQueue;

    status = WdfIoQueueCreate (device,
                               &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &pDevExt->ReadQueue);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    " WdfIoQueueCreate for Read failed %Xh\r\n",
                    status);
        return status;
    }

    // Write Queue.

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             WdfIoQueueDispatchManual);

    queueConfig.EvtIoStop = SerialEvtIoStop;
    queueConfig.EvtIoResume = SerialEvtIoResume;
    queueConfig.EvtIoCanceledOnQueue = SerialEvtCanceledOnQueue;

    status = WdfIoQueueCreate (device,
                               &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &pDevExt->WriteQueue);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    " WdfIoQueueCreate for Write failed %Xh\r\n",
                    status);

        return status;
    }

    // Mask Queue...

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             WdfIoQueueDispatchManual);

    queueConfig.EvtIoCanceledOnQueue = SerialEvtCanceledOnQueue;

    queueConfig.EvtIoStop = SerialEvtIoStop;
    queueConfig.EvtIoResume = SerialEvtIoResume;

    status = WdfIoQueueCreate (device,
                               &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &pDevExt->MaskQueue);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    " WdfIoQueueCreate for Mask failed %Xh\r\n",
                    status);

        return status;
    }

    // Purge Queue..

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             WdfIoQueueDispatchManual);

    queueConfig.EvtIoCanceledOnQueue = SerialEvtCanceledOnQueue;

    queueConfig.EvtIoStop = SerialEvtIoStop;
    queueConfig.EvtIoResume = SerialEvtIoResume;

    status = WdfIoQueueCreate (device,
                               &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES,
                               &pDevExt->PurgeQueue);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    " WdfIoQueueCreate for Purge failed %Xh\r\n",
                    status);

        return status;
    }

    // All the incoming I/O requests are routed to the default queue and dispatch to the
    // appropriate callback events. These callback event will check to see if another
    // request is currently active. If so then it will forward it to other manual queues.
    // All the queues are auto managed by the framework in response to the PNP
    // and Power events.

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);

    queueConfig.EvtIoRead   = SerialEvtIoRead;
    queueConfig.EvtIoWrite  = SerialEvtIoWrite;
    queueConfig.EvtIoDeviceControl = SerialEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = SerialEvtIoInternalDeviceControl;
    queueConfig.EvtIoCanceledOnQueue = SerialEvtCanceledOnQueue;

    queueConfig.EvtIoStop = SerialEvtIoStop;
    queueConfig.EvtIoResume = SerialEvtIoResume;

    status = WdfIoQueueCreate(device,
                            &queueConfig,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &defaultqueue);

    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "WdfIoQueueCreate failed %Xh\r\n",
                    status);

        return status;
    }

    // Create WDFINTERRUPT object. Let us leave the ShareVector to  default value and
    // let the framework decide whether to share the interrupt or not based on the
    // ShareDisposition provided by the bus driver in the resource descriptor.

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
                              SerialISR,
                              NULL);

    interruptConfig.EvtInterruptDisable = SerialEvtInterruptDisable;
    interruptConfig.EvtInterruptEnable = SerialEvtInterruptEnable;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SERIAL_INTERRUPT_CONTEXT);

    status = WdfInterruptCreate(device,
                                &interruptConfig,
                                &attributes,
                                &pDevExt->WdfInterrupt);

    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "Couldn't create interrupt for %wZ\r\n",
                    &pDevExt->DeviceName);

        return status;
    }

    // Interrupt state wait lock.

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = pDevExt->WdfInterrupt;

    interruptContext = SerialGetInterruptContext(pDevExt->WdfInterrupt);

    status = WdfWaitLockCreate(&attributes,
                               &interruptContext->InterruptStateLock);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    " WdfWaitLockCreate for InterruptStateLock failed %Xh\r\n",
                    status);

        return status;
    }

    // Set interrupt policy

    SerialSetInterruptPolicy(pDevExt->WdfInterrupt);

    // Timers and DPCs...

    status = SerialCreateTimersAndDpcs(pDevExt);
    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialCreateTimersAndDpcs failed %Xh\r\n",
                    status);

        return status;
    }

    // Register with WMI.

    status = SerialWmiRegistration(device);
    if(!NT_SUCCESS (status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialWmiRegistration failed %Xh\r\n",
                    status);

        return status;

    }

    // Up to this point, if we fail, we don't have to worry about freeing any resource because
    // framework will free all the objects.

    // Finally increment the global system configuration that keeps track of number of serial ports.

    countSoFar = &IoGetConfigurationInformation()->SerialCount;
    (*countSoFar)++;
    pDevExt->IsSystemConfigInfoUpdated = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "--SerialEvtDeviceAdd()=%Xh\r\n",
                status);

    return status;
}

/*++

Routine Description:

    SerialSetPortNameDevInterfProp is called to set serial port device interface property.


Arguments:

    Device - device object created in DriverEntry

    SerPortName - Unicode string containing property name.

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS SerialSetPortNameDevInterfProp(WDFDEVICE Device, PCWSTR SerPortName)
{
    NTSTATUS status=STATUS_SUCCESS;

    UNICODE_STRING symlinkName;
    WDFSTRING strSymlinkNameWdfString=NULL;
    DEVPROP_BOOLEAN isRestricted = DEVPROP_FALSE;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "++SerialSetPortNameDevInterfProp()\r\n");

    status = WdfStringCreate(NULL,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &strSymlinkNameWdfString);

    if(!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialSetPortNameDevInterfProp(ERR) %Xh from WdfStringCreate\r\n",
                    status);
        goto DevPropEnd2;
    }

    status = WdfDeviceRetrieveDeviceInterfaceString(Device,
                                                    (LPGUID) &GUID_DEVINTERFACE_COMPORT,
                                                    NULL,
                                                    strSymlinkNameWdfString);

    if(!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialSetPortNameDevInterfProp(ERR) %Xh from WdfDeviceRetrieveDeviceInterfaceString\r\n",
                    status);

        goto DevPropEnd;
    }

    WdfStringGetUnicodeString(strSymlinkNameWdfString, &symlinkName);

    // set mini Uart device interface property to allow UWP application to access mini Uart device
    // Note: this is in addition to allowing user mode application to access mini Uart device, 
    // which is done via SDDL reg key in .inf file

    status = IoSetDeviceInterfacePropertyData(&symlinkName,
                                            &DEVPKEY_DeviceInterface_Serial_PortName,
                                            LOCALE_NEUTRAL,
                                            0,
                                            DEVPROP_TYPE_STRING,
                                            (ULONG)(sizeof(wchar_t)*(wcslen(SerPortName)+1)),
                                            (PVOID)SerPortName);

    if(!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialSetPortNameDevInterfProp(ERR) %Xh from IoSetDevicePropertyData1\r\n",
                    status);
    }

    // need to explicitly set property [IsRestricted]=false to allow UWP application 
    // gain access to mini Uart driver,
    // since it is considered internal device and therefore is placed in system container

    status = IoSetDeviceInterfacePropertyData(&symlinkName,
                                            &DEVPKEY_DeviceInterface_Restricted,
                                            LOCALE_NEUTRAL,
                                            0,
                                            DEVPROP_TYPE_BOOLEAN,
                                            (ULONG)sizeof(isRestricted),
                                            &isRestricted);

    if(!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "SerialSetPortNameDevInterfProp(ERR) %Xh from IoSetDevicePropertyData2\r\n",
                    status);
    }

DevPropEnd:
    WdfObjectDelete(strSymlinkNameWdfString);

DevPropEnd2:
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "--SerialSetPortNameDevInterfProp()=%Xh\r\n",
                status);

    return status;
}

/*++

Routine Description:

   EvtDeviceContextCleanup event callback cleans up anything done in
   EvtDeviceAdd, except those things that are automatically cleaned
   up by the Framework.

   In a driver derived from this sample, it's quite likely that this function could
   be deleted.

Arguments:

    Device - Handle to a framework device object.

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID
SerialEvtDeviceContextCleanup (
    WDFOBJECT Device
    )
{
    PSERIAL_DEVICE_EXTENSION deviceExtension;
    PULONG countSoFar;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "++SerialDeviceContextCleanup\r\n");

    PAGED_CODE();

    deviceExtension = SerialGetDeviceExtension (Device);

    if (deviceExtension->InterruptReadBuffer != NULL) {
       ExFreePool(deviceExtension->InterruptReadBuffer);
       deviceExtension->InterruptReadBuffer = NULL;
    }
    
    // Update the global configuration count for serial device.

    if(deviceExtension->IsSystemConfigInfoUpdated) {
        countSoFar = &IoGetConfigurationInformation()->SerialCount;
        (*countSoFar)--;
    }

    SerialUndoExternalNaming(deviceExtension);
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--SerialDeviceContextCleanup\r\n");
    return;
}

/*++

Routine Description:

    SerialEvtPrepareHardware event callback performs operations that are necessary
    to make the device operational. The framework calls the driver's
    SerialEvtPrepareHardware callback when the PnP manager sends an IRP_MN_START_DEVICE
    request to the driver stack.

Arguments:

    Device - Handle to a framework device object.

    Resources - Handle to a collection of framework resource objects.
                This collection identifies the raw (bus-relative) hardware
                resources that have been assigned to the device.

    ResourcesTranslated - Handle to a collection of framework resource objects.
                This collection identifies the translated (system-physical)
                hardware resources that have been assigned to the device.
                The resources appear from the CPU's point of view.
                Use this list of resources to map I/O space and
                device-accessible memory into virtual address space

Return Value:

    WDF status code

--*/
_Use_decl_annotations_
NTSTATUS
SerialEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST Resources,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PSERIAL_DEVICE_EXTENSION pDevExt;
    NTSTATUS status;
    CONFIG_DATA config;
    PCONFIG_DATA pConfig = &config;
    ULONG defaultClockRate = 250000000;

    PAGED_CODE();

    TraceEvents (TRACE_LEVEL_INFORMATION, DBG_PNP, 
                 "++SerialEvtPrepareHardware\r\n");

    pDevExt = SerialGetDeviceExtension (Device);

    RtlZeroMemory(pConfig, sizeof(CONFIG_DATA));

    // Initialize a config data structure with default values for those that
    // may not already be initialized.

    pConfig->LogFifo = DriverDefaults.LogFifoDefault;

    // Get the hw resources for the device.

    status = SerialMapHWResources(Device, Resources, ResourcesTranslated, pConfig);

    if (!NT_SUCCESS(status)) {
        goto End;
    }

    // If we have a conflict with the debugger, and SerialMapHWResources was 
    // successful, it means we also have an GPIO function configuration that was 
    // successfully committed to prevent other application/driver from muxing-out
    // the debugger.
    // In this case, we avoid accessing the hardware, and we do not expose
    // mini Uart device.

    if (pDevExt->DebugPortInUse) {
        goto End;
    }

    // Open the "Device Parameters" section of registry for this device and get parameters.

    if(!SerialGetRegistryKeyValue (Device,
                                  L"DisablePort",
                                  &pConfig->DisablePort)) {
        pConfig->DisablePort = 0;
    }

// - check - TODO - on RPi mini Uart has non-configurable fifo

    if(!SerialGetRegistryKeyValue (Device,
                                   L"ForceFifoEnable",
                                   &pConfig->ForceFifoEnable)) {
        pConfig->ForceFifoEnable = DriverDefaults.ForceFifoEnableDefault;
    }

// end check

    if(!SerialGetRegistryKeyValue (Device,
                                   L"RxFIFO",
                                   &pConfig->RxFIFO)) {
        pConfig->RxFIFO = DriverDefaults.RxFIFODefault;
    }

    if(!SerialGetRegistryKeyValue (Device,
                                   L"TxFIFO",
                                   &pConfig->TxFIFO)) {
        pConfig->TxFIFO = DriverDefaults.TxFIFODefault;
    }

// todo - check - on RPi - mini Uart interrupt is always shared

    if(!SerialGetRegistryKeyValue (Device,
                                   L"Share System Interrupt",
                                   &pConfig->PermitShare)) {
        pConfig->PermitShare = DriverDefaults.PermitShareDefault;
    }

    // Try getting the core clock rate from registry. 
    // If it's not set there, query from mailbox instead.

    if(!SerialGetRegistryKeyValue (Device,
                                   L"ClockRate",
                                   &pConfig->ClockRate)) {
        status = QuerySystemClockFrequency(&pConfig->ClockRate);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                DBG_PNP,
                "Failed to query system clock frequency from RPIQ - falling back to default. (status = %!STATUS!, defaultClockRate = %d)",
                status,
                defaultClockRate);

            pConfig->ClockRate = defaultClockRate;
        }        
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, 
                "Com Port ClockRate: %lu Hz (%Xh)\r\n",
                pConfig->ClockRate, pConfig->ClockRate);

    status = SerialInitController(pDevExt, pConfig);

    if(!NT_SUCCESS(status)) {
       TraceEvents (TRACE_LEVEL_ERROR, DBG_PNP, 
                    "SerialInitController Failed! Err=%Xh", 
                    status);
       goto End;
    } else {


        // print miniuart registers

#if DBG
        PrintMiniUartregs(pDevExt);
#endif
    };

    // If device interface has already been enabled,
    // nothing more to do here...

    if (!pDevExt->IsDeviceInterfaceEnabled) {

        // Make the device visible after we verified we are not
        // conflicting with the debugger.

        status = SerialDoExternalNaming(pDevExt);

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                "SerialDoExternalNaming Failed - Status %Xh\r\n",
                status);
            goto End;
        }

        // Modify device properties to allow UWP application to access 
        // miniUart device.
        // Use UART0 name like RhProxy uses until DDA property in UEFI 
        // becomes available.

        status = SerialSetPortNameDevInterfProp(Device, L"UART0");

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                "SerialSetPortNameDevInterfProp Failed -Status %Xh\r\n",
                status);

            goto End;
        }

        pDevExt->IsDeviceInterfaceEnabled = TRUE;
    }

    status = STATUS_SUCCESS;

End:

   TraceEvents (TRACE_LEVEL_INFORMATION, DBG_PNP, 
                "--SerialEvtPrepareHardware %Xh\r\n", 
                status);

   return status;
}

/*++

Routine Description:

    EvtDeviceReleaseHardware is called by the framework whenever the PnP manager
    is revoking ownership of our resources.  This may be in response to either
    IRP_MN_STOP_DEVICE or IRP_MN_REMOVE_DEVICE.  The callback is made before
    passing down the IRP to the lower driver.

    In this callback, do anything necessary to free those resources.
    In this driver, we will not receive this callback when there is open handle to
    the device. We explicitly tell the framework (WdfDeviceSetStaticStopRemove) to
    fail stop and query-remove when handle is open.

Arguments:

    Device - Handle to a framework device object.

    ResourcesTranslated - Handle to a collection of framework resource objects.
                This collection identifies the translated (system-physical)
                hardware resources that have been assigned to the device.
                The resources appear from the CPU's point of view.
                Use this list of resources to map I/O space and
                device-accessible memory into virtual address space

Return Value:

    NTSTATUS - Failures will be logged, but not acted on.

--*/
_Use_decl_annotations_
NTSTATUS
SerialEvtReleaseHardware(
     WDFDEVICE Device,
     WDFCMRESLIST ResourcesTranslated
    )
{
    PSERIAL_DEVICE_EXTENSION pDevExt;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "++SerialEvtReleaseHardware\r\n");

    pDevExt = SerialGetDeviceExtension (Device);

    if (pDevExt->DebugPortInUse) {
        goto End;
    }

    // Reset and put the device into a known initial state before releasing the hw resources.
    // In this driver we can recieve this callback only when there is no handle open because
    // we tell the framework to disable stop by calling WdfDeviceSetStaticStopRemove.
    // Since we have already reset the device in our close handler, we don't have to
    // do anything other than unmapping the I/O resources.

    // Unmap any Memory-Mapped registers. Disconnecting from the interrupt will
    // be done automatically by the framework.

    SerialUnmapHWResources(pDevExt);

End:

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                 "--SerialEvtReleaseHardware\r\n");

    return STATUS_SUCCESS;
}

/*++

Routine Description:

    EvtDeviceD0EntryPostInterruptsEnabled is called by the framework after the
    driver has enabled the device's hardware interrupts.

    This function is not marked pageable because this function is in the
    device power up path. When a function is marked pagable and the code
    section is paged out, it will generate a page fault which could impact
    the fast resume behavior because the client driver will have to wait
    until the system drivers can service this page fault.

Arguments:

    Device - Handle to a framework device object.

    PreviousState - A WDF_POWER_DEVICE_STATE-typed enumerator that identifies
                    the previous device power state.

Return Value:

    NTSTATUS - Failures will be logged, but not acted on.

--*/
_Use_decl_annotations_
NTSTATUS
SerialEvtDeviceD0EntryPostInterruptsEnabled(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PSERIAL_DEVICE_EXTENSION extension = SerialGetDeviceExtension(Device);
    PSERIAL_INTERRUPT_CONTEXT interruptContext;
    WDF_INTERRUPT_INFO info;

    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "++SerialEvtDeviceD0EntryPostInterruptsEnabled\r\n");

    if (extension->DebugPortInUse) {
        goto End;
    }

    interruptContext = SerialGetInterruptContext(extension->WdfInterrupt);

    // The following lines of code show how to call WdfInterruptGetInfo.

    WDF_INTERRUPT_INFO_INIT(&info);
    WdfInterruptGetInfo(extension->WdfInterrupt, &info);

    WdfWaitLockAcquire(interruptContext->InterruptStateLock, NULL);
    interruptContext->IsInterruptConnected = TRUE;
    WdfWaitLockRelease(interruptContext->InterruptStateLock);

End:

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "--SerialEvtDeviceD0EntryPostInterruptsEnabled\r\n");

    return STATUS_SUCCESS;
}


/*++

Routine Description:

    EvtDeviceD0ExitPreInterruptsDisabled is called by the framework before the
    driver disables the device's hardware interrupts.

Arguments:

    Device - Handle to a framework device object.

    TargetState - A WDF_POWER_DEVICE_STATE-typed enumerator that identifies the
                  device power state that the device is about to enter.

Return Value:

    NTSTATUS - Failures will be logged, but not acted on.

--*/
_Use_decl_annotations_
NTSTATUS
SerialEvtDeviceD0ExitPreInterruptsDisabled(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    PSERIAL_DEVICE_EXTENSION extension = SerialGetDeviceExtension(Device);
    PSERIAL_INTERRUPT_CONTEXT interruptContext;

    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "++SerialEvtDeviceD0ExitPreInterruptsDisabled\r\n");

    if (extension->DebugPortInUse) {
        goto End;
    }

    interruptContext = SerialGetInterruptContext(extension->WdfInterrupt);

    WdfWaitLockAcquire(interruptContext->InterruptStateLock, NULL);
    interruptContext->IsInterruptConnected = FALSE;
    WdfWaitLockRelease(interruptContext->InterruptStateLock);

End:

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "--SerialEvtDeviceD0ExitPreInterruptsDisabled\r\n");
    return STATUS_SUCCESS;
}


/*++

Routine Description:

    SerialSetPowerPolicy is called by the framework.

Arguments:

    DeviceExtension - pointer to device extension


Return Value:

    NTSTATUS - Failures will be logged, but not acted on.

--*/
_Use_decl_annotations_
NTSTATUS
SerialSetPowerPolicy(
    PSERIAL_DEVICE_EXTENSION DeviceExtension
    )
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
    NTSTATUS    status = STATUS_SUCCESS;
    WDFDEVICE hDevice = DeviceExtension->WdfDevice;
    ULONG powerOnClose;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER, "++SerialSetPowerPolicy\r\n");

    PAGED_CODE();

    // Find out whether we want to power down the device when there no handles open.

    SerialGetRegistryKeyValue(hDevice, L"EnablePowerManagement",  &powerOnClose);
    DeviceExtension->RetainPowerOnClose = powerOnClose ? TRUE : FALSE;

    // In some drivers, the device must be specifically programmed to enable
    // wake signals. On RPi platform mini Uart doesn't register wake arm/disarm
    // callbacks. 

    // Init the idle policy structure. By setting IdleCannotWakeFromS0 we tell the framework
    // to power down the device without arming for wake. The only way the device can come
    // back to D0 is when we call WdfDeviceStopIdle in SerialEvtDeviceFileCreate.
    // We can't choose IdleCanWakeFromS0 by default is because onboard serial ports typically
    // don't have wake capability. If the driver is used for plugin boards that does support
    // wait-wake, you can update the settings to match that. If MS provided modem driver
    // is used on ports that does support wake on ring, then it will update the settings
    // by sending an internal ioctl to us.

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
    if(DeviceExtension->OwnsPowerPolicy && !DeviceExtension->RetainPowerOnClose) {

        // for RPi mini Uart we disable idle settings as default, but allow to change it

        idleSettings.Enabled=WdfFalse;
        idleSettings.UserControlOfIdleSettings = IdleAllowUserControl;

        status = WdfDeviceAssignS0IdleSettings(hDevice, &idleSettings);
        if ( !NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_POWER,
                        "WdfDeviceSetPowerPolicyS0IdlePolicy failed %Xh\r\n",
                        status);

            return status;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER, "--SerialSetPowerPolicy()=%Xh\r\n",status);

    return status;
}

/*++

Routine Description:

    This routine returns the max baud rate given a selection of rates

Arguments:

   Bauds  -  Bit-encoded list of supported bauds


  Return Value:

   The max baud rate listed in Bauds

--*/
_Use_decl_annotations_
UINT32
SerialReportMaxBaudRate(ULONG Bauds)
{
    int i;
    UINT32 uRetVal=0;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER,
                "++SerialReportMaxBaudRate(bauds=%Xh)\r\n",
                Bauds);

    for(i=0; SupportedBaudRates[i].BaudRate != SERIAL_BAUD_INVALID; i++) {

        if(Bauds & SupportedBaudRates[i].Mask) {

            uRetVal=SupportedBaudRates[i].BaudRate;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_POWER,
                "--SerialReportMaxBaudRate()=%lu\r\n",
                uRetVal);

    return uRetVal;
}

/*++

Routine Description:

    Really too many things to mention here.  In general initializes
    kernel synchronization structures, allocates the typeahead buffer,
    sets up defaults, etc.

Arguments:

    PDevObj       - Device object for the device to be started

    PConfigData   - Pointer to a record for a single port.

Return Value:

    STATUS_SUCCCESS if everything went ok.  A !NT_SUCCESS status
    otherwise.

--*/
_Use_decl_annotations_
NTSTATUS
SerialInitController(
    PSERIAL_DEVICE_EXTENSION pDevExt,
    PCONFIG_DATA PConfigData
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    SHORT sTemp;
    int i;
    const int ciMaxIter=999;
    LARGE_INTEGER liTimeout={ 0 };

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "++SerialInitController for %wZ\r\n",
                &pDevExt->DeviceName);

    // Save the value of clock input to the part.  We use this to calculate
    // the divisor latch value.  The value is in Hertz.

    pDevExt->ClockRate = PConfigData->ClockRate;

    // Map the memory for the control registers for the serial device
    // into virtual memory.

    pDevExt->Controller =
      SerialGetMappedAddress(PConfigData->TrController,
                             PConfigData->SpanOfController,
                             (BOOLEAN)PConfigData->AddressSpace,
                             &pDevExt->UnMapRegisters);

    if (!pDevExt->Controller) {

      TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                    "Could not map memory for device registers for %wZ\r\n",
                    &pDevExt->DeviceName);

      pDevExt->UnMapRegisters = FALSE;
      status = STATUS_NONE_MAPPED;
      goto ExtensionCleanup;

    }

    pDevExt->AddressSpace = PConfigData->AddressSpace;
    pDevExt->SpanOfController = PConfigData->SpanOfController;

    // Save off the interface type and the bus number.

    pDevExt->Vector = PConfigData->TrVector;
    pDevExt->Irql = (UCHAR)PConfigData->TrIrql;
    pDevExt->InterruptMode = PConfigData->InterruptMode;
    pDevExt->Affinity = PConfigData->Affinity;

    // If the user said to permit sharing within the device, propagate this
    // through.

    pDevExt->PermitShare = PConfigData->PermitShare;

    // Save the GPIO function configuration connection ID, if any, so
    // we can claim (force the required function) during device open,
    // and release during device close.

    pDevExt->FunctionConfigConnectionId = PConfigData->FunctionConfigConnectionId;

    // Before we test whether the port exists (which will enable the FIFO)
    // convert the rx trigger value to what should be used in the register.
    //
    // If a bogus value was given - crank them down to 1.
    //
    // If this is a "souped up" UART with like a 64 byte FIFO, they
    // should use the appropriate "spoofing" value to get the desired
    // results.  I.e., if on their chip 0xC0 in the FCR is for 64 bytes,
    // they should specify 14 in the registry.

    switch (PConfigData->RxFIFO) 
    {

    case 1:

      pDevExt->RxFifoTrigger = SERIAL_1_BYTE_HIGH_WATER;
      break;

    case 4:

      pDevExt->RxFifoTrigger = SERIAL_4_BYTE_HIGH_WATER;
      break;

    default:

      pDevExt->RxFifoTrigger = SERIAL_1_BYTE_HIGH_WATER;
      break;

    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "RxFifoTrigger=%u\r\n",
                pDevExt->RxFifoTrigger);

    if(PConfigData->TxFIFO < 1) 
    {

      pDevExt->TxFifoAmount = 1;

    } else {

      pDevExt->TxFifoAmount = PConfigData->TxFIFO;

    };

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                "TxFifoAmount=%u\r\n",
                pDevExt->TxFifoAmount);

    // Enabling the Mini UART Interface is cruical, otherwise we won't be able
    // to access any Mini UART registers

    UCHAR auxEnableReg = READ_MINIUART_ENABLE(pDevExt, pDevExt->Controller);

    if(auxEnableReg & MINIUART_ENABLE_MASK)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Mini UART interface is enabled\r\n");
    }
    else
    {
        auxEnableReg |= MINIUART_ENABLE_MASK;
        WRITE_MINIUART_ENABLE(pDevExt, pDevExt->Controller, auxEnableReg);

        // allow pi firmware some time to perform enabling of mini Uart hardware

        liTimeout.QuadPart = (LONGLONG)10*IDLETIMEµs * (LONGLONG)(-10);

        for(i=0;i<ciMaxIter;i++)
        {
            KeDelayExecutionThread(KernelMode, FALSE, &liTimeout);

            if(READ_MINIUART_ENABLE(pDevExt,pDevExt->Controller) & MINIUART_ENABLE_MASK) {

                TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                            "Mini UART interface was disabled, now enabled succesfully\r\n");
                break;
            }
        }


        if(i>=ciMaxIter) {

            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "Time out Eabling the Mini UART interface\r\n");
            status = STATUS_NO_SUCH_DEVICE;
            goto ExtensionCleanup;
        }
    }

    // next we enable both receiver and transmitter parts of mini Uart

    WRITE_MINIUARTRXTX_ENABLE(pDevExt, pDevExt->Controller, 0x3);    
    if((READ_MINIUARTRXTX_ENABLE(pDevExt, pDevExt->Controller) & 0x3) == 0x0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "Mini UART receive and transmit parts NOT enabled!\r\n");

        status = STATUS_NO_SUCH_DEVICE;
        goto ExtensionCleanup;
    }

    if(FALSE==SerialDoesPortExist(pDevExt,
                                PConfigData->ForceFifoEnable)) {

      // We couldn't verify that there was actually a
      // port. No need to log an error as the port exist
      // code will log exactly why.

      TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                    "DoesPortExist() DLAB presence test failed for %wZ\r\n",
                    &pDevExt->DeviceName);

      status = STATUS_NO_SUCH_DEVICE;
      goto ExtensionCleanup;

    }

    // If the user requested that we disable the port, then
    // do it now.  Log the fact that the port has been disabled.

    if (PConfigData->DisablePort) {

      TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                    "disabled port %wZ as requested in configuration\r\n",
                    &pDevExt->DeviceName);

      status = STATUS_NO_SUCH_DEVICE;

      goto ExtensionCleanup;

    }

    // Set up the default device control fields.
    // Note that if the values are changed after
    // the file is open, they do NOT revert back
    // to the old value at file close.

    pDevExt->SpecialChars.XonChar      = SERIAL_DEF_XON;
    pDevExt->SpecialChars.XoffChar     = SERIAL_DEF_XOFF;
    pDevExt->HandFlow.ControlHandShake = 0;
    pDevExt->HandFlow.FlowReplace      = 0;

    // Default Line control protocol.
    //
    // Eight data bits, no parity, 1 Stop bit.

    pDevExt->LineControl = SERIAL_8_DATA |
                           SERIAL_1_STOP |
                           SERIAL_NONE_PARITY;

    pDevExt->ValidDataMask = 0x7f;
    pDevExt->CurrentBaud   = 9600;

    // We set up the default xon/xoff limits.
    //
    // This may be a bogus value.  It looks like the BufferSize
    // is not set up until the device is actually opened.

    pDevExt->HandFlow.XoffLimit    = pDevExt->BufferSize >> 3;
    pDevExt->HandFlow.XonLimit     = pDevExt->BufferSize >> 1;

    pDevExt->BufferSizePt8 = ((3*(pDevExt->BufferSize>>2))+
                                  (pDevExt->BufferSize>>4));

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                " The default interrupt read buffer size is:"
                " %d\r\n------  The XoffLimit is                         : %d\r\n"
                "------  The XonLimit is                          : %d\r\n"
                "------  The pt 8 size is                         : %d\r\n",
                pDevExt->BufferSize,
                pDevExt->HandFlow.XoffLimit,
                pDevExt->HandFlow.XonLimit,
                pDevExt->BufferSizePt8);

    // Go through all the "named" baud rates to find out which ones
    // can be supported with this port.

    pDevExt->SupportedBauds = SERIAL_BAUD_USER;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, " determing supported baud rates...\r\n");

    for(i=0; SupportedBaudRates[i].BaudRate != SERIAL_BAUD_INVALID; i++)
    {

        status=SerialGetDivisorFromBaud(pDevExt->ClockRate,
                                        (LONG)SupportedBaudRates[i].BaudRate,
                                        &sTemp);

        if(!NT_ERROR(status)) {

            pDevExt->SupportedBauds |= SupportedBaudRates[i].Mask;
            TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                        " %lu Ok. ",
                        SupportedBaudRates[i].BaudRate);
        } else {

            TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                        " %lu - no. ", 
                        SupportedBaudRates[i].BaudRate);
        }
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT," mask=%Xh\r\n",pDevExt->SupportedBauds);
    }

    // Mark this device as not being opened by anyone.  We keep a
    // variable around so that spurious interrupts are easily
    // dismissed by the ISR.

    SetDeviceIsOpened(pDevExt, FALSE, FALSE);

    // Store values into the extension for interval timing.

    // If the interval timer is less than a second then come
    // in with a short "polling" loop.
    //
    // For large (> then 2 seconds) use a 1 second poller.

    pDevExt->ShortIntervalAmount.QuadPart  = -1;
    pDevExt->LongIntervalAmount.QuadPart   = -10000000;
    pDevExt->CutOverAmount.QuadPart        = 200000000;

    DISABLE_ALL_INTERRUPTS (pDevExt, pDevExt->Controller);

    WRITE_MODEM_CONTROL(pDevExt, pDevExt->Controller, (UCHAR)0);

    // make sure there is no escape character currently set

    pDevExt->EscapeChar = 0;

    // This should set up everything as it should be when
    // a device is to be opened.  We do need to lower the
    // modem lines, and disable the recalcitrant fifo
    // so that it will show up if the user boots to dos.
    //

    // __WARNING_IRQ_SET_TOO_HIGH:  we are calling interrupt synchronize routine directly. 
    // Suppress it because interrupt is not connected yet.
    // __WARNING_INVALID_PARAM_VALUE_1: Interrupt is UNREFERENCED_PARAMETER, so it can be NULL

#pragma warning(suppress: __WARNING_IRQ_SET_TOO_HIGH; suppress: __WARNING_INVALID_PARAM_VALUE_1) 
    SerialReset(NULL, pDevExt);

#pragma warning(suppress: __WARNING_IRQ_SET_TOO_HIGH; suppress: __WARNING_INVALID_PARAM_VALUE_1) 
    SerialMarkClose(NULL, pDevExt);

#pragma warning(suppress: __WARNING_IRQ_SET_TOO_HIGH; suppress: __WARNING_INVALID_PARAM_VALUE_1) 
    SerialClrRTS(NULL, pDevExt);

#pragma warning(suppress: __WARNING_IRQ_SET_TOO_HIGH; suppress: __WARNING_INVALID_PARAM_VALUE_1) 
    SerialClrDTR(NULL, pDevExt);

    // Fill in WMI hardware data

    pDevExt->WmiHwData.IrqNumber = pDevExt->Irql;
    pDevExt->WmiHwData.IrqLevel = pDevExt->Irql;
    pDevExt->WmiHwData.IrqVector = pDevExt->Vector;
    pDevExt->WmiHwData.IrqAffinityMask = pDevExt->Affinity;
    pDevExt->WmiHwData.InterruptType = pDevExt->InterruptMode == Latched
       ? SERIAL_WMI_INTTYPE_LATCHED : SERIAL_WMI_INTTYPE_LEVEL;
    pDevExt->WmiHwData.BaseIOAddress = (ULONG_PTR)pDevExt->Controller;

    // Fill in WMI device state data (as defaults)

    pDevExt->WmiCommData.BaudRate = pDevExt->CurrentBaud;
    pDevExt->WmiCommData.BitsPerByte = (pDevExt->LineControl & 0x03) + 5;
    pDevExt->WmiCommData.ParityCheckEnable = (pDevExt->LineControl & 0x08)
       ? TRUE : FALSE;

    switch (pDevExt->LineControl & SERIAL_PARITY_MASK) {
    case SERIAL_NONE_PARITY:
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_NONE;
       break;

    case SERIAL_ODD_PARITY:
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_ODD;
       break;

    case SERIAL_EVEN_PARITY:
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_EVEN;
       break;

    case SERIAL_MARK_PARITY:
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_MARK;
       break;

    case SERIAL_SPACE_PARITY:
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_SPACE;
       break;

    default:
       ASSERTMSG(0, "Illegal Parity setting for WMI");
       pDevExt->WmiCommData.Parity = SERIAL_WMI_PARITY_NONE;
       break;
    }

    // set miniUart WMI for its parameters

    pDevExt->WmiCommData.StopBits = pDevExt->LineControl & SERIAL_STOP_MASK
       ? (pDevExt->WmiCommData.BitsPerByte == 5 ? SERIAL_WMI_STOP_1_5
          : SERIAL_WMI_STOP_2) : SERIAL_WMI_STOP_1;
    pDevExt->WmiCommData.XoffCharacter = pDevExt->SpecialChars.XoffChar;
    pDevExt->WmiCommData.XoffXmitThreshold = pDevExt->HandFlow.XoffLimit;
    pDevExt->WmiCommData.XonCharacter = pDevExt->SpecialChars.XonChar;
    pDevExt->WmiCommData.XonXmitThreshold = pDevExt->HandFlow.XonLimit;
    pDevExt->WmiCommData.MaximumBaudRate
       = SerialReportMaxBaudRate(pDevExt->SupportedBauds);
    pDevExt->WmiCommData.MaximumOutputBufferSize = (UINT32)((ULONG)-1);
    pDevExt->WmiCommData.MaximumInputBufferSize = (UINT32)((ULONG)-1);
    pDevExt->WmiCommData.Support16BitMode = FALSE;
    pDevExt->WmiCommData.SupportDTRDSR = FALSE;
    pDevExt->WmiCommData.SupportIntervalTimeouts = TRUE;
    pDevExt->WmiCommData.SupportParityCheck = TRUE;

    // miniUart has these lines in hardware, bit not wired outside, thus not usable with WMI

    pDevExt->WmiCommData.SupportRTSCTS = FALSE;
     
    pDevExt->WmiCommData.SupportXonXoff = TRUE;
    pDevExt->WmiCommData.SettableBaudRate = TRUE;
    pDevExt->WmiCommData.SettableDataBits = TRUE;

    // No miniUart flow control pins are wired outside,  not usable with WMI

    pDevExt->WmiCommData.SettableFlowControl = FALSE; 
    pDevExt->WmiCommData.SettableParity = TRUE;
    pDevExt->WmiCommData.SettableParityCheck = TRUE;
    pDevExt->WmiCommData.SettableStopBits = TRUE;
    pDevExt->WmiCommData.IsBusy = FALSE;

    // Common error path cleanup.  If the status is
    // bad, get rid of the device extension, device object
    // and any memory associated with it.

ExtensionCleanup:

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--SerialInitController %Xh\r\n", status);

    return status;
}

/*++

Routine Description:

    This routine will get the configuration information and put
    it and the translated values into CONFIG_DATA structures.

Arguments:

    Device - Handle to a framework device object.

    Resources - Handle to a collection of framework resource objects.
                This collection identifies the raw (bus-relative) hardware
                resources that have been assigned to the device.

    ResourcesTranslated - Handle to a collection of framework resource objects.
                This collection identifies the translated (system-physical)
                hardware resources that have been assigned to the device.
                The resources appear from the CPU's point of view.
                Use this list of resources to map I/O space and
                device-accessible memory into virtual address space

Return Value:

    STATUS_SUCCESS if consistant configuration was found - otherwise.
    returns STATUS_SERIAL_NO_DEVICE_INITED.

--*/
_Use_decl_annotations_
NTSTATUS
SerialMapHWResources(
                    WDFDEVICE Device,
                    WDFCMRESLIST PResList,
                    WDFCMRESLIST PTrResList,
                    PCONFIG_DATA PConfig
                    )
{
    PSERIAL_DEVICE_EXTENSION pDevExt;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pPartialTrResourceDesc, pPartialRawResourceDesc;
    ULONG gotInt = 0;
    ULONG gotIO = 0;
    ULONG gotMem = 0;
    ULONG gotConnectionId = 0;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "++SerialMapHWResources\r\n");

    pDevExt = SerialGetDeviceExtension (Device);

    if ((PResList == NULL) || (PTrResList == NULL)) {
        ASSERT(PResList != NULL);
        ASSERT(PTrResList != NULL);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    for (i = 0; i < WdfCmResourceListGetCount(PTrResList); i++) {

        pPartialTrResourceDesc = WdfCmResourceListGetDescriptor(PTrResList, i);
        pPartialRawResourceDesc = WdfCmResourceListGetDescriptor(PResList, i);

        switch (pPartialTrResourceDesc->Type) 
        {
        case CmResourceTypePort:

            // Since RPi mini Uart is on ARM platform, it cannot have IO ports space

            TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                        "ERROR - I/O port resource detected for ARM platform\r\n");

            ASSERT(FALSE);
            break;

        // mini Uart uses memory mapped hardware 

        case CmResourceTypeMemory:

        if ((gotMem == 0) && (gotIO == 0)
                            && (pPartialTrResourceDesc->u.Memory.Length
                            >= SERIAL_REGISTER_SPAN )) {
            gotMem = 1;
            PConfig->TrController = pPartialTrResourceDesc->u.Memory.Start;

            if (!PConfig->TrController.LowPart) {
                TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                            "Bogus I/O memory address %Xh\r\n",
                            PConfig->TrController.LowPart);

                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                goto End;
            }

            PConfig->Controller = pPartialRawResourceDesc->u.Memory.Start;
            PConfig->AddressSpace = CM_RESOURCE_PORT_MEMORY;
            PConfig->SpanOfController = SERIAL_REGISTER_SPAN;
            pDevExt->SerialReadUChar = SerialReadRegisterUChar;
            pDevExt->SerialWriteUChar = SerialWriteRegisterUChar;
        }
        break;

        case CmResourceTypeInterrupt:
            if (gotInt == 0) {
                gotInt = 1;
                PConfig->TrVector = pPartialTrResourceDesc->u.Interrupt.Vector;

                if (!PConfig->TrVector) {
                    TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "Bogus interrupt vector 0\r\n");
                    status = STATUS_DEVICE_CONFIGURATION_ERROR;
                    goto End;
                }

                if (pPartialTrResourceDesc->ShareDisposition == CmResourceShareShared) {
                    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                                "Sharing interrupt with other devices\r\n");
                } else {
                    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                                "Interrupt is not shared with other devices\r\n");
                }

                PConfig->TrIrql = pPartialTrResourceDesc->u.Interrupt.Level;
                PConfig->Affinity = pPartialTrResourceDesc->u.Interrupt.Affinity;
            }
        break;

        // to configure RPi mini Uart hardware we need to manipulate GPIO pins
        // for that reason a separate Conenction Id ACPI resource is used

        case CmResourceTypeConnection:
            if (gotConnectionId == 0) {
                if ((pPartialTrResourceDesc->u.Connection.Class ==
                        CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG) &&
                    (pPartialTrResourceDesc->u.Connection.Type ==
                        CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG)) {

                    gotConnectionId = 1;
                    PConfig->FunctionConfigConnectionId.LowPart =
                        pPartialTrResourceDesc->u.Connection.IdLowPart;
                    PConfig->FunctionConfigConnectionId.HighPart =
                        pPartialTrResourceDesc->u.Connection.IdHighPart;
                }
            }
        break;

        default:
        break;
        } 

    }

    if(!((gotMem  || gotIO) && gotInt) )
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    // First check what type of AddressSpace this port is in. Then check
    // if the debugger is using this port. If it is, set DebugPortInUse to TRUE.

#if !SERIAL_IS_DONT_CHANGE_HW
    if(PConfig->AddressSpace == CM_RESOURCE_PORT_MEMORY) {

        PHYSICAL_ADDRESS  KdComPhysical;

        KdComPhysical = MmGetPhysicalAddress(*KdComPortInUse);

        if(KdComPhysical.LowPart == PConfig->Controller.LowPart) {
            pDevExt->DebugPortInUse = TRUE;
        }

    } else {

        // This compare is done using **untranslated** values since that is what
        // the kernel shoves in regardless of the architecture.

        if ((*KdComPortInUse) == (ULongToPtr(PConfig->Controller.LowPart)))    {
            pDevExt->DebugPortInUse = TRUE;
        }
    }
#endif !SERIAL_IS_DONT_CHANGE_HW

    if (pDevExt->DebugPortInUse) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "Kernel debugger is using port at address %p\r\n",
                    *KdComPortInUse);

        // If the kernel debugger is in use, and the platforms supports 
        // alternate GPIO settings, reserve it, so no application/device driver
        // can mux-out the debugger.
        // For that reason, we do NOT fail the device, and keep it loaded.

        if (PConfig->FunctionConfigConnectionId.QuadPart != 0LL) {
            pDevExt->FunctionConfigConnectionId = 
                PConfig->FunctionConfigConnectionId;

            // Reserve the function configuration resource and keep
            // the device loaded, if successful.

            status = SerialReserveFunctionConfig(Device, FALSE);
            goto End;
        }

        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "Serial driver will not load port\r\n");

        // Avoid retry loading the driver 

        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);

        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }

    End:

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--SerialMapHWResources()=%Xh\r\n", status);

    return status;
}

/*++

Routine Description:

    Releases resources (not pool) stored in the device extension.

Arguments:

    PDevExt - Pointer to the device extension to release resources from.

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID
SerialUnmapHWResources(
    PSERIAL_DEVICE_EXTENSION PDevExt
    )
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "++SerialUnMapResources(%p)\r\n",
                    PDevExt);
    PAGED_CODE();

    // If necessary, unmap the device registers.

    if (PDevExt->UnMapRegisters) {
        MmUnmapIoSpace(PDevExt->Controller, PDevExt->SpanOfController);
        PDevExt->UnMapRegisters = FALSE;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--SerialUnMapResources\r\n");
}

/*++

Routine Description:

    Reeads symbolic name of serial device.

Arguments:

    PDevExt - Pointer to the device extension to release resources from.

Return Value:

    NTstatus

--*/
_Use_decl_annotations_
NTSTATUS
SerialReadSymName(
    WDFDEVICE Device,
    PWSTR RegName,
    PUSHORT SizeOfRegName
    )
{
    NTSTATUS status;
    WDFKEY hKey;
    UNICODE_STRING value;
    UNICODE_STRING valueName;
    USHORT requiredLength;

    PAGED_CODE();

    value.Buffer = RegName;
    value.MaximumLength = *SizeOfRegName;
    value.Length = 0;

    status = WdfDeviceOpenRegistryKey(Device,
                                      PLUGPLAY_REGKEY_DEVICE,
                                      STANDARD_RIGHTS_ALL,
                                      WDF_NO_OBJECT_ATTRIBUTES,
                                      &hKey);

    if (NT_SUCCESS (status)) {

        // Fetch PortName which contains the suggested REG_SZ symbolic name.

        RtlInitUnicodeString(&valueName, L"PortName");

        status = WdfRegistryQueryUnicodeString (hKey,
                                              &valueName,
                                              &requiredLength,
                                              &value);

        if (!NT_SUCCESS (status)) {

            // This is for PCMCIA which currently puts the name under Identifier.

            RtlInitUnicodeString(&valueName, L"Identifier");
            status = WdfRegistryQueryUnicodeString (hKey,
                                                      &valueName,
                                                      &requiredLength,
                                                      &value);

            if (!NT_SUCCESS(status)) {

                // Either we have to pick a name or bail out

                TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                            "Getting PortName/Identifier failed - %Xh\r\n",
                            status);
            }
        }

        WdfRegistryClose(hKey);
    }

    if(NT_SUCCESS(status)) {

        // NULL terminate the string and return number of characters in the string.

        if(value.Length > *SizeOfRegName - sizeof(WCHAR)) {
            return STATUS_UNSUCCESSFUL;
        }

        *SizeOfRegName = value.Length;
        RegName[*SizeOfRegName/sizeof(WCHAR)] = UNICODE_NULL;
    }
    return status;
}


/*++

Routine Description:

    This routine will be used to create a symbolic link
    to the driver name in the given object directory.

    It will also create an entry in the device map for
    this device - IF we could create the symbolic link.

Arguments:

    Extension - Pointer to the device extension.

Return Value:

    None.

--*/
_Use_decl_annotations_
NTSTATUS
SerialDoExternalNaming(PSERIAL_DEVICE_EXTENSION PDevExt)
{
    NTSTATUS status = STATUS_SUCCESS;
    WCHAR pRegName[SYMBOLIC_NAME_LENGTH];
    USHORT nameSize = sizeof(pRegName);
    WDFSTRING stringHandle = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    DECLARE_UNICODE_STRING_SIZE(symbolicLinkName,SYMBOLIC_NAME_LENGTH ) ;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = PDevExt->WdfDevice;
    status = WdfStringCreate(NULL, &attributes, &stringHandle);
    if(!NT_SUCCESS(status)) {
        goto SerialDoExternalNamingError;
    }

    status = WdfDeviceRetrieveDeviceName(PDevExt->WdfDevice, stringHandle);
    if(!NT_SUCCESS(status)) {
        goto SerialDoExternalNamingError;
    }

    // Since we are storing the buffer pointer of the string handle in our
    // extension, we will hold onto string handle until the device is deleted.

    WdfStringGetUnicodeString(stringHandle, &PDevExt->DeviceName);

    SerialGetRegistryKeyValue(PDevExt->WdfDevice,
                                L"SerialSkipExternalNaming",
                                &PDevExt->SkipNaming);

    if (PDevExt->SkipNaming) {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                    "Will skip external naming due to registry settings\r\n");
    }

    // call below fails on Windows 10 IoT Core, since it needs desktop

    status = SerialReadSymName(PDevExt->WdfDevice, pRegName, &nameSize); 

    if (!NT_SUCCESS(status) && PDevExt->SkipNaming==0) {
        goto SerialDoExternalNamingError;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "DosName is %ws\r\n",
                pRegName);

    status = RtlUnicodeStringPrintf(&symbolicLinkName,
                                    L"%ws%ws",
                                    L"\\DosDevices\\",
                                    pRegName);

    if (!NT_SUCCESS(status) && PDevExt->SkipNaming==0) {
      goto SerialDoExternalNamingError;
    }

    status = WdfDeviceCreateSymbolicLink(PDevExt->WdfDevice, &symbolicLinkName);

    if (!NT_SUCCESS(status) && PDevExt->SkipNaming==0) {

      TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                "Couldn't create the symbolic link for port %wZ\r\n",
                &symbolicLinkName);

      goto SerialDoExternalNamingError;
    }

    PDevExt->CreatedSymbolicLink = TRUE;

    status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                    SERIAL_DEVICE_MAP,
                                    PDevExt->DeviceName.Buffer,
                                    REG_SZ,
                                    pRegName,
                                    nameSize);

    if (!NT_SUCCESS(status) && PDevExt->SkipNaming==0) {

      TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "Couldn't create the device map entry for port %ws\r\n",
                    PDevExt->DeviceName.Buffer);

      goto SerialDoExternalNamingError;
    }

    PDevExt->CreatedSerialCommEntry = TRUE;

    // Make the device visible via a device association as well.
    // The reference string is the eight digit device index

    status = WdfDeviceCreateDeviceInterface(PDevExt->WdfDevice,
                                            (LPGUID) &GUID_DEVINTERFACE_COMPORT,
                                            NULL);

    if (!NT_SUCCESS (status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "Couldn't register class association for port %wZ\r\n",
                    &PDevExt->DeviceName);

        goto SerialDoExternalNamingError;
    }

    return status;

SerialDoExternalNamingError:

    // Clean up error conditions

    PDevExt->DeviceName.Buffer = NULL;

    if (PDevExt->CreatedSerialCommEntry) {
        _Analysis_assume_(NULL != PDevExt->DeviceName.Buffer);

        RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                SERIAL_DEVICE_MAP,
                                PDevExt->DeviceName.Buffer);
    }

    if(stringHandle) {
        WdfObjectDelete(stringHandle);
    }
    
    return status;
}

/*++

Routine Description:

    This routine will be used to delete a symbolic link
    to the driver name in the given object directory.

    It will also delete an entry in the device map for
    this device if the symbolic link had been created.

Arguments:

    Extension - Pointer to the device extension.

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialUndoExternalNaming(PSERIAL_DEVICE_EXTENSION Extension)
{

   NTSTATUS status;
   PWCHAR   deviceName = Extension->DeviceName.Buffer;

   PAGED_CODE();

   TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "In SerialUndoExternalNaming for extension: %p of port %ws\r\n",
                Extension,
                deviceName);

   // Maybe there is nothing for us to do

   if (Extension->SkipNaming) {
      return;
   }

   // We're cleaning up here.  One reason we're cleaning up
   // is that we couldn't allocate space for the NtNameOfPort.

   if ((deviceName !=  NULL)  && Extension->CreatedSerialCommEntry) {

      status = RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                      SERIAL_DEVICE_MAP,
                                      deviceName);
      if (!NT_SUCCESS(status)) {

         TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                          "Couldn't delete value entry %ws\r\n",
                          deviceName);
      }
   }
}

/*++

Routine Description:

   This routine completes any irps pending for the passed device object.

Arguments:

    PDevObj - Pointer to the device object whose irps must die.

Return Value:

    VOID

--*/
_Use_decl_annotations_
VOID
SerialPurgePendingRequests(PSERIAL_DEVICE_EXTENSION pDevExt)
{
    NTSTATUS status;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "++SerialPurgePendingRequests(%p)\r\n",
                pDevExt);

    // Then cancel all the reads and writes.

    SerialPurgeRequests(pDevExt->WriteQueue,  &pDevExt->CurrentWriteRequest);

    SerialPurgeRequests(pDevExt->ReadQueue,  &pDevExt->CurrentReadRequest);

    // Next get rid of purges.

    SerialPurgeRequests(pDevExt->PurgeQueue,  &pDevExt->CurrentPurgeRequest);

    // Get rid of any mask operations.

    SerialPurgeRequests( pDevExt->MaskQueue,   &pDevExt->CurrentMaskRequest);

    // Now get rid of pending wait mask request.

    if (pDevExt->CurrentWaitRequest) {

        status = SerialClearCancelRoutine(pDevExt->CurrentWaitRequest, TRUE );
        if (NT_SUCCESS(status)) {

            SerialCompleteRequest(pDevExt->CurrentWaitRequest, STATUS_CANCELLED, 0);
            pDevExt->CurrentWaitRequest = NULL;

        }

    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--SerialPurgePendingRequests\r\n");
}

/*++

Routine Description:

    This routine examines several of what might be the serial device
    registers.  It ensures that the bits that should be zero are zero.

    In addition, this routine will determine if the device supports
    fifo's.  If it does it will enable the fifo's and turn on a boolean
    in the extension that indicates the fifo's presence.

    NOTE: If there is indeed a serial port at the address specified
          it will absolutely have interrupts inhibited upon return
          from this routine.

    NOTE: Since this routine should be called fairly early in
          the device driver initialization, the only element
          that needs to be filled in is the base register address.

    NOTE: These tests all assume that this code is the only
          code that is looking at these ports or this memory.

          This is a not to unreasonable assumption even on
          multiprocessor systems.

Arguments:

    Extension - A pointer to a serial device extension.
    InsertString - String to place in an error log entry.
    ForceFifo - !0 forces the fifo to be left on if found.
    LogFifo - !0 forces a log message if fifo found.

Return Value:

    Will return true if the port really exists, otherwise it
    will return false.

--*/
_Use_decl_annotations_
BOOLEAN
SerialDoesPortExist(
                   PSERIAL_DEVICE_EXTENSION Extension,
                   ULONG ForceFifo
                   )
{
    UCHAR regContents;
    BOOLEAN returnValue = TRUE;
    UCHAR oldIERContents;
    UCHAR oldLCRContents;
    USHORT value1;
    USHORT value2;
    KIRQL oldIrql;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "++SerialDoesPortExist\r\n");

    // Save of the line control.

    oldLCRContents = READ_LINE_CONTROL(Extension, Extension->Controller);

    // Set DLAB=0 Make sure that we are *aren't* accessing the divsior latch.

    WRITE_LINE_CONTROL(Extension,
                        Extension->Controller,
                        (UCHAR)(oldLCRContents & ~SERIAL_LCR_DLAB));

    oldIERContents = READ_INTERRUPT_ENABLE(Extension, Extension->Controller);

    // Go up to power level for a very short time to prevent
    // any interrupts from this device from coming in.

    KeRaiseIrql(POWER_LEVEL,
                &oldIrql);

    WRITE_INTERRUPT_ENABLE(Extension,
                            Extension->Controller,
                            0x0f);

    value1 = READ_INTERRUPT_ENABLE(Extension, Extension->Controller);
    value1 = value1 << 8;
    value1 |= READ_RECEIVE_BUFFER(Extension, Extension->Controller);

    READ_DIVISOR_LATCH(Extension,
                        Extension->Controller,
                        (PSHORT) &value2);

    WRITE_LINE_CONTROL(Extension,
                        Extension->Controller,
                        oldLCRContents);

    // Put the ier back to where it was before.  If we are on a
    // level sensitive port this should prevent the interrupts
    // from coming in.  If we are on a latched, we don't care
    // cause the interrupts generated will just get dropped.

    WRITE_INTERRUPT_ENABLE(Extension,
                            Extension->Controller,
                            oldIERContents);

    KeLowerIrql(oldIrql);

    // note: check comparing value1 == value2 may fail on Rpi mini Uart
    // due to limited compatibility with 16550 UART hardware
    // such failed check does not mean RPi mini Uart is not present

    // If we think that there is a serial device then we determine
    // if a fifo is present.

    if (returnValue) {

        // we think it's a serial device. 
        // Prevent interrupts from occuring.
        //
        // We disable all the interrupt enable bits, and
        // on 16550 push down all the lines in the modem control
        // on PC's we only needed to push down OUT2
        // but on RPi we disable interrupts here

        DISABLE_ALL_INTERRUPTS(Extension, Extension->Controller);

        WRITE_MODEM_CONTROL(Extension, Extension->Controller, (UCHAR)0);

        // See if this is a 16550.  We do this by writing to
        // what would be the fifo control register with a bit
        // pattern that tells the device to enable fifo's.
        // We then read the iterrupt Id register to see if the
        // bit pattern is present that identifies the 16550.

        WRITE_FIFO_CONTROL(Extension,
                        Extension->Controller,
                        SERIAL_FCR_ENABLE);

        regContents = READ_INTERRUPT_ID_REG(Extension, Extension->Controller);

        if (regContents & SERIAL_IIR_FIFOS_ENABLED) {

            // Save off that the device supports fifos.

            Extension->FifoPresent = TRUE;

            // There is a fine new "super" IO chip out there that
            // will get stuck with a line status interrupt if you
            // attempt to clear the fifo and enable it at the same
            // time if data is present.  The best workaround seems
            // to be that you should turn off the fifo read a single
            // byte, and then re-enable the fifo.

            WRITE_FIFO_CONTROL(Extension,
                            Extension->Controller,
                            (UCHAR)0);

            READ_RECEIVE_BUFFER(Extension, Extension->Controller);

            // There are fifos on this card.  Set the value of the
            // receive fifo to interrupt when 4 characters are present.

            WRITE_FIFO_CONTROL(Extension, Extension->Controller,
                            (UCHAR)(SERIAL_FCR_ENABLE
                                    | Extension->RxFifoTrigger
                                    | SERIAL_FCR_RCVR_RESET
                                    | SERIAL_FCR_TXMT_RESET));

        }

        // The !Extension->FifoPresent is included in the test so that
        // broken chips like the WinBond will still work after we test
        // for the fifo.

        if (!ForceFifo || !Extension->FifoPresent) {

            Extension->FifoPresent = FALSE;
            WRITE_FIFO_CONTROL(Extension,
                            Extension->Controller,
                            (UCHAR)0);

        }

        if (Extension->FifoPresent) {

            TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                            "Fifo's detected at port address: %ph\r\n",
                            Extension->Controller);
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "--SerialDoesPortExist()=%Xh\r\n",
                returnValue);

    return returnValue;
}


/*++

Routine Description:

    This places the hardware in a standard configuration.

    NOTE: This assumes that it is called at interrupt level.


Arguments:

    Context - The device extension for serial device
    being managed.

Return Value:

    Always FALSE.

--*/
_Use_decl_annotations_
BOOLEAN
SerialReset(
    WDFINTERRUPT Interrupt,
    PVOID Context
    )
{
   PSERIAL_DEVICE_EXTENSION extension = Context;
   UCHAR regContents;
   UCHAR oldModemControl;
   ULONG i;

   UNREFERENCED_PARAMETER(Interrupt);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "++SerialReset()\r\n");

   // Adjust the out2 bit.
   // This will also prevent any interrupts from occuring on 16550 when on PC.
   // has no effect on RPi mini Uart

   oldModemControl = READ_MODEM_CONTROL(extension, extension->Controller);

   WRITE_MODEM_CONTROL(extension, extension->Controller,
                       (UCHAR)(oldModemControl & ~SERIAL_MCR_OUT2));

   // Reset the fifo's if there are any.

   if (extension->FifoPresent) 
   {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                    "SerialReset() mapped IO chip workaround\r\n");

      // There is a fine new "super" IO chip out there that
      // will get stuck with a line status interrupt if you
      // attempt to clear the fifo and enable it at the same
      // time if data is present. The best workaround seems
      // to be that you should turn off the fifo read a single
      // byte, and then re-enable the fifo.

      WRITE_FIFO_CONTROL(extension,
                        extension->Controller,
                        (UCHAR)0);

      READ_RECEIVE_BUFFER(extension, extension->Controller);

      WRITE_FIFO_CONTROL(extension,
                        extension->Controller,
                        (UCHAR)(SERIAL_FCR_ENABLE | extension->RxFifoTrigger |
                                SERIAL_FCR_RCVR_RESET | SERIAL_FCR_TXMT_RESET));
   }

   // Make sure that the line control set up correct.
   //
   // 1) Make sure that the Divisor latch select is set
   //    up to select the transmit and receive register.
   //
   // 2) Make sure that we aren't in a break state.

   regContents = READ_LINE_CONTROL(extension, extension->Controller);
   regContents &= ~(SERIAL_LCR_DLAB | SERIAL_LCR_BREAK);

   WRITE_LINE_CONTROL(extension,
                     extension->Controller,
                     regContents);

   // Read the receive buffer until the line status is
   // clear.  Give up after a 5 reads.

   for (i = 0; i < 5; i++) {

             READ_RECEIVE_BUFFER(extension, extension->Controller);
             if (!(READ_LINE_STATUS(extension, extension->Controller) & 1)) {

                break;

      } else {

          // I get incorrect data when read empty buffer.
          // But do not read no data! for PC98!

          if (!(READ_LINE_STATUS(extension, extension->Controller) & 1)) {

             break;

          }
          READ_RECEIVE_BUFFER(extension, extension->Controller);
      }
   }

   // Read the modem status until the low 4 bits are
   // clear.  Give up after a 5 reads.

   for (i = 0; i < 5; i++) {

      if (!(READ_MODEM_STATUS(extension, extension->Controller) & 0x0f)) {

         break;

      }
   }

   // Now we set the line control, modem control, and the
   // baud to what they should be.

   TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                "SerialReset() set the line control, modem control, and the baud\r\n");

   SerialSetLineControl(extension->WdfInterrupt, extension);

   SerialSetupNewHandFlow(extension,
                         &extension->HandFlow);

   SerialHandleModemUpdate(extension,
                          FALSE);

   {
      SHORT  appropriateDivisor;
      SERIAL_IOCTL_SYNC s;

      SerialGetDivisorFromBaud(extension->ClockRate,
                                extension->CurrentBaud,
                                &appropriateDivisor);

      s.Extension = extension;
      s.Data = (PVOID) (ULONG_PTR) appropriateDivisor;
      SerialSetBaud(extension->WdfInterrupt, &s);
   }

   // Read the interrupt id register until the low bit is
   // set.  Give up after a 5 reads

   for (i = 0; i < 5; i++) {

      if (READ_INTERRUPT_ID_REG(extension, extension->Controller) & 0x01) {

         break;

      }
   }

   // Now we know that nothing could be transmitting at this point
   // so we set the HoldingEmpty indicator.

    extension->HoldingEmpty = TRUE;
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--SerialReset()\r\n");

    return FALSE;
}

/*++

Routine Description:

    This routine maps an IO address to system address space.

Arguments:

    IoAddress - base device address to be mapped.
    NumberOfBytes - number of bytes for which address is valid.
    AddressSpace - Denotes whether the address is in io space or memory.
    MappedAddress - indicates whether the address was mapped.
                    This only has meaning if the address returned
                    is non-null.

Return Value:

    Mapped address

--*/
_Use_decl_annotations_
PVOID
SerialGetMappedAddress(
    PHYSICAL_ADDRESS IoAddress,
    ULONG NumberOfBytes,
    ULONG AddressSpace,
    PBOOLEAN MappedAddress
    )
{
   PVOID address;

   PAGED_CODE();

   // Map the device base address into the virtual address space
   // if the address is in memory space.

   if (!AddressSpace) {

      address = LocalMmMapIoSpace(IoAddress,
                                  NumberOfBytes);

      *MappedAddress = (BOOLEAN)((address)?(TRUE):(FALSE));


   } else {

      address = ULongToPtr(IoAddress.LowPart);
      *MappedAddress = FALSE;

   }

   return address;
}

/*++

Routine Description:

    This routine shows how to set the interrupt policy preferences.

Arguments:

    WdfInterrupt - Interrupt object handle.

Return Value:

    None

--*/
_Use_decl_annotations_
VOID
SerialSetInterruptPolicy(
   WDFINTERRUPT WdfInterrupt
   )
{
    WDF_INTERRUPT_EXTENDED_POLICY   policyAndGroup;

    WDF_INTERRUPT_EXTENDED_POLICY_INIT(&policyAndGroup);
    policyAndGroup.Priority = WdfIrqPriorityNormal;

     TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INTERRUPT, "++SerialSetInterruptPolicy()\r\n");

    // Set interrupt policy and group preference.

    WdfInterruptSetExtendedPolicy(WdfInterrupt, &policyAndGroup);
    TraceEvents(TRACE_LEVEL_INFORMATION,DBG_INTERRUPT, "--SerialSetInterruptPolicy()\r\n");
}

/*++

Routine Description:

    This routine prints all mini Uart resgiters.

Arguments:

    PSERIAL_DEVICE_EXTENSION - poiter to device extension

Return Value:

    TRUE if succeeded, FALSE if failed to print registers

--*/
_Use_decl_annotations_
BOOLEAN PrintMiniUartregs(PSERIAL_DEVICE_EXTENSION pDevExt)
{
    BOOLEAN bResult=FALSE;
    UCHAR uchTempReg=0x00;
    USHORT uchTempReg16=0x00;
    ULONG uchTempReg32=0x00;

    if(NULL==pDevExt || NULL==pDevExt->Controller) {

        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "bPrintMiniUartregs() - null ptr\r\n");
    } else {

        // 0x3E215004 Enable
        uchTempReg=READ_MINIUART_ENABLE(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Enabled=%Xh\r\n",uchTempReg);

        // 0x3E215040 THR/RCV
        uchTempReg=READ_RECEIVE_BUFFER(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "THR/RCV=%Xh\r\n",uchTempReg);

        // 0x3E215044 IER
        uchTempReg=READ_INTERRUPT_ENABLE(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "IER=%Xh\r\n",uchTempReg);

        // 0x3E215048 IIR
        uchTempReg=READ_INTERRUPT_ID_REG(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "IIR=%Xh\r\n",uchTempReg);

        // 0x3E21504C LCR
        uchTempReg=READ_LINE_CONTROL(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "LCR=%Xh\r\n",uchTempReg);

        // 0x3E215050 MCR
        uchTempReg=READ_MODEM_CONTROL(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "MCR=%Xh\r\n",uchTempReg);

        // 0x3E215054 LSR
        uchTempReg=READ_LINE_STATUS(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "LSR=%Xh\r\n",uchTempReg);

        // 0x3E215058 MSR
        uchTempReg=READ_MODEM_STATUS(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "MSR=%Xh\r\n",uchTempReg);

        // 0x3E215060 extra control
        uchTempReg=READ_MINIUARTRXTX_ENABLE(pDevExt, pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "ExtraCTRL=%Xh\r\n",uchTempReg);

        // 0x3E215064 extra status, 32-bit
        uchTempReg32=READ_EXTRA_STATUS(pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "ExtraSTS=%08Xh\r\n",uchTempReg32);

        // 0x3E215068 extra baud rate, 16-bit
        uchTempReg16=READ_EXTRA_BAUD(pDevExt->Controller); 
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "ExtraBAUD=%04Xh\r\n",uchTempReg16);

        bResult=TRUE;
    }

    return bResult;
}