//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//    serialp.h
//
// Abstract:
//
//    Prototypes and macros that are used throughout RPi3 mini Uart driver.
//

typedef
VOID
(*PSERIAL_START_ROUTINE) (_In_ PSERIAL_DEVICE_EXTENSION);

typedef
VOID
(*PSERIAL_GET_NEXT_ROUTINE) (
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ WDFQUEUE QueueToProcess,
    _Out_ WDFREQUEST* NewRequest,
    _In_ BOOLEAN CompleteCurrent,
    _In_ PSERIAL_DEVICE_EXTENSION Extension);

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD SerialEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP SerialEvtDriverContextCleanup;
EVT_WDF_DEVICE_CONTEXT_CLEANUP SerialEvtDeviceContextCleanup;
    
EVT_WDF_DEVICE_D0_ENTRY SerialEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT SerialEvtDeviceD0Exit;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED SerialEvtDeviceD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED SerialEvtDeviceD0ExitPreInterruptsDisabled;
EVT_WDF_DEVICE_PREPARE_HARDWARE SerialEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SerialEvtReleaseHardware;

EVT_WDF_DEVICE_FILE_CREATE SerialEvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE SerialEvtFileClose;

EVT_WDF_IO_QUEUE_IO_READ SerialEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE SerialEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SerialEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL SerialEvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE SerialEvtCanceledOnQueue;
EVT_WDF_IO_QUEUE_IO_STOP SerialEvtIoStop;
EVT_WDF_IO_QUEUE_IO_RESUME SerialEvtIoResume;

EVT_WDF_INTERRUPT_ENABLE SerialEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE SerialEvtInterruptDisable;

EVT_WDF_DPC SerialCompleteRead;
EVT_WDF_DPC SerialCompleteWrite;
EVT_WDF_DPC SerialCommError;
EVT_WDF_DPC SerialCompleteImmediate;
EVT_WDF_DPC SerialCompleteXoff;
EVT_WDF_DPC SerialCompleteWait;
EVT_WDF_DPC SerialStartTimerLowerRTS;

EVT_WDF_TIMER SerialReadTimeout;
EVT_WDF_TIMER SerialIntervalReadTimeout;
EVT_WDF_TIMER SerialWriteTimeout;
EVT_WDF_TIMER SerialTimeoutImmediate;
EVT_WDF_TIMER SerialTimeoutXoff;
EVT_WDF_TIMER SerialInvokePerhapsLowerRTS;

_Check_return_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS SerialSetPortNameDevInterfProp(_In_ WDFDEVICE Device, _Inout_ PCWSTR SerPortName);

