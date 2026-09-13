#pragma once

#include <cstddef>

namespace endgate::raknet {

/**
 * Where a reliability layer puts the datagrams it has decided to send.
 *
 * Replaces RakNet::RakNetSocket2, which ReliabilityLayer used only to reach a socket it did not own.
 * The peer address is the sink's, not the layer's.
 */
class DatagramSink {
public:
    virtual ~DatagramSink() = default;
    virtual void send(const char *data, int length) = 0;
};

}  // namespace endgate::raknet
