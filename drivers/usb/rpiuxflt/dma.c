#include "rpiuxflt.h"

#define FILTER_ADAPTER_POOL_TAG (ULONG)'afuR'
#define SCATTER_GATHER_LIST_MIN_SIZE (sizeof(SCATTER_GATHER_LIST) + sizeof(SCATTER_GATHER_ELEMENT))
#define FILTER_ADAPTER_MAX_PAGES 128
#define FILTER_SCATTER_GATHER_MAX_SIZE (FILTER_ADAPTER_MAX_PAGES * PAGE_SIZE)
#define FILTER_MAX_DMA_PHYSICAL_ADDRESS 0xbfffffff

typedef struct _FILTER_DMA_ADAPTER
{
    DMA_ADAPTER Adapter;
    PDMA_ADAPTER AttachedAdapter;
    KEVENT BounceBufferAvailableEvent;
    PVOID BounceBufferVa;
    PMDL Mdl;
    SCATTER_GATHER_LIST ScatterGather;
} FILTER_DMA_ADAPTER, *PFILTER_DMA_ADAPTER;

#define FILTER_DMA_ADAPTER_SIZE (sizeof(FILTER_DMA_ADAPTER) + sizeof(SCATTER_GATHER_ELEMENT))

static
PVOID
Dma_AllocateCommonBuffer(
    IN PDMA_ADAPTER DmaAdapter,
    IN ULONG Length,
    OUT PPHYSICAL_ADDRESS LogicalAddress,
    IN BOOLEAN CacheEnabled
    )
{
    PFILTER_DMA_ADAPTER pFilterAdapter = (PFILTER_DMA_ADAPTER)DmaAdapter;
    PHYSICAL_ADDRESS maximumAddress = { FILTER_MAX_DMA_PHYSICAL_ADDRESS };
    PVOID virtualAddress;

    UNREFERENCED_PARAMETER(CacheEnabled);

    virtualAddress = pFilterAdapter->AttachedAdapter->DmaOperations->AllocateCommonBufferEx(
        pFilterAdapter->AttachedAdapter,
        &maximumAddress,
        Length,
        LogicalAddress,
        FALSE,
        0);

    return virtualAddress;
}

static
VOID
Dma_FreeCommonBuffer(
    IN PDMA_ADAPTER DmaAdapter,
    IN ULONG Length,
    IN PHYSICAL_ADDRESS LogicalAddress,
    IN PVOID VirtualAddress,
    IN BOOLEAN CacheEnabled
    )
{
    PFILTER_DMA_ADAPTER pFilterAdapter = (PFILTER_DMA_ADAPTER)DmaAdapter;

    pFilterAdapter->AttachedAdapter->DmaOperations->FreeCommonBuffer(
        pFilterAdapter->AttachedAdapter,
        Length,
        LogicalAddress,
        VirtualAddress,
        CacheEnabled);
}

static
ULONG
Dma_GetDmaAlignment(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);

    return 1;
}

static
NTSTATUS
Dma_CalculateScatterGatherList(
    IN PDMA_ADAPTER DmaAdapter,
    IN OPTIONAL PMDL Mdl,
    IN PVOID CurrentVa,
    IN ULONG Length,
    OUT PULONG ScatterGatherListSize,
    OUT OPTIONAL PULONG pNumberOfMapRegisters
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(CurrentVa);

    *ScatterGatherListSize = SCATTER_GATHER_LIST_MIN_SIZE;
    *pNumberOfMapRegisters = Length / PAGE_SIZE;

    return STATUS_SUCCESS;
}

static
NTSTATUS
Dma_GetDmaTransferInfo(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN ULONGLONG Offset,
    IN ULONG Length,
    IN BOOLEAN WriteOnly,
    IN OUT PDMA_TRANSFER_INFO TransferInfo
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(WriteOnly);

    if (NULL == TransferInfo) {
        return STATUS_INVALID_PARAMETER;
    }

    if (1 != TransferInfo->Version) {
        return STATUS_NOT_SUPPORTED;
    }

    TransferInfo->V1.MapRegisterCount = (Length + PAGE_SIZE-1) / PAGE_SIZE;
    TransferInfo->V1.ScatterGatherElementCount = 1;
    TransferInfo->V1.ScatterGatherListSize = SCATTER_GATHER_LIST_MIN_SIZE;

    return STATUS_SUCCESS;
}

