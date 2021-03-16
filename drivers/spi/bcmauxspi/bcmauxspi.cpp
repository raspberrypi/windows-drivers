//
// Copyright (C) Microsoft.  All rights reserved.
//
//
// Module Name:
//
//   bcmauxspi.cpp
//
// Abstract:
//
//   BCM AUX SPI Driver Implementation
//

#include "precomp.h"

#include "trace.h"
#include "bcmauxspi.tmh"

#include "bcmauxspi-hw.h"
#include "bcmauxspi.h"

namespace { // static

    WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AUXSPI_DEVICE, GetDeviceContext);

    typedef AUXSPI_DEVICE::_INTERRUPT_CONTEXT _AUXSPI_INTERRUPT_CONTEXT;
    WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
        _AUXSPI_INTERRUPT_CONTEXT,
        GetInterruptContext);

    typedef AUXSPI_DEVICE::_TARGET_CONTEXT _AUXSPI_TARGET_CONTEXT;
    WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(_AUXSPI_TARGET_CONTEXT, GetTargetContext);

} // namespace static

AUXSPI_NONPAGED_SEGMENT_BEGIN; //==============================================

ULONG AUXSPI_DRIVER::systemClockFrequency;

_Use_decl_annotations_
BOOLEAN AUXSPI_DEVICE::EvtInterruptIsr (
    WDFINTERRUPT WdfInterrupt,
    ULONG /*MessageID*/
    )
{
    _INTERRUPT_CONTEXT* interruptContextPtr = GetInterruptContext(WdfInterrupt);
    volatile BCM_AUXSPI_REGISTERS* registersPtr =
            interruptContextPtr->RegistersPtr;

    //
    // Determine if the interrupt is meant for this device
    //
    {
        BCM_AUX_IRQ_REG auxIrqReg = {READ_REGISTER_NOFENCE_ULONG(
                &interruptContextPtr->AuxRegistersPtr->Irq)};

        const bool isSpi1 =
                registersPtr == &interruptContextPtr->AuxRegistersPtr->Spi1;

        if (isSpi1) {
            if (!auxIrqReg.Spi1Irq) return FALSE;
        } else {
            NT_ASSERT(registersPtr == &interruptContextPtr->AuxRegistersPtr->Spi2);
            if (!auxIrqReg.Spi2Irq) return FALSE;
        }
    }

    NT_ASSERT(interruptContextPtr->Request.SpbRequest);

    //
    // Tx FIFO should ALWAYS be empty when an interrupt occurs
    //
    BCM_AUXSPI_STAT_REG statReg =
            {READ_REGISTER_NOFENCE_ULONG(&registersPtr->StatReg)};
    if (statReg.Busy && !statReg.TxEmpty) {
        AUXSPI_LOG_WARNING(
            "Interrupt occurred, but TX FIFO is not empty! (statReg = 0x%x)",
            statReg.AsUlong);
        NT_ASSERT(FALSE);
        return TRUE;
    }

    switch (interruptContextPtr->Request.TransferState) {
    case _TRANSFER_STATE::WRITE:
    {
        const size_t bytesToWrite = interruptContextPtr->Request.Write.BytesToWrite;
        size_t bytesWritten = interruptContextPtr->Request.Write.BytesWritten;

        // if all bytes have been written, go to DPC
        if (bytesWritten == bytesToWrite) break;

        bytesWritten += writeFifo(
                registersPtr,
                interruptContextPtr->Request.Write.WriteBufferPtr + bytesWritten,
                bytesToWrite - bytesWritten,
                interruptContextPtr->Request.FifoMode);

        NT_ASSERT(bytesWritten > interruptContextPtr->Request.Write.BytesWritten);
        interruptContextPtr->Request.Write.BytesWritten = bytesWritten;
        return TRUE;
    }
    case _TRANSFER_STATE::READ:
    {
        const size_t bytesToRead = interruptContextPtr->Request.Read.BytesToRead;
        size_t bytesRead = interruptContextPtr->Request.Read.BytesRead;

        // We should have transitioned to the DPC after reading all bytes
        NT_ASSERT(bytesRead < bytesToRead);

        bytesRead += readFifo(
                registersPtr,
                interruptContextPtr->Request.Read.ReadBufferPtr + bytesRead,
                bytesToRead - bytesRead,
                interruptContextPtr->Request.FifoMode);

        NT_ASSERT(bytesRead > interruptContextPtr->Request.Read.BytesRead);
        interruptContextPtr->Request.Read.BytesRead = bytesRead;

        // if all bytes have been read, go to DPC
        if (bytesRead == bytesToRead) break;

        return TRUE;
    }
    case _TRANSFER_STATE::SEQUENCE_WRITE:
    {
        const size_t bytesToWrite = interruptContextPtr->Request.Sequence.BytesToWrite;
        size_t bytesWritten = interruptContextPtr->Request.Sequence.BytesWritten;

        NT_ASSERT(bytesWritten < bytesToWrite);

        bytesWritten += writeFifoMdl(
                registersPtr,
                &interruptContextPtr->Request.Sequence.CurrentWriteMdl,
                &interruptContextPtr->Request.Sequence.CurrentWriteMdlOffset,
                interruptContextPtr->Request.FifoMode);

        NT_ASSERT(bytesWritten > interruptContextPtr->Request.Sequence.BytesWritten);
        interruptContextPtr->Request.Sequence.BytesWritten = bytesWritten;
        if (bytesWritten == bytesToWrite) {
            // if we've queued all bytes, advance to the read portion of the transfer
            interruptContextPtr->Request.TransferState =
                _TRANSFER_STATE::SEQUENCE_READ_INIT;
        }
        return TRUE;
    }
    case _TRANSFER_STATE::SEQUENCE_READ_INIT:
    {
        // The write just completed. Need to reprogram variable width mode
        // and get the read started
        const size_t bytesToWrite = interruptContextPtr->Request.Sequence.BytesToWrite;
        const size_t bytesToRead = interruptContextPtr->Request.Sequence.BytesToRead;

        NT_ASSERT(interruptContextPtr->Request.Sequence.BytesWritten == bytesToWrite);
        NT_ASSERT(interruptContextPtr->Request.Sequence.BytesRead == 0);

        // clear the read FIFO
        BCM_AUXSPI_CNTL0_REG cntl0 = interruptContextPtr->ControlRegs.Cntl0Reg;
        cntl0.ClearFifos = 1;
        WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl0Reg, cntl0.AsUlong);

        // compute next FIFO mode and take FIFO out of reset
        _FIFO_MODE newFifoMode = selectFifoMode(
                interruptContextPtr->Request.TargetContextPtr->DataMode,
                bytesToRead);
        _CONTROL_REGS controlRegs = computeControlRegisters(
                interruptContextPtr->Request.TargetContextPtr,
                newFifoMode);

        // take FIFOs out of reset and start the read portion of the transfer
        WRITE_REGISTER_NOFENCE_ULONG(
            &registersPtr->Cntl0Reg,
            controlRegs.Cntl0Reg.AsUlong);
        writeFifoZeros(registersPtr, bytesToRead, newFifoMode);

        interruptContextPtr->Request.FifoMode = newFifoMode;
        interruptContextPtr->ControlRegs = controlRegs;

        // after kicking off the read portion of the transfer, advance to
        // the reading state
        interruptContextPtr->Request.TransferState =
                _TRANSFER_STATE::SEQUENCE_READ;
        return TRUE;
    }
    case _TRANSFER_STATE::SEQUENCE_READ:
    {
        const size_t bytesToRead = interruptContextPtr->Request.Sequence.BytesToRead;
        size_t bytesRead = interruptContextPtr->Request.Sequence.BytesRead;

        NT_ASSERT(bytesRead < bytesToRead);

        NT_ASSERT(
            interruptContextPtr->Request.Sequence.BytesWritten ==
            interruptContextPtr->Request.Sequence.BytesToWrite);

        bytesRead += readFifoMdl(
                registersPtr,
                bytesToRead - bytesRead,
                &interruptContextPtr->Request.Sequence.CurrentReadMdl,
                &interruptContextPtr->Request.Sequence.CurrentReadMdlOffset,
                interruptContextPtr->Request.FifoMode);

        NT_ASSERT(bytesRead > interruptContextPtr->Request.Sequence.BytesRead);
        interruptContextPtr->Request.Sequence.BytesRead = bytesRead;

        // if all bytes have been read, go to DPC
        if (bytesRead == bytesToRead) break;

        return TRUE;
    }
    case _TRANSFER_STATE::FULL_DUPLEX:
    {
        const _FIFO_MODE fifoMode = interruptContextPtr->Request.FifoMode;
        const size_t bytesToWrite = interruptContextPtr->Request.Sequence.BytesToWrite;
        size_t bytesWritten = interruptContextPtr->Request.Sequence.BytesWritten;
        const size_t bytesToRead = interruptContextPtr->Request.Sequence.BytesToRead;
        size_t bytesRead = interruptContextPtr->Request.Sequence.BytesRead;

        NT_ASSERT(bytesRead < bytesToRead);
        NT_ASSERT(bytesWritten <= bytesToWrite);

        // Read raw FIFO contents into local buffer, then queue next batch of
        // bytes to get read going again as soon as possible
        ULONG fifoBuffer[BCM_AUXSPI_FIFO_DEPTH];
        for (int i = 0; i < ARRAYSIZE(fifoBuffer); ++i) {
            fifoBuffer[i] = READ_REGISTER_NOFENCE_ULONG(&registersPtr->IoReg);
        }

        // write bytes from the MDL if we need to
        if (bytesWritten != bytesToWrite) {
            bytesWritten += writeFifoMdl(
                    registersPtr,
                    &interruptContextPtr->Request.Sequence.CurrentWriteMdl,
                    &interruptContextPtr->Request.Sequence.CurrentWriteMdlOffset,
                    fifoMode);

            NT_ASSERT(bytesWritten > interruptContextPtr->Request.Sequence.BytesWritten);
            interruptContextPtr->Request.Sequence.BytesWritten = bytesWritten;
        }

        const size_t fifoCapacity = getFifoCapacity(fifoMode);
        const size_t bytesToReadChunk = min(fifoCapacity, bytesToRead - bytesRead);

        // extract bytes from fifo buffer into intermediate buffer
        ULONG buf[BCM_AUXSPI_FIFO_DEPTH];
        size_t bytesExtracted = extractFifoBuffer(
                fifoBuffer,
                reinterpret_cast<BYTE*>(buf),
                bytesToReadChunk,
                fifoMode);
        NT_ASSERT(bytesExtracted == bytesToReadChunk);

        // copy bytes from intermediate buffer to MDL
        size_t bytesCopied = copyBytesToMdl(
            &interruptContextPtr->Request.Sequence.CurrentReadMdl,
            &interruptContextPtr->Request.Sequence.CurrentReadMdlOffset,
            reinterpret_cast<const BYTE*>(buf),
            bytesExtracted);
        NT_ASSERT(bytesCopied == bytesExtracted);

        bytesRead += bytesCopied;
        NT_ASSERT(bytesRead > interruptContextPtr->Request.Sequence.BytesRead);
        interruptContextPtr->Request.Sequence.BytesRead = bytesRead;
        if (bytesRead == bytesToRead) break;

        return TRUE;
    }
    default:
        NT_ASSERT(FALSE);
        WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl0Reg, 0);
        WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl1Reg, 0);
        return TRUE;
    }

    _CONTROL_REGS controlRegs = interruptContextPtr->ControlRegs;

    // Disable interrupts
    controlRegs.Cntl1Reg.DoneIrq = 0;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl1Reg,
        controlRegs.Cntl1Reg.AsUlong);

    // Begin deasserting CS if controller is not locked
    if (!interruptContextPtr->SpbControllerLocked) {
        controlRegs.Cntl0Reg.VariableWidth = 0;
        controlRegs.Cntl0Reg.ShiftLength = 0;
        WRITE_REGISTER_NOFENCE_ULONG(
            &registersPtr->Cntl0Reg,
            controlRegs.Cntl0Reg.AsUlong);
        WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->IoReg, 0);
    }

    // Queue DPC
    WdfInterruptQueueDpcForIsr(WdfInterrupt);
    return TRUE;
}

