//
//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//    PL011device.cpp
//
// Abstract:
//
//    This module contains the ARM PL011 UART controller's PNP device functions.
//    This controller driver uses the Serial WDF class extension (SerCx2).
//
// Environment:
//
//    kernel-mode only
//
#include "precomp.h"
#pragma hdrstop

#define _PL011_DEVICE_CPP_

// Logging header files
#include "PL011logging.h"
#include "PL011device.tmh"

// Common driver header files
#include "PL011common.h"

// Module specific header files
#include "PL011rx.h"
#include "PL011tx.h"
#include "PL011interrupt.h"
#include "PL011driver.h"


#ifdef ALLOC_PRAGMA
    #pragma alloc_text(PAGE, PL011EvtDeviceAdd)
    #pragma alloc_text(PAGE, PL011EvtDevicePrepareHardware)
    #pragma alloc_text(PAGE, PL011EvtDeviceReleaseHardware)
    #pragma alloc_text(PAGE, PL011EvtSerCx2ApplyConfig)

    #pragma alloc_text(PAGE, PL011pDeviceExtensionInit)
    #pragma alloc_text(PAGE, PL011pDeviceSerCx2Init)
    #pragma alloc_text(PAGE, PL011pDeviceParseResources)
    #pragma alloc_text(PAGE, PL011pDeviceMapResources)
    #pragma alloc_text(PAGE, PL011pDeviceCreateDeviceInterface)
    #pragma alloc_text(PAGE, PL011pDeviceReserveFunctionConfigResource)
    #pragma alloc_text(PAGE, PL011pDeviceGetSupportedFeatures)
#endif // ALLOC_PRAGMA


//
// This is exported from the kernel.  It is used to point
// to the address that the kernel debugger is using.
//
extern "C" extern PUCHAR* KdComPortInUse;


//
// Routine Description:
//
//  PL011EvtDeviceAdd is called by the framework in response to AddDevice
//  call from the PnP manager.
//  The function creates and initializes a new PL011 WDF device, and registers it
//  with the SerCx framework.
//
// Arguments:
//
//  DeviceInitPtr - Pointer to a framework-allocated WDFDEVICE_INIT structure.
//
// Return Value:
//
//   Device creation and SerCx2 registration status.
//
_Use_decl_annotations_
NTSTATUS
PL011EvtDeviceAdd(
    WDFDRIVER WdfDriver,
    PWDFDEVICE_INIT DeviceInitPtr
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(WdfDriver);

    //
    // Set P&P and Power callbacks
    //
    {
        WDF_PNPPOWER_EVENT_CALLBACKS wdfPnppowerEventCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&wdfPnppowerEventCallbacks);
        wdfPnppowerEventCallbacks.EvtDevicePrepareHardware = PL011EvtDevicePrepareHardware;
        wdfPnppowerEventCallbacks.EvtDeviceReleaseHardware = PL011EvtDeviceReleaseHardware;

        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInitPtr, &wdfPnppowerEventCallbacks);

    } // P&P and Power

    //
    // Call SerCx2InitializeDeviceInit() to attach the SerCX2 to the
    // WDF pipeline.
    //
    // Note:
    //  SerCx2InitializeDeviceInit MUST be called before calling
    //  WdfDeviceCreate()!
    //
    NTSTATUS status = SerCx2InitializeDeviceInit(DeviceInitPtr);
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "SerCx2InitializeDeviceInit failed, (status = %!STATUS!)", status
            );
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(
        SDDL_DEVOBJ_SERCX_SYS_ALL_ADM_ALL_UMDF_ALL_USERS_RDWR,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;UD)(A;;GRGW;;;BU)"
        );
    status = WdfDeviceInitAssignSDDLString(
            DeviceInitPtr,
            &SDDL_DEVOBJ_SERCX_SYS_ALL_ADM_ALL_UMDF_ALL_USERS_RDWR
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfDeviceInitAssignSDDLString failed. (status = %!STATUS!, SDDL_DEVOBJ_SERCX_SYS_ALL_ADM_ALL_UMDF_ALL_USERS_RDWR = %wZ)",
            status,
            &SDDL_DEVOBJ_SERCX_SYS_ALL_ADM_ALL_UMDF_ALL_USERS_RDWR
            );
        return status;
    }

    //
    // Create and initialize the WDF device.
    //
    WDFDEVICE wdfDevice;
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PL011_DEVICE_EXTENSION);

        status = WdfDeviceCreate(&DeviceInitPtr, &attributes, &wdfDevice);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "WdfDeviceCreate failed, (status = %!STATUS!)", status
                );
            return status;
        }

        status = PL011pDeviceExtensionInit(wdfDevice);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "PL011DeviceExtensionInit failed, (status = %!STATUS!)", status
                );
            return status;
        }

    } // Create the WDF device

    //
    // Initialize and register the device with SerCx2 class extension.
    //
    status = PL011pDeviceSerCx2Init(wdfDevice);
    if (!NT_SUCCESS(status)) {

        return status;
    }

    return status;
}