static
NTSTATUS
Dma_GetScatterGatherList(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PMDL Mdl,
    IN PVOID CurrentVa,
    IN ULONG Length,
    IN PDRIVER_LIST_CONTROL ExecutionRoutine,
    IN PVOID Context,
    IN BOOLEAN WriteToDevice
    )
{
    NTSTATUS status;
    LARGE_INTEGER timeout = {0};
    PFILTER_DMA_ADAPTER pFilterAdapter = (PFILTER_DMA_ADAPTER)DmaAdapter;
    PVOID systemCurrentVa;

    UNREFERENCED_PARAMETER(CurrentVa);

    if (NULL == Mdl) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NULL != Mdl->Next) {
        return STATUS_NOT_SUPPORTED;
    }

    if (Length > FILTER_SCATTER_GATHER_MAX_SIZE) {
        return STATUS_BUFFER_OVERFLOW;
    }

    status = KeWaitForSingleObject(
        &pFilterAdapter->BounceBufferAvailableEvent,
        Executive,
        KernelMode,
        FALSE,
        &timeout);

    if (STATUS_SUCCESS != status) {
        return STATUS_UNSUCCESSFUL;
    }

    if (WriteToDevice) {
        systemCurrentVa = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        RtlCopyMemory(pFilterAdapter->BounceBufferVa, systemCurrentVa, Length);
    }

    pFilterAdapter->Mdl = Mdl;
    pFilterAdapter->ScatterGather.Elements[0].Length = Length;

    ExecutionRoutine(
        DeviceObject,
        NULL,
        &pFilterAdapter->ScatterGather,
        Context);

    return STATUS_SUCCESS;
}

static
VOID
Dma_PutScatterGatherList(
    IN PDMA_ADAPTER DmaAdapter,
    IN PSCATTER_GATHER_LIST ScatterGather,
    IN BOOLEAN WriteToDevice
    )
{
    PFILTER_DMA_ADAPTER pFilterAdapter = (PFILTER_DMA_ADAPTER)DmaAdapter;
    PVOID systemCurrentVa;

    UNREFERENCED_PARAMETER(ScatterGather);

    ASSERT(&pFilterAdapter->ScatterGather == ScatterGather);

    if (!WriteToDevice) {
        systemCurrentVa = MmGetSystemAddressForMdlSafe(pFilterAdapter->Mdl, NormalPagePriority);
        RtlCopyMemory(systemCurrentVa, pFilterAdapter->BounceBufferVa,
                      pFilterAdapter->ScatterGather.Elements[0].Length);
    }

    KeSetEvent(
        &pFilterAdapter->BounceBufferAvailableEvent,
        IO_NO_INCREMENT,
        FALSE);
}

static
VOID
Dma_PutDmaAdapter(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    PFILTER_DMA_ADAPTER pFilterAdapter = (PFILTER_DMA_ADAPTER)DmaAdapter;

    if (NULL != pFilterAdapter->BounceBufferVa) {
        Dma_FreeCommonBuffer(
            DmaAdapter,
            FILTER_SCATTER_GATHER_MAX_SIZE,
            pFilterAdapter->ScatterGather.Elements[0].Address,
            pFilterAdapter->BounceBufferVa,
            FALSE);
    }

    if (NULL != pFilterAdapter->AttachedAdapter) {
        pFilterAdapter->AttachedAdapter->DmaOperations->PutDmaAdapter(
            pFilterAdapter->AttachedAdapter);
    }

    ExFreePoolWithTag(pFilterAdapter, FILTER_ADAPTER_POOL_TAG);
}

static
NTSTATUS
Dma_AllocateAdapterChannel(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG NumberOfMapRegisters,
    IN PDRIVER_CONTROL ExecutionRoutine,
    IN PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(NumberOfMapRegisters);
    UNREFERENCED_PARAMETER(ExecutionRoutine);
    UNREFERENCED_PARAMETER(Context);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
BOOLEAN
Dma_FlushAdapterBuffers(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN PVOID MapRegisterBase,
    IN PVOID CurrentVa,
    IN ULONG Length,
    IN BOOLEAN WriteToDevice
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(CurrentVa);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(WriteToDevice);

    ASSERT(FALSE);

    return FALSE;
}

static
VOID
Dma_FreeAdapterChannel(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);

    ASSERT(FALSE);
}