//
// Verify that interrupts are disabled and all bytes were transferred, then
// put FIFOs in reset and complete request.
//
_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtInterruptDpc (
    WDFINTERRUPT WdfInterrupt,
    WDFOBJECT /*AssociatedObject*/
    )
{
    NT_ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
    _INTERRUPT_CONTEXT* interruptContextPtr = GetInterruptContext(WdfInterrupt);
    volatile BCM_AUXSPI_REGISTERS* registersPtr =
            interruptContextPtr->RegistersPtr;

#ifdef DBG

    //
    // Interrupts should always be disabled at DPC entry
    //
    {
        // Ensure interrupts are disabled
        BCM_AUXSPI_CNTL1_REG cntl1Reg =
            {READ_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl1Reg)};
        NT_ASSERT(!cntl1Reg.DoneIrq && !cntl1Reg.TxEmptyIrq);
    }

#endif // DBG

    // Acquire ownership of the request
    SPBREQUEST spbRequest = static_cast<SPBREQUEST>(InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile *>(&interruptContextPtr->Request.SpbRequest),
            nullptr));
    if (!spbRequest) {
        AUXSPI_LOG_INFORMATION("Cannot complete request - already claimed by cancellation routine.");
        return;
    }

    NTSTATUS status = WdfRequestUnmarkCancelable(spbRequest);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRequestUnmarkCancelable(...) failed. (spbRequest = 0x%p, status = %!STATUS!)",
            spbRequest,
            status);
        if (status != STATUS_CANCELLED) {
            SpbRequestComplete(spbRequest, status);
        }
        return;
    }

    ULONG_PTR information;
    status = processRequestCompletion(interruptContextPtr, &information);

    // Put FIFOs in reset
    BCM_AUXSPI_CNTL0_REG cntl0Reg = interruptContextPtr->ControlRegs.Cntl0Reg;
    cntl0Reg.ClearFifos = 1;
    WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl0Reg, cntl0Reg.AsUlong);

    interruptContextPtr->Request.TransferState = _TRANSFER_STATE::INVALID;
    WdfRequestSetInformation(spbRequest, information);
    SpbRequestComplete(spbRequest, status);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbControllerLock (
    WDFDEVICE WdfDevice,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;
    volatile BCM_AUXSPI_REGISTERS* registersPtr = interruptContextPtr->RegistersPtr;
    _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);

    NT_ASSERT(!interruptContextPtr->SpbControllerLocked);

    _FIFO_MODE fifoMode = selectFifoMode(targetContextPtr->DataMode, 4 /* abritrary */);
    _CONTROL_REGS controlRegs = computeControlRegisters(
            targetContextPtr,
            fifoMode);

    //
    // Assert CS
    //
    {
        assertCsBegin(registersPtr, controlRegs);
        interruptContextPtr->ControlRegs = controlRegs;
        assertCsComplete(registersPtr, controlRegs);
    }

    // put FIFOs back in reset
    controlRegs.Cntl0Reg.ClearFifos = 1;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl0Reg,
        controlRegs.Cntl0Reg.AsUlong);

    interruptContextPtr->SpbControllerLocked = true;
    SpbRequestComplete(SpbRequest, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbControllerUnlock (
    WDFDEVICE WdfDevice,
    SPBTARGET /*SpbTarget*/,
    SPBREQUEST SpbRequest
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;

    NT_ASSERT(interruptContextPtr->SpbControllerLocked);

    deassertCs(
        interruptContextPtr->RegistersPtr,
        interruptContextPtr->ControlRegs.Cntl0Reg);

    interruptContextPtr->SpbControllerLocked = false;
    SpbRequestComplete(SpbRequest, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbIoRead (
    WDFDEVICE WdfDevice,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    NTSTATUS status;

    PVOID outputBufferPtr;
    {
        size_t outputBufferLength;
        status = WdfRequestRetrieveOutputBuffer(
                    SpbRequest,
                    1,          // MinimumRequiredSize
                    &outputBufferPtr,
                    &outputBufferLength);
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "Failed to retreive output buffer from request. (SpbRequest = %p, status = %!STATUS!)",
                SpbRequest,
                status);
            SpbRequestComplete(SpbRequest, status);
            return;
        }

        NT_ASSERT(outputBufferLength == Length);
    }

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    volatile BCM_AUXSPI_REGISTERS* registersPtr = thisPtr->registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;

    const _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);

    _FIFO_MODE fifoMode = selectFifoMode(targetContextPtr->DataMode, Length);
    _CONTROL_REGS controlRegs = computeControlRegisters(
            targetContextPtr,
            fifoMode);

    //
    // Assert CS and do some useful work (i.e. setting up the request context)
    // while we're waiting for CS to assert
    //
    {
        assertCsBegin(registersPtr, controlRegs);

        // prepare request context
        new (&interruptContextPtr->Request) _INTERRUPT_CONTEXT::_REQUEST(
            _TRANSFER_STATE::READ,
            fifoMode,
            SpbRequest,
            targetContextPtr);

        new (&interruptContextPtr->Request.Read) _READ_CONTEXT{
            static_cast<BYTE*>(outputBufferPtr),
            Length,
            0 /* BytesRead */};

        interruptContextPtr->ControlRegs = controlRegs;

        assertCsComplete(registersPtr, controlRegs);
    }

    // queue dummy bytes to the FIFO
    writeFifoZeros(registersPtr, Length, fifoMode);

    status = WdfRequestMarkCancelableEx(SpbRequest, EvtRequestCancel);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRequestMarkCancelableEx(...) failed. (SpbRequest = %p, status = %!STATUS!)",
            SpbRequest,
            status);
        abortTransfer(interruptContextPtr);
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    // enable interrupts
    controlRegs.Cntl1Reg.DoneIrq = 1;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl1Reg,
        controlRegs.Cntl1Reg.AsUlong);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbIoWrite (
    WDFDEVICE WdfDevice,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t Length
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    NTSTATUS status;

    PVOID inputBufferPtr;
    {
        size_t inputBufferLength;
        status = WdfRequestRetrieveInputBuffer(
                    SpbRequest,
                    1,          // MinimumRequiredSize
                    &inputBufferPtr,
                    &inputBufferLength);
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "WdfRequestRetrieveInputBuffer(..) failed. (status = %!STATUS!, SpbRequest = 0x%p, Length = %Iu)",
                status,
                SpbRequest,
                Length);
            SpbRequestComplete(SpbRequest, status);
            return;
        }

        NT_ASSERT(inputBufferLength == Length);
    }
    const BYTE* const writeBufferPtr = static_cast<const BYTE*>(inputBufferPtr);

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    volatile BCM_AUXSPI_REGISTERS* registersPtr = thisPtr->registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;

    const _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);

    _FIFO_MODE fifoMode = selectFifoMode(targetContextPtr->DataMode, Length);
    _CONTROL_REGS controlRegs = computeControlRegisters(
            targetContextPtr,
            fifoMode);

    //
    // Assert CS and do some useful work (i.e. setting up the request context)
    // while we're waiting for CS to assert
    //
    {
        assertCsBegin(registersPtr, controlRegs);

        // prepare request context
        new (&interruptContextPtr->Request) _INTERRUPT_CONTEXT::_REQUEST(
            _TRANSFER_STATE::WRITE,
            fifoMode,
            SpbRequest,
            targetContextPtr);

        new (&interruptContextPtr->Request.Write) _WRITE_CONTEXT{
            writeBufferPtr,
            Length};

        interruptContextPtr->ControlRegs = controlRegs;

        assertCsComplete(registersPtr, controlRegs);
    }

    interruptContextPtr->Request.Write.BytesWritten = writeFifo(
            registersPtr,
            writeBufferPtr,
            Length,
            fifoMode);

    status = WdfRequestMarkCancelableEx(SpbRequest, EvtRequestCancel);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRequestMarkCancelableEx(...) failed. (SpbRequest = %p, status = %!STATUS!)",
            SpbRequest,
            status);
        abortTransfer(interruptContextPtr);
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    // enable interrupts
    controlRegs.Cntl1Reg.DoneIrq = 1;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl1Reg,
        controlRegs.Cntl1Reg.AsUlong);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbIoSequence (
    WDFDEVICE WdfDevice,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    ULONG TransferCount
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    if (TransferCount != 2) {
        AUXSPI_LOG_ERROR(
            "Unsupported sequence attempted. Only Write-Read and FullDuplex sequences are supported. (TransferCount = %lu)",
            TransferCount);
        SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
        return;
    }

    PMDL writeMdl;
    ULONG bytesToWrite;
    PMDL readMdl;
    ULONG bytesToRead;
    {
        SPB_TRANSFER_DESCRIPTOR writeDescriptor;
        SPB_TRANSFER_DESCRIPTOR_INIT(&writeDescriptor);
        SpbRequestGetTransferParameters(
            SpbRequest,
            0,
            &writeDescriptor,
            &writeMdl);

        // validate first transfer descriptor to make sure it's a write
        if ((writeDescriptor.Direction != SpbTransferDirectionToDevice)) {
            AUXSPI_LOG_ERROR(
                "Unsupported sequence attempted. The first transfer must be a write. (writeDescriptor.Direction = %d)",
                writeDescriptor.Direction);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        if (writeDescriptor.DelayInUs != 0) {
            AUXSPI_LOG_ERROR(
                "Delays are not supported. (writeDescriptor.DelayInUs = %lu)",
                writeDescriptor.DelayInUs);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        // validate second transfer descriptor to make sure it's a read
        SPB_TRANSFER_DESCRIPTOR readDescriptor;
        SPB_TRANSFER_DESCRIPTOR_INIT(&readDescriptor);
        SpbRequestGetTransferParameters(
            SpbRequest,
            1,
            &readDescriptor,
            &readMdl);
        if (readDescriptor.Direction != SpbTransferDirectionFromDevice) {
            AUXSPI_LOG_ERROR(
                "Unsupported sequence attempted. The second transfer must be a read. (readDescriptor.Direction = %d)",
                readDescriptor.Direction);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        if (readDescriptor.DelayInUs != 0) {
            AUXSPI_LOG_ERROR(
                "Delays are not supported. (readDescriptor.DelayInUs = %lu)",
                readDescriptor.DelayInUs);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        bytesToWrite = 0;
        for (PMDL currentMdl = writeMdl;
             currentMdl;
             currentMdl = currentMdl->Next) {

            const PVOID ptr = MmGetSystemAddressForMdlSafe(
                currentMdl,
                NormalPagePriority | MdlMappingNoWrite | MdlMappingNoExecute);
            if (!ptr) {
                AUXSPI_LOG_LOW_MEMORY(
                    "MmGetSystemAddressForMdlSafe() failed. (currentMdl = %p)",
                    currentMdl);
                SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }
            NT_ASSERT(MmGetMdlByteCount(currentMdl) != 0);
            bytesToWrite += MmGetMdlByteCount(currentMdl);
        }

        NT_ASSERT(bytesToWrite == writeDescriptor.TransferLength);

        bytesToRead = 0;
        for (PMDL currentMdl = readMdl;
             currentMdl;
             currentMdl = currentMdl->Next) {

            PVOID ptr = MmGetSystemAddressForMdlSafe(
                currentMdl,
                NormalPagePriority | MdlMappingNoExecute);
            if (!ptr) {
                AUXSPI_LOG_LOW_MEMORY(
                    "MmGetSystemAddressForMdlSafe() failed. (currentMdl = %p)",
                    currentMdl);
                SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }

            NT_ASSERT(MmGetMdlByteCount(currentMdl) != 0);
            bytesToRead += MmGetMdlByteCount(currentMdl);
        }

        NT_ASSERT(bytesToRead == readDescriptor.TransferLength);
    }

    // for WriteRead transfer, need to write, then read part way through
    // transfer

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    volatile BCM_AUXSPI_REGISTERS* registersPtr = thisPtr->registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;
    const _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);

    _FIFO_MODE fifoMode = selectFifoMode(targetContextPtr->DataMode, bytesToWrite);
    _CONTROL_REGS controlRegs = computeControlRegisters(
            targetContextPtr,
            fifoMode);

    // Assert CS
    {
        assertCsBegin(registersPtr, controlRegs);

        new (&interruptContextPtr->Request) _INTERRUPT_CONTEXT::_REQUEST(
            _TRANSFER_STATE::SEQUENCE_WRITE,
            fifoMode,
            SpbRequest,
            targetContextPtr);

        new (&interruptContextPtr->Request.Sequence) _SEQUENCE_CONTEXT{
            writeMdl,
            bytesToWrite,
            0,                  // BytesWritten
            0,                  // CurrentWriteMdlOffset
            readMdl,
            bytesToRead,
            0,                  // BytesRead
            0                   // CurrentReadMdlOffset
            };

        interruptContextPtr->ControlRegs = controlRegs;

        assertCsComplete(registersPtr, controlRegs);
    }

    size_t bytesWritten = writeFifoMdl(
            registersPtr,
            &interruptContextPtr->Request.Sequence.CurrentWriteMdl,
            &interruptContextPtr->Request.Sequence.CurrentWriteMdlOffset,
            fifoMode);

    interruptContextPtr->Request.Sequence.BytesWritten = bytesWritten;
    if (bytesWritten == bytesToWrite) {
        // If all bytes have been written, advance to the read portion
        // of the transfer
        interruptContextPtr->Request.TransferState =
            _TRANSFER_STATE::SEQUENCE_READ_INIT;
    }

    NTSTATUS status = WdfRequestMarkCancelableEx(SpbRequest, EvtRequestCancel);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRequestMarkCancelableEx(...) failed. (SpbRequest = %p, status = %!STATUS!)",
            SpbRequest,
            status);
        abortTransfer(interruptContextPtr);
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    // enable interrupts
    controlRegs.Cntl1Reg.DoneIrq = 1;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl1Reg,
        controlRegs.Cntl1Reg.AsUlong);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtSpbIoOther (
    WDFDEVICE WdfDevice,
    SPBTARGET SpbTarget,
    SPBREQUEST SpbRequest,
    size_t /*OutputBufferLength*/,
    size_t /*InputBufferLength*/,
    ULONG IoControlCode
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    // All other IOCTLs should have been filtered out in EvtIoInCallerContext
    UNREFERENCED_PARAMETER(IoControlCode);
    NT_ASSERT(IoControlCode == IOCTL_SPB_FULL_DUPLEX);

    PMDL writeMdl;
    PMDL readMdl;
    size_t length;
    {
        SPB_REQUEST_PARAMETERS requestParams;
        SPB_REQUEST_PARAMETERS_INIT(&requestParams);
        SpbRequestGetParameters(SpbRequest, &requestParams);
        if (requestParams.SequenceTransferCount != 2) {
            AUXSPI_LOG_ERROR(
                "Full-duplex transfer must have exactly 2 entries in transfer list. (requestParams.SequenceTransferCount = %lu)",
                requestParams.SequenceTransferCount);
            SpbRequestComplete(SpbRequest, STATUS_INVALID_PARAMETER);
            return;
        }

        SPB_TRANSFER_DESCRIPTOR writeDescriptor;
        SPB_TRANSFER_DESCRIPTOR_INIT(&writeDescriptor);
        SpbRequestGetTransferParameters(
            SpbRequest,
            0,
            &writeDescriptor,
            &writeMdl);

        // validate first transfer descriptor to make sure it's a write
        if ((writeDescriptor.Direction != SpbTransferDirectionToDevice)) {
            AUXSPI_LOG_ERROR(
                "Unsupported sequence attempted. The first transfer must be a write. (writeDescriptor.Direction = %d)",
                writeDescriptor.Direction);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        if (writeDescriptor.DelayInUs != 0) {
            AUXSPI_LOG_ERROR(
                "Delays are not supported. (writeDescriptor.DelayInUs = %lu)",
                writeDescriptor.DelayInUs);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        // validate second transfer descriptor to make sure it's a read
        SPB_TRANSFER_DESCRIPTOR readDescriptor;
        SPB_TRANSFER_DESCRIPTOR_INIT(&readDescriptor);
        SpbRequestGetTransferParameters(
            SpbRequest,
            1,
            &readDescriptor,
            &readMdl);
        if (readDescriptor.Direction != SpbTransferDirectionFromDevice) {
            AUXSPI_LOG_ERROR(
                "Unsupported sequence attempted. The second transfer must be a read. (readDescriptor.Direction = %d)",
                readDescriptor.Direction);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        if (readDescriptor.DelayInUs != 0) {
            AUXSPI_LOG_ERROR(
                "Delays are not supported. (readDescriptor.DelayInUs = %lu)",
                readDescriptor.DelayInUs);
            SpbRequestComplete(SpbRequest, STATUS_NOT_SUPPORTED);
            return;
        }

        if (writeDescriptor.TransferLength != readDescriptor.TransferLength) {
            AUXSPI_LOG_ERROR(
                "Write buffer length must be equal to read buffer length for full-duplex transfer. (readDescriptor.TransferLength = %lld, writeDescriptor.TransferLength = %lld)",
                readDescriptor.TransferLength,
                writeDescriptor.TransferLength);
            SpbRequestComplete(SpbRequest, STATUS_INVALID_PARAMETER);
            return;
        }

        size_t bytesToWrite = 0;
        for (PMDL currentMdl = writeMdl;
             currentMdl;
             currentMdl = currentMdl->Next) {

            const PVOID ptr = MmGetSystemAddressForMdlSafe(
                currentMdl,
                NormalPagePriority | MdlMappingNoWrite | MdlMappingNoExecute);
            if (!ptr) {
                AUXSPI_LOG_LOW_MEMORY(
                    "MmGetSystemAddressForMdlSafe() failed. (currentMdl = %p)",
                    currentMdl);
                SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }
            NT_ASSERT(MmGetMdlByteCount(currentMdl) != 0);
            bytesToWrite += MmGetMdlByteCount(currentMdl);
        }

        NT_ASSERT(bytesToWrite == writeDescriptor.TransferLength);

        size_t bytesToRead = 0;
        for (PMDL currentMdl = readMdl;
             currentMdl;
             currentMdl = currentMdl->Next) {

            PVOID ptr = MmGetSystemAddressForMdlSafe(
                currentMdl,
                NormalPagePriority | MdlMappingNoExecute);
            if (!ptr) {
                AUXSPI_LOG_LOW_MEMORY(
                    "MmGetSystemAddressForMdlSafe() failed. (currentMdl = %p)",
                    currentMdl);
                SpbRequestComplete(SpbRequest, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }

            NT_ASSERT(MmGetMdlByteCount(currentMdl) != 0);
            bytesToRead += MmGetMdlByteCount(currentMdl);
        }
        NT_ASSERT(bytesToRead == readDescriptor.TransferLength);

        length = writeDescriptor.TransferLength;
    }

    // For full duplex transfer, write and read at the same time

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    volatile BCM_AUXSPI_REGISTERS* registersPtr = thisPtr->registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;
    const _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);

    _FIFO_MODE fifoMode = selectFifoMode(targetContextPtr->DataMode, length);
    _CONTROL_REGS controlRegs = computeControlRegisters(
            targetContextPtr,
            fifoMode);

    //
    // Prepare request context while asserting CS
    //
    {
        assertCsBegin(registersPtr, controlRegs);

        // prepare request context
        new (&interruptContextPtr->Request) _INTERRUPT_CONTEXT::_REQUEST(
            _TRANSFER_STATE::FULL_DUPLEX,
            fifoMode,
            SpbRequest,
            targetContextPtr);

        new (&interruptContextPtr->Request.Sequence) _SEQUENCE_CONTEXT{
            writeMdl,
            length,             // BytesToWrite
            0,                  // BytesWritten
            0,                  // CurrentWriteMdlOffset
            readMdl,
            length,             // BytesToRead
            0,                  // BytesRead
            0                   // CurrentReadMdlOffset
            };

        interruptContextPtr->ControlRegs = controlRegs;

        assertCsComplete(registersPtr, controlRegs);
    }

    // kick off the transfer by writing bytes
    interruptContextPtr->Request.Sequence.BytesWritten = writeFifoMdl(
            registersPtr,
            &interruptContextPtr->Request.Sequence.CurrentWriteMdl,
            &interruptContextPtr->Request.Sequence.CurrentWriteMdlOffset,
            fifoMode);

    NTSTATUS status = WdfRequestMarkCancelableEx(SpbRequest, EvtRequestCancel);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRequestMarkCancelableEx(...) failed. (SpbRequest = %p, status = %!STATUS!)",
            SpbRequest,
            status);
        abortTransfer(interruptContextPtr);
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    // enable interrupts
    controlRegs.Cntl1Reg.DoneIrq = 1;
    WRITE_REGISTER_NOFENCE_ULONG(
        &registersPtr->Cntl1Reg,
        controlRegs.Cntl1Reg.AsUlong);
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtIoInCallerContext (
    WDFDEVICE WdfDevice,
    WDFREQUEST WdfRequest
    )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(WdfRequest, &params);

    switch (params.Type) {
    case WdfRequestTypeDeviceControl:
    case WdfRequestTypeDeviceControlInternal:
        break;
    default:
        WdfRequestComplete(WdfRequest, STATUS_NOT_SUPPORTED);
        return;
    }

    switch (params.Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_SPB_FULL_DUPLEX:
        break;
    default:
        WdfRequestComplete(WdfRequest, STATUS_NOT_SUPPORTED);
        return;
    }

    NTSTATUS status = SpbRequestCaptureIoOtherTransferList(
            static_cast<SPBREQUEST>(WdfRequest));
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "SpbRequestCaptureIoOtherTransferList(...) failed. (status = %!STATUS!)",
            status);
        WdfRequestComplete(WdfRequest, status);
        return;
    }

    status = WdfDeviceEnqueueRequest(WdfDevice, WdfRequest);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfDeviceEnqueueRequest(...) failed. (status = %!STATUS!)",
            status);
        WdfRequestComplete(WdfRequest, status);
        return;
    }
}