//
// Routine Description:
//
//  PL011EvtDevicePrepareHardware() is called by the framework when a PL011
//  device is coming online, after being it's resources have been negotiated and
//  translated.
//  The routine reads and map the device resources and initializes the device.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  ResourcesRaw - Hardware resource list.
//
//  ResourcesTranslated - Hardware translated resource list.
//
// Return Value:
//
//  Resources mapping status and controller initialization completion code.
//
_Use_decl_annotations_
NTSTATUS
PL011EvtDevicePrepareHardware(
    WDFDEVICE WdfDevice,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourcesRaw);

    NTSTATUS status = PL011pDeviceParseResources(
        WdfDevice,
        ResourcesTranslated
        );
    if (!NT_SUCCESS(status)) {

        return status;
    }

    //
    // Check if we are conflicting with the debugger, and if so, do not
    // create a device interface or initialize hardware.
    //
    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);
    if (devExtPtr->IsDebuggerConflict) {

        //
        // If we don't have an FunctionConfig() resource that we need to hold
        // on to, fail the load of the driver.
        //
        if (devExtPtr->PL011ResourceData.FunctionConfigConnectionId.QuadPart == 0LL) {

            PL011_LOG_ERROR("Detected conflict with kernel debugger, failing load.");

            // Tell WDF not to attempt it's usual retry loop
            WdfDeviceSetFailed(WdfDevice, WdfDeviceFailedNoRestart);
            return STATUS_UNSUCCESSFUL;
        }

        //
        // Reserve the FunctionConfig() resource so that another client can't
        // steal the pins away from the kernel debugger.
        //
        status = PL011pDeviceReserveFunctionConfigResource(devExtPtr);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "PL011pDeviceReserveFunctionConfigResource(...) failed. (status = %!STATUS!, FunctionConfigConnectionId = %llx)",
                status,
                devExtPtr->PL011ResourceData.FunctionConfigConnectionId.QuadPart
                );
            return status;
        }

        return STATUS_SUCCESS;
    }

    status = PL011pDeviceMapResources(
        WdfDevice,
        ResourcesRaw,
        ResourcesTranslated
        );
    if (!NT_SUCCESS(status)) {

        return status;
    }

    //
    // If we received a UartSerialBus resource, make device accessible
    // to usermode.
    //
    if (devExtPtr->PL011ResourceData.ConnectionId.QuadPart != 0LL) {

        status = PL011pDeviceCreateDeviceInterface(
                WdfDevice,
                devExtPtr->PL011ResourceData.ConnectionId
                );
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "PL011pDeviceCreateDeviceInterface failed. (status = %!STATUS!, devExtPtr->PL011ResourceData.ConnectionId = %llx)",
                status,
                devExtPtr->PL011ResourceData.ConnectionId.QuadPart
                );
            return status;
        }
    } else {

        PL011_LOG_INFORMATION("Skipping creation of device interface due to absence of UartSerialBus() descriptor.");
    }

    return PL011HwInitController(WdfDevice);
}


//
// Routine Description:
//
//  PL011EvtDeviceReleaseHardware() is called by the framework when a PL011
//  device is offline and not accessible anymore.
//  The routine just releases device resources.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  ResourcesTranslated - Hardware translated resource list.
//
// Return Value:
//
//  STATUS_SUCCESS
//
_Use_decl_annotations_
NTSTATUS
PL011EvtDeviceReleaseHardware(
    WDFDEVICE WdfDevice,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();

    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);
    PPL011_RESOURCE_DATA resourceDataPtr = &devExtPtr->PL011ResourceData;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (devExtPtr->PL011RegsPtr != nullptr) {

        PL011_ASSERT(resourceDataPtr->RegsSpan == ULONG(PL011_REG_FILE::REG_FILE_SIZE));

        MmUnmapIoSpace(
            PVOID(devExtPtr->PL011RegsPtr),
            resourceDataPtr->RegsSpan
            );
    }

    //
    // Disconnecting from interrupt will automatically be done
    // by the framework....
    //

    //
    // Clear resource information
    //
    RtlZeroMemory(resourceDataPtr, sizeof(PL011_RESOURCE_DATA));
    devExtPtr->PL011RegsPtr = nullptr;
    devExtPtr->WdfUartInterrupt = NULL;

    return STATUS_SUCCESS;
}


//
// Routine Description:
//
//  PL011DeviceExtensionInit is called by translate received PL011 errors
//  into SERIAL_ERROR_??? codes, and record those on the device context, to be picked
//  up when through IOCTL_SERIAL_GET_COMMSTATUS.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  PL011ErrorEventsMask - The new PL011 errors events mask
//
// Return Value:
//
_Use_decl_annotations_
VOID
PL011DeviceRecordErrors(
    PL011_DEVICE_EXTENSION* DevExtPtr,
    ULONG PL011ErrorEventsMask
    )
{
    LONG newErrorTypes = 0;

    if ((PL011ErrorEventsMask & UARTIMSC_OEIM) != 0) {

        newErrorTypes |= SERIAL_ERROR_OVERRUN;
    }

    if ((PL011ErrorEventsMask & UARTIMSC_FEIM) != 0) {

        newErrorTypes |= SERIAL_ERROR_FRAMING;
    }

    if ((PL011ErrorEventsMask & UARTIMSC_PEIM) != 0) {

        newErrorTypes |= SERIAL_ERROR_PARITY;
    }

    if ((PL011ErrorEventsMask & UARTIMSC_BEIM) != 0) {

        newErrorTypes |= SERIAL_ERROR_BREAK;
    }

    InterlockedOr(
        reinterpret_cast<volatile LONG*>(&DevExtPtr->UartErrorTypes),
        newErrorTypes
        );
}


