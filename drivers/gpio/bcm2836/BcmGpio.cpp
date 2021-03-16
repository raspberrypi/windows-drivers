//
// Copyright (C) Microsoft.  All rights reserved.
//
//
// Module Name:
//
//    BcmGpio.Cpp
//
// Abstract:
//
//    BCM2835/2836 GPIO controller driver.
//

#include "precomp.hpp"
#pragma hdrstop

#include "BcmUtility.hpp"
#include "BcmGpio.hpp"

BCM_NONPAGED_SEGMENT_BEGIN; //=================================================

//
// Placement new definition. Note <new> cannot be included in kernel mode.
//

void* __cdecl operator new (size_t, void* Ptr) throw ()
{
    return Ptr;
} // operator new (size_t, void*)

//
// Definition of delete operator for C++ modules.
//

void __cdecl operator delete (
    void* Ptr
    ) throw ()
{
    if (Ptr) ExFreePool(Ptr);
} // operator delete (void*)

//
// Definition of delete operator for C++ modules. VS2015 compatible.
//

void __cdecl operator delete (void*, unsigned int) throw ()
{} // void operator delete (void*, unsigned int)

void __cdecl operator delete (void*, unsigned __int64) throw ()
{} // void operator delete (void*, unsigned __int64)

namespace { // static

    typedef BCM_GPIO::_DPC_CONTEXT DPC_CONTEXT;

    WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
        DPC_CONTEXT,
        bcmGpioDpcContextFromWdfObject);

    typedef BCM_GPIO::_DPC_CONTEXT TIMER_CONTEXT;

    WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
        TIMER_CONTEXT,
        bcmGpioTimerContextFromWdfObject);

} // namespace static

//
// Register interrupt for pins in the supplied mask in the supplied mode
//
void BCM_GPIO::_INTERRUPT_REGISTERS::Add (
    ULONG Mask,
    KINTERRUPT_MODE InterruptMode,
    KINTERRUPT_POLARITY Polarity
    )
{
    switch (InterruptMode) {
    case LevelSensitive:
        switch (Polarity) {
        case InterruptActiveHigh:
            this->GPHEN |= Mask;
            break;
        case InterruptActiveLow:
            this->GPLEN |= Mask;
            break;
        default:
            NT_ASSERT(!"Invalid interrupt mode/level");
            return;
        } // switch (Polarity)
        break;
    case Latched:
        switch (Polarity) {
        case InterruptRisingEdge:
            this->GPAREN |= Mask;
            break;
        case InterruptFallingEdge:
            this->GPAFEN |= Mask;
            break;
        case InterruptActiveBoth:
        default:
            NT_ASSERT(!"Invalid interrupt mode/level");
            return;
        } // switch (Polarity)
        break;
    default:
        NT_ASSERT(!"Invalid interrupt mode/level");
        return;
    } // switch (InterruptMode)
} // BCM_GPIO::_INTERRUPT_REGISTERS::Add (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::PreProcessControllerInterrupt (
    PVOID ContextPtr,
    BANK_ID BankId,
    ULONG64 /*EnabledMask*/
    )
{
    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    _INTERRUPT_CONTEXT* interruptContextPtr =
        thisPtr->interruptContext + BankId;

    const ULONG changedMask =
        READ_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[BankId]);

    // do watchdog accounting and temporarily disable pins that are causing
    // an interrupt storm
    ULONG disableMask = 0;
    ULONG i = 0;
    ULONG firstSetIndex;
    while (_BitScanForward(&firstSetIndex, changedMask >> i)) {
        i += firstSetIndex;
        NT_ASSERT(i < ARRAYSIZE(interruptContextPtr->watchdogCount));
        NT_ASSERT(interruptContextPtr->watchdogCount[i] <= WATCHDOG_RESET);

        if (--interruptContextPtr->watchdogCount[i] == 0) {
            disableMask |= 1 << i;
        }
        ++i;
    }

    // move interrupts from the enabled list to the disabled list
    if (disableMask) {
        interruptContextPtr->enabledMask &= ~disableMask;
        interruptContextPtr->disabledMask |= disableMask;
        thisPtr->programInterruptRegisters(BankId);
        WRITE_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[BankId], disableMask);
    }

    WdfDpcEnqueue(interruptContextPtr->dpc);

    return STATUS_SUCCESS;
}  // BCM_GPIO::PreProcessControllerInterrupt (...)