_Use_decl_annotations_
VOID AUXSPI_DEVICE::EvtRequestCancel ( WDFREQUEST  WdfRequest )
{
    AUXSPI_ASSERT_MAX_IRQL(DISPATCH_LEVEL);

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfFileObjectGetDevice(
            WdfRequestGetFileObject(WdfRequest)));
    volatile BCM_AUXSPI_REGISTERS* registersPtr = thisPtr->registersPtr;
    _INTERRUPT_CONTEXT* interruptContextPtr = thisPtr->interruptContextPtr;

    // synchronize with ISR when disabling interrupts
    _CONTROL_REGS controlRegs;
    {
        WdfInterruptAcquireLock(thisPtr->wdfInterrupt);
        struct _RELEASE_LOCK {
            WDFINTERRUPT const wdfInterrupt;

            _Releases_lock_(this->wdfInterrupt)
            ~_RELEASE_LOCK ()
            {
                WdfInterruptReleaseLock(this->wdfInterrupt);
            }
        } releaseLock = {thisPtr->wdfInterrupt};

        // Attempt to acquire ownership of the request
        SPBREQUEST currentRequest = static_cast<SPBREQUEST>(
            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile *>(&interruptContextPtr->Request.SpbRequest),
                nullptr));
        if (!currentRequest) {
            AUXSPI_LOG_TRACE("Cannot cancel request - already claimed by DPC.");
            return;
        }
        NT_ASSERT(WdfRequest == currentRequest);

        // read current value of control registers
        controlRegs = interruptContextPtr->ControlRegs;

        // Disable interrupts
        controlRegs.Cntl1Reg.TxEmptyIrq = 0;
        controlRegs.Cntl1Reg.DoneIrq = 0;
        WRITE_REGISTER_NOFENCE_ULONG(
            &registersPtr->Cntl1Reg,
            controlRegs.Cntl1Reg.AsUlong);
    } // release interrupt lock

    abortTransfer(interruptContextPtr);

    AUXSPI_LOG_INFORMATION(
        "Canceling request. (WdfRequest = 0x%p, interruptContextPtr = 0x%p)",
        WdfRequest,
        interruptContextPtr);

    SpbRequestComplete(static_cast<SPBREQUEST>(WdfRequest), STATUS_CANCELLED);
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::writeFifo (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    _In_reads_(Length) const BYTE* WriteBufferPtr,
    size_t Length,
    _FIFO_MODE FifoMode
    )
{
    NT_ASSERT(Length != 0);

    switch (FifoMode) {
    case _FIFO_MODE::FIXED_4:

        ASSERT_ULONG_ALIGNED(WriteBufferPtr, Length);
        return sizeof(ULONG) * _FIFO_FIXED_4::Write(
                RegistersPtr,
                reinterpret_cast<const ULONG*>(WriteBufferPtr),
                Length / sizeof(ULONG));

    case _FIFO_MODE::VARIABLE_3:

        return _FIFO_VARIABLE_3::Write(
                    RegistersPtr,
                    WriteBufferPtr,
                    Length);

    case _FIFO_MODE::FIXED_3_SHIFTED:

        return _FIFO_FIXED_3_SHIFTED::Write(
                RegistersPtr,
                WriteBufferPtr,
                Length);

    case _FIFO_MODE::VARIABLE_2_SHIFTED:

        return _FIFO_VARIABLE_2_SHIFTED::Write(
                RegistersPtr,
                WriteBufferPtr,
                Length);

    default:
        NT_ASSERT(FALSE);
        return 0;
    }
}

