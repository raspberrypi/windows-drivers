//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     precomp.h
//
// Abstract:
//  
//  Common header files are included here
//

#include <stddef.h>
#include <stdarg.h>
#define WIN9X_COMPAT_SPINLOCK
#include "ntddk.h"
#include <wdf.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include <initguid.h>
#include <devpkey.h>
#include "ntddser.h"
#include <wmilib.h>
#include <initguid.h>
#include <wmidata.h>
#include "serial.h"
#include "serialp.h"
#include "trace.h"

#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#include <rpiq.h>

//
// RPi3 mini Uart hardware as well as this driver supports higher baud rates, compared to
// typical 16550 UART. Macros for standard baud rates 110 - 115200 baud rates are 
// defined in ntddser.h header. 
// Macros for higher speed serial port masks are set below
//

#define SERIAL_BAUD_230400       ((ULONG)0x00080000)
#define SERIAL_BAUD_460800       ((ULONG)0x00100000)
#define SERIAL_BAUD_921600       ((ULONG)0x00200000)