//
// Routine Description:
//
//  PL011DeviceNotifyEvents is called by translate received PL011 events
//  into SERIAL_EV_??? codes, and notify the framework if the new events
//  correspond to the 'wait event mask' previously set by the framework
//  through EvtSerCx2SetWaitMask() (IOCTL_SERIAL_SET_WAIT_MASK).
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  PL011EventsMask - The new PL011 events mask.
//
// Return Value:
//
_Use_decl_annotations_
VOID
PL011DeviceNotifyEvents(
    PL011_DEVICE_EXTENSION* DevExtPtr,
    ULONG PL011EventsMask
    )
{
    //
    // Translate received events to SERIAL_EV_??? so we
    // can notify the framework if needed.
    //
    ULONG waitEvents = 0;

    if ((PL011EventsMask & UART_INTERUPPTS_ERRORS) != 0) {

        waitEvents |= SERIAL_EV_ERR;
    }
    if ((PL011EventsMask & UARTRIS_BEIS) != 0) {

        waitEvents |= SERIAL_EV_BREAK;
    }

    //
    // Modem status interrupt
    //
    if ((PL011EventsMask & UARTRIS_CTSMIS) != 0) {

        waitEvents |= SERIAL_EV_CTS;
    }
    if ((PL011EventsMask & UARTRIS_DSRMIS) != 0) {

        waitEvents |= SERIAL_EV_DSR;
    }

    //
    // Complete 'wait events', if any...
    //
    {
        KIRQL oldIrql = ExAcquireSpinLockShared(&DevExtPtr->ConfigLock);

        ULONG currentWaitMask = DevExtPtr->WaitEventMask;

        if ((waitEvents != 0) && ((waitEvents & currentWaitMask) != 0)) {

            SerCx2CompleteWait(DevExtPtr->WdfDevice, waitEvents);
        }

        ExReleaseSpinLockShared(&DevExtPtr->ConfigLock, oldIrql);

    } // Complete 'wait events', if any...
}