size_t AUXSPI_DEVICE::writeFifoMdl (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    PMDL* MdlPtr,
    size_t* OffsetPtr,
    _FIFO_MODE FifoMode
    )
{
    const size_t fifoCapacity = getFifoCapacity(FifoMode);

    ULONG fifoBuffer[BCM_AUXSPI_FIFO_DEPTH];
    size_t bytesCopied = copyBytesFromMdl(
            MdlPtr,
            OffsetPtr,
            reinterpret_cast<BYTE*>(fifoBuffer),
            fifoCapacity);

    return writeFifo(
            RegistersPtr,
            reinterpret_cast<const BYTE*>(fifoBuffer),
            bytesCopied,
            FifoMode);
}

size_t AUXSPI_DEVICE::writeFifoZeros (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    size_t MaxCount,
    _FIFO_MODE FifoMode
    )
{
    // must be ULONG-aligned
    ULONG zeros[BCM_AUXSPI_FIFO_DEPTH] = {0};
    return writeFifo(
        RegistersPtr,
        reinterpret_cast<const BYTE*>(zeros),
        MaxCount,
        FifoMode);
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::readFifo (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    BYTE* ReadBufferPtr,
    size_t Length,
    _FIFO_MODE FifoMode
    )
{
    NT_ASSERT(Length != 0);

    // Read raw FIFO contents into local buffer, then queue next batch of
    // bytes to get read going again as soon as possible
    ULONG fifoBuffer[BCM_AUXSPI_FIFO_DEPTH];
    for (int i = 0; i < ARRAYSIZE(fifoBuffer); ++i) {
        fifoBuffer[i] = READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->IoReg);
    }

    const size_t fifoCapacity = getFifoCapacity(FifoMode);
    const size_t bytesToReadChunk = min(Length, fifoCapacity);

    // get the next chunk going now that we've drained the read buffer
    NT_ASSERT(Length >= bytesToReadChunk);
    const size_t remainingBytesToWrite = Length - bytesToReadChunk;
    if (remainingBytesToWrite) {
        writeFifoZeros(RegistersPtr, remainingBytesToWrite, FifoMode);
    }

    size_t bytesExtracted = extractFifoBuffer(
            fifoBuffer,
            ReadBufferPtr,
            bytesToReadChunk,
            FifoMode);
    NT_ASSERT(bytesExtracted == bytesToReadChunk);
    return bytesExtracted;
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::readFifoMdl (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    size_t Length,
    PMDL* MdlPtr,
    size_t* OffsetPtr,
    _FIFO_MODE FifoMode
    )
{
    ULONG buf[BCM_AUXSPI_FIFO_DEPTH];
    const size_t bytesRead = readFifo(
            RegistersPtr,
            reinterpret_cast<BYTE*>(buf),
            Length,
            FifoMode);

    return copyBytesToMdl(
            MdlPtr,
            OffsetPtr,
            reinterpret_cast<const BYTE*>(buf),
            bytesRead);
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::extractFifoBuffer (
    const ULONG* FifoBuffer,
    BYTE* ReadBufferPtr,
    size_t Length,
    _FIFO_MODE FifoMode
    )
{
    switch (FifoMode) {
    case _FIFO_MODE::FIXED_4:

        ASSERT_ULONG_ALIGNED(ReadBufferPtr, Length);
        _FIFO_FIXED_4::Extract(
            FifoBuffer,
            reinterpret_cast<ULONG*>(ReadBufferPtr),
            Length / sizeof(ULONG));

        break;

    case _FIFO_MODE::VARIABLE_3:

        _FIFO_VARIABLE_3::Extract(
            FifoBuffer,
            ReadBufferPtr,
            Length);

        break;

    case _FIFO_MODE::FIXED_3_SHIFTED:

        _FIFO_FIXED_3_SHIFTED::Extract(
            FifoBuffer,
            ReadBufferPtr,
            Length);

        break;

    case _FIFO_MODE::VARIABLE_2_SHIFTED:

        _FIFO_VARIABLE_2_SHIFTED::Extract(
            FifoBuffer,
            ReadBufferPtr,
            Length);

        break;

    default:
        NT_ASSERT(FALSE);
    }

    return Length;
}

//
// Writes a ULONG-aligned buffer to the FIFO in fixed 32-bit mode
//
_Use_decl_annotations_
size_t AUXSPI_DEVICE::_FIFO_FIXED_4::Write(
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const ULONG* WriteBufferPtr,
    size_t Length
    )
{
    NT_ASSERT(Length != 0);

    const size_t count = min(Length, BCM_AUXSPI_FIFO_DEPTH);
    for (size_t i = count; i; --i) {
        // Input sequence: 0x78563412
        // Output sequence: 0x12345678
        WRITE_REGISTER_NOFENCE_ULONG(
            &RegistersPtr->TxHoldReg,               // keep CS asserted
            RtlUlongByteSwap(*WriteBufferPtr++));
    }
    return count;
}

_Use_decl_annotations_
void AUXSPI_DEVICE::_FIFO_FIXED_4::Extract (
    const ULONG* FifoBuffer,
    ULONG* ReadBufferPtr,
    size_t Length
    )
{
    NT_ASSERT((Length != 0) && (Length <= FIFO_CAPACITY));

    for (size_t i = 0; i < Length; ++i) {
        // Input sequence: 0x12345678
        // Output sequence: 0x78563412
        ReadBufferPtr[i] = RtlUlongByteSwap(FifoBuffer[i]);
    }
}

//
// Write bytes to the FIFO in variable shift mode
//
_Use_decl_annotations_
size_t AUXSPI_DEVICE::_FIFO_VARIABLE_3::Write (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const BYTE* WriteBufferPtr,
    size_t Length
    )
{
    NT_ASSERT(Length != 0);

    const size_t bytesToQueue = min(FIFO_CAPACITY, Length);
    for (size_t i = 0; i < (bytesToQueue / 3); ++i) {
        // Input Sequence: 12 34 56 ab cd
        // Output Sequence: 0x00123456 0x00abcd00
        BCM_AUXSPI_IO_REG dataReg = {0};
        dataReg.Width = 24;
        dataReg.Data = (WriteBufferPtr[i * 3] << 16) |
            (WriteBufferPtr[i * 3 + 1] << 8) | WriteBufferPtr[i * 3 + 2];

        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, dataReg.AsUlong);
    }

    // Handle last one or two bytes
    switch (bytesToQueue % 3) {
    case 0: break;
    case 1:
    {
        BCM_AUXSPI_IO_REG dataReg = {0};
        dataReg.Width = 8;
        dataReg.Data = WriteBufferPtr[bytesToQueue - 1] << 16;
        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, dataReg.AsUlong);
        break;
    }
    case 2:
    {
        BCM_AUXSPI_IO_REG dataReg = {0};
        dataReg.Width = 16;
        dataReg.Data = (WriteBufferPtr[bytesToQueue - 1] << 8) |
                       (WriteBufferPtr[bytesToQueue - 2] << 16);
        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, dataReg.AsUlong);
        break;
    }
    } // switch

    return bytesToQueue;
}