_Use_decl_annotations_
VOID BCM_GPIO::evtDpcFunc ( WDFDPC WdfDpc )
{
    DPC_CONTEXT* dpcContextPtr = bcmGpioDpcContextFromWdfObject(WdfDpc);
    _INTERRUPT_CONTEXT* interruptContextPtr = dpcContextPtr->interruptContextPtr;
    BCM_GPIO* thisPtr = dpcContextPtr->thisPtr;

    // must schedule timer outside of the interrupt spinlock
    bool scheduleReenableTimer;
    {
        GPIO_CLX_AcquireInterruptLock(
            thisPtr,
            BANK_ID(interruptContextPtr->bankId));

        // move disabled interrupts onto the pending reenable list
        if (interruptContextPtr->disabledMask) {
            interruptContextPtr->pendingReenableMask |=
                interruptContextPtr->disabledMask;
            interruptContextPtr->disabledMask = 0;
            scheduleReenableTimer = true;
        } else {
            scheduleReenableTimer = false;
        }

        interruptContextPtr->resetWatchdogCount();

        GPIO_CLX_ReleaseInterruptLock(
            thisPtr,
            BANK_ID(interruptContextPtr->bankId));
    } // release lock

    // Schedule a timer to reenable the interrupt after a delay. The delay is
    // necessary to allow the storm to clear.
    if (scheduleReenableTimer) {
        WdfTimerStart(
            interruptContextPtr->interruptReenableTimer,
            WDF_REL_TIMEOUT_IN_MS(1));
    }
} // BCM_GPIO::evtDpcFunc (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::MaskInterrupts (
    PVOID ContextPtr,
    PGPIO_MASK_INTERRUPT_PARAMETERS MaskParametersPtr
    )
{
    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = MaskParametersPtr->BankId;
    const ULONG mask = ULONG(MaskParametersPtr->PinMask);

    thisPtr->interruptContext[bankId].registers.Remove(mask);
    thisPtr->programInterruptRegisters(bankId);
    WRITE_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[bankId], mask);

    return STATUS_SUCCESS;
} // BCM_GPIO::MaskInterrupts (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::UnmaskInterrupt (
    PVOID ContextPtr,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS UnmaskParametersPtr
    )
{
    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);

    const BANK_ID bankId = UnmaskParametersPtr->BankId;
    const PIN_NUMBER pinNumber = UnmaskParametersPtr->PinNumber;
    const ULONG mask = 1 << pinNumber;

    thisPtr->interruptContext[bankId].registers.Add(
        mask,
        UnmaskParametersPtr->InterruptMode,
        UnmaskParametersPtr->Polarity);

    WRITE_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[bankId], mask);
    thisPtr->programInterruptRegisters(bankId);

    return STATUS_SUCCESS;
} // BCM_GPIO::UnmaskInterrupt (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::QueryActiveInterrupts (
    PVOID ContextPtr,
    PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS QueryActiveParametersPtr
    )
{
    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);

    QueryActiveParametersPtr->ActiveMask = READ_REGISTER_NOFENCE_ULONG(
        &thisPtr->registersPtr->GPEDS[QueryActiveParametersPtr->BankId]);

    return STATUS_SUCCESS;
} // BCM_GPIO::QueryActiveInterrupts (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ClearActiveInterrupts (
    PVOID ContextPtr,
    PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS ClearParametersPtr
    )
{
    BCM_GPIO_REGISTERS* hw = static_cast<BCM_GPIO*>(ContextPtr)->registersPtr;

    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPEDS[ClearParametersPtr->BankId],
        ULONG(ClearParametersPtr->ClearActiveMask));

    return STATUS_SUCCESS;
} // BCM_GPIO::ClearActiveInterrupts (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::QueryEnabledInterrupts (
    PVOID ContextPtr,
    PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS QueryEnabledParametersPtr
    )
{
    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);

    QueryEnabledParametersPtr->EnabledMask =
        thisPtr->interruptContext[QueryEnabledParametersPtr->BankId].
        registers.EnabledMask();

    return STATUS_SUCCESS;
} // BCM_GPIO::QueryEnabledInterrupts (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ReconfigureInterrupt (
    PVOID /*ContextPtr*/,
    PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS ReconfigureParametersPtr
    )
{
    UNREFERENCED_PARAMETER(ReconfigureParametersPtr);

    // Since MaskInterrupts is always called before ReconfigureInterrupt,
    // and UnmaskInterrupt is always called after ReconfigureInterrupt,
    // it is not necessary for this routine to do anything.

    // Reconfigure is supported only for level sensitive interrupts
    NT_ASSERT(ReconfigureParametersPtr->InterruptMode == LevelSensitive);

    return STATUS_SUCCESS;
} // BCM_GPIO::ReconfigureInterrupt (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ReadGpioPinsUsingMask (
    PVOID ContextPtr,
    PGPIO_READ_PINS_MASK_PARAMETERS ReadParametersPtr
    )
{
    BCM_GPIO_REGISTERS* hw = static_cast<BCM_GPIO*>(ContextPtr)->registersPtr;

    *ReadParametersPtr->PinValues =
        READ_REGISTER_NOFENCE_ULONG(&hw->GPLEV[ReadParametersPtr->BankId]);

    return STATUS_SUCCESS;
} // BCM_GPIO::ReadGpioPinsUsingMask (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::WriteGpioPinsUsingMask (
    PVOID ContextPtr,
    PGPIO_WRITE_PINS_MASK_PARAMETERS WriteParametersPtr
    )
{
    BCM_GPIO_REGISTERS* hw = static_cast<BCM_GPIO*>(ContextPtr)->registersPtr;

    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPCLR[WriteParametersPtr->BankId],
        ULONG(WriteParametersPtr->ClearMask));
    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPSET[WriteParametersPtr->BankId],
        ULONG(WriteParametersPtr->SetMask));

    return STATUS_SUCCESS;
} // BCM_GPIO::WriteGpioPinsUsingMask (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::StartController (
    PVOID ContextPtr,
    BOOLEAN /*RestoreContext*/,
    WDF_POWER_DEVICE_STATE /*PreviousPowerState*/
    )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    BCM_GPIO_REGISTERS* hw = thisPtr->registersPtr;

    static_assert(
        sizeof(thisPtr->gpfsel) == sizeof(hw->GPFSEL),
        "Verifying size of gpfsel bitfield");

    // read initial gpfsel register values
    for (ULONG i = 0; i < ARRAYSIZE(hw->GPFSEL); ++i) {
        ULONG value = READ_REGISTER_NOFENCE_ULONG(&hw->GPFSEL[i]);
        thisPtr->gpfsel[i] = value;
        thisPtr->initialGpfsel[i] = value;
    } // for (ULONG i = ...)

    // Initialize registers by resetting interrupt state
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPREN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPREN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPFEN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPFEN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPHEN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPHEN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPLEN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPLEN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPAREN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPAREN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPAFEN[0], 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPAFEN[1], 0);

    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPEDS[0], 0xffffffff);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPEDS[1], 0xffffffff);

    return STATUS_SUCCESS;
} // BCM_GPIO::StartController (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::StopController (
    PVOID /*ContextPtr*/,
    BOOLEAN /*SaveContext*/,
    WDF_POWER_DEVICE_STATE /*TargetState*/
    )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);
    return STATUS_SUCCESS;
} // BCM_GPIO::StopController (...)

