/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifndef __RAKNET_VERSION_H
#define __RAKNET_VERSION_H

#define RAKNET_VERSION "4.081"

// Mojang raised this from upstream's 6. It is the version a peer is rejected over with
// ID_INCOMPATIBLE_PROTOCOL_VERSION, so it is what a Bedrock client expects to see.
#define RAKNET_PROTOCOL_VERSION 11

#endif