_Use_decl_annotations_
#pragma prefast(suppress:6101, "ReadBufferPtr is always written to")
void AUXSPI_DEVICE::_FIFO_VARIABLE_3::Extract (
    const ULONG* FifoBuffer,
    BYTE* ReadBufferPtr,
    size_t Length
    )
{
    NT_ASSERT((Length != 0) && (Length <= FIFO_CAPACITY));

    // each fifo entry contains up to 3 byte-reversed words
    for (size_t i = 0; i < (Length / 3); ++i) {
        // Input sequence: 0x00123456 0x0000abcd
        // Output sequence: 12 34 56 ab cd
        ULONG data = FifoBuffer[i];
        ReadBufferPtr[i * 3] = static_cast<BYTE>(data >> 16);
        ReadBufferPtr[i * 3 + 1] = static_cast<BYTE>(data >> 8);
        ReadBufferPtr[i * 3 + 2] = static_cast<BYTE>(data);
    }

    // handle last 1 or 2 bytes
    ULONG data = FifoBuffer[(Length - 1) / 3];
    switch (Length % 3) {
    case 0:
        break;
    case 2:
        ReadBufferPtr[Length - 2] = static_cast<BYTE>(data >> 8);
        __fallthrough;
    case 1:
        ReadBufferPtr[Length - 1] = static_cast<BYTE>(data);
    }
}

//
// Write bytes to the FIFO in 24-bit fixed width mode with data shift
//
_Use_decl_annotations_
size_t AUXSPI_DEVICE::_FIFO_FIXED_3_SHIFTED::Write (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const BYTE* WriteBufferPtr,
    size_t Length
    )
{
    NT_ASSERT((Length != 0) && ((Length % 3) == 0));

    const size_t bytesToQueue = min(Length, FIFO_CAPACITY);
    for (size_t i = 0; i < (bytesToQueue / 3); ++i) {
        // Input sequence: ab cd ef 12 34 56 ...
        // Output sequence: (0xabcdef00 >> 1), (0x12345600 >> 1) ...
        ULONG data = (WriteBufferPtr[i * 3] << 23) |
                     (WriteBufferPtr[i * 3 + 1] << 15) |
                     (WriteBufferPtr[i * 3 + 2] << 7);

        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, data);
    }

    return bytesToQueue;
}

_Use_decl_annotations_
void AUXSPI_DEVICE::_FIFO_FIXED_3_SHIFTED::Extract (
    const ULONG* FifoBuffer,
    BYTE* ReadBufferPtr,
    size_t Length
    )
{
    NT_ASSERT((Length != 0) && (Length <= FIFO_CAPACITY) && ((Length % 3) == 0));

    for (size_t i = 0; i < (Length / 3); ++i) {
        // Input Sequence: 0x00123456 0x00abcdef
        // Output Sequence: 12 34 56 ab cd ef
        ULONG data = FifoBuffer[i];
        ReadBufferPtr[i * 3] = static_cast<BYTE>(data >> 16);
        ReadBufferPtr[i * 3 + 1] = static_cast<BYTE>(data >> 8);
        ReadBufferPtr[i * 3 + 2] = static_cast<BYTE>(data);
    }
}

//
// Write bytes to the FIFO in variable shift mode
//
_Use_decl_annotations_
size_t AUXSPI_DEVICE::_FIFO_VARIABLE_2_SHIFTED::Write (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const BYTE* WriteBufferPtr,
    size_t Length
    )
{
    NT_ASSERT(Length != 0);

    // Input Sequence: 12 34 56 78 ab
    // Output Sequence: (0x00123400 >> 1) (0x00567800 >> 1) (0x00ab0000 >> 1)
    const size_t bytesToQueue = min(FIFO_CAPACITY, Length);
    for (size_t i = 0; i < (bytesToQueue / 2); ++i) {
        BCM_AUXSPI_IO_REG dataReg = {0};
        dataReg.Width = 16;
        dataReg.Data = (WriteBufferPtr[i * 2] << 15) |
                       (WriteBufferPtr[i * 2 + 1] << 7);
        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, dataReg.AsUlong);
    }

    // handle last byte
    if ((bytesToQueue % 2) != 0) {
        BCM_AUXSPI_IO_REG dataReg = {0};
        dataReg.Width = 8;
        dataReg.Data = (WriteBufferPtr[bytesToQueue - 1] << 15);
        WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, dataReg.AsUlong);
    }

    return bytesToQueue;
}

_Use_decl_annotations_
#pragma prefast(suppress:6101, "ReadBufferPtr is always written to")
void AUXSPI_DEVICE::_FIFO_VARIABLE_2_SHIFTED::Extract (
    const ULONG* FifoBuffer,
    BYTE* ReadBufferPtr,
    size_t Length
    )
{
    NT_ASSERT((Length != 0) && (Length <= FIFO_CAPACITY));

    // Input Sequence: 0x00001234 0x000056ab 0x000000cd
    // Output Sequence: 12 34 56 ab cd
    for (size_t i = 0; i < (Length / 2); ++i) {
        ULONG data = FifoBuffer[i];
        ReadBufferPtr[i * 2] = static_cast<BYTE>(data >> 8);
        ReadBufferPtr[i * 2 + 1] = static_cast<BYTE>(data);
    }

    // Handle last byte
    if ((Length % 2) != 0) {
        ReadBufferPtr[Length - 1] = static_cast<BYTE>(FifoBuffer[(Length / 2)]);
    }
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::copyBytesToMdl (
    PMDL* MdlPtr,
    size_t* MdlOffsetPtr,
    const BYTE* Buffer,
    size_t Length
    )
{
    PMDL currentMdl = *MdlPtr;
    size_t offset = *MdlOffsetPtr;

    NT_ASSERT(currentMdl);

    // copy from buffer to chained MDL
    size_t bytesCopied = 0;
    for (;;) {
        if (offset == MmGetMdlByteCount(currentMdl)) {
            currentMdl = currentMdl->Next;
            offset = 0;
            if (!currentMdl) break;
            continue;
        }

        if (bytesCopied == Length) break;

        reinterpret_cast<BYTE*>(currentMdl->MappedSystemVa)[offset] =
            reinterpret_cast<const BYTE*>(Buffer)[bytesCopied];

        ++offset;
        ++bytesCopied;
    }

    *MdlPtr = currentMdl;
    *MdlOffsetPtr = offset;
    return bytesCopied;
}

_Use_decl_annotations_
size_t AUXSPI_DEVICE::copyBytesFromMdl (
    PMDL* MdlPtr,
    size_t* MdlOffsetPtr,
    BYTE* Buffer,
    size_t Length
    )
{
    PMDL currentMdl = *MdlPtr;
    size_t offset = *MdlOffsetPtr;

    NT_ASSERT(currentMdl);

    size_t bytesCopied = 0;
    for (;;) {
        if (offset == MmGetMdlByteCount(currentMdl)) {
            currentMdl = currentMdl->Next;
            offset = 0;
            if (!currentMdl) break;
            continue;
        }

        if (bytesCopied == Length) break;

        Buffer[bytesCopied] = reinterpret_cast<const BYTE*>(
                currentMdl->MappedSystemVa)[offset];

        ++offset;
        ++bytesCopied;
    }

    *MdlPtr = currentMdl;
    *MdlOffsetPtr = offset;
    return bytesCopied;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DEVICE::processRequestCompletion (
    const _INTERRUPT_CONTEXT* InterruptContextPtr,
    ULONG_PTR* InformationPtr
    )
{
    switch (InterruptContextPtr->Request.TransferState) {
    case _TRANSFER_STATE::WRITE:
        // All bytes should have been written
        NT_ASSERT(
            InterruptContextPtr->Request.Write.BytesWritten ==
            InterruptContextPtr->Request.Write.BytesToWrite);
        *InformationPtr = InterruptContextPtr->Request.Write.BytesWritten;
        return STATUS_SUCCESS;
    case _TRANSFER_STATE::READ:
        // All bytes should have been read
        NT_ASSERT(
            InterruptContextPtr->Request.Read.BytesRead ==
            InterruptContextPtr->Request.Read.BytesToRead);

        *InformationPtr = InterruptContextPtr->Request.Read.BytesRead;
        return STATUS_SUCCESS;
    case _TRANSFER_STATE::FULL_DUPLEX:
        NT_ASSERT(
            InterruptContextPtr->Request.Sequence.BytesWritten ==
            InterruptContextPtr->Request.Sequence.BytesRead);
        __fallthrough;
    case _TRANSFER_STATE::SEQUENCE_READ:
        NT_ASSERT(
            InterruptContextPtr->Request.Sequence.BytesWritten ==
            InterruptContextPtr->Request.Sequence.BytesToWrite);
        NT_ASSERT(
            InterruptContextPtr->Request.Sequence.BytesRead ==
            InterruptContextPtr->Request.Sequence.BytesToRead);

        *InformationPtr =
            InterruptContextPtr->Request.Sequence.BytesWritten +
            InterruptContextPtr->Request.Sequence.BytesRead;
        return STATUS_SUCCESS;
    default:
        NT_ASSERT(FALSE);
        *InformationPtr = 0;
        return STATUS_INTERNAL_ERROR;
    }
}

AUXSPI_DEVICE::_CONTROL_REGS AUXSPI_DEVICE::computeControlRegisters (
    const _TARGET_CONTEXT* TargetContextPtr,
    _FIFO_MODE FifoMode
    )
{
    BCM_AUXSPI_CNTL0_REG cntl0Reg = {0};
    cntl0Reg.ClearFifos = 0;
    cntl0Reg.ShiftOutMsbFirst = 1;
    switch (TargetContextPtr->DataMode) {
    case _SPI_DATA_MODE::Mode0:
        cntl0Reg.InvertSpiClk = 0;
        cntl0Reg.OutRising = 0;
        cntl0Reg.InRising = 1;
        break;
    case _SPI_DATA_MODE::Mode1:
        cntl0Reg.InvertSpiClk = 0;
        cntl0Reg.OutRising = 1;
        cntl0Reg.InRising = 0;
        break;
    case _SPI_DATA_MODE::Mode2:
        cntl0Reg.InvertSpiClk = 1;
        cntl0Reg.OutRising = 1;
        cntl0Reg.InRising = 0;
        break;
    case _SPI_DATA_MODE::Mode3:
        cntl0Reg.InvertSpiClk = 1;
        cntl0Reg.OutRising = 0;
        cntl0Reg.InRising = 1;
        break;
    default:
        NT_ASSERT(!"DataMode should have been validated in TargetConnect");
    }

    cntl0Reg.Enable = 1;
    cntl0Reg.DoutHoldTime = 0;

    switch (FifoMode) {
    case _FIFO_MODE::FIXED_4:
        cntl0Reg.VariableWidth = 0;
        cntl0Reg.ShiftLength = 32;
        break;
    case _FIFO_MODE::VARIABLE_3:;
        cntl0Reg.VariableWidth = 1;
        cntl0Reg.ShiftLength = 0;
        break;
    case _FIFO_MODE::FIXED_3_SHIFTED:
        cntl0Reg.VariableWidth = 0;
        cntl0Reg.ShiftLength = 24;
        break;
    case _FIFO_MODE::VARIABLE_2_SHIFTED:
        cntl0Reg.VariableWidth = 1;
        cntl0Reg.ShiftLength = 0;
        break;
    default:
        NT_ASSERT(FALSE);
    }

    cntl0Reg.VariableCs = 0;
    cntl0Reg.PostInputMode = 0;
    cntl0Reg.ChipSelects = 0x7 & ~(1 << ULONG(TargetContextPtr->ChipSelectLine));

    // From datasheet: spi_clk_freq = system_clock_freq / (2 * (speed + 1))
    const ULONG systemClockFreq = AUXSPI_DRIVER::SystemClockFrequency();
    int speed = (systemClockFreq / (2 * TargetContextPtr->ClockFrequency)) - 1;
    NT_ASSERT((speed >= 0) && (speed < (1 << 12)));
    cntl0Reg.Speed = static_cast<ULONG>(speed);

    BCM_AUXSPI_CNTL1_REG cntl1Reg = {0};
    cntl1Reg.KeepInput = 0;
    cntl1Reg.ShiftInMsbFirst = 1;
    cntl1Reg.DoneIrq = 0;
    cntl1Reg.TxEmptyIrq = 0;
    cntl1Reg.CsHighTime = 0;

    return _CONTROL_REGS{cntl0Reg, cntl1Reg};
}

//
// Begin asserting the chip select line. It takes around 3 SCK cycles
// for the chip select line to finish asserting. Call assertCsComplete()
// to wait for CS to finish asserting, and to put the control registers
// into a state where data may be written.
//
void AUXSPI_DEVICE::assertCsBegin (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const _CONTROL_REGS& ControlRegs
    )
{

#ifdef DBG

    //
    // FIFOs should always be in reset before a transfer is started
    //
    {
        BCM_AUXSPI_CNTL0_REG dbgCntl0 =
            {READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->Cntl0Reg)};
        NT_ASSERT(dbgCntl0.ClearFifos);
    }

#endif // DBG

    // Interrupts should be disabled
    NT_ASSERT(!ControlRegs.Cntl1Reg.DoneIrq && !ControlRegs.Cntl1Reg.TxEmptyIrq);

    WRITE_REGISTER_NOFENCE_ULONG(
        &RegistersPtr->Cntl1Reg,
        ControlRegs.Cntl1Reg.AsUlong);

    // set up zero shift length for initial CS assertion
    BCM_AUXSPI_CNTL0_REG cntl0Reg = ControlRegs.Cntl0Reg;
    cntl0Reg.VariableWidth = 0;
    cntl0Reg.ShiftLength = 0;
    WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->Cntl0Reg, cntl0Reg.AsUlong);

    // Assert CS
    WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->TxHoldReg, 0);
}