VOID
SerialStartRead(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialStartWrite(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialStartMask(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialStartImmediate(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialStartPurge(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialGetNextWrite(
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ WDFQUEUE QueueToProcess,
    _In_ WDFREQUEST* NewRequest,
    _In_ BOOLEAN CompleteCurrent,
    _In_ PSERIAL_DEVICE_EXTENSION Extension);

EVT_WDFDEVICE_WDM_IRP_PREPROCESS SerialWdmDeviceFileCreate;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS SerialWdmFileClose;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS SerialFlush;
    
EVT_WDFDEVICE_WDM_IRP_PREPROCESS SerialQueryInformationFile;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS SerialSetInformationFile;

NTSTATUS
SerialDeviceFileCreateWorker (_In_ WDFDEVICE Device);

VOID
SerialFileCloseWorker(_In_ WDFDEVICE Device);

NTSTATUS
SerialReserveFunctionConfig(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN IsCommit);

EVT_WDF_INTERRUPT_SYNCHRONIZE SerialProcessEmptyTransmit;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetDTR;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialClrDTR;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetRTS;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialClrRTS;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetBaud;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetLineControl;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetHandFlow;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialTurnOnBreak;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialTurnOffBreak;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialPretendXoff;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialPretendXon;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialReset;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialPerhapsLowerRTS;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialMarkOpen;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialMarkClose;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialGetStats;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialClearStats;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetChars;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetMCRContents;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialGetMCRContents;
EVT_WDF_INTERRUPT_SYNCHRONIZE SerialSetFCRContents;

BOOLEAN
SerialSetupNewHandFlow(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ PSERIAL_HANDFLOW NewHandFlow);


VOID
SerialHandleReducedIntBuffer(_In_ PSERIAL_DEVICE_EXTENSION Extension);

_IRQL_requires_min_(DISPATCH_LEVEL)
VOID
SerialProdXonXoff(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ BOOLEAN SendXon);

EVT_WDF_REQUEST_CANCEL SerialCancelWait;

EVT_WDF_INTERRUPT_SYNCHRONIZE SerialPurgeInterruptBuff;
    
VOID
SerialPurgeRequests(
    _In_ WDFQUEUE QueueToClean,
    _In_ WDFREQUEST* CurrentOpRequest);

VOID
SerialFlushRequests(
    _In_ WDFQUEUE QueueToClean,
    _In_ WDFREQUEST* CurrentOpRequest);

VOID
SerialGetNextRequest(
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ WDFQUEUE QueueToProcess,
    _Out_ WDFREQUEST* NextIrp,
    _In_ BOOLEAN CompleteCurrent,
    _In_ PSERIAL_DEVICE_EXTENSION extension);


VOID
SerialTryToCompleteCurrent(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ PFN_WDF_INTERRUPT_SYNCHRONIZE SynchRoutine OPTIONAL,
    _In_ NTSTATUS StatusToUse,
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ WDFQUEUE QueueToProcess,
    _In_ WDFTIMER IntervalTimer,
    _In_ WDFTIMER TotalTimer,
    _In_ PSERIAL_START_ROUTINE Starter,
    _In_ PSERIAL_GET_NEXT_ROUTINE GetNextIrp,
    _In_ LONG RefType);

VOID
SerialStartOrQueue(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ WDFREQUEST Request,
    _In_ WDFQUEUE QueueToExamine,
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ PSERIAL_START_ROUTINE Starter);

NTSTATUS
SerialCompleteIfError(
    _In_ PSERIAL_DEVICE_EXTENSION extension,
    _Inout_ WDFREQUEST Request);

_IRQL_requires_min_(DISPATCH_LEVEL)
ULONG
SerialHandleModemUpdate(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ BOOLEAN DoingTX);

EVT_WDF_INTERRUPT_ISR SerialISR;

NTSTATUS
SerialGetDivisorFromBaud(
    _In_ ULONG ClockRate,
    _In_ LONG DesiredBaud,
    _Out_ PSHORT AppropriateDivisor);

VOID
SerialCleanupDevice(_In_ PSERIAL_DEVICE_EXTENSION Extension);

UCHAR
SerialProcessLSR(_In_ PSERIAL_DEVICE_EXTENSION Extension);

LARGE_INTEGER
SerialGetCharTime(_In_ PSERIAL_DEVICE_EXTENSION Extension);


VOID
SerialPutChar(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ UCHAR CharToPut);

NTSTATUS
SerialGetConfigDefaults(
    _In_ PSERIAL_FIRMWARE_DATA DriverDefaultsPtr,
    _In_ WDFDRIVER Driver);

VOID
SerialGetProperties(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ PSERIAL_COMMPROP Properties);

NTSTATUS
SerialMapHWResources(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResListRaw,
    _In_ WDFCMRESLIST ResListTran,
    _Out_ PCONFIG_DATA Config);

VOID
SerialUnmapHWResources(_In_ PSERIAL_DEVICE_EXTENSION DevExt);

BOOLEAN
SerialGetRegistryKeyValue (
    _In_  WDFDEVICE WdfDevice,
    _In_ PCWSTR Name,
    _Out_ PULONG Value);


BOOLEAN
SerialPutRegistryKeyValue (
    _In_ WDFDEVICE WdfDevice,
    _In_ PCWSTR Name,
    _In_ ULONG Value);

NTSTATUS
SerialInitController(
    _In_ PSERIAL_DEVICE_EXTENSION DevExt,
    _In_ PCONFIG_DATA ConfigData);

BOOLEAN PrintMiniUartregs(_In_ PSERIAL_DEVICE_EXTENSION DevExt);

NTSTATUS
SerialDoExternalNaming(_In_ PSERIAL_DEVICE_EXTENSION PDevExt);

PVOID
SerialGetMappedAddress(
    _In_ PHYSICAL_ADDRESS IoAddress,
    _In_ ULONG NumberOfBytes,
    _In_ ULONG AddressSpace,
    _Out_ PBOOLEAN MappedAddress);

PVOID LocalMmMapIoSpace(
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ SIZE_T NumberOfBytes);

_IRQL_requires_same_
BOOLEAN
SerialDoesPortExist(
    _In_ PSERIAL_DEVICE_EXTENSION Extension,
    _In_ ULONG ForceFifo);

VOID
SerialUndoExternalNaming(_In_ PSERIAL_DEVICE_EXTENSION Extension);

VOID
SerialPurgePendingRequests(_In_ PSERIAL_DEVICE_EXTENSION PDevExt);

VOID
SerialDisableUART(_In_ PVOID Context);

VOID
SerialDrainUART(
    _In_ PSERIAL_DEVICE_EXTENSION PDevExt,
    _In_ PLARGE_INTEGER PDrainTime);

VOID
SerialSaveDeviceState(_In_ PSERIAL_DEVICE_EXTENSION PDevExt);

NTSTATUS
SerialSetPowerPolicy(_In_ PSERIAL_DEVICE_EXTENSION DeviceExtension);

UINT32
SerialReportMaxBaudRate(_In_ ULONG Bauds);

BOOLEAN
SerialInsertQueueDpc(_In_ WDFDPC Dpc);

BOOLEAN
SerialSetTimer(
    _In_ WDFTIMER Timer,
    _In_ LARGE_INTEGER DueTime);

BOOLEAN
SerialCancelTimer(
    _In_ WDFTIMER Timer,
    _In_ PSERIAL_DEVICE_EXTENSION PDevExt);

VOID
SerialMarkHardwareBroken(
    _In_ PSERIAL_DEVICE_EXTENSION PDevExt
    );

VOID
SetDeviceIsOpened(
    _In_ PSERIAL_DEVICE_EXTENSION PDevExt,
    _In_ BOOLEAN DeviceIsOpened,
    _In_ BOOLEAN Reopen);

BOOLEAN
IsQueueEmpty(_In_ WDFQUEUE Queue);

NTSTATUS
SerialCreateTimersAndDpcs(_In_ PSERIAL_DEVICE_EXTENSION DevExt);

VOID
SerialDrainTimersAndDpcs(_In_ PSERIAL_DEVICE_EXTENSION DevExt);

VOID
SerialSetCancelRoutine(
    _In_ WDFREQUEST Request,
    _In_ PFN_WDF_REQUEST_CANCEL CancelRoutine);

NTSTATUS
SerialClearCancelRoutine(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN ClearReference);

NTSTATUS
SerialWmiRegistration(_In_ WDFDEVICE Device);

NTSTATUS
SerialReadSymName(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(*SizeOfRegName) PWSTR RegName,
    _Inout_ PUSHORT SizeOfRegName);

VOID
SerialCompleteRequest(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Info);

BOOLEAN
SerialGetFdoRegistryKeyValue(
    _In_ PWDFDEVICE_INIT  DeviceInit,
    _In_ PCWSTR Name,
    _Out_ PULONG Value);

VOID
SerialSetInterruptPolicy(_In_ WDFINTERRUPT WdfInterrupt);

typedef struct _SERIAL_UPDATE_CHAR {
    PSERIAL_DEVICE_EXTENSION Extension;
    ULONG CharsCopied;
    BOOLEAN Completed;
    } SERIAL_UPDATE_CHAR,*PSERIAL_UPDATE_CHAR;

// The following simple structure is used to send a pointer
// the device extension and an ioctl specific pointer
// to data.

typedef struct _SERIAL_IOCTL_SYNC {
    PSERIAL_DEVICE_EXTENSION Extension;
    PVOID Data;
    } SERIAL_IOCTL_SYNC,*PSERIAL_IOCTL_SYNC;

// The following three macros are used to initialize, set
// and clear references in IRPs that are used by
// this driver.  The reference is stored in the fourth
// argument of the request, which is never used by any operation
// accepted by this driver.

#define SERIAL_REF_ISR         (0x00000001)
#define SERIAL_REF_CANCEL      (0x00000002)
#define SERIAL_REF_TOTAL_TIMER (0x00000004)
#define SERIAL_REF_INT_TIMER   (0x00000008)
#define SERIAL_REF_XOFF_REF    (0x00000010)

#define SERIAL_INIT_REFERENCE(ReqContext) { \
    (ReqContext)->RefCount = NULL; \
    }

#define SERIAL_SET_REFERENCE(ReqContext, RefType) \
   do { \
       LONG _refType = (RefType); \
       PULONG_PTR _arg4 = (PVOID)&(ReqContext)->RefCount; \
       ASSERT(!(*_arg4 & _refType)); \
       *_arg4 |= _refType; \
   } while (0)

#define SERIAL_CLEAR_REFERENCE(ReqContext, RefType) \
   do { \
       LONG _refType = (RefType); \
       PULONG_PTR _arg4 = (PVOID)&(ReqContext)->RefCount; \
       ASSERT(*_arg4 & _refType); \
       *_arg4 &= ~_refType; \
   } while (0)

#define SERIAL_REFERENCE_COUNT(ReqContext) \
    ((ULONG_PTR)(((ReqContext)->RefCount)))

#define SERIAL_TEST_REFERENCE(ReqContext, RefType) ((ULONG_PTR)ReqContext ->RefCount & RefType)

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS 
QuerySystemClockFrequency(_Out_ ULONG* ClockFrequencyPtr);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS 
OpenDevice(
    const UNICODE_STRING* FileNamePtr,
    ACCESS_MASK DesiredAccess,
    ULONG ShareAccess,
    _Outptr_ FILE_OBJECT** FileObjectPPtr);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS 
SendIoctlSynchronously(
    FILE_OBJECT* FileObjectPtr,
    ULONG IoControlCode,
    _In_reads_bytes_(InputBufferLength) PVOID InputBufferPtr,
    ULONG InputBufferLength,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBufferPtr,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    _Out_ ULONG_PTR* InformationPtr);