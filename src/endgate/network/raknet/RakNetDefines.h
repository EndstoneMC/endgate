/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifndef __RAKNET_DEFINES_H
#define __RAKNET_DEFINES_H

#include <cassert>

#define _FILE_AND_LINE_ __FILE__, __LINE__

#define RakAssert(x) assert(x)

#define BITSTREAM_STACK_ALLOCATION_SIZE  256
#define DATAGRAM_MESSAGE_ID_ARRAY_LENGTH 512
#define RESEND_BUFFER_ARRAY_LENGTH       512
#define RESEND_BUFFER_ARRAY_MASK         511
#define INTERNAL_PACKET_PAGE_SIZE        8

#endif