//
// Programs the hardware with the current values of the interrupt configuration
// registers, masking off pins that are currently disabled by the storm
// detection logic.
//
void BCM_GPIO::programInterruptRegisters ( ULONG BankId )
{
    BCM_GPIO_REGISTERS* hw = this->registersPtr;
    const _INTERRUPT_REGISTERS* interruptRegistersPtr =
        &this->interruptContext[BankId].registers;
    const ULONG enabledMask = this->interruptContext[BankId].enabledMask;

    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPHEN[BankId],
        interruptRegistersPtr->GPHEN & enabledMask);
    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPLEN[BankId],
        interruptRegistersPtr->GPLEN & enabledMask);
    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPAREN[BankId],
        interruptRegistersPtr->GPAREN & enabledMask);
    WRITE_REGISTER_NOFENCE_ULONG(
        &hw->GPAFEN[BankId],
        interruptRegistersPtr->GPAFEN & enabledMask);
} // BCM_GPIO::programInterruptRegisters (...)

_Use_decl_annotations_
void BCM_GPIO::setDriveMode (
    BANK_ID BankId,
    PIN_NUMBER PinNumber,
    BCM_GPIO_FUNCTION Function,
    UCHAR AcpiPullConfig
    )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    const ULONG absolutePinNumber = BankId * BCM_GPIO_PINS_PER_BANK + PinNumber;

    if (Function != BCM_GPIO_FUNCTION_OUTPUT) {
        BCM_GPIO_PULL pullMode;
        switch (AcpiPullConfig) {
        case GPIO_PIN_PULL_CONFIGURATION_PULLUP:
            pullMode = BCM_GPIO_PULL_UP;
            break;
        case GPIO_PIN_PULL_CONFIGURATION_PULLDOWN:
            pullMode = BCM_GPIO_PULL_DOWN;
            break;
        case GPIO_PIN_PULL_CONFIGURATION_NONE:
            pullMode = BCM_GPIO_PULL_DISABLE;
            break;
        default:
            NT_ASSERT(!"Invalid AcpiPullConfig value");
            __fallthrough;
        case GPIO_PIN_PULL_CONFIGURATION_DEFAULT:
            pullMode = BCM_GPIO_PULL(
                this->defaultPullConfig.Get(absolutePinNumber));
        }

        // when changing to an input, configure pull before changing
        // pin direction to avoid any time potentially spent floating
        this->updatePullMode(BankId, PinNumber, pullMode);
    } // if

    this->gpfsel.Set(absolutePinNumber, Function);
    auto index = this->gpfsel.MakeIndex(absolutePinNumber);
    WRITE_REGISTER_NOFENCE_ULONG(
        &this->registersPtr->GPFSEL[index.StorageIndex],
        this->gpfsel[index.StorageIndex]);
} // BCM_GPIO::setDriveMode (...)

