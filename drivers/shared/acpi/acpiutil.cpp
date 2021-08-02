// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Module Name:
//
//   acpiutil.cpp
//
// Abstract:
//
//  This module contains the ACPI utility functions declaration. Used to access
//  a PDO ACPI stored information, traverse its objects, verify, execute its
//  method and parse its returned content
//
// Environment:
//
//  Kernel mode only
//

#include <Ntddk.h>

extern "C" {
    #include <acpiioct.h>
    #include <initguid.h>
} // extern "C"

#include <acpiutil.hpp>

#define NONPAGED_SEGMENT_BEGIN \
    __pragma(code_seg(push)) \
    //__pragma(code_seg(.text))

#define NONPAGED_SEGMENT_END \
    __pragma(code_seg(pop))

#define ASSERT_MAX_IRQL(IRQL) NT_ASSERT(KeGetCurrentIrql() <= (IRQL))

template<class T>
__forceinline
T Min(
    _In_ const T& v1,
    _In_ const T& v2
    ) 
{
    if (v1 <= v2) {
        return v1;
    } else {
        return v2;
    }
}

template<class T>
__forceinline
T Max(
    _In_ const T& v1,
    _In_ const T& v2
) 
{
    if (v1 >= v2) {
        return v1;
    } else {
        return v2;
    }
}

//
// Internal Parsing and Evaluation
//

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
AcpiFormatDsmFunctionNoParamsInputBuffer(
    _In_ const GUID* GuidPtr,
    _In_ UINT32 RevisionId,
    _In_ UINT32 FunctionIdx,
    _Outptr_result_bytebuffer_(*InputBufferSizePtr) ACPI_EVAL_INPUT_BUFFER_COMPLEX** InputBufferPptr,
    _Out_ UINT32* InputBufferSizePtr);

//
// Internal Misc
//

_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
AcpiSendIoctlSynchronously(
    _In_ DEVICE_OBJECT* PdoPtr,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_(InputBufferSize) ACPI_EVAL_INPUT_BUFFER* InputBufferPtr,
    _In_ ULONG InputBufferSize,
    _Out_writes_bytes_to_(OutputBufferSize, *BytesReturnedCountPtr) ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* OutputBufferPtr,
    _In_ ULONG OutputBufferSize,
    _Out_opt_ ULONG* BytesReturnedCountPtr);

NONPAGED_SEGMENT_BEGIN //=================================================