static
VOID
Dma_FreeMapRegisters(
    IN PDMA_ADAPTER DmaAdapter,
    PVOID MapRegisterBase,
    ULONG NumberOfMapRegisters
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(NumberOfMapRegisters);

    ASSERT(FALSE);
}

static
PHYSICAL_ADDRESS
Dma_MapTransfer(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN PVOID MapRegisterBase,
    IN PVOID CurrentVa,
    IN OUT PULONG Length,
    IN BOOLEAN WriteToDevice
    )
{
    PHYSICAL_ADDRESS result = {0};

    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(CurrentVa);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(WriteToDevice);

    ASSERT(FALSE);

    return result;
}

static
ULONG
Dma_ReadDmaCounter(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);

    return 0;
}

static
NTSTATUS
Dma_BuildScatterGatherList(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PMDL Mdl,
    IN PVOID CurrentVa,
    IN ULONG Length,
    IN PDRIVER_LIST_CONTROL ExecutionRoutine,
    IN PVOID Context,
    IN BOOLEAN WriteToDevice,
    IN PVOID ScatterGatherBuffer,
    IN ULONG ScatterGatherLength
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(CurrentVa);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(ExecutionRoutine);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(WriteToDevice);
    UNREFERENCED_PARAMETER(ScatterGatherBuffer);
    UNREFERENCED_PARAMETER(ScatterGatherLength);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_BuildMdlFromScatterGatherList(
    IN PDMA_ADAPTER DmaAdapter,
    IN PSCATTER_GATHER_LIST ScatterGather,
    IN PMDL OriginalMdl,
    OUT PMDL *TargetMdl
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(ScatterGather);
    UNREFERENCED_PARAMETER(OriginalMdl);
    UNREFERENCED_PARAMETER(TargetMdl);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_GetDmaAdapterInfo(
    IN PDMA_ADAPTER DmaAdapter,
    IN OUT PDMA_ADAPTER_INFO AdapterInfo
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(AdapterInfo);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_InitializeDmaTransferContext(
    IN PDMA_ADAPTER DmaAdapter,
    OUT PVOID DmaTransferContext
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DmaTransferContext);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
PVOID
Dma_AllocateCommonBufferEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN OPTIONAL PPHYSICAL_ADDRESS MaximumAddress,
    IN ULONG Length,
    OUT PPHYSICAL_ADDRESS LogicalAddress,
    IN BOOLEAN CacheEnabled,
    IN NODE_REQUIREMENT PreferredNode
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(MaximumAddress);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(LogicalAddress);
    UNREFERENCED_PARAMETER(CacheEnabled);
    UNREFERENCED_PARAMETER(PreferredNode);

    ASSERT(FALSE);

    return NULL;
}

static
NTSTATUS
Dma_AllocateAdapterChannelEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID DmaTransferContext,
    IN ULONG NumberOfMapRegisters,
    IN ULONG Flags,
    IN OPTIONAL PDRIVER_CONTROL ExecutionRoutine,
    IN OPTIONAL PVOID ExecutionContext,
    OUT OPTIONAL PVOID *MapRegisterBase
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DmaTransferContext);
    UNREFERENCED_PARAMETER(NumberOfMapRegisters);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ExecutionRoutine);
    UNREFERENCED_PARAMETER(ExecutionContext);
    UNREFERENCED_PARAMETER(MapRegisterBase);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_ConfigureAdapterChannel(
    IN PDMA_ADAPTER DmaAdapter,
    IN ULONG FunctionNumber,
    IN PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(FunctionNumber);
    UNREFERENCED_PARAMETER(Context);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
BOOLEAN
Dma_CancelAdapterChannel(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID DmaTransferContext
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DmaTransferContext);

    ASSERT(FALSE);

    return FALSE;
}