//
// Call this function to wait for the chip select line to finish asserting,
// and to put the control registers into a state where data may be written.
// This function is optimized to skip the spin wait for fast clock speeds,
// where the time to assert CS may only be a few microseconds.
//
void AUXSPI_DEVICE::assertCsComplete (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    const _CONTROL_REGS& ControlRegs
    )
{
    BCM_AUXSPI_STAT_REG statReg =
        {READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->StatReg)};
    for (ULONG spinCount = 100; statReg.Busy && spinCount; --spinCount) {
        // stall for 3 SCK cycles (0 is OK)
        const ULONG clockFrequency = AUXSPI_DRIVER::SystemClockFrequency() /
                                    (2 * (ControlRegs.Cntl0Reg.Speed + 1));
        KeStallExecutionProcessor(3000000 / clockFrequency);
        statReg.AsUlong = READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->StatReg);
    }
    NT_ASSERT(!statReg.Busy);

    // program proper width setting
    NT_ASSERT(
        ControlRegs.Cntl0Reg.ShiftLength ||
        ControlRegs.Cntl0Reg.VariableWidth);
    WRITE_REGISTER_NOFENCE_ULONG(
        &RegistersPtr->Cntl0Reg,
        ControlRegs.Cntl0Reg.AsUlong);

    // clear zero-width item from RX FIFO
    READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->IoReg);
}

void AUXSPI_DEVICE::deassertCs (
    volatile BCM_AUXSPI_REGISTERS* RegistersPtr,
    BCM_AUXSPI_CNTL0_REG Cntl0Reg
    )
{

#ifdef DBG

    //
    // FIFOs should already be in reset
    //
    {
        BCM_AUXSPI_CNTL0_REG cntl0Reg =
            {READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->Cntl0Reg)};
        NT_ASSERT(cntl0Reg.ClearFifos);
    }

#endif // DBG

    // Begin deasserting CS
    BCM_AUXSPI_CNTL0_REG cntl0Reg = Cntl0Reg;
    cntl0Reg.ClearFifos = 0;
    cntl0Reg.VariableWidth = 0;
    cntl0Reg.ShiftLength = 0;

    WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->Cntl0Reg, cntl0Reg.AsUlong);
    WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->IoReg, 0);

    // compute clock frequency from speed field
    const ULONG clockFrequency = AUXSPI_DRIVER::SystemClockFrequency() /
                                    (2 * (cntl0Reg.Speed + 1));

    // wait for CS to finish deasserting
    ULONG spinCount = 100;
    BCM_AUXSPI_STAT_REG statReg;
    do {
        // Stall for 3 SCK cycles
        KeStallExecutionProcessor(3000000 / clockFrequency);
        statReg.AsUlong = READ_REGISTER_NOFENCE_ULONG(&RegistersPtr->StatReg);
    } while (statReg.Busy && --spinCount);
    NT_ASSERT(!statReg.Busy);

    // put FIFOs back in reset
    cntl0Reg.ClearFifos = 1;
    WRITE_REGISTER_NOFENCE_ULONG(&RegistersPtr->Cntl0Reg, cntl0Reg.AsUlong);
}

void AUXSPI_DEVICE::abortTransfer ( _INTERRUPT_CONTEXT* InterruptContextPtr )
{
    volatile BCM_AUXSPI_REGISTERS* registersPtr = InterruptContextPtr->RegistersPtr;

    // mark transfer invalid
    InterruptContextPtr->Request.TransferState = _TRANSFER_STATE::INVALID;

    // Clear FIFOs
    BCM_AUXSPI_CNTL0_REG cntl0Reg = InterruptContextPtr->ControlRegs.Cntl0Reg;
    cntl0Reg.ClearFifos = 1;
    WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl0Reg, cntl0Reg.AsUlong);

    // deassert CS if the controller is not locked
    if (!InterruptContextPtr->SpbControllerLocked) {
        deassertCs(registersPtr, cntl0Reg);
    }
}

AUXSPI_DEVICE::_FIFO_MODE AUXSPI_DEVICE::selectFifoMode (
    _SPI_DATA_MODE SpiDataMode,
    size_t Length
    )
{
    switch (SpiDataMode) {
    case _SPI_DATA_MODE::Mode0:
    case _SPI_DATA_MODE::Mode2:
        return ((Length % sizeof(ULONG)) == 0) ?
            _FIFO_MODE::FIXED_4 : _FIFO_MODE::VARIABLE_3;
    case _SPI_DATA_MODE::Mode1:
    case _SPI_DATA_MODE::Mode3:
        return ((Length % 3) == 0) ?
            _FIFO_MODE::FIXED_3_SHIFTED : _FIFO_MODE::VARIABLE_2_SHIFTED;
    default:
        NT_ASSERT(FALSE);
        return _FIFO_MODE(-1);
    }
}

ULONG AUXSPI_DEVICE::getMinClock ()
{
    return 1 + AUXSPI_DRIVER::SystemClockFrequency() / (2 * ((1 << 12) + 1));
}

AUXSPI_NONPAGED_SEGMENT_END; //================================================
AUXSPI_PAGED_SEGMENT_BEGIN; //=================================================