_Use_decl_annotations_
void BCM_GPIO::revertPinToDefault (BANK_ID BankId, PIN_NUMBER PinNumber)
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    const ULONG absolutePinNumber = BankId * BCM_GPIO_PINS_PER_BANK + PinNumber;

    this->setDriveMode(
        BankId,
        PinNumber,
        static_cast<BCM_GPIO_FUNCTION>(this->initialGpfsel.Get(absolutePinNumber)),
        GPIO_PIN_PULL_CONFIGURATION_DEFAULT);
} // BCM_GPIO::revertPinToDefault (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::EnableInterrupt (
    PVOID ContextPtr,
    PGPIO_ENABLE_INTERRUPT_PARAMETERS EnableParametersPtr
    )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);

    const BANK_ID bankId = EnableParametersPtr->BankId;
    const PIN_NUMBER pinNumber = EnableParametersPtr->PinNumber;
    const ULONG mask = 1 << pinNumber;
    _INTERRUPT_CONTEXT* interruptContextPtr =
        thisPtr->interruptContext + bankId;

    NT_ASSERT(!(thisPtr->openInterruptPins[bankId] & mask));
    thisPtr->openInterruptPins[bankId] |= mask;

    // Configure to GPIO input function if not already configured through
    // ConnectIoPins
    if (!(thisPtr->openIoPins[bankId] & mask)) {
        thisPtr->setDriveMode(
            bankId,
            pinNumber,
            BCM_GPIO_FUNCTION_INPUT,
            EnableParametersPtr->PullConfiguration);
    } else {
        NT_ASSERT(
            thisPtr->gpfsel.Get(bankId * BCM_GPIO_PINS_PER_BANK + pinNumber) ==
            BCM_GPIO_FUNCTION_INPUT);
    } // iff

    {
        GPIO_CLX_AcquireInterruptLock(ContextPtr, bankId);

        NT_ASSERT(!(interruptContextPtr->enabledMask & mask));
        NT_ASSERT(!(interruptContextPtr->disabledMask & mask));
        NT_ASSERT(!(interruptContextPtr->pendingReenableMask & mask));

        interruptContextPtr->registers.Add(
            mask,
            EnableParametersPtr->InterruptMode,
            EnableParametersPtr->Polarity);

        interruptContextPtr->enabledMask |= mask;
        WRITE_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[bankId], mask);
        thisPtr->programInterruptRegisters(bankId);

        GPIO_CLX_ReleaseInterruptLock(ContextPtr, bankId);
    } // release lock

    return STATUS_SUCCESS;
} // BCM_GPIO::EnableInterrupt (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::DisableInterrupt (
    PVOID ContextPtr,
    PGPIO_DISABLE_INTERRUPT_PARAMETERS DisableParametersPtr
    )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = DisableParametersPtr->BankId;
    const PIN_NUMBER pinNumber = DisableParametersPtr->PinNumber;
    const ULONG mask = 1 << pinNumber;
    _INTERRUPT_CONTEXT* interruptContextPtr =
        thisPtr->interruptContext + bankId;

    {
        GPIO_CLX_AcquireInterruptLock(ContextPtr, bankId);

        interruptContextPtr->enabledMask &= ~mask;
        interruptContextPtr->disabledMask &= ~mask;
        interruptContextPtr->pendingReenableMask &= ~mask;
        interruptContextPtr->registers.Remove(mask);

        thisPtr->programInterruptRegisters(bankId);
        WRITE_REGISTER_NOFENCE_ULONG(&thisPtr->registersPtr->GPEDS[bankId], mask);

        GPIO_CLX_ReleaseInterruptLock(ContextPtr, bankId);
    } // release lock

    // Revert IO configuration if pin is not opened for IO
    if (!(thisPtr->openIoPins[bankId] & mask)) {
        thisPtr->revertPinToDefault(bankId, pinNumber);
    } // if

    thisPtr->openInterruptPins[bankId] &= ~mask;

    return STATUS_SUCCESS;
} // BCM_GPIO::DisableInterrupt (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ConnectFunctionConfigPins (
    PVOID ContextPtr,
    PGPIO_CONNECT_FUNCTION_CONFIG_PINS_PARAMETERS ConnectParametersPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    // this function should only be called for alternate functions and
    // nothing else
    switch (ConnectParametersPtr->FunctionNumber) {
    case  BCM_GPIO_FUNCTION_ALT0:
    case  BCM_GPIO_FUNCTION_ALT1:
    case  BCM_GPIO_FUNCTION_ALT2:
    case  BCM_GPIO_FUNCTION_ALT3:
    case  BCM_GPIO_FUNCTION_ALT4:
    case  BCM_GPIO_FUNCTION_ALT5:
        break;
    default:
        NT_ASSERT(FALSE);
        return STATUS_INVALID_PARAMETER;
    }

    auto function =
        static_cast<BCM_GPIO_FUNCTION>(ConnectParametersPtr->FunctionNumber);

    switch (ConnectParametersPtr->PullConfiguration) {
    case GPIO_PIN_PULL_CONFIGURATION_PULLUP:
    case GPIO_PIN_PULL_CONFIGURATION_PULLDOWN:
    case GPIO_PIN_PULL_CONFIGURATION_DEFAULT:
    case GPIO_PIN_PULL_CONFIGURATION_NONE:
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    } // switch (ConnectParametersPtr->PullConfiguration)

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = ConnectParametersPtr->BankId;

    // set pins to requested drive mode
    for (USHORT i = 0; i < ConnectParametersPtr->PinCount; ++i) {
        const PIN_NUMBER pinNumber = ConnectParametersPtr->PinNumberTable[i];

        thisPtr->setDriveMode(
                bankId,
                pinNumber,
                function,
                ConnectParametersPtr->PullConfiguration);
    } // for (USHORT i = ...)

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS BCM_GPIO::DisconnectFunctionConfigPins (
    PVOID ContextPtr,
    PGPIO_DISCONNECT_FUNCTION_CONFIG_PINS_PARAMETERS DisconnectParametersPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = DisconnectParametersPtr->BankId;

    for (USHORT i = 0; i < DisconnectParametersPtr->PinCount; ++i) {
        const PIN_NUMBER pinNumber = DisconnectParametersPtr->PinNumberTable[i];
        thisPtr->revertPinToDefault(bankId, pinNumber);
    } // for (USHORT i = ...)

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID BCM_GPIO::evtReenableInterruptTimerFunc ( WDFTIMER WdfTimer )
{
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    TIMER_CONTEXT* timerContextPtr = bcmGpioTimerContextFromWdfObject(WdfTimer);
    _INTERRUPT_CONTEXT* interruptContextPtr =
        timerContextPtr->interruptContextPtr;
    BCM_GPIO* thisPtr = timerContextPtr->thisPtr;
    BCM_GPIO_REGISTERS* hw = thisPtr->registersPtr;

    // move disabled interrupts back onto the enabled list
    {
        GPIO_CLX_AcquireInterruptLock(
            thisPtr,
            BANK_ID(interruptContextPtr->bankId));

        WRITE_REGISTER_NOFENCE_ULONG(
            &hw->GPEDS[interruptContextPtr->bankId],
            interruptContextPtr->pendingReenableMask);

        interruptContextPtr->enabledMask |=
            interruptContextPtr->pendingReenableMask;
        interruptContextPtr->pendingReenableMask = 0;

        thisPtr->programInterruptRegisters(interruptContextPtr->bankId);

        GPIO_CLX_ReleaseInterruptLock(
            thisPtr,
            BANK_ID(interruptContextPtr->bankId));
    } // release lock
} // BCM_GPIO::evtReenableInterruptTimerFunc (...)

BCM_NONPAGED_SEGMENT_END; //===================================================
BCM_PAGED_SEGMENT_BEGIN; //====================================================

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ConnectIoPins (
    PVOID ContextPtr,
    PGPIO_CONNECT_IO_PINS_PARAMETERS ConnectParametersPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    BCM_GPIO_FUNCTION function;
    switch (ConnectParametersPtr->ConnectMode) {
    case ConnectModeInput:
        function = BCM_GPIO_FUNCTION_INPUT;
        break;
    case ConnectModeOutput:
        function = BCM_GPIO_FUNCTION_OUTPUT;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    } // switch (ConnectParametersPtr->ConnectMode)

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = ConnectParametersPtr->BankId;

    // set pins to requested drive mode
    for (USHORT i = 0; i < ConnectParametersPtr->PinCount; ++i) {
        const PIN_NUMBER pinNumber = ConnectParametersPtr->PinNumberTable[i];
        NT_ASSERT(!(thisPtr->openIoPins[bankId] & (1 << pinNumber)));
        thisPtr->openIoPins[bankId] |= 1 << pinNumber;
        thisPtr->setDriveMode(
            bankId,
            pinNumber,
            function,
            ConnectParametersPtr->PullConfiguration);
    } // for (USHORT i = ...)

    return STATUS_SUCCESS;
} // BCM_GPIO::ConnectIoPins (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::DisconnectIoPins (
    PVOID ContextPtr,
    PGPIO_DISCONNECT_IO_PINS_PARAMETERS DisconnectParametersPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    const bool preserveConfiguration =
        DisconnectParametersPtr->DisconnectFlags.PreserveConfiguration;

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    const BANK_ID bankId = DisconnectParametersPtr->BankId;

    for (USHORT i = 0; i < DisconnectParametersPtr->PinCount; ++i) {
        const PIN_NUMBER pinNumber = DisconnectParametersPtr->PinNumberTable[i];

        // Only revert pin if interrupts also disconnected
        if (!preserveConfiguration &&
            !(thisPtr->openInterruptPins[bankId] & (1 << pinNumber))) {

            thisPtr->revertPinToDefault(bankId, pinNumber);
        }

        thisPtr->openIoPins[bankId] &= ~(1 << pinNumber);
    } // for (USHORT i = ...)

    return STATUS_SUCCESS;
} // BCM_GPIO::DisconnectIoPins (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::QueryControllerBasicInformation (
    PVOID /*ContextPtr*/,
    PCLIENT_CONTROLLER_BASIC_INFORMATION ControllerInformationPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    ControllerInformationPtr->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    ControllerInformationPtr->Size = sizeof(*ControllerInformationPtr);
    ControllerInformationPtr->TotalPins = BCM_GPIO_PIN_COUNT;
    ControllerInformationPtr->NumberOfPinsPerBank = BCM_GPIO_PINS_PER_BANK;
    ControllerInformationPtr->Flags.MemoryMappedController = TRUE;
    ControllerInformationPtr->Flags.ActiveInterruptsAutoClearOnRead = FALSE;
    ControllerInformationPtr->Flags.FormatIoRequestsAsMasks = TRUE;
    ControllerInformationPtr->Flags.DeviceIdlePowerMgmtSupported = FALSE;
    ControllerInformationPtr->Flags.EmulateDebouncing = TRUE;
    ControllerInformationPtr->Flags.EmulateActiveBoth = TRUE;

    // Indicate that the H/W registers used for I/O can be accessed seperately
    // from the registers used for interrupt processing.
    //
    // N.B.: Setting this flag to TRUE will cause the GPIO class extension
    // to optimize I/O processing by skipping the acquisition of
    // interrupt-related locks in I/O paths.
    ControllerInformationPtr->Flags.IndependentIoHwSupported = TRUE;

    return STATUS_SUCCESS;
} // BCM_GPIO::QueryControllerBasicInformation (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::initialize ( WDFDEVICE WdfDevice )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    for (ULONG bankId = 0;
         bankId < ARRAYSIZE(this->interruptContext);
         ++bankId)
    {
        _INTERRUPT_CONTEXT* interruptContextPtr =
            this->interruptContext + bankId;

        // create DPC object
        WDFDPC dpc;
        {
            WDF_DPC_CONFIG dpcConfig;
            WDF_DPC_CONFIG_INIT(&dpcConfig, evtDpcFunc);
            dpcConfig.AutomaticSerialization = FALSE;

            WDF_OBJECT_ATTRIBUTES dpcAttributes;
            WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&dpcAttributes, DPC_CONTEXT);
            dpcAttributes.ParentObject = WdfDevice;

            NTSTATUS status = WdfDpcCreate(&dpcConfig, &dpcAttributes, &dpc);
            switch (status) {
            case STATUS_SUCCESS:
                break;
            case STATUS_INSUFFICIENT_RESOURCES:
                return status;
            default:
                NT_ASSERT(!"Incorrect usage of WdfDpcCreate");
                return STATUS_INTERNAL_ERROR;
            }

            auto dpcContextPtr = bcmGpioDpcContextFromWdfObject(dpc);
            dpcContextPtr->interruptContextPtr = interruptContextPtr;
            dpcContextPtr->thisPtr = this;
        } // dpc

        // create interrupt reenable timer
        WDFTIMER timer;
        {
            WDF_TIMER_CONFIG timerConfig;
            WDF_TIMER_CONFIG_INIT(&timerConfig, evtReenableInterruptTimerFunc);
            timerConfig.Period = 0;     // not periodic
            timerConfig.AutomaticSerialization = FALSE;

            WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
            WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                &wdfObjectAttributes,
                TIMER_CONTEXT);
            wdfObjectAttributes.ParentObject = WdfDevice;
            wdfObjectAttributes.ExecutionLevel = WdfExecutionLevelPassive;

            NTSTATUS status = WdfTimerCreate(
                &timerConfig,
                &wdfObjectAttributes,
                &timer);
            switch (status) {
            case STATUS_SUCCESS:
                break;
            case STATUS_INSUFFICIENT_RESOURCES:
                return status;
            default:
                NT_ASSERT(!"Incorrect usage of WdfTimerCreate");
                return STATUS_INTERNAL_ERROR;
            }

            auto timerContextPtr = bcmGpioTimerContextFromWdfObject(timer);
            timerContextPtr->interruptContextPtr = interruptContextPtr;
            timerContextPtr->thisPtr = this;
        } // timer

        interruptContextPtr->initialize(bankId, dpc, timer);
    } // for (ULONG bankId = ...)

    return STATUS_SUCCESS;
} // BCM_GPIO::initialize ( )