//
// Routine Description:
//
//  Read device-specific data from ACPI.
//
// Arguments:
//
//  WdfDevice - The PL011 device instance
//
//  ClockFrequencyPtr - Pointer to an ULONG representing the clock frequency (Hz)
//
// Return Value:
//
//  NTSTATUS
//
_Use_decl_annotations_
NTSTATUS
PL011pReadDeviceSpecificData(
    WDFDEVICE WdfDevice,
    ULONG* ClockFrequencyPtr
    )
{
    NT_ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    if (ClockFrequencyPtr == nullptr)
        return STATUS_INVALID_PARAMETER;

    PDEVICE_OBJECT pdoPtr = WdfDeviceWdmGetPhysicalDevice(WdfDevice);
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* dsdBufferPtr = nullptr;
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* clckObjOutputBufferPtr = nullptr;
    UINT32 clockFrequency = 0;
    NTSTATUS status;

    status = AcpiQueryDsd(pdoPtr, &dsdBufferPtr);
    if (!NT_SUCCESS(status)) {
        PL011_LOG_ERROR(
            "AcpiQueryDsd failed. (status = %!STATUS!)", status
        );
        goto done;
    }

    const ACPI_METHOD_ARGUMENT UNALIGNED* devicePropertiesPkgPtr = nullptr;
    status = AcpiParseDsdAsDeviceProperties(dsdBufferPtr, &devicePropertiesPkgPtr);
    if (!NT_SUCCESS(status)) {
        PL011_LOG_ERROR(
            "AcpiParseDsdAsDeviceProperties failed. (status = %!STATUS!)", status
        );
        goto done;
    }

    const ACPI_METHOD_ARGUMENT UNALIGNED* currentPairEntryPtr = nullptr;
    status = AcpiDevicePropertiesQueryValue(
        devicePropertiesPkgPtr,
        "clock-frequency",
        &currentPairEntryPtr);

    if (!NT_SUCCESS(status)) {
        PL011_LOG_ERROR(
            "AcpiDevicePropertiesQueryValue failed. (status = %!STATUS!)", status
        );
        goto done;
    }

    //
    // Normally we expect an ACPI_METHOD_ARGUMENT_INTEGER,
    // but our _DSD is declared as follows:
    // 
    //      Name(CLCK, 48000000)
    //
    //      Name(_DSD, Package()
    //      {
    //          ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"), Package()
    //          {
    //              Package(2) { "clock-frequency", CLCK },
    //          }
    //      })
    // 
    // The call above returns the string "CLCK", so we evaluate
    // this object and get its argument.
    //
    if (currentPairEntryPtr->Type == ACPI_METHOD_ARGUMENT_STRING) {
        CHAR objectName[ARRAYSIZE(ACPI_EVAL_INPUT_BUFFER::MethodName) + 1];
        UINT32 length;

        status = AcpiArgumentParseString(
            currentPairEntryPtr,
            ARRAYSIZE(objectName),
            &length,
            objectName);

        if (!NT_SUCCESS(status)) {
            PL011_LOG_ERROR(
                "AcpiArgumentParseString failed. (status = %!STATUS!)", status
            );
            goto done;
        }
        
        ACPI_EVAL_INPUT_BUFFER clckObjInputBuffer;

        RtlZeroMemory(&clckObjInputBuffer, sizeof(clckObjInputBuffer));
        clckObjInputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
        RtlCopyMemory(clckObjInputBuffer.MethodName, objectName, ARRAYSIZE(clckObjInputBuffer.MethodName));

        status = AcpiEvaluateMethod(
            pdoPtr,
            &clckObjInputBuffer,
            sizeof(ACPI_EVAL_INPUT_BUFFER),
            &clckObjOutputBufferPtr);

        if (!NT_SUCCESS(status)) {
            PL011_LOG_ERROR(
                "AcpiEvaluateMethod failed. (status = %!STATUS!)", status
            );
            goto done;
        }

        if (clckObjOutputBufferPtr->Count != 1) {
            PL011_LOG_ERROR(
                "clckObjOutputBufferPtr should've had 1 argument, but it has %u", 
                clckObjOutputBufferPtr->Count
            );
            status = STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;
            goto done;
        }

        currentPairEntryPtr = nullptr;
        status = AcpiOutputBufferGetNextArgument(clckObjOutputBufferPtr, &currentPairEntryPtr);
        if (!NT_SUCCESS(status)) {
            PL011_LOG_ERROR(
                "AcpiOutputBufferGetNextArgument failed. (status = %!STATUS!)", status
            );
            goto done;
        }
    }

    //
    // Try to parse the argument as integer.
    // currentPairEntryPtr points to either the value of the "clock-frequency" key
    // or the argument of CLCK.
    //
    status = AcpiArgumentParseInteger(currentPairEntryPtr, &clockFrequency);
    if (!NT_SUCCESS(status)) {
        PL011_LOG_ERROR(
            "AcpiArgumentParseInteger failed. (status = %!STATUS!)", status
        );
        goto done;
    }

    *ClockFrequencyPtr = static_cast<ULONG>(clockFrequency);

done:

    if (dsdBufferPtr != nullptr) {
        ExFreePoolWithTag(dsdBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
    }

    if (clckObjOutputBufferPtr != nullptr) {
        ExFreePoolWithTag(clckObjOutputBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
    }

    return status;
}

//
// Routine Description:
//
//  PL011DeviceExtensionInit is called by PL011EvtDeviceAdd to initialize the
//  WDF device extension.
//  The routine initializes context objects and registers the driver ISR/DPC.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
// Return Value:
//
//  STATUS_SUCCESS, or STATUS_NOT_SUPPORTED if board does not support the
//  features mask that was set in INF file.
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceExtensionInit(
    WDFDEVICE WdfDevice
    )
{
    PAGED_CODE();

    const PL011_DRIVER_EXTENSION* drvExtPtr = PL011DriverGetExtension(WdfGetDriver());
    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);

    devExtPtr->WdfDevice = WdfDevice;
    devExtPtr->OpenCount = 0;
    devExtPtr->ConfigLock = 0;
    devExtPtr->UartSupportedControlsMask = PL011_DEFAULT_SUPPORTED_CONTROLS;

    if (drvExtPtr->UartClockHz.IsFallbackValue) {
        NTSTATUS status = PL011pReadDeviceSpecificData(WdfDevice, &devExtPtr->CurrentConfiguration.UartClockHz);

        if (!NT_SUCCESS(status)) {
            PL011_LOG_WARNING(
                "PL011pReadDeviceSpecificData failed. (status = %!STATUS!) -- falling back to defaults",
                status
            );
            devExtPtr->CurrentConfiguration.UartClockHz = drvExtPtr->UartClockHz.Value;
        }
    }

    if (drvExtPtr->MaxBaudRateBPS.IsFallbackValue) {
        devExtPtr->CurrentConfiguration.MaxBaudRateBPS = devExtPtr->CurrentConfiguration.UartClockHz / 16;
    } else {
        devExtPtr->CurrentConfiguration.MaxBaudRateBPS = drvExtPtr->MaxBaudRateBPS.Value;
    }

    KeInitializeSpinLock(&devExtPtr->Lock);
    KeInitializeSpinLock(&devExtPtr->RegsLock);

    //
    // Get the features supported by this board
    //
    return PL011pDeviceGetSupportedFeatures(WdfDevice);
}


//
// Routine Description:
//
//  PL011DeviceSerCx2Init is called by PL011EvtDeviceAdd to initialize the
//  SerCx2 class extension with the device.
//  The routine registers the driver callbacks with SerCx2 class extension.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
// Return Value:
//
//  SerCxe initialization status.
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceSerCx2Init(
    WDFDEVICE WdfDevice
    )
{
    PAGED_CODE();

    NTSTATUS status;
    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);

    //
    // Initialize SerCx2 class extension.
    //
    {
        SERCX2_CONFIG serCx2Config;
        SERCX2_CONFIG_INIT(
            &serCx2Config,
            PL011EvtSerCx2ApplyConfig,
            PL011EvtSerCx2Control,
            PL011EvtSerCx2PurgeFifos
            );
        serCx2Config.EvtSerCx2SetWaitMask = PL011EvtSerCx2SetWaitMask;
        serCx2Config.EvtSerCx2FileOpen = PL011EvtSerCx2FileOpen;
        serCx2Config.EvtSerCx2FileClose = PL011EvtSerCx2FileClose;

        status = SerCx2InitializeDevice(WdfDevice, &serCx2Config);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "SerCx2InitializeDevice failed, (status = %!STATUS!)", status
                );
            return status;
        }

    } // Initialize SerCx2 class extension.

    //
    // Configure receive PIO contexts and callbacks
    //
    {
        SERCX2_PIO_RECEIVE_CONFIG serCx2PioReceiveConfig;
        SERCX2_PIO_RECEIVE_CONFIG_INIT(
            &serCx2PioReceiveConfig,
            PL011SerCx2EvtPioReceiveReadBuffer,
            PL011SerCx2EvtPioReceiveEnableReadyNotification,
            PL011SerCx2EvtPioReceiveCancelReadyNotification
            );

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &attributes,
            PL011_SERCXPIORECEIVE_CONTEXT
            );

        //
        // Create the SerCx2 RX object
        //
        status = SerCx2PioReceiveCreate(
            WdfDevice,
            &serCx2PioReceiveConfig,
            &attributes,
            &devExtPtr->SerCx2PioReceive
            );
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "SerCx2PioTransmitCreate failed, (status = %!STATUS!)", status
                );
            return status;
        }

        //
        // Initialize the SerCx2 RX object context
        //
        status = PL011RxPioReceiveInit(WdfDevice, devExtPtr->SerCx2PioReceive);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "PL011RxPioReceiveInit failed, (status = %!STATUS!)", status
                );
            return status;
        }

    } // Configure receive PIO

    //
    // Configure transmit PIO contexts and callbacks
    //
    {
        SERCX2_PIO_TRANSMIT_CONFIG serCx2PioTransmitConfig;
        SERCX2_PIO_TRANSMIT_CONFIG_INIT(
            &serCx2PioTransmitConfig,
            PL011SerCx2EvtPioTransmitWriteBuffer,
            PL011SerCx2EvtPioTransmitEnableReadyNotification,
            PL011SerCx2EvtPioTransmitCancelReadyNotification
            );
        serCx2PioTransmitConfig.EvtSerCx2PioTransmitDrainFifo =
            PL011SerCx2EvtPioTransmitDrainFifo;
        serCx2PioTransmitConfig.EvtSerCx2PioTransmitCancelDrainFifo =
            PL011SerCx2EvtPioTransmitCancelDrainFifo;
        serCx2PioTransmitConfig.EvtSerCx2PioTransmitPurgeFifo =
            PL011SerCx2EvtPioTransmitPurgeFifo;

        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &attributes,
            PL011_SERCXPIOTRANSMIT_CONTEXT
            );

        //
        // Create the SerCx2 TX object
        //
        status = SerCx2PioTransmitCreate(
            WdfDevice,
            &serCx2PioTransmitConfig,
            &attributes,
            &devExtPtr->SerCx2PioTransmit
            );
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "SerCx2PioTransmitCreate failed, (status = %!STATUS!)", status
                );
            return status;
        }

        //
        // Initialize the SerCx2 TX object context
        //
        status = PL011TxPioTransmitInit(WdfDevice, devExtPtr->SerCx2PioTransmit);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "PL011RxPioReceiveInit failed, (status = %!STATUS!)", status
                );
            return status;
        }

    } // Configure transmit PIO

    return status;
}