_Use_decl_annotations_
NTSTATUS AUXSPI_DEVICE::EvtSpbTargetConnect (
    WDFDEVICE /*WdfDevice*/,
    SPBTARGET SpbTarget
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    //
    // SPI Connection Descriptor, defined in ACPI 5.0 spec table 6-192
    //
    #include <pshpack1.h>
    struct PNP_SPI_SERIAL_BUS_DESCRIPTOR : public PNP_SERIAL_BUS_DESCRIPTOR {
        ULONG ConnectionSpeed;
        UCHAR DataBitLength;
        UCHAR Phase;
        UCHAR Polarity;
        USHORT DeviceSelection;
        // follwed by optional Vendor Data
        // followed by PNP_IO_DESCRIPTOR_RESOURCE_NAME
    };
    #include <poppack.h> // pshpack1.h

    //
    // See section 6.4.3.8.2 of the ACPI 5.0 specification
    //
    enum PNP_SERIAL_BUS_TYPE {
        PNP_SERIAL_BUS_TYPE_I2C = 0x1,
        PNP_SERIAL_BUS_TYPE_SPI = 0x2,
        PNP_SERIAL_BUS_TYPE_UART = 0x3,
    };

    enum : ULONG {
        PNP_SERIAL_GENERAL_FLAGS_SLV_BIT = 0x1, // 0 = ControllerInitiated, 1 = DeviceInitiated
        PNP_SPI_WIREMODE_BIT = 0x1,             // 0 = FourWireMode, 1 = ThreeWireMode
        PNP_SPI_DEVICEPOLARITY_BIT = 0x2,       // 0 = ActiveLow, 1 = ActiveHigh
    };

    //
    // Get ACPI descriptor
    //
    const PNP_SPI_SERIAL_BUS_DESCRIPTOR* spiDescriptorPtr;
    {
        SPB_CONNECTION_PARAMETERS params;
        SPB_CONNECTION_PARAMETERS_INIT(&params);

        SpbTargetGetConnectionParameters(SpbTarget, &params);

        const auto rhBufferPtr =
            static_cast<RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER*>(
                params.ConnectionParameters);
        if (rhBufferPtr->PropertiesLength < sizeof(*spiDescriptorPtr)) {
            AUXSPI_LOG_ERROR(
                "Connection properties is too small. (rhBufferPtr->PropertiesLength = %lu, sizeof(*spiDescriptorPtr) = %lu)",
                rhBufferPtr->PropertiesLength,
                sizeof(*spiDescriptorPtr));
            return STATUS_INVALID_PARAMETER;
        }

        spiDescriptorPtr = reinterpret_cast<PNP_SPI_SERIAL_BUS_DESCRIPTOR*>(
            &rhBufferPtr->ConnectionProperties);

        if (spiDescriptorPtr->SerialBusType != PNP_SERIAL_BUS_TYPE_SPI) {
            AUXSPI_LOG_ERROR(
                "ACPI Connnection descriptor is not an SPI connection descriptor. (spiDescriptorPtr->SerialBusType = 0x%lx, PNP_SERIAL_BUS_TYPE_SPI = 0x%lx)",
                spiDescriptorPtr->SerialBusType,
                PNP_SERIAL_BUS_TYPE_SPI);
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (spiDescriptorPtr->GeneralFlags & PNP_SERIAL_GENERAL_FLAGS_SLV_BIT) {
        AUXSPI_LOG_ERROR("Auxspi does not support slave mode.");
        return STATUS_NOT_SUPPORTED;
    }

    if (spiDescriptorPtr->TypeSpecificFlags & PNP_SPI_WIREMODE_BIT) {
        AUXSPI_LOG_ERROR("Auxspi does not support 3-wire mode.");
        return STATUS_NOT_SUPPORTED;
    }

    if (spiDescriptorPtr->TypeSpecificFlags & PNP_SPI_DEVICEPOLARITY_BIT) {
        AUXSPI_LOG_ERROR("Auxspi does not support inverted device polarity (not implemented).");
        return STATUS_NOT_SUPPORTED;
    }

    if ((spiDescriptorPtr->ConnectionSpeed > getMaxClock()) ||
        (spiDescriptorPtr->ConnectionSpeed < getMinClock())) {

        AUXSPI_LOG_ERROR(
            "Clock speed is out of range. (spiDescriptorPtr->ConnectionSpeed = %lu, BCM_AUXSPI_MAX_CLOCK = %lu, BCM_AUXSPI_MIN_CLOCK = %lu)",
            spiDescriptorPtr->ConnectionSpeed,
            getMaxClock(),
            getMinClock());
        return STATUS_NOT_SUPPORTED;
    }

    if (spiDescriptorPtr->DataBitLength != 8) {
        AUXSPI_LOG_ERROR(
            "Only 8-bit data bit length is supported. (spiDescriptorPtr->DataBitLength = %d)",
            spiDescriptorPtr->DataBitLength);
        return STATUS_NOT_SUPPORTED;
    }

    _SPI_DATA_MODE mode;
    if (spiDescriptorPtr->Polarity) {
        mode = spiDescriptorPtr->Phase ? _SPI_DATA_MODE::Mode3 : _SPI_DATA_MODE::Mode2;
    } else {
        mode = spiDescriptorPtr->Phase ? _SPI_DATA_MODE::Mode1 : _SPI_DATA_MODE::Mode0;
    }

    _CHIP_SELECT_LINE chipSelectLine;
    switch (spiDescriptorPtr->DeviceSelection) {
    case 0:
        chipSelectLine = _CHIP_SELECT_LINE::CE0;
        break;
    case 1:
        chipSelectLine = _CHIP_SELECT_LINE::CE1;
        break;
    case 2:
        chipSelectLine = _CHIP_SELECT_LINE::CE2;
        break;
    default:
        AUXSPI_LOG_ERROR(
            "Invalid device selection value (must be 0-2). (spiDescriptorPtr->DeviceSelection = %d)",
            spiDescriptorPtr->DeviceSelection);
        return STATUS_INVALID_PARAMETER;
    }

    _TARGET_CONTEXT* targetContextPtr = GetTargetContext(SpbTarget);
    new (targetContextPtr) _TARGET_CONTEXT{
            spiDescriptorPtr->ConnectionSpeed,
            spiDescriptorPtr->DataBitLength,
            mode,
            chipSelectLine};

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DEVICE::EvtDevicePrepareHardware (
    WDFDEVICE WdfDevice,
    WDFCMRESLIST /*ResourcesRaw*/,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    const CM_PARTIAL_RESOURCE_DESCRIPTOR* memResourcePtr = nullptr;
    ULONG memResourceCount = 0;
    ULONG interruptResourceCount = 0;

    // Look for single memory resource and single interrupt resource
    const ULONG resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < resourceCount; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* resourcePtr =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        switch (resourcePtr->Type) {
        case CmResourceTypeMemory:
            switch (memResourceCount) {
            case 0:
                memResourcePtr = resourcePtr;
                break;
            default:
                AUXSPI_LOG_WARNING(
                    "Received unexpected memory resource. (memResourceCount = %lu, resourcePtr = 0x%p)",
                    memResourceCount,
                    resourcePtr);
            }
            ++memResourceCount;
            break;
        case CmResourceTypeInterrupt:
            switch (interruptResourceCount) {
            case 0: break;
            default:
                AUXSPI_LOG_WARNING(
                    "Received unexpected interrupt resource. (interruptResourceCount = %lu, resourcePtr = 0x%p)",
                    interruptResourceCount,
                    resourcePtr);
            }
            ++interruptResourceCount;
            break;
        }
    }

    if (!memResourcePtr ||
        (memResourcePtr->u.Memory.Length < sizeof(BCM_AUXSPI_REGISTERS)) ||
        (interruptResourceCount == 0)) {

        AUXSPI_LOG_ERROR(
            "Did not receive required memory resource and interrupt resource. "
            "(ResourcesTranslated = %p, memResourceCount = %lu, interruptResourceCount = %lu)",
            ResourcesTranslated,
            memResourceCount,
            interruptResourceCount);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // determine whether we're SPI1 or SPI2
    const ULONG peripheralOffset = memResourcePtr->u.Memory.Start.LowPart & 0xfff;
    switch (peripheralOffset) {
    case FIELD_OFFSET(BCM_AUX_REGISTERS, Spi1):
    case FIELD_OFFSET(BCM_AUX_REGISTERS, Spi2):
        break;
    default:
        AUXSPI_LOG_ERROR(
            "Peripheral offset is not SPI1 or SPI2. (peripheralOffset = 0x%x, Spi1Offset = 0x%x, Spi2Offset = 0x%x)",
            peripheralOffset,
            FIELD_OFFSET(BCM_AUX_REGISTERS, Spi1),
            FIELD_OFFSET(BCM_AUX_REGISTERS, Spi2));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // ReleaseHardware is ALWAYS called, even if PrepareHardware fails, so
    // the cleanup of registersPtr is handled there.
    //
    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    NT_ASSERT(memResourcePtr->Type == CmResourceTypeMemory);
    PHYSICAL_ADDRESS pageAlignedPhysAddress = memResourcePtr->u.Memory.Start;
    pageAlignedPhysAddress.LowPart &= ~0xfff;
    thisPtr->auxRegistersPtr = static_cast<BCM_AUX_REGISTERS*>(MmMapIoSpaceEx(
            pageAlignedPhysAddress,
            sizeof(BCM_AUX_REGISTERS),
            PAGE_READWRITE | PAGE_NOCACHE));

    if (!thisPtr->auxRegistersPtr) {
        AUXSPI_LOG_LOW_MEMORY(
            "Failed to map registers - returning STATUS_INSUFFICIENT_RESOURCES. (memResourcePtr->u.Memory.Start = 0x%llx, memResourcePtr->u.Memory.Length = %lu)",
            memResourcePtr->u.Memory.Start.QuadPart,
            memResourcePtr->u.Memory.Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG enableMask;
    volatile BCM_AUXSPI_REGISTERS* registersPtr;
    switch (peripheralOffset) {
    case FIELD_OFFSET(BCM_AUX_REGISTERS, Spi1):
        enableMask = 0x2;   // Spi1Enable
        registersPtr = &thisPtr->auxRegistersPtr->Spi1;
        break;
    case FIELD_OFFSET(BCM_AUX_REGISTERS, Spi2):
        enableMask = 0x4;   // Spi2Enable
        registersPtr = &thisPtr->auxRegistersPtr->Spi2;
        break;
    default:
        NT_ASSERT(!"peripheralOffset should have been validated above");
        return STATUS_INTERNAL_ERROR;
    }

    //
    // Ensure device is enabled. This is a shared register for all devices
    // on the AUX peripheral, so we cannot safely modify it without
    // synchronizing with all the other AUX devices. If the peripheral is not
    // enabled, fail the load of the driver.
    //
    BCM_AUX_ENABLES_REG enablesReg =
        {READ_REGISTER_NOFENCE_ULONG(&thisPtr->auxRegistersPtr->Enables)};

    if (!(enablesReg.AsUlong & enableMask)) {
        NTSTATUS status = queryForceEnableSetting(WdfDeviceGetDriver(WdfDevice));
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "The device is not enabled. The device must be enabled in the "
                "AUX_ENABLES register prior to driver load. "
                "(enablesReg.AsUlong = 0x%x, enableMask = 0x%x)",
                enablesReg.AsUlong,
                enableMask);
            WdfDeviceSetFailed(WdfDevice, WdfDeviceFailedNoRestart);
            return STATUS_DEVICE_HARDWARE_ERROR;
        }

        AUXSPI_LOG_WARNING(
            "The device is not enabled in the AUX_ENABLES register - "
            "force enabling the device per the ForceEnable registry setting.");

        enablesReg.AsUlong |= enableMask;
        WRITE_REGISTER_NOFENCE_ULONG(
            &thisPtr->auxRegistersPtr->Enables,
            enablesReg.AsUlong);
    }

    thisPtr->registersPtr = registersPtr;

    // Ensure controller and interrupts are disabled
    BCM_AUXSPI_CNTL0_REG cntl0Reg = {0};
    cntl0Reg.ClearFifos = 1;
    WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl0Reg, cntl0Reg.AsUlong);

    BCM_AUXSPI_CNTL1_REG cntl1Reg = {0};
    WRITE_REGISTER_NOFENCE_ULONG(&registersPtr->Cntl1Reg, cntl1Reg.AsUlong);

    // initialize interrupt context
    thisPtr->interruptContextPtr =
        new (GetInterruptContext(thisPtr->wdfInterrupt)) _INTERRUPT_CONTEXT(
            thisPtr->auxRegistersPtr,
            registersPtr);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DEVICE::EvtDeviceReleaseHardware (
    WDFDEVICE WdfDevice,
    WDFCMRESLIST /*ResourcesTranslated*/
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    AUXSPI_DEVICE* thisPtr = GetDeviceContext(WdfDevice);
    if (thisPtr->auxRegistersPtr) {
        MmUnmapIoSpace(
            const_cast<BCM_AUX_REGISTERS*>(thisPtr->auxRegistersPtr), // cast away volatile
            sizeof(BCM_AUX_REGISTERS));

        thisPtr->auxRegistersPtr = nullptr;
        thisPtr->registersPtr = nullptr;
    }

    return STATUS_SUCCESS;
}

//
// Returns:
//   STATUS_SUCCESS if the force enable setting is enabled
//   Other NTSTATUS - the force enable setting is not enabled or an error occurred
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS AUXSPI_DEVICE::queryForceEnableSetting ( WDFDRIVER WdfDriver )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    struct _LOCAL_KEY {
        WDFKEY WdfKey = WDF_NO_HANDLE;
        ~_LOCAL_KEY ()
        {
            PAGED_CODE();
            if (WdfKey == WDF_NO_HANDLE) return;
            WdfRegistryClose(WdfKey);
        }
    } key;
    NTSTATUS status = WdfDriverOpenParametersRegistryKey(
            WdfDriver,
            KEY_QUERY_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES,
            &key.WdfKey);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "Failed to open driver registry key. (status = %!STATUS!)",
            status);
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(valueName, REGSTR_VAL_AUXSPI_FORCE_ENABLE);
    ULONG forceEnable;
    status = WdfRegistryQueryULong(
            key.WdfKey,
            &valueName,
            &forceEnable);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "WdfRegistryQueryULong(...) failed. (valueName = %wZ)",
            &valueName);
        return status;
    }

    return (forceEnable != 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DRIVER::EvtDriverDeviceAdd (
    WDFDRIVER /*WdfDriver*/,
    WDFDEVICE_INIT* DeviceInitPtr
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    NTSTATUS status;

    //
    // Configure DeviceInit structure
    //
    status = SpbDeviceInitConfig(DeviceInitPtr);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "SpbDeviceInitConfig() failed. (DeviceInitPtr = %p, status = %!STATUS!)",
            DeviceInitPtr,
            status);
        return status;
    }

    //
    // Setup PNP/Power callbacks.
    //
    {
        WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

        pnpCallbacks.EvtDevicePrepareHardware =
                AUXSPI_DEVICE::EvtDevicePrepareHardware;
        pnpCallbacks.EvtDeviceReleaseHardware =
                AUXSPI_DEVICE::EvtDeviceReleaseHardware;

        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInitPtr, &pnpCallbacks);
    }

    //
    // Create the device.
    //
    WDFDEVICE wdfDevice;

    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AUXSPI_DEVICE);

        status = WdfDeviceCreate(&DeviceInitPtr, &attributes, &wdfDevice);
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "Failed to create WDFDEVICE. (DeviceInitPtr = %p, status = %!STATUS!)",
                DeviceInitPtr,
                status);
            return status;
        }

        // We want to be able to read/write buffers in ULONG-sized chunks
        WdfDeviceSetAlignmentRequirement(wdfDevice, FILE_LONG_ALIGNMENT);
    }

    //
    // Bind an SPB controller object to the device.
    //
    {
        SPB_CONTROLLER_CONFIG spbConfig;
        SPB_CONTROLLER_CONFIG_INIT(&spbConfig);

        spbConfig.ControllerDispatchType = WdfIoQueueDispatchSequential;

        spbConfig.EvtSpbTargetConnect = AUXSPI_DEVICE::EvtSpbTargetConnect;
        spbConfig.EvtSpbControllerLock = AUXSPI_DEVICE::EvtSpbControllerLock;
        spbConfig.EvtSpbControllerUnlock = AUXSPI_DEVICE::EvtSpbControllerUnlock;
        spbConfig.EvtSpbIoRead = AUXSPI_DEVICE::EvtSpbIoRead;
        spbConfig.EvtSpbIoWrite = AUXSPI_DEVICE::EvtSpbIoWrite;
        spbConfig.EvtSpbIoSequence = AUXSPI_DEVICE::EvtSpbIoSequence;

        status = SpbDeviceInitialize(wdfDevice, &spbConfig);
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "SpbDeviceInitialize failed. (wdfDevice = %p, status = %!STATUS!)",
                wdfDevice,
                status);
            return status;
        }

        // Register for other ("full duplex") callbacks
        SpbControllerSetIoOtherCallback(
            wdfDevice,
            AUXSPI_DEVICE::EvtSpbIoOther,
            AUXSPI_DEVICE::EvtIoInCallerContext);
    }

    //
    // Set target object attributes.
    //
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &attributes,
            _AUXSPI_TARGET_CONTEXT);

        SpbControllerSetTargetAttributes(wdfDevice, &attributes);
    }

    //
    // Create an interrupt object
    //
    WDFINTERRUPT wdfInterrupt;
    {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &attributes,
            _AUXSPI_INTERRUPT_CONTEXT);

        WDF_INTERRUPT_CONFIG interruptConfig;
        WDF_INTERRUPT_CONFIG_INIT(
            &interruptConfig,
            AUXSPI_DEVICE::EvtInterruptIsr,
            AUXSPI_DEVICE::EvtInterruptDpc);

        status = WdfInterruptCreate(
                wdfDevice,
                &interruptConfig,
                &attributes,
                &wdfInterrupt);
        if (!NT_SUCCESS(status)) {
            AUXSPI_LOG_ERROR(
                "Failed to create interrupt object. (wdfDevice = %p, status = %!STATUS!)",
                wdfDevice,
                status);
            return status;
        }
    }

    new (GetDeviceContext(wdfDevice)) AUXSPI_DEVICE(wdfDevice, wdfInterrupt);

    NT_ASSERT(NT_SUCCESS(status));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID AUXSPI_DRIVER::EvtDriverUnload ( WDFDRIVER WdfDriver )
{
    PAGED_CODE();

    DRIVER_OBJECT* driverObjectPtr = WdfDriverWdmGetDriverObject(WdfDriver);
    WPP_CLEANUP(driverObjectPtr);
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DRIVER::QuerySystemClockFrequency (ULONG* ClockFrequencyPtr)
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    DECLARE_CONST_UNICODE_STRING(rpiqDeviceName, RPIQ_SYMBOLIC_NAME);

    struct _LOCAL_FILE_OBJECT {
        FILE_OBJECT* Ptr = nullptr;
        ~_LOCAL_FILE_OBJECT ()
        {
            PAGED_CODE();
            if (!this->Ptr) return;
            ObDereferenceObjectWithTag(this->Ptr, AUXSPI_POOL_TAG);
        }
    } rpiqFileObject;
    NTSTATUS status = OpenDevice(
            &rpiqDeviceName,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &rpiqFileObject.Ptr);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "Failed to open handle to RPIQ. (status = %!STATUS!, rpiqDeviceName = %wZ)",
            status,
            &rpiqDeviceName);
        return status;
    }

    // Build input buffer to query clock
    MAILBOX_GET_CLOCK_RATE inputBuffer;
    INIT_MAILBOX_GET_CLOCK_RATE(&inputBuffer, MAILBOX_CLOCK_ID_CORE);

    ULONG_PTR information;
    status = SendIoctlSynchronously(
            rpiqFileObject.Ptr,
            IOCTL_MAILBOX_PROPERTY,
            &inputBuffer,
            sizeof(inputBuffer),
            &inputBuffer,
            sizeof(inputBuffer),
            FALSE,                  // InternalDeviceIoControl
            &information);
    if (!NT_SUCCESS(status) ||
       (inputBuffer.Header.RequestResponse != RESPONSE_SUCCESS)) {

        AUXSPI_LOG_ERROR(
            "SendIoctlSynchronously(...IOCTL_MAILBOX_PROPERTY...) failed. (status = %!STATUS!, inputBuffer.Header.RequestResponse = 0x%x)",
            status,
            inputBuffer.Header.RequestResponse);
        return status;
    }

    AUXSPI_LOG_INFORMATION(
        "Successfully queried system core clock. (inputBuffer.Rate = %d Hz)",
        inputBuffer.Rate);

    *ClockFrequencyPtr = inputBuffer.Rate;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DRIVER::OpenDevice (
    const UNICODE_STRING* FileNamePtr,
    ACCESS_MASK DesiredAccess,
    ULONG ShareAccess,
    FILE_OBJECT** FileObjectPPtr
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(
        &attributes,
        const_cast<UNICODE_STRING*>(FileNamePtr),
        OBJ_KERNEL_HANDLE,
        NULL,       // RootDirectory
        NULL);      // SecurityDescriptor

    HANDLE fileHandle;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = ZwCreateFile(
            &fileHandle,
            DesiredAccess,
            &attributes,
            &iosb,
            nullptr,                    // AllocationSize
            FILE_ATTRIBUTE_NORMAL,
            ShareAccess,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE,    // CreateOptions
            nullptr,                    // EaBuffer
            0);                         // EaLength
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "ZwCreateFile(...) failed. (status = %!STATUS!, FileNamePtr = %wZ, DesiredAccess = 0x%x, ShareAccess = 0x%x)",
            status,
            FileNamePtr,
            DesiredAccess,
            ShareAccess);
        return status;
    }

    status = ObReferenceObjectByHandleWithTag(
            fileHandle,
            DesiredAccess,
            *IoFileObjectType,
            KernelMode,
            AUXSPI_POOL_TAG,
            reinterpret_cast<PVOID*>(FileObjectPPtr),
            nullptr);   // HandleInformation

    NTSTATUS closeStatus = ZwClose(fileHandle);
    UNREFERENCED_PARAMETER(closeStatus);
    NT_ASSERT(NT_SUCCESS(closeStatus));

    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "ObReferenceObjectByHandleWithTag(...) failed. (status = %!STATUS!)",
            status);
        return status;
    }

    NT_ASSERT(*FileObjectPPtr);
    NT_ASSERT(NT_SUCCESS(status));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AUXSPI_DRIVER::SendIoctlSynchronously (
    FILE_OBJECT* FileObjectPtr,
    ULONG IoControlCode,
    void* InputBufferPtr,
    ULONG InputBufferLength,
    void* OutputBufferPtr,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    ULONG_PTR* InformationPtr
    )
{
    PAGED_CODE();
    AUXSPI_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    DEVICE_OBJECT* deviceObjectPtr = IoGetRelatedDeviceObject(FileObjectPtr);
    auto iosb = IO_STATUS_BLOCK();
    IRP* irpPtr = IoBuildDeviceIoControlRequest(
            IoControlCode,
            deviceObjectPtr,
            InputBufferPtr,
            InputBufferLength,
            OutputBufferPtr,
            OutputBufferLength,
            InternalDeviceIoControl,
            &event,
            &iosb);
    if (!irpPtr) {
        AUXSPI_LOG_LOW_MEMORY(
            "IoBuildDeviceIoControlRequest(...) failed. (IoControlCode=0x%x, deviceObjectPtr=%p, FileObjectPtr=%p)",
            IoControlCode,
            deviceObjectPtr,
            FileObjectPtr);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IO_STACK_LOCATION* irpSp = IoGetNextIrpStackLocation(irpPtr);
    irpSp->FileObject = FileObjectPtr;

    iosb.Status = STATUS_NOT_SUPPORTED;
    NTSTATUS status = IoCallDriver(deviceObjectPtr, irpPtr);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(
            &event,
            Executive,
            KernelMode,
            FALSE,          // Alertable
            nullptr);       // Timeout

        status = iosb.Status;
    }

    *InformationPtr = iosb.Information;
    return status;
}