_Use_decl_annotations_
NTSTATUS BCM_GPIO::PrepareController (
    WDFDEVICE WdfDevice,
    PVOID ContextPtr,
    WDFCMRESLIST /*ResourcesRaw*/,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    PCM_PARTIAL_RESOURCE_DESCRIPTOR memResource = nullptr;
    ULONG interruptResourceCount = 0;

    // Look for single memory resource and single interrupt resource
    const ULONG resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (ULONG i = 0; i < resourceCount; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR res =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        switch (res->Type) {
        case CmResourceTypeMemory:
            // take the first memory resource found
            if (!memResource) {
                memResource = res;
            } // if
            break;
        case CmResourceTypeInterrupt:
            ++interruptResourceCount;
            break;
        } // switch (res->Type)
    } // for (ULONG i = ...)

    if (!memResource ||
        (memResource->u.Memory.Length < sizeof(BCM_GPIO_REGISTERS)) ||
        (interruptResourceCount < BCM_GPIO_BANK_COUNT))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    } // if

    auto registersPtr = static_cast<BCM_GPIO_REGISTERS*>(MmMapIoSpaceEx(
            memResource->u.Memory.Start,
            memResource->u.Memory.Length,
            PAGE_READWRITE | PAGE_NOCACHE));

    if (!registersPtr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    } // if

    auto thisPtr = new (ContextPtr) BCM_GPIO(
        registersPtr,
        memResource->u.Memory.Length);
    NT_ASSERT(thisPtr->signature == _SIGNATURE::CONSTRUCTED);

    NTSTATUS status = thisPtr->initialize(WdfDevice);
    if (!NT_SUCCESS(status)) {
        thisPtr->~BCM_GPIO();
        return status;
    } // if

    return STATUS_SUCCESS;
} // BCM_GPIO::PrepareController (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::ReleaseController (
    WDFDEVICE /*WdfDevice*/,
    PVOID ContextPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    auto thisPtr = static_cast<BCM_GPIO*>(ContextPtr);
    if (thisPtr->signature == _SIGNATURE::CONSTRUCTED) {
        thisPtr->~BCM_GPIO();
    }

    return STATUS_SUCCESS;
} // BCM_GPIO::ReleaseController (...)

