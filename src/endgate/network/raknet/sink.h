#pragma once

namespace endgate::raknet {

/**
 * Where a reliability layer puts the datagrams it has decided to send, and what it tells about the ones it drops.
 *
 * Replaces RakNet::RakNetSocket2, which ReliabilityLayer used only to reach a socket it did not own, and the
 * PluginInterface2 list it used only to report. The peer address is the sink's, not the layer's.
 *
 * @see RakNet::PluginInterface2::OnReliabilityLayerNotification
 */
class DatagramSink {
public:
    virtual ~DatagramSink() = default;
    virtual void send(const char *data, int length) = 0;
    virtual void onReliabilityNotification(const char *reason, bool is_error) = 0;
};

}  // namespace endgate::raknet
