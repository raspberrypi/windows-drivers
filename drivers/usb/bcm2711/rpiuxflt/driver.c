#include "rpiuxflt.h"

#define BUS_INTERFACE_STANDARD_VERSION 1

DRIVER_INITIALIZE DriverEntry;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

static
VOID
Driver_Unload(
    IN PDRIVER_OBJECT Driver
    )
{
    UNREFERENCED_PARAMETER(Driver);

    return;
}

static
NTSTATUS
Driver_AddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject;
    PFILTER_DEVICE_DATA pDeviceData;

    status = IoCreateDevice(DriverObject, sizeof(FILTER_DEVICE_DATA), NULL,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &deviceObject);

    if (NT_SUCCESS(status)) {
        pDeviceData = (PFILTER_DEVICE_DATA)deviceObject->DeviceExtension;
        RtlFillMemory(pDeviceData, sizeof(FILTER_DEVICE_DATA), 0);

        pDeviceData->Self = deviceObject;
        pDeviceData->TopOfStack = IoAttachDeviceToDeviceStack(
            deviceObject, PhysicalDeviceObject);

        if (!pDeviceData->TopOfStack) {
            IoDeleteDevice(deviceObject);
            return STATUS_UNSUCCESSFUL;
        }

        if (pDeviceData->TopOfStack->Flags & DO_BUFFERED_IO) {
            deviceObject->Flags |= DO_BUFFERED_IO;
        } else if (pDeviceData->TopOfStack->Flags & DO_DIRECT_IO) {
            deviceObject->Flags |= DO_DIRECT_IO;
        }

        deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    }

    return status;
}

static
NTSTATUS
Driver_DispatchPassthrough(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PFILTER_DEVICE_DATA pDeviceData;

    pDeviceData = (PFILTER_DEVICE_DATA)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    status = IoCallDriver(pDeviceData->TopOfStack, Irp);

    return status;
}

static
NTSTATUS
Driver_QueryInterfaceBusCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    PFILTER_DEVICE_DATA pDeviceData;
    PIO_STACK_LOCATION irpStack;
    PBUS_INTERFACE_STANDARD pBusInterface;

    UNREFERENCED_PARAMETER(Context);

    if (TRUE == Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

    pDeviceData = (PFILTER_DEVICE_DATA)DeviceObject->DeviceExtension;
    irpStack = IoGetCurrentIrpStackLocation(Irp);
    pBusInterface = (PBUS_INTERFACE_STANDARD)irpStack->Parameters.QueryInterface.Interface;

    Bus_WrapBusInterface(pDeviceData, pBusInterface);

    return STATUS_CONTINUE_COMPLETION;
}

static
NTSTATUS
Driver_DispatchPnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PFILTER_DEVICE_DATA pDeviceData;
    PIO_STACK_LOCATION irpStack;

    pDeviceData = (PFILTER_DEVICE_DATA)DeviceObject->DeviceExtension;
    irpStack = IoGetCurrentIrpStackLocation(Irp);

    switch (irpStack->MinorFunction) {
    case IRP_MN_QUERY_INTERFACE:
        if (RtlCompareMemory(
                irpStack->Parameters.QueryInterface.InterfaceType,
                &GUID_BUS_INTERFACE_STANDARD,
                sizeof(GUID))
                == sizeof(GUID)
            && irpStack->Parameters.QueryInterface.Size == sizeof(BUS_INTERFACE_STANDARD)
            && irpStack->Parameters.QueryInterface.Version == BUS_INTERFACE_STANDARD_VERSION) {
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp,
                                   Driver_QueryInterfaceBusCompletion,
                                   NULL,
                                   TRUE,
                                   TRUE,
                                   TRUE);
        } else {
            IoSkipCurrentIrpStackLocation(Irp);
        }

        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        break;
    }

    status = IoCallDriver(pDeviceData->TopOfStack, Irp);

    return status;   
}

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = Driver_Unload;
    DriverObject->DriverExtension->AddDevice = Driver_AddDevice;

    for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = Driver_DispatchPassthrough;
    }

    DriverObject->MajorFunction[IRP_MJ_PNP] = Driver_DispatchPnP;

    return STATUS_SUCCESS;
}