_Use_decl_annotations_
NTSTATUS BCM_GPIO::EvtDriverDeviceAdd (
    WDFDRIVER WdfDriver,
    WDFDEVICE_INIT* DeviceInitPtr
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES wdfDeviceAttributes;
    status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(
        WdfDriver,
        DeviceInitPtr,
        &wdfDeviceAttributes);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDFDEVICE wdfDevice;
    status = WdfDeviceCreate(
        &DeviceInitPtr,
        &wdfDeviceAttributes,
        &wdfDevice);
    switch (status) {
    case STATUS_SUCCESS: break;
    case STATUS_INSUFFICIENT_RESOURCES: return status;
    default:
        NT_ASSERT(!"Incorrect usage of WdfDeviceCreate");
        return STATUS_INTERNAL_ERROR;
    } // switch

    status = GPIO_CLX_ProcessAddDevicePostDeviceCreate(WdfDriver, wdfDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
} // BCM_GPIO::EvtDriverDeviceAdd (...)

_Use_decl_annotations_
VOID BCM_GPIO::EvtDriverUnload ( WDFDRIVER WdfDriver )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    GPIO_CLX_UnregisterClient(WdfDriver);
} // BCM_GPIO::EvtDriverUnload (...)

_Use_decl_annotations_
void BCM_GPIO::updatePullMode (
    BANK_ID BankId,
    PIN_NUMBER PinNumber,
    BCM_GPIO_PULL PullMode
    )
{
    PAGED_CODE();
    BCM_ASSERT_MAX_IRQL(PASSIVE_LEVEL);

    const ULONG absolutePinNumber = BankId * BCM_GPIO_PINS_PER_BANK + PinNumber;
    if (PullMode == BCM_GPIO_PULL(this->pullConfig.Get(absolutePinNumber))) {
        return;
    }
    this->pullConfig.Set(absolutePinNumber, PullMode);

    BCM_GPIO_REGISTERS* hw = this->registersPtr;
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPPUD, PullMode);
    KeStallExecutionProcessor(1);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPPUDCLK[BankId], 1 << PinNumber);
    KeStallExecutionProcessor(1);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPPUD, 0);
    WRITE_REGISTER_NOFENCE_ULONG(&hw->GPPUDCLK[BankId], 0);
} // BCM_GPIO::updatePullMode (...)

