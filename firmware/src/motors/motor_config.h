#pragma once

#if MOTOR_WANZHIDA_ONCE_TOP
#include "motors/wanzhida_once_top.h"
#elif MOTOR_MAD2804
#include "motors/mad2804.h"
#elif MOTOR_MATOUCH_3205B
#include "motors/matouch_3205b.h"
#else
#error "No motor configuration specified!"
#endif