//
// Routine Description:
//
//  PL011pDeviceMapResources() is called by PL011EvtDevicePrepareHardware() to
//  parse the PL011 hardware resources. The routine verifies the resource
//  list and saves the resource information in the device context.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  ResourcesTranslated - Hardware translated resource list.
//
// Return Value:
//
//  STATUS_SUCCESS - Valid resources were parsed and mapped, or
//  STATUS_ACPI_INVALID_DATA - Missing resources or invalid resources
//      were parsed.
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceParseResources(
    WDFDEVICE WdfDevice,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();

    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);
    PPL011_RESOURCE_DATA resourceDataPtr = &devExtPtr->PL011ResourceData;
    ULONG numResourses = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG numMemResourcesFound = 0;
    ULONG numIntResourcesFound = 0;
    ULONG numDmaResourcesFound = 0;
    ULONG numSerialConnectionResourcesFound = 0;
    ULONG numFunctionConfigResourcesFound = 0;

    for (ULONG resInx = 0; resInx < numResourses; ++resInx) {

        PCM_PARTIAL_RESOURCE_DESCRIPTOR resDescPtr =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, resInx);

        switch (resDescPtr->Type) {

        case CmResourceTypeMemory:
            ++numMemResourcesFound;
            PL011_ASSERT(numMemResourcesFound == 1);

            if (resDescPtr->u.Memory.Length == ULONG(PL011_REG_FILE::REG_FILE_SIZE)) {
                //
                // Make sure the debugger is not configured to the
                // same port as this PL011 device.
                //
                if (PL011IsDebuggerPresent()) {

                    PHYSICAL_ADDRESS kdComPA =
                        MmGetPhysicalAddress(*KdComPortInUse);

                    if (kdComPA.QuadPart ==
                        resDescPtr->u.Memory.Start.QuadPart) {

                        PL011_LOG_ERROR("Kernel debugger is in use!");

                        #if !IS_DONT_CHANGE_HW
                            //
                            // We cannot fail the driver loading, thus we mark it,
                            // and let the device to load. We will block any open request.
                            //
                            devExtPtr->IsDebuggerConflict = TRUE;
                        #endif //!IS_DONT_CHANGE_HW

                    } // Debugger resource conflict

                } // Make sure debugger is not using the port

                resourceDataPtr->RegsResInx = resInx;
                resourceDataPtr->RegsPA = resDescPtr->u.Memory.Start;
                resourceDataPtr->RegsSpan = resDescPtr->u.Memory.Length;

            } else {

                PL011_LOG_ERROR(
                    "Invalid PL011 register file span (%lu)!",
                    resDescPtr->u.Memory.Length
                    );
                return STATUS_ACPI_INVALID_DATA;
            }
            break;

        case CmResourceTypeInterrupt:
            ++numIntResourcesFound;
            PL011_ASSERT(numIntResourcesFound == 1);

            resourceDataPtr->IntResInx = resInx;
            resourceDataPtr->IntVector = resDescPtr->u.Interrupt.Vector;
            resourceDataPtr->IntLevel = resDescPtr->u.Interrupt.Level;
            resourceDataPtr->Intffinity = resDescPtr->u.Interrupt.Affinity;
            break;

        case CmResourceTypeDma:
            ++numDmaResourcesFound;
            PL011_ASSERT(numDmaResourcesFound <= 2);

            //
            // To be implemented...
            //

            break;

        case CmResourceTypeConnection:
            switch (resDescPtr->u.Connection.Class) {

            case CM_RESOURCE_CONNECTION_CLASS_SERIAL:
                switch (resDescPtr->u.Connection.Type) {

                case CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART:
                    ++numSerialConnectionResourcesFound;
                    PL011_ASSERT(numSerialConnectionResourcesFound == 1);

                    resourceDataPtr->ConnectionId.LowPart =
                        resDescPtr->u.Connection.IdLowPart;
                    resourceDataPtr->ConnectionId.HighPart =
                        resDescPtr->u.Connection.IdHighPart;

                    PL011_ASSERT(resourceDataPtr->ConnectionId.QuadPart != 0LL);
                    break; // CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART

                } // switch (resDescPtr->u.Connection.Type)
                break; // CM_RESOURCE_CONNECTION_CLASS_SERIAL

            case CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG:
                switch (resDescPtr->u.Connection.Type) {

                case CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG:
                    ++numFunctionConfigResourcesFound;
                    PL011_ASSERT(numFunctionConfigResourcesFound == 1);

                    resourceDataPtr->FunctionConfigConnectionId.LowPart =
                        resDescPtr->u.Connection.IdLowPart;
                    resourceDataPtr->FunctionConfigConnectionId.HighPart =
                        resDescPtr->u.Connection.IdHighPart;

                    PL011_ASSERT(resourceDataPtr->FunctionConfigConnectionId.QuadPart != 0LL);
                    break; // CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG

                } // switch (resDescPtr->u.Connection.Type)
                break; // CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG

            } // switch (resDescPtr->u.Connection.Class)
            break;

        default:
            PL011_ASSERT(FALSE);
            break;

        } // switch

    } // for (resource list)

    //
    // Make sure we got everything we need...
    //
    if (numMemResourcesFound != 1) {

        PL011_LOG_ERROR("Invalid or no memory resource!");
        return STATUS_ACPI_INVALID_DATA;
    }

    if (numIntResourcesFound != 1) {

        PL011_LOG_ERROR("Invalid or not interrupt resource!");
        return STATUS_ACPI_INVALID_DATA;
    }

    //
    // (DMA is optional)
    //
    if ((numDmaResourcesFound != 2) && (numDmaResourcesFound != 0)) {

        PL011_LOG_ERROR(
            "Invalid number of DMA channels found (%lu)!",
            numDmaResourcesFound
            );
        return STATUS_ACPI_INVALID_DATA;
    }

    return STATUS_SUCCESS;
}


