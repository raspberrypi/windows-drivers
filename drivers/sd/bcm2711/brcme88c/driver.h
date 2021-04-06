#pragma once

_IRQL_requires_max_(HIGH_LEVEL)
__forceinline bool
DriverShouldLogVerboseInfo()
{
    static UINT32 s_verboseInfoEventCount;
    bool const shouldLog = s_verboseInfoEventCount < 100;

    // Not interlocked. A few extra events won't hurt anything.
    s_verboseInfoEventCount += shouldLog;

    return shouldLog;
}

_IRQL_requires_max_(APC_LEVEL)
bool CODE_SEG_PAGE
DriverRpiqIsAvailable();

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CODE_SEG_PAGE
DriverRpiqProperty(
    _Inout_ MAILBOX_HEADER* item);