static
NTSTATUS
Dma_MapTransferEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN PVOID MapRegisterBase,
    IN ULONGLONG Offset,
    IN ULONG DeviceOffset,
    IN OUT PULONG Length,
    IN BOOLEAN WriteToDevice,
    OUT PSCATTER_GATHER_LIST ScatterGatherBuffer,
    IN ULONG ScatterGatherBufferLength,
    IN OPTIONAL PDMA_COMPLETION_ROUTINE DmaCompletionRoutine,
    IN OPTIONAL PVOID CompletionContext
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(DeviceOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(WriteToDevice);
    UNREFERENCED_PARAMETER(ScatterGatherBuffer);
    UNREFERENCED_PARAMETER(ScatterGatherBufferLength);
    UNREFERENCED_PARAMETER(DmaCompletionRoutine);
    UNREFERENCED_PARAMETER(CompletionContext);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_GetScatterGatherListEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID DmaTransferContext,
    IN PMDL Mdl,
    IN ULONGLONG Offset,
    IN ULONG Length,
    IN ULONG Flags,
    IN OPTIONAL PDRIVER_LIST_CONTROL ExecutionRoutine,
    IN OPTIONAL PVOID Context,
    IN BOOLEAN WriteToDevice,
    IN OPTIONAL PDMA_COMPLETION_ROUTINE DmaCompletionRoutine,
    IN OPTIONAL PVOID CompletionContext,
    OUT OPTIONAL PSCATTER_GATHER_LIST *ScatterGatherList
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DmaTransferContext);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ExecutionRoutine);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(WriteToDevice);
    UNREFERENCED_PARAMETER(DmaCompletionRoutine);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(ScatterGatherList);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_BuildScatterGatherListEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID DmaTransferContext,
    IN PMDL Mdl,
    IN ULONGLONG Offset,
    IN ULONG Length,
    IN ULONG Flags,
    IN OPTIONAL PDRIVER_LIST_CONTROL ExecutionRoutine,
    IN OPTIONAL PVOID Context,
    IN BOOLEAN WriteToDevice,
    IN PVOID ScatterGatherBuffer,
    IN ULONG ScatterGatherLength,
    IN OPTIONAL PDMA_COMPLETION_ROUTINE DmaCompletionRoutine,
    IN OPTIONAL PVOID CompletionContext,
    OUT OPTIONAL PVOID ScatterGatherList
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(DmaTransferContext);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ExecutionRoutine);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(WriteToDevice);
    UNREFERENCED_PARAMETER(ScatterGatherBuffer);
    UNREFERENCED_PARAMETER(ScatterGatherLength);
    UNREFERENCED_PARAMETER(DmaCompletionRoutine);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(ScatterGatherList);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_FlushAdapterBuffersEx(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN PVOID MapRegisterBase,
    IN ULONGLONG Offset,
    IN ULONG Length,
    IN BOOLEAN WriteToDevice
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(MapRegisterBase);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(WriteToDevice);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
Dma_FreeAdapterObject(
    IN PDMA_ADAPTER DmaAdapter,
    IN IO_ALLOCATION_ACTION AllocationAction
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(AllocationAction);

    ASSERT(FALSE);
}

static
NTSTATUS
Dma_CancelMappedTransfer(
    IN PDMA_ADAPTER DmaAdapter,
    IN PVOID DmaTransferContext
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DmaTransferContext);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_AllocateDomainCommonBuffer(
    IN PDMA_ADAPTER DmaAdapter,
    IN HANDLE DomainHandle,
    IN OPTIONAL PPHYSICAL_ADDRESS MaximumAddress,
    IN ULONG Length,
    IN ULONG Flags,
    IN OPTIONAL MEMORY_CACHING_TYPE *CacheType,
    IN NODE_REQUIREMENT PreferredNode,
    OUT PPHYSICAL_ADDRESS LogicalAddress,
    OUT PVOID *VirtualAddress
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DomainHandle);
    UNREFERENCED_PARAMETER(MaximumAddress);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(CacheType);
    UNREFERENCED_PARAMETER(PreferredNode);
    UNREFERENCED_PARAMETER(LogicalAddress);
    UNREFERENCED_PARAMETER(VirtualAddress);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_FlushDmaBuffer(
    IN PDMA_ADAPTER DmaAdapter,
    IN PMDL Mdl,
    IN BOOLEAN ReadOperation
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Mdl);
    UNREFERENCED_PARAMETER(ReadOperation);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_JoinDmaDomain(
    IN PDMA_ADAPTER DmaAdapter,
    IN HANDLE DomainHandle
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(DomainHandle);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
Dma_LeaveDmaDomain(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
HANDLE
Dma_GetDmaDomain(
    IN PDMA_ADAPTER DmaAdapter
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);

    ASSERT(FALSE);

    return NULL;
}

static
PVOID
Dma_AllocateCommonBufferWithBounds(
    IN PDMA_ADAPTER DmaAdapter,
    IN OPTIONAL PPHYSICAL_ADDRESS MinimumAddress,
    IN OPTIONAL PPHYSICAL_ADDRESS MaximumAddress,
    IN ULONG Length,
    IN ULONG Flags,
    IN OPTIONAL MEMORY_CACHING_TYPE *CacheType,
    IN NODE_REQUIREMENT PreferredNode,
    OUT PPHYSICAL_ADDRESS LogicalAddress
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(MaximumAddress);
    UNREFERENCED_PARAMETER(MinimumAddress);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(CacheType);
    UNREFERENCED_PARAMETER(PreferredNode);
    UNREFERENCED_PARAMETER(LogicalAddress);

    ASSERT(FALSE);

    return NULL;
}

static
NTSTATUS
Dma_AllocateCommonBufferVector(
    IN PDMA_ADAPTER DmaAdapter,
    IN PHYSICAL_ADDRESS LowAddress,
    IN PHYSICAL_ADDRESS HighAddress,
    IN MEMORY_CACHING_TYPE CacheType,
    IN ULONG IdealNode,
    IN ULONG Flags,
    IN ULONG NumberOfElements,
    IN ULONGLONG SizeOfElements,
    OUT PDMA_COMMON_BUFFER_VECTOR *VectorOut
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(LowAddress);
    UNREFERENCED_PARAMETER(HighAddress);
    UNREFERENCED_PARAMETER(CacheType);
    UNREFERENCED_PARAMETER(IdealNode);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(NumberOfElements);
    UNREFERENCED_PARAMETER(SizeOfElements);
    UNREFERENCED_PARAMETER(VectorOut);

    ASSERT(FALSE);

    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
Dma_GetCommonBufferFromVectorByIndex(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDMA_COMMON_BUFFER_VECTOR Vector,
    IN ULONG Index,
    OUT PVOID *VirtualAddressOut,
    OUT PPHYSICAL_ADDRESS LogicalAddressOut
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Vector);
    UNREFERENCED_PARAMETER(Index);
    UNREFERENCED_PARAMETER(VirtualAddressOut);
    UNREFERENCED_PARAMETER(LogicalAddressOut);

    ASSERT(FALSE);
}

static
VOID
Dma_FreeCommonBufferFromVector(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDMA_COMMON_BUFFER_VECTOR Vector,
    IN ULONG Index
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Vector);
    UNREFERENCED_PARAMETER(Index);

    ASSERT(FALSE);
}

static
VOID
Dma_FreeCommonBufferVector(
    IN PDMA_ADAPTER DmaAdapter,
    IN PDMA_COMMON_BUFFER_VECTOR Vector
    )
{
    UNREFERENCED_PARAMETER(DmaAdapter);
    UNREFERENCED_PARAMETER(Vector);

    ASSERT(FALSE);
}

static
DMA_OPERATIONS
Dma_DmaOperations = {
    .Size = sizeof(DMA_OPERATIONS),
    .PutDmaAdapter = Dma_PutDmaAdapter,
    .AllocateCommonBuffer = Dma_AllocateCommonBuffer,
    .FreeCommonBuffer = Dma_FreeCommonBuffer,
    .AllocateAdapterChannel = Dma_AllocateAdapterChannel,
    .FlushAdapterBuffers = Dma_FlushAdapterBuffers,
    .FreeAdapterChannel = Dma_FreeAdapterChannel,
    .FreeMapRegisters = Dma_FreeMapRegisters,
    .MapTransfer = Dma_MapTransfer,
    .GetDmaAlignment = Dma_GetDmaAlignment,
    .ReadDmaCounter = Dma_ReadDmaCounter,
    .GetScatterGatherList = Dma_GetScatterGatherList,
    .PutScatterGatherList = Dma_PutScatterGatherList,
    .CalculateScatterGatherList = Dma_CalculateScatterGatherList,
    .BuildScatterGatherList = Dma_BuildScatterGatherList,
    .BuildMdlFromScatterGatherList = Dma_BuildMdlFromScatterGatherList,
    .GetDmaAdapterInfo = Dma_GetDmaAdapterInfo,
    .GetDmaTransferInfo = Dma_GetDmaTransferInfo,
    .InitializeDmaTransferContext = Dma_InitializeDmaTransferContext,
    .AllocateCommonBufferEx = Dma_AllocateCommonBufferEx,
    .AllocateAdapterChannelEx = Dma_AllocateAdapterChannelEx,
    .ConfigureAdapterChannel = Dma_ConfigureAdapterChannel,
    .CancelAdapterChannel = Dma_CancelAdapterChannel,
    .MapTransferEx = Dma_MapTransferEx,
    .GetScatterGatherListEx = Dma_GetScatterGatherListEx,
    .BuildScatterGatherListEx = Dma_BuildScatterGatherListEx,
    .FlushAdapterBuffersEx = Dma_FlushAdapterBuffersEx,
    .FreeAdapterObject = Dma_FreeAdapterObject,
    .CancelMappedTransfer = Dma_CancelMappedTransfer,
    .AllocateDomainCommonBuffer = Dma_AllocateDomainCommonBuffer,
    .FlushDmaBuffer = Dma_FlushDmaBuffer,
    .JoinDmaDomain = Dma_JoinDmaDomain,
    .LeaveDmaDomain = Dma_LeaveDmaDomain,
    .GetDmaDomain = Dma_GetDmaDomain,
    .AllocateCommonBufferWithBounds = Dma_AllocateCommonBufferWithBounds,
    .AllocateCommonBufferVector = Dma_AllocateCommonBufferVector,
    .GetCommonBufferFromVectorByIndex = Dma_GetCommonBufferFromVectorByIndex,
    .FreeCommonBufferFromVector = Dma_FreeCommonBufferFromVector,
    .FreeCommonBufferVector = Dma_FreeCommonBufferVector
};

PDMA_ADAPTER
Dma_CreateDmaAdapter(
    IN PFILTER_DEVICE_DATA DeviceData,
    IN PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG NumberOfMapRegisters
    )
{
    PFILTER_DMA_ADAPTER pFilterAdapter;
    PDMA_ADAPTER pAttachedAdapter;
    PHYSICAL_ADDRESS bounceBufferLa = {0};
    PVOID bounceBufferVa;

    pFilterAdapter = ExAllocatePoolWithTag(NonPagedPoolNx,
                                           FILTER_DMA_ADAPTER_SIZE,
                                           FILTER_ADAPTER_POOL_TAG);

    if (NULL == pFilterAdapter) {
        return NULL;
    }

    pAttachedAdapter = DeviceData->AttachedBusInterface.GetDmaAdapter(
        DeviceData->AttachedBusInterface.Context,
        DeviceDescriptor,
        NumberOfMapRegisters);

    if (NULL == pAttachedAdapter) {
        Dma_PutDmaAdapter((PDMA_ADAPTER)pFilterAdapter);
        return NULL;
    }

    *NumberOfMapRegisters = FILTER_ADAPTER_MAX_PAGES + 2;

    pFilterAdapter->Adapter.Version = 1;
    pFilterAdapter->Adapter.Size = FILTER_DMA_ADAPTER_SIZE;
    pFilterAdapter->Adapter.DmaOperations = &Dma_DmaOperations;
    pFilterAdapter->AttachedAdapter = pAttachedAdapter;

    bounceBufferVa = Dma_AllocateCommonBuffer(
        (PDMA_ADAPTER)pFilterAdapter,
        FILTER_SCATTER_GATHER_MAX_SIZE,
        &bounceBufferLa,
        FALSE);

    if (NULL == bounceBufferVa) {
        Dma_PutDmaAdapter((PDMA_ADAPTER)pFilterAdapter);
        return NULL;
    }

    KeInitializeEvent(
        &pFilterAdapter->BounceBufferAvailableEvent,
        SynchronizationEvent,
        TRUE);

    pFilterAdapter->BounceBufferVa = bounceBufferVa;
    pFilterAdapter->ScatterGather.NumberOfElements = 1;
    pFilterAdapter->ScatterGather.Elements[0].Address = bounceBufferLa;

    return (PDMA_ADAPTER)pFilterAdapter;
}