//
// Routine Description:
//
//  PL011pDeviceMapResources() is called by PL011EvtDevicePrepareHardware() to
//  map the PL011 hardware resources. The routine maps the required resources.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  ResourcesRaw - Hardware resource list.
//
//  ResourcesTranslated - Hardware translated resource list.
//
// Return Value:
//
//  STATUS_SUCCESS - Valid resources were parsed and mapped, or
//  STATUS_ACPI_INVALID_DATA - Missing resources or invalid resources
//      were parsed.
//  STATUS_INSUFFICIENT_RESOURCES - Failed to map PL011 register file into
//      virtual memory
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceMapResources(
    WDFDEVICE WdfDevice,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();

    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PL011_DEVICE_EXTENSION* devExtPtr = PL011DeviceGetExtension(WdfDevice);
    PPL011_RESOURCE_DATA resourceDataPtr = &devExtPtr->PL011ResourceData;

    PL011_ASSERT(devExtPtr->PL011RegsPtr == nullptr);

    //
    // Map device registers into virtual memory...
    //
    devExtPtr->PL011RegsPtr = static_cast<volatile ULONG*>(MmMapIoSpaceEx(
        resourceDataPtr->RegsPA,
        resourceDataPtr->RegsSpan,
        PAGE_READWRITE | PAGE_NOCACHE
        ));
    if (devExtPtr->PL011RegsPtr == nullptr) {

        PL011_LOG_ERROR(
            "Failed to map PL011 regs, span %lu bytes!",
            resourceDataPtr->RegsSpan
            );
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PL011_LOG_TRACE(
        "PL011 regs: PA 0x%08X, span %lu bytes, VA %p",
        resourceDataPtr->RegsPA.LowPart,
        resourceDataPtr->RegsSpan,
        PVOID(devExtPtr->PL011RegsPtr)
        );

    //
    // Setup interrupt handling
    //
    {
        //
        // Create a spin lock for the interrupt object
        //
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = WdfDevice;

        WDFSPINLOCK interruptSpinLock;
        status = WdfSpinLockCreate(&attributes, &interruptSpinLock);
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "WdfSpinLockCreate failed for interrupt lock, "
                "(status = %!STATUS!)",
                status
                );
            return status;
        }

        //
        // Configure and create the interrupt object
        //
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resRawDescPtr =
            WdfCmResourceListGetDescriptor(
                ResourcesRaw,
                resourceDataPtr->IntResInx
                );
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resTranslatedDescPtr =
            WdfCmResourceListGetDescriptor(
                ResourcesTranslated,
                resourceDataPtr->IntResInx
                );
        WDF_INTERRUPT_CONFIG wdfInterruptConfig;
        WDF_INTERRUPT_CONFIG_INIT(
            &wdfInterruptConfig,
            PL011EvtInterruptIsr,
            PL011EvtInterruptDpc
            );
        wdfInterruptConfig.SpinLock = interruptSpinLock;
        wdfInterruptConfig.InterruptRaw = resRawDescPtr;
        wdfInterruptConfig.InterruptTranslated = resTranslatedDescPtr;

        status = WdfInterruptCreate(
            WdfDevice,
            &wdfInterruptConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &devExtPtr->WdfUartInterrupt
            );
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "WdfInterruptCreate failed , (status = %!STATUS!)", status
                );
            return status;
        }

    } // Setup interrupt handling

    return  STATUS_SUCCESS;
}


