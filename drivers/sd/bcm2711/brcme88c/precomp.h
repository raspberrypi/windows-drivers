#pragma once

#include <wdm.h>
#include <TraceLoggingProvider.h>
#include <evntrace.h>

extern "C" {
    #include <sdport.h>
}

#define BYTE UCHAR
#include <rpiq.h>
#undef BYTE

#define CODE_SEG_INIT __declspec(code_seg("INIT"))
#define CODE_SEG_PAGE __declspec(code_seg("PAGE"))
#define CODE_SEG_TEXT __declspec(code_seg(".text"))