_Use_decl_annotations_
NTSTATUS
AcpiQueryDsd(
    DEVICE_OBJECT* PdoPtr,
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED** DsdBufferPptr)
{
    if (!ARGUMENT_PRESENT(PdoPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(DsdBufferPptr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    NTSTATUS status;
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* dsdBufferPtr;
    ACPI_EVAL_INPUT_BUFFER inputBuffer;

    RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));
    inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    inputBuffer.MethodNameAsUlong = ACPI_METHOD_ULONG_DSD;

    status =
        AcpiEvaluateMethod(
            PdoPtr,
            &inputBuffer,
            sizeof(ACPI_EVAL_INPUT_BUFFER),
            &dsdBufferPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *DsdBufferPptr = dsdBufferPtr;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiDevicePropertiesQueryValue(
    const ACPI_METHOD_ARGUMENT UNALIGNED* DevicePropertiesPkgPtr,
    const CHAR* KeyNamePtr,
    const ACPI_METHOD_ARGUMENT UNALIGNED** ValuePtr
)
{
    if (!ARGUMENT_PRESENT(DevicePropertiesPkgPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(KeyNamePtr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    if (!ARGUMENT_PRESENT(ValuePtr)) {
        return STATUS_INVALID_PARAMETER_3;
    }

    if (DevicePropertiesPkgPtr->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    NTSTATUS status;
    const ACPI_METHOD_ARGUMENT UNALIGNED* currentListEntryPtr = nullptr;
    ANSI_STRING keyNameStr;

    status = RtlInitAnsiStringEx(&keyNameStr, KeyNamePtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Example Device Properties
    //
    // Package ()
    // {
    //   Package (2) { "Key1", Value1 },
    //   Package (2) { "Key2", Value2 },
    //   Package (2) { "Key3", Value3 }
    // }
    //
    for (;;) {

        status =
            AcpiPackageGetNextArgument(
                DevicePropertiesPkgPtr,
                &currentListEntryPtr);
        if (!NT_SUCCESS(status)) {
            //
            // Enumeration ended and no key match found
            //
            NT_ASSERT(status == STATUS_NO_MORE_ENTRIES);
            return STATUS_NOT_FOUND;
        }

        //
        // Each element in the Device Properties package is a non-empty package
        // key/value pair
        //
        if (currentListEntryPtr->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
            return STATUS_ACPI_INVALID_ARGTYPE;
        }

        const ACPI_METHOD_ARGUMENT UNALIGNED* currentPairEntryPtr = nullptr;

        status =
            AcpiPackageGetNextArgument(
                currentListEntryPtr,
                &currentPairEntryPtr);
        if (status == STATUS_NO_MORE_ENTRIES) {
            return STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;
        }
        NT_ASSERT(NT_SUCCESS(status));

        //
        // Key should be string
        //
        if (currentPairEntryPtr->Type != ACPI_METHOD_ARGUMENT_STRING) {
            return STATUS_ACPI_INVALID_DATA;
        }

        ANSI_STRING currentKeyNameStr;

        RtlInitAnsiStringEx(
            &currentKeyNameStr,
            reinterpret_cast<PCSZ>(currentPairEntryPtr->Data));
        if (!NT_SUCCESS(status)) {
            return status;
        }

        if (!RtlEqualString(&currentKeyNameStr, &keyNameStr, FALSE)) {
            continue;
        }

        status =
            AcpiPackageGetNextArgument(
                currentListEntryPtr,
                &currentPairEntryPtr);
        if (status == STATUS_NO_MORE_ENTRIES) {
            return STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;
        }
        NT_ASSERT(NT_SUCCESS(status));

        *ValuePtr = currentPairEntryPtr;
        break;
    }

    return STATUS_SUCCESS;
}

template<>
_Use_decl_annotations_
NTSTATUS
AcpiDevicePropertiesQueryIntegerValue<UINT32>(
    const ACPI_METHOD_ARGUMENT UNALIGNED* DevicePropertiesPkgPtr,
    const CHAR* KeyNamePtr,
    UINT32* ValuePtr
    )
{
    if (!ARGUMENT_PRESENT(DevicePropertiesPkgPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(KeyNamePtr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    if (!ARGUMENT_PRESENT(ValuePtr)) {
        return STATUS_INVALID_PARAMETER_3;
    }

    if (DevicePropertiesPkgPtr->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    NTSTATUS status;
    const ACPI_METHOD_ARGUMENT UNALIGNED* currentPairEntryPtr = nullptr;

    status = AcpiDevicePropertiesQueryValue(DevicePropertiesPkgPtr, KeyNamePtr, &currentPairEntryPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AcpiArgumentParseInteger(currentPairEntryPtr, ValuePtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiDevicePropertiesQueryStringValue(
    const ACPI_METHOD_ARGUMENT UNALIGNED* DevicePropertiesPkgPtr,
    const CHAR* KeyNamePtr,
    UINT32 MaxLength,
    UINT32* OutLength,
    PCHAR ValuePtr
    )
{
    if (!ARGUMENT_PRESENT(DevicePropertiesPkgPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(KeyNamePtr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    if (!ARGUMENT_PRESENT(OutLength)) {
        return STATUS_INVALID_PARAMETER_4;
    }

    if (!ARGUMENT_PRESENT(ValuePtr)) {
        return STATUS_INVALID_PARAMETER_5;
    }

    if (DevicePropertiesPkgPtr->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    NTSTATUS status;
    const ACPI_METHOD_ARGUMENT UNALIGNED* currentPairEntryPtr = nullptr;

    status = AcpiDevicePropertiesQueryValue(DevicePropertiesPkgPtr, KeyNamePtr, &currentPairEntryPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AcpiArgumentParseString(currentPairEntryPtr, MaxLength, OutLength, ValuePtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiSendIoctlSynchronously(
    DEVICE_OBJECT* PdoPtr,
    ULONG IoControlCode,
    ACPI_EVAL_INPUT_BUFFER* InputBufferPtr,
    ULONG InputBufferSize,
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* OutputBufferPtr,
    ULONG OutputBufferSize,
    ULONG* BytesReturnedCountPtr
    )
{
    ASSERT_MAX_IRQL(APC_LEVEL);

    NT_ASSERT(ARGUMENT_PRESENT(PdoPtr));
    NT_ASSERT(IoControlCode != 0);
    NT_ASSERT(ARGUMENT_PRESENT(InputBufferPtr));
    NT_ASSERTMSG("ACPI can't accept an empty input buffer", InputBufferSize > 0);
    NT_ASSERTMSG("Only IOCTLs with Buffered IO",
              METHOD_FROM_CTL_CODE(IoControlCode) == METHOD_BUFFERED);
    NT_ASSERT(ARGUMENT_PRESENT(OutputBufferPtr));

    NTSTATUS status;
    VOID* intermediateBufferPtr = nullptr;

    IRP* irpPtr = IoAllocateIrp(PdoPtr->StackSize, FALSE);
    if (irpPtr == nullptr) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    intermediateBufferPtr = 
        ExAllocatePoolWithTag(
            NonPagedPoolNx,
            Max(InputBufferSize, OutputBufferSize),
            ACPI_TAG_IOCTL_INTERMEDIATE_BUFFER);
    if (intermediateBufferPtr == nullptr) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlCopyMemory(intermediateBufferPtr, InputBufferPtr, InputBufferSize);

    irpPtr->AssociatedIrp.SystemBuffer = intermediateBufferPtr;
    irpPtr->Flags = 0;
    irpPtr->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irpPtr->IoStatus.Information = 0;
    irpPtr->UserBuffer = nullptr;

    IO_STACK_LOCATION* irpStackPtr = IoGetNextIrpStackLocation(irpPtr);
    NT_ASSERT(irpStackPtr != nullptr);
    irpStackPtr->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    irpStackPtr->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    irpStackPtr->Parameters.DeviceIoControl.InputBufferLength = InputBufferSize;
    irpStackPtr->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferSize;

    status = IoSynchronousCallDriver(PdoPtr, irpPtr);
    //
    // If the output buffer is not large enough to contain the output buffer
    // header, the IoSynchronousCallDriver will return STATUS_BUFFER_TOO_SMALL.
    // If the output buffer is large enough to contain the output buffer header,
    // but is not large enough to contain all the output arguments from the control
    // method, the IoSynchronousCallDriver will return STATUS_BUFFER_OVERFLOW,
    // and intermediateBufferPtr->Length is set to the required length of the
    // output buffer.
    //
    if (NT_SUCCESS(status) ||
        (status == STATUS_BUFFER_OVERFLOW) ||
        (status == STATUS_BUFFER_TOO_SMALL)) {
        NT_ASSERT(irpPtr->IoStatus.Information <= OutputBufferSize);
        RtlCopyMemory(
            OutputBufferPtr,
            intermediateBufferPtr,
            irpPtr->IoStatus.Information);
    }

    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    _Analysis_assume_(OutputBufferPtr->Length <= OutputBufferSize);
    if ((OutputBufferPtr->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) ||
        (OutputBufferPtr->Count == 0)) {
        status = STATUS_ACPI_INVALID_DATA;
        goto Cleanup;
    }

    if (ARGUMENT_PRESENT(BytesReturnedCountPtr)) {
        *BytesReturnedCountPtr = static_cast<ULONG>(irpPtr->IoStatus.Information);
    }

Cleanup:
    
    if (irpPtr != nullptr) {
        IoFreeIrp(irpPtr);
    }

    if (intermediateBufferPtr != nullptr) {
        ExFreePoolWithTag(
            intermediateBufferPtr,
            ACPI_TAG_IOCTL_INTERMEDIATE_BUFFER);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
AcpiParseDsdAsDeviceProperties(
    const ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* DsdBufferPtr,
    const ACPI_METHOD_ARGUMENT UNALIGNED** DevicePropertiesPkgPptr
    )
{
    if (!ARGUMENT_PRESENT(DsdBufferPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(DevicePropertiesPkgPptr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    //
    // A Device Properties _DSD should have a specific format as described by specs
    // see the comment at ACPI_DEVICE_PROPERTIES_DSD_GUID definition
    //

    //
    // _DSD should be a package of 2 elements, first is UUID and second is another
    // package of key/value pairs
    //
    if (DsdBufferPtr->Count != 2) {
        return STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;
    }

    NTSTATUS status;
    const ACPI_METHOD_ARGUMENT UNALIGNED* currentArgumentPtr = nullptr;

    status = AcpiOutputBufferGetNextArgument(DsdBufferPtr, &currentArgumentPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    GUID dsdGuid;

    //
    // First argument of the _DSD should be a UUID
    //
    status = AcpiArgumentParseGuid(currentArgumentPtr, &dsdGuid);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Verify that the UUID is for Device Properties per specs
    //
    if (!InlineIsEqualGUID(dsdGuid, ACPI_DEVICE_PROPERTIES_DSD_GUID)) {
        return STATUS_ACPI_INVALID_DATA;
    }

    //
    // Second argument should be a package containing a set of key/value entries
    //
    status = AcpiOutputBufferGetNextArgument(DsdBufferPtr, &currentArgumentPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (currentArgumentPtr->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    *DevicePropertiesPkgPptr = currentArgumentPtr;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiQueryDsm(
    DEVICE_OBJECT* PdoPtr,
    const GUID* GuidPtr,
    UINT32 RevisionId,
    UINT32* SupportedFunctionsMaskPtr)
{
    ASSERT_MAX_IRQL(APC_LEVEL);

    if (!ARGUMENT_PRESENT(PdoPtr)) {
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!ARGUMENT_PRESENT(GuidPtr)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    if (!ARGUMENT_PRESENT(SupportedFunctionsMaskPtr)) {
        return STATUS_INVALID_PARAMETER_4;
    }

    NTSTATUS status;
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* returnBufferPtr = nullptr;

    status =
        AcpiExecuteDsmFunctionNoParams(
            PdoPtr,
            GuidPtr,
            RevisionId,
            ACPI_DSM_FUNCTION_IDX_QUERY,
            &returnBufferPtr);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    if ((returnBufferPtr == nullptr) ||
        (returnBufferPtr->Length < sizeof(ACPI_EVAL_OUTPUT_BUFFER)) ||
        (returnBufferPtr->Count == 0) ||
        (returnBufferPtr->Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER))
    {
        status = STATUS_ACPI_INVALID_DATA;
        goto Cleanup;
    }

    switch (returnBufferPtr->Argument->DataLength) {
    case sizeof(UINT8):
        *SupportedFunctionsMaskPtr =
            *(reinterpret_cast<UINT8*>(returnBufferPtr->Argument->Data));
        break;
    case sizeof (UINT16):
        *SupportedFunctionsMaskPtr =
            *(reinterpret_cast<UINT16*>(returnBufferPtr->Argument->Data));
        break;
    case sizeof(UINT32):
        *SupportedFunctionsMaskPtr =
            *(reinterpret_cast<UINT32*>(returnBufferPtr->Argument->Data));
        break;
    default:
        NT_ASSERTMSG("Invalid _DSM query return size", FALSE);
        status = STATUS_ACPI_INVALID_DATA;
    }

Cleanup:

    if (returnBufferPtr != nullptr) {
        ExFreePoolWithTag(returnBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
AcpiExecuteDsmFunctionNoParams(
    DEVICE_OBJECT* PdoPtr,
    const GUID* GuidPtr,
    UINT32 RevisionId,
    UINT32 FunctionIdx,
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED** ReturnBufferPptr
    )
{
    ASSERT_MAX_IRQL(APC_LEVEL);

    NTSTATUS status;
    ACPI_EVAL_INPUT_BUFFER_COMPLEX* inputBufferPtr = nullptr;
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* outputBufferPtr = nullptr;

    if (!ARGUMENT_PRESENT(PdoPtr)) {
        status = STATUS_INVALID_PARAMETER_1;
        goto Cleanup;
    }

    if (!ARGUMENT_PRESENT(GuidPtr)) {
        status = STATUS_INVALID_PARAMETER_2;
        goto Cleanup;
    }

    UINT32 inputBufferSize;
    status = 
        AcpiFormatDsmFunctionNoParamsInputBuffer(
            GuidPtr,
            RevisionId,
            FunctionIdx,
            &inputBufferPtr,
            &inputBufferSize);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    status =
        AcpiEvaluateMethod(
            PdoPtr,
            reinterpret_cast<ACPI_EVAL_INPUT_BUFFER*>(inputBufferPtr),
            inputBufferSize,
            &outputBufferPtr);
    if (!NT_SUCCESS(status)) {
        goto Cleanup;
    }

    if (ARGUMENT_PRESENT(ReturnBufferPptr)) {
        *ReturnBufferPptr = outputBufferPtr;
    }

Cleanup:

    if (inputBufferPtr != nullptr) {
        ExFreePoolWithTag(inputBufferPtr, ACPI_TAG_EVAL_INPUT_BUFFER);
    }

    //
    // In case the optional parameter ReturnBufferPptr is not provided, then
    // output buffer ownership is ours and we have to free it before returning
    //
    if (!ARGUMENT_PRESENT(ReturnBufferPptr) && (outputBufferPtr != nullptr)) {
        ExFreePoolWithTag(outputBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
AcpiEvaluateMethod(
    DEVICE_OBJECT* PdoPtr,
    ACPI_EVAL_INPUT_BUFFER* InputBufferPtr,
    UINT32 InputBufferSize,
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED** ReturnBufferPptr
    )
{
    NT_ASSERT(ARGUMENT_PRESENT(PdoPtr));
    NT_ASSERT(ARGUMENT_PRESENT(InputBufferPtr));
    NT_ASSERT(ARGUMENT_PRESENT(ReturnBufferPptr));

    NTSTATUS status;
    UINT32 retries = 2;
    UINT32 outputBufferSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER);
    ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* outputBufferPtr;
    ULONG sizeReturned;

    //
    // Build and send the ACPI request
    //
    // First attempt gets the required output buffer size, second one allocates
    // and resends the IRP with big enough output buffer
    //

    do
    {
        outputBufferPtr =
            static_cast<ACPI_EVAL_OUTPUT_BUFFER*>(
                ExAllocatePoolWithTag(
                    NonPagedPoolNx,
                    outputBufferSize,
                    ACPI_TAG_EVAL_OUTPUT_BUFFER));
        if (outputBufferPtr == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        
        RtlZeroMemory(outputBufferPtr, outputBufferSize);

        status =
            AcpiSendIoctlSynchronously(
                PdoPtr,
                IOCTL_ACPI_EVAL_METHOD,
                InputBufferPtr,
                InputBufferSize,
                outputBufferPtr,
                outputBufferSize,
                &sizeReturned);
        if (status == STATUS_BUFFER_OVERFLOW) {
            outputBufferSize = outputBufferPtr->Length;
            ExFreePoolWithTag(outputBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
            outputBufferPtr = nullptr;
        }

        --retries;
    } while ((status == STATUS_BUFFER_OVERFLOW) && (retries > 0));

    //
    // If successful and data returned, return data to caller. If the method
    // returned no data, then set the return values to NULL
    //
    if (NT_SUCCESS(status)) {
        if (sizeReturned > 0) {

            NT_ASSERT(sizeReturned >=
                        (sizeof(ACPI_EVAL_OUTPUT_BUFFER) - sizeof(ACPI_METHOD_ARGUMENT)));

            NT_ASSERT(outputBufferPtr->Signature == ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE);
            NT_ASSERT(sizeReturned <= outputBufferSize);
            *ReturnBufferPptr = outputBufferPtr;

        } else {
            *ReturnBufferPptr = nullptr;
        }
    }

Cleanup:

    if (!NT_SUCCESS(status) && (outputBufferPtr != nullptr)) {
        ExFreePoolWithTag(outputBufferPtr, ACPI_TAG_EVAL_OUTPUT_BUFFER);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
AcpiFormatDsmFunctionNoParamsInputBuffer(
    const GUID* GuidPtr,
    UINT32 RevisionId,
    UINT32 FunctionIdx,
    ACPI_EVAL_INPUT_BUFFER_COMPLEX** InputBufferPptr,
    UINT32* InputBufferSizePtr
    )
{
    ASSERT_MAX_IRQL(APC_LEVEL);

    NT_ASSERT(ARGUMENT_PRESENT(GuidPtr));
    NT_ASSERT(ARGUMENT_PRESENT(InputBufferPptr));
    NT_ASSERT(ARGUMENT_PRESENT(InputBufferSizePtr));

    NTSTATUS status;
    ACPI_EVAL_INPUT_BUFFER_COMPLEX* inputBufferPtr;

    //
    // Device Specific Method (_DSM) takes 4 args:
    //  Arg0 : Buffer containing a UUID [16 bytes]
    //  Arg1 : Integer containing the Revision ID
    //  Arg2 : Integer containing the Function Index 
    //  Arg3 : Package that contains function-specific arguments
    //
    const UINT32 inputBufferSize =
        (sizeof(ACPI_EVAL_INPUT_BUFFER_COMPLEX) -
            sizeof(ACPI_METHOD_ARGUMENT)) + // Input Buffer Header without the first argument
        (sizeof(ACPI_METHOD_ARGUMENT) - sizeof(ULONG) + sizeof(GUID)) + // GUID Argument
        (sizeof(ACPI_METHOD_ARGUMENT) * (ACPI_DSM_ARGUMENT_COUNT - 1)); // Revision Id, Function Index, and Empty Parameters Buffer Arguments


    inputBufferPtr =static_cast<ACPI_EVAL_INPUT_BUFFER_COMPLEX*>(
        ExAllocatePoolWithTag(
            NonPagedPoolNx,
            inputBufferSize,
            ACPI_TAG_EVAL_INPUT_BUFFER));
    if (inputBufferPtr == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    RtlZeroMemory(inputBufferPtr, inputBufferSize);
    inputBufferPtr->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    inputBufferPtr->MethodNameAsUlong = ACPI_METHOD_ULONG_DSM;
    inputBufferPtr->Size = inputBufferSize;
    inputBufferPtr->ArgumentCount = ACPI_DSM_ARGUMENT_COUNT;

    //
    // Argument 0: UUID
    //
    ACPI_METHOD_ARGUMENT UNALIGNED* argumentPtr = &inputBufferPtr->Argument[0];
    ACPI_METHOD_SET_ARGUMENT_BUFFER(argumentPtr, GuidPtr, sizeof(GUID));

    //
    // Argument 1: Revision Id
    //
    argumentPtr = ACPI_METHOD_NEXT_ARGUMENT(argumentPtr);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(argumentPtr, RevisionId);

    //
    // Argument 2: Function index
    //
    argumentPtr = ACPI_METHOD_NEXT_ARGUMENT(argumentPtr);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(argumentPtr, FunctionIdx);

    //
    // Argument 3: Empty parameters package
    //
    argumentPtr = ACPI_METHOD_NEXT_ARGUMENT(argumentPtr);
    argumentPtr->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
    argumentPtr->DataLength = sizeof(ULONG);
    argumentPtr->Argument = 0;

    NT_ASSERT(
        ULONG_PTR(ACPI_METHOD_NEXT_ARGUMENT(argumentPtr)) ==
            (ULONG_PTR(inputBufferPtr) + inputBufferSize));

    *InputBufferPptr = inputBufferPtr;
    *InputBufferSizePtr = inputBufferSize;

    status = STATUS_SUCCESS;

Cleanup:

    if (!NT_SUCCESS(status) && (inputBufferPtr != nullptr)) {
        ExFreePoolWithTag(inputBufferPtr, ACPI_TAG_EVAL_INPUT_BUFFER);
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
AcpiOutputBufferGetNextArgument(
    const ACPI_EVAL_OUTPUT_BUFFER UNALIGNED* OutputBufferPtr,
    const ACPI_METHOD_ARGUMENT UNALIGNED** ArgumentPptr
    )
{
    NT_ASSERT(ARGUMENT_PRESENT(OutputBufferPtr));
    NT_ASSERT(ARGUMENT_PRESENT(ArgumentPptr));

    //
    // Output is either empty or incomplete
    //
    if ((ACPI_EVAL_OUTPUT_BUFFER_ARGUMENT_LENGTH(OutputBufferPtr) <
            sizeof(ACPI_METHOD_ARGUMENT))) {
        *ArgumentPptr = nullptr;
        return STATUS_NO_MORE_ENTRIES;
    }

    //
    // A null value indicates start enumeration from first argument
    //
    if (*ArgumentPptr == nullptr) {
        *ArgumentPptr = OutputBufferPtr->Argument;
    } else {
        const ACPI_METHOD_ARGUMENT UNALIGNED* currentArgumentPtr = *ArgumentPptr;
        currentArgumentPtr = ACPI_METHOD_NEXT_ARGUMENT(currentArgumentPtr);
        if ((currentArgumentPtr >=
            ACPI_EVAL_OUTPUT_BUFFER_ARGUMENTS_END(OutputBufferPtr))) {
            return STATUS_NO_MORE_ENTRIES;
        }
        *ArgumentPptr = currentArgumentPtr;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiPackageGetNextArgument(
    const ACPI_METHOD_ARGUMENT UNALIGNED* PkgPtr,
    const ACPI_METHOD_ARGUMENT UNALIGNED** ArgumentPptr
    )
{
    NT_ASSERT(ARGUMENT_PRESENT(PkgPtr));
    NT_ASSERT(ARGUMENT_PRESENT(ArgumentPptr));

    //
    // Package is either empty or incomplete
    //
    if (PkgPtr->DataLength < sizeof(ACPI_METHOD_ARGUMENT)) {
        *ArgumentPptr = nullptr;
        return STATUS_NO_MORE_ENTRIES;
    }

    //
    // A null value indicates start enumeration from first argument
    //
    if (*ArgumentPptr == nullptr) {
        *ArgumentPptr = reinterpret_cast<const ACPI_METHOD_ARGUMENT UNALIGNED*>(PkgPtr->Data);
    } else {
        const ACPI_METHOD_ARGUMENT UNALIGNED* currentArgumentPtr = *ArgumentPptr;
        currentArgumentPtr = ACPI_METHOD_NEXT_ARGUMENT(currentArgumentPtr);
        if (ULONG_PTR(currentArgumentPtr) >=
                ULONG_PTR(PkgPtr->Data + PkgPtr->DataLength)) {
            return STATUS_NO_MORE_ENTRIES;
        }
        *ArgumentPptr = currentArgumentPtr;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiArgumentParseGuid(
    const ACPI_METHOD_ARGUMENT UNALIGNED* ArgumentPtr,
    GUID* GuidPtr
    )
{
    NT_ASSERT(ARGUMENT_PRESENT(ArgumentPtr));
    NT_ASSERT(ARGUMENT_PRESENT(GuidPtr));

    if ((ArgumentPtr->Type != ACPI_METHOD_ARGUMENT_BUFFER) ||
        (ArgumentPtr->DataLength != sizeof(GUID))) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    RtlCopyMemory(GuidPtr, ArgumentPtr->Data, ArgumentPtr->DataLength);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiArgumentParseInteger(
    const ACPI_METHOD_ARGUMENT UNALIGNED* ArgumentPtr,
    UINT32* ValuePtr
    )
{
    NT_ASSERT(ARGUMENT_PRESENT(ArgumentPtr));
    NT_ASSERT(ARGUMENT_PRESENT(ValuePtr));

    C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
    if ((ArgumentPtr->Type != ACPI_METHOD_ARGUMENT_INTEGER) ||
        (ArgumentPtr->DataLength != sizeof(UINT32))) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    *ValuePtr = static_cast<UINT32>(ArgumentPtr->Argument);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
AcpiArgumentParseString(
    const ACPI_METHOD_ARGUMENT UNALIGNED* ArgumentPtr,
    UINT32 MaxLength,
    UINT32* OutLength,
    PCHAR ValuePtr
)
{
    NT_ASSERT(ARGUMENT_PRESENT(ArgumentPtr));
    NT_ASSERT(ARGUMENT_PRESENT(OutLength));
    NT_ASSERT(ARGUMENT_PRESENT(ValuePtr));

    C_ASSERT(sizeof(UINT32) == sizeof(ULONG));
    if (ArgumentPtr->Type != ACPI_METHOD_ARGUMENT_STRING) {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    *OutLength = ArgumentPtr->DataLength;
    if (ArgumentPtr->DataLength > MaxLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(ValuePtr, ArgumentPtr->Data, ArgumentPtr->DataLength);
    return STATUS_SUCCESS;
}

NONPAGED_SEGMENT_END //===================================================