//
// Routine Description:
//
//  Creates a device interface, using the supplied resource hub connection ID
//  as a reference string. When SerCx2 receives a FileCreate request, it
//  looks for the connection ID in the filename, and queries the resource
//  hub to determine initial connection settings.
//
//  The _DDN ACPI method is queried to determine the device friendly name
//  as seen by WinRT. If the _DDN method is not present, no friendly name
//  will be assigned.
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
//  ConnectionId - A valid resource hub Connection Id.
//
// Return Value:
//
//  STATUS_SUCCESS - On success
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceCreateDeviceInterface(
    WDFDEVICE WdfDevice,
    LARGE_INTEGER ConnectionId
    )
{
    PAGED_CODE();

    NT_ASSERT(ConnectionId.QuadPart != 0LL);

    DECLARE_UNICODE_STRING_SIZE(
        referenceString,
        RESOURCE_HUB_CONNECTION_FILE_SIZE
        );
    NTSTATUS status = RESOURCE_HUB_ID_TO_FILE_NAME(
            ConnectionId.LowPart,
            ConnectionId.HighPart,
            referenceString.Buffer
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "RESOURCE_HUB_ID_TO_FILE_NAME() failed. (status = %!STATUS!)",
            status
            );
        return status;
    }
    referenceString.Length = RESOURCE_HUB_CONNECTION_FILE_SIZE - sizeof(WCHAR);

    status = WdfDeviceCreateDeviceInterface(
            WdfDevice,
            &GUID_DEVINTERFACE_COMPORT,
            &referenceString
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfDeviceCreateDeviceInterface failed. (status = %!STATUS!, GUID_DEVINTERFACE_COMPORT = %!GUID!, referenceString = %wZ)",
            status,
            &GUID_DEVINTERFACE_COMPORT,
            &referenceString
            );
        return status;
    }

    struct _SYMLINK_NAME {
        WDFSTRING WdfString = WDF_NO_HANDLE;
        ~_SYMLINK_NAME ()
        {
            if (this->WdfString != WDF_NO_HANDLE)
                WdfObjectDelete(this->WdfString);
        }
    } symlinkName;
    status = WdfStringCreate(
            nullptr,
            WDF_NO_OBJECT_ATTRIBUTES,
            &symlinkName.WdfString
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfStringCreate failed. (status = %!STATUS!)",
            status
            );
        return status;
    }

    status = WdfDeviceRetrieveDeviceInterfaceString(
            WdfDevice,
            &GUID_DEVINTERFACE_COMPORT,
            &referenceString,
            symlinkName.WdfString
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ASSERTION(
            "WdfDeviceRetrieveDeviceInterfaceString() failed. (status = %!STATUS!, GUID_DEVINTERFACE_COMPORT = %!GUID!)",
            status,
            &GUID_DEVINTERFACE_COMPORT
            );
        return status;
    }

    UNICODE_STRING symlinkNameWsz;
    WdfStringGetUnicodeString(symlinkName.WdfString, &symlinkNameWsz);

    //
    // IsRestricted interface property works in conjunction with the
    // RestrictedOverrideForSystemContainerAllowed flag set on the interface
    // class to allow access to internal devices (belonging to system
    // container) through the device broker. Setting it to FALSE is required
    // to explicitly grant access.
    //

    DEVPROP_BOOLEAN isRestricted = DEVPROP_FALSE;
    status = IoSetDeviceInterfacePropertyData(
            &symlinkNameWsz,
            &DEVPKEY_DeviceInterface_Restricted,
            0,
            0, // Flags
            DEVPROP_TYPE_BOOLEAN,
            sizeof(isRestricted),
            &isRestricted
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "IoSetDeviceInterfacePropertyData(...DEVPKEY_DeviceInterface_Restricted...) failed. (status = %!STATUS!, symlinkNameWsz = %wZ)",
            status,
            &symlinkNameWsz
            );
        return status;
    }

    //
    // Get DosDeviceName from registry
    //
    struct _PARAMETERS_KEY {
        WDFKEY WdfKey = WDF_NO_HANDLE;
        ~_PARAMETERS_KEY ()
        {
            if (this->WdfKey != WDF_NO_HANDLE)
                WdfRegistryClose(this->WdfKey);
        }
    } parametersKey;
    status = WdfDeviceOpenRegistryKey(
            WdfDevice,
            PLUGPLAY_REGKEY_DEVICE,
            KEY_QUERY_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES,
            &parametersKey.WdfKey
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "Failed to open device parameters registry key. (status = %!STATUS!)",
            status
            );
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(dosDeviceNameRegvalStr, L"DosDeviceName");
    DECLARE_UNICODE_STRING_SIZE(portName, 64);
    status = WdfRegistryQueryUnicodeString(
            parametersKey.WdfKey,
            &dosDeviceNameRegvalStr,
            nullptr, // ValueByteLength
            &portName
            );
    if (NT_SUCCESS(status) &&
       ((portName.Length + sizeof(WCHAR)) < portName.MaximumLength)) {

        // Null-terminate PortName
        portName.Buffer[portName.Length / sizeof(WCHAR)] = UNICODE_NULL;

        // Set the port friendly name
        status = IoSetDeviceInterfacePropertyData(
                &symlinkNameWsz,
                &DEVPKEY_DeviceInterface_Serial_PortName,
                LOCALE_NEUTRAL,
                0, // Flags
                DEVPROP_TYPE_STRING,
                portName.Length + sizeof(WCHAR),
                portName.Buffer
                );
        if (!NT_SUCCESS(status)) {

            PL011_LOG_ERROR(
                "IoSetDeviceInterfacePropertyData(...DEVPKEY_DeviceInterface_Serial_PortName...) failed. (status = %!STATUS!, symlinkNameWsz = %wZ, portName = %wZ)",
                status,
                &symlinkNameWsz,
                &portName
                );
            return status;
        }

        PL011_LOG_INFORMATION(
            "Successfully assigned PortName to device interface. (symlinkNameWsz = %wZ, portName = %wZ)",
            &symlinkNameWsz,
            &portName
            );

    } else {
        PL011_LOG_WARNING(
            "Failed to query DosDeviceName from registry. Skipping assignment of PortName. (status = %!STATUS!, dosDeviceNameRegvalStr = %wZ, portName.Length = %d, portName.MaximumLength = %d)",
            status,
            &dosDeviceNameRegvalStr,
            portName.Length,
            portName.MaximumLength
            );
    }

    PL011_LOG_INFORMATION(
        "Successfully created device interface. (symlinkNameWsz = %wZ)",
        &symlinkNameWsz
        );

    return STATUS_SUCCESS;
}