_Use_decl_annotations_
BCM_GPIO::BCM_GPIO (
    BCM_GPIO_REGISTERS* RegistersPtr,
    ULONG RegistersLength
    ) :
    registersPtr(RegistersPtr),
    registersLength(RegistersLength),
    openIoPins(),
    openInterruptPins()
{
    PAGED_CODE();

    // there is no way to read current pull configuration; these come from
    // datasheet defaults
    this->pullConfig[0] = this->defaultPullConfig[0] = 0x5556aaaa;
    this->pullConfig[1] = this->defaultPullConfig[1] = 0x50555555;
    this->pullConfig[2] = this->defaultPullConfig[2] = 0xa05556a5;
    this->pullConfig[3] = this->defaultPullConfig[3] = 0x00000aaa;

    this->signature = _SIGNATURE::CONSTRUCTED;
} // BCM_GPIO::BCM_GPIO (...)

_Use_decl_annotations_
BCM_GPIO::~BCM_GPIO ()
{
    PAGED_CODE();

    NT_ASSERT(this->signature == _SIGNATURE::CONSTRUCTED);
    NT_ASSERT(this->registersPtr);
    NT_ASSERT(this->registersLength);

    MmUnmapIoSpace(this->registersPtr, this->registersLength);
    this->registersPtr = nullptr;
    this->registersLength = 0;

    this->signature = _SIGNATURE::DESTRUCTED;
} // BCM_GPIO::~BCM_GPIO ()