AUXSPI_PAGED_SEGMENT_END; //===================================================
AUXSPI_INIT_SEGMENT_BEGIN; //==================================================

_Use_decl_annotations_
NTSTATUS DriverEntry (
    DRIVER_OBJECT* DriverObjectPtr,
    UNICODE_STRING* RegistryPathPtr
    )
{
    PAGED_CODE();

    //
    // Initialize logging
    //
    {
        WPP_INIT_TRACING(DriverObjectPtr, RegistryPathPtr);
        RECORDER_CONFIGURE_PARAMS recorderConfigureParams;
        RECORDER_CONFIGURE_PARAMS_INIT(&recorderConfigureParams);
        WppRecorderConfigure(&recorderConfigureParams);
#if DBG
        WPP_RECORDER_LEVEL_FILTER(AUXSPI_TRACING_DEFAULT) = TRUE;
#endif // DBG
    }

    //
    // Query system clock frequency from RPIQ
    //
    NTSTATUS status = AUXSPI_DRIVER::QuerySystemClockFrequency(
            &AUXSPI_DRIVER::systemClockFrequency);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_WARNING(
            "Failed to query system clock frequency from RPIQ - falling back to default. (status = %!STATUS!, BCM_DEFAULT_SYSTEM_CLOCK_FREQ = %d)",
            status,
            BCM_DEFAULT_SYSTEM_CLOCK_FREQ);

        AUXSPI_DRIVER::systemClockFrequency = BCM_DEFAULT_SYSTEM_CLOCK_FREQ;
    }

    WDF_DRIVER_CONFIG wdfDriverConfig;
    WDF_DRIVER_CONFIG_INIT(&wdfDriverConfig, AUXSPI_DRIVER::EvtDriverDeviceAdd);
    wdfDriverConfig.DriverPoolTag = AUXSPI_POOL_TAG;
    wdfDriverConfig.EvtDriverUnload = AUXSPI_DRIVER::EvtDriverUnload;

    WDFDRIVER wdfDriver;
    status = WdfDriverCreate(
            DriverObjectPtr,
            RegistryPathPtr,
            WDF_NO_OBJECT_ATTRIBUTES,
            &wdfDriverConfig,
            &wdfDriver);
    if (!NT_SUCCESS(status)) {
        AUXSPI_LOG_ERROR(
            "Failed to create WDF driver object. (DriverObjectPtr = %p, RegistryPathPtr = %p)",
            DriverObjectPtr,
            RegistryPathPtr);
        return status;
    }

    return STATUS_SUCCESS;
}

AUXSPI_INIT_SEGMENT_END; //====================================================