_Use_decl_annotations_
NTSTATUS
PL011pDeviceReserveFunctionConfigResource(
    PL011_DEVICE_EXTENSION* DevExtPtr
    )
{
    PAGED_CODE();

    NT_ASSERT(
        (DevExtPtr->FunctionConfigHandle == WDF_NO_HANDLE) &&
        (DevExtPtr->PL011ResourceData.FunctionConfigConnectionId.QuadPart != 0LL));

    DECLARE_UNICODE_STRING_SIZE(devicePath, RESOURCE_HUB_PATH_CHARS);
    NTSTATUS status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
            &devicePath,
            DevExtPtr->PL011ResourceData.FunctionConfigConnectionId.LowPart,
            DevExtPtr->PL011ResourceData.FunctionConfigConnectionId.HighPart
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "RESOURCE_HUB_CREATE_PATH_FROM_ID failed. (status = %!STATUS!)",
            status
            );
        return status;
    }

    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
    wdfObjectAttributes.ParentObject = DevExtPtr->WdfDevice;

    status = WdfIoTargetCreate(
            DevExtPtr->WdfDevice,
            &wdfObjectAttributes,
            &DevExtPtr->FunctionConfigHandle
            );
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfIoTargetCreate() failed. (status = %!STATUS!)",
            status
            );
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &devicePath,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE
        );

    status = WdfIoTargetOpen(DevExtPtr->FunctionConfigHandle, &openParams);
    if (!NT_SUCCESS(status)) {

        PL011_LOG_ERROR(
            "WdfIoTargetOpen failed. (status = %!STATUS!, devicePath = %wZ)",
            status,
            &devicePath
            );
        return status;
    }

    return STATUS_SUCCESS;
}


//
// Routine Description:
//
//  PL011pDeviceGetSupportedFeatures() is called by PL011pDeviceExtensionInit() to
//  get the HW capabilities supported by the board.
//  The PL011 supports HW flow control, and manual control, but some boards like
//  Rpi2 do not expose the control lines.
//  The device decides if to expose the interface to control lines and HW control
//  based on the configuration that is in the INF file for the specific board.
//  The routine translates the UART_SERIAL_??? bit fields to PL011 UARTCR_??? .
//
// Arguments:
//
//  WdfDevice - The WdfDevice object the represent the PL011 this instance of
//      the PL011 controller.
//
// Return Value:
//
//  STATUS_SUCCESS, or STATUS_NOT_SUPPORTED if board does not support the
//  features mask that was set in INF file.
//
_Use_decl_annotations_
NTSTATUS
PL011pDeviceGetSupportedFeatures(
    WDFDEVICE WdfDevice
    )
{
    PAGED_CODE();

    const PL011_DRIVER_EXTENSION* drvExtPtr =
        PL011DriverGetExtension(WdfGetDriver());
    PL011_DEVICE_EXTENSION* devExtPtr =
        PL011DeviceGetExtension(WdfDevice);

    USHORT uartFlowControlParams =
        drvExtPtr->UartFlowControl.Value & UART_SERIAL_FLAG_FLOW_CTL_MASK;

    switch (uartFlowControlParams) {

    case UART_SERIAL_FLAG_FLOW_CTL_NONE:
    {
        if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_RTS) != 0) {

            devExtPtr->UartSupportedControlsMask |= UARTCR_RTS;
        }

        if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_DTR) != 0) {

            devExtPtr->UartSupportedControlsMask |= UARTCR_DTR;
        }
        break;

    } // UART_SERIAL_FLAG_FLOW_CTL_NONE

    case UART_SERIAL_FLAG_FLOW_CTL_XONXOFF:
    {
        PL011_LOG_ERROR(
            "Software flow control is not implemented, (status = %!STATUS!)",
            STATUS_NOT_IMPLEMENTED
            );
        return STATUS_NOT_IMPLEMENTED;

    } // UART_SERIAL_FLAG_FLOW_CTL_XONXOFF

    case UART_SERIAL_FLAG_FLOW_CTL_HW:
    {
        if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_RTS) != 0) {

            devExtPtr->UartSupportedControlsMask |= UARTCR_RTSEn;
        }

        if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_CTS) != 0) {

            devExtPtr->UartSupportedControlsMask |= UARTCR_CTSEn;
        }
        break;

    } // UART_SERIAL_FLAG_FLOW_CTL_HW

    default:
        PL011_LOG_ERROR(
            "Unsupported flow control setup parameter 0x%04X,"
            "(status = %!STATUS!)",
            uartFlowControlParams,
            STATUS_NOT_SUPPORTED
            );
        return STATUS_NOT_SUPPORTED;

    } // switch (uartFlowControlParams)

    //
    // Are IO pins OUT1,OUT2 exposed?
    //
    if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_OUT1) != 0) {

        devExtPtr->UartSupportedControlsMask |= UARTCR_Out1;
    }
    if ((drvExtPtr->UartControlLines.Value & UART_SERIAL_LINES_OUT2) != 0) {

        devExtPtr->UartSupportedControlsMask |= UARTCR_Out2;
    }

    return STATUS_SUCCESS;
}

#undef _PL011_DEVICE_CPP_