BCM_PAGED_SEGMENT_END; //======================================================
BCM_INIT_SEGMENT_BEGIN; //=====================================================

_Use_decl_annotations_
NTSTATUS DriverEntry (
    DRIVER_OBJECT* DriverObjectPtr,
    UNICODE_STRING* RegistryPathPtr
    )
{
    PAGED_CODE();

    NTSTATUS status;

    WDFDRIVER wdfDriver;
    {
        WDF_DRIVER_CONFIG wdfDriverConfig;
        WDF_DRIVER_CONFIG_INIT(
            &wdfDriverConfig,
            BCM_GPIO::EvtDriverDeviceAdd);
        wdfDriverConfig.DriverPoolTag = BCM_GPIO_ALLOC_TAG;
        wdfDriverConfig.EvtDriverUnload = BCM_GPIO::EvtDriverUnload;

        status = WdfDriverCreate(
                DriverObjectPtr,
                RegistryPathPtr,
                WDF_NO_OBJECT_ATTRIBUTES,
                &wdfDriverConfig,
                &wdfDriver);
        if (!NT_SUCCESS(status)) {
            return status;
        } // if
    } // wdfDriver

    bool stormMitigationEnabled = false;
    {
        WDFKEY wdfKey;
        status = WdfDriverOpenParametersRegistryKey(
                wdfDriver,
                KEY_READ,
                WDF_NO_OBJECT_ATTRIBUTES,
                &wdfKey);

        if (NT_SUCCESS(status)) {
            DECLARE_CONST_UNICODE_STRING(valueName, L"StormMitigationEnabled");
            ULONG value;
            status = WdfRegistryQueryULong(
                    wdfKey,
                    &valueName,
                    &value);

            if (NT_SUCCESS(status)) {
                stormMitigationEnabled = (value != 0);
            }

            WdfRegistryClose(wdfKey);
        }
    }

    // Register with GpioClx
    GPIO_CLIENT_REGISTRATION_PACKET registrationPacket = {
        GPIO_CLIENT_VERSION,
        sizeof(GPIO_CLIENT_REGISTRATION_PACKET),
        0,          // Flags
        sizeof(BCM_GPIO),
        0,          // Reserved
        BCM_GPIO::PrepareController,
        BCM_GPIO::ReleaseController,
        BCM_GPIO::StartController,
        BCM_GPIO::StopController,
        BCM_GPIO::QueryControllerBasicInformation,
        nullptr,    // CLIENT_QuerySetControllerInformation
        BCM_GPIO::EnableInterrupt,
        BCM_GPIO::DisableInterrupt,
        BCM_GPIO::UnmaskInterrupt,
        BCM_GPIO::MaskInterrupts,
        BCM_GPIO::QueryActiveInterrupts,
        BCM_GPIO::ClearActiveInterrupts,
        BCM_GPIO::ConnectIoPins,
        BCM_GPIO::DisconnectIoPins,
        nullptr,    // CLIENT_ReadGpioPins
        nullptr,    // CLIENT_WriteGpioPins
        nullptr,    // CLIENT_SaveBankHardwareContext
        nullptr,    // CLIENT_RestoreBankHardwareContext
        stormMitigationEnabled ? BCM_GPIO::PreProcessControllerInterrupt : nullptr,
        nullptr,    // CLIENT_ControllerSpecificFunction
        BCM_GPIO::ReconfigureInterrupt,
        BCM_GPIO::QueryEnabledInterrupts,
        BCM_GPIO::ConnectFunctionConfigPins,
        BCM_GPIO::DisconnectFunctionConfigPins,
    }; // registrationPacket

    registrationPacket.CLIENT_ReadGpioPinsUsingMask =
        BCM_GPIO::ReadGpioPinsUsingMask;

    registrationPacket.CLIENT_WriteGpioPinsUsingMask =
        BCM_GPIO::WriteGpioPinsUsingMask;

    status = GPIO_CLX_RegisterClient(
        wdfDriver,
        &registrationPacket,
        RegistryPathPtr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    NT_ASSERT(status == STATUS_SUCCESS);
    return status;
} // DriverEntry (...)

BCM_INIT_SEGMENT_END; //======================================================
