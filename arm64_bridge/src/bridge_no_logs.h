#pragma once

// Forced into release bridge translation units after the platform declarations
// have been parsed. Function-like replacement keeps log arguments unevaluated,
// removes the liblog imports, and also silences logs forwarded from the ARM32
// guest through its registered Android logging thunks.
#include <android/log.h>

#define WARBAND_BRIDGE_NO_LOGS 1
#define __android_log_print(...) 0
#define __android_log_write(...) 0
#define __android_log_vprint(...) 0
