/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file GetTime.h
/// \brief Returns the value from QueryPerformanceCounter.  This is the function RakNet uses to represent time. This
/// time won't match the time returned by GetTimeCount(). See
/// http://www.jenkinssoftware.com/forum/index.php?topic=2798.0
///

#ifndef __GET_TIME_H
#define __GET_TIME_H

#include <cstdint>

#include "RakNetDefines.h"

namespace RakNet {
typedef uint64_t Time;
typedef uint32_t TimeMS;
typedef uint64_t TimeUS;
}  // namespace RakNet

namespace RakNet {
/// Same as GetTimeMS
RakNet::Time GetTime(void);

/// Return the time as 32 bit
/// \note The maximum delta between returned calls is 1 second - however, RakNet calls this constantly anyway. See
/// NormalizeTime() in the cpp.
RakNet::TimeMS GetTimeMS(void);

/// Return the time as 64 bit
/// \note The maximum delta between returned calls is 1 second - however, RakNet calls this constantly anyway. See
/// NormalizeTime() in the cpp.
RakNet::TimeUS GetTimeUS(void);
}  // namespace RakNet

#endif
