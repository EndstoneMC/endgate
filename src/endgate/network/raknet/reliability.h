#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "endgate/network/buffer.h"
#include "endgate/network/raknet/congestion.h"
#include "endgate/network/raknet/datagram.h"
#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet {

/**
 * @see RakNet::InternalPacket
 */
struct InternalPacket : FrameHeader {
    PacketPriority priority{PacketPriority::High};
    std::uint32_t send_receipt_serial{0};
    ByteBuf data;
    std::size_t header_length{0};
    std::uint8_t times_sent{0};
    TimePoint creation_time{};
    TimePoint next_action_time{};
};

/**
 * @see RakNet::ReliabilityLayer
 */
class ReliabilityLayer {
public:
    class Listener {
    public:
        virtual void sendDatagram(std::span<const std::byte> datagram) = 0;
        virtual void onReliabilityLayerNotification(std::string_view message, bool is_error) = 0;

    protected:
        ~Listener() = default;
    };

    static constexpr std::size_t RESEND_BUFFER_ARRAY_LENGTH = 512;
    static constexpr std::size_t DATAGRAM_MESSAGE_ID_ARRAY_LENGTH = 512;
    static constexpr std::uint32_t MAXIMUM_HOLE_COUNT = 1000000;
    static constexpr std::uint32_t MAX_SPLIT_PACKET_COUNT = 2048;
    static constexpr std::size_t MAX_SPLIT_PACKET_CHANNELS = 256;
    static constexpr std::size_t MAX_SPLIT_PACKET_BYTES = 2 * MAX_SPLIT_PACKET_COUNT * MAXIMUM_MTU_SIZE;
    static constexpr std::size_t MAXIMUM_ORDERED_PACKETS_PER_HEAP = 4096;

    ReliabilityLayer(Listener &listener, std::size_t mtu, TimePoint now);

    void setTimeoutTime(std::chrono::milliseconds time) { timeout_time_ = time; }
    [[nodiscard]] std::chrono::milliseconds getTimeoutTime() const { return timeout_time_; }
    void setUnreliableTimeout(std::chrono::milliseconds timeout) { unreliable_timeout_ = timeout; }

    /**
     * @see RakNet::ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer
     */
    void handleSocketReceiveFromConnectedPlayer(std::span<const std::byte> datagram, TimePoint now);

    /**
     * @see RakNet::ReliabilityLayer::Receive
     */
    [[nodiscard]] std::optional<ByteBuf> receive();

    /**
     * @see RakNet::ReliabilityLayer::Send
     */
    bool send(ByteBuf data, PacketPriority priority, PacketReliability reliability, std::uint8_t ordering_channel,
              std::uint32_t receipt, TimePoint now);

    /**
     * @see RakNet::ReliabilityLayer::Update
     */
    void update(TimePoint now);

    [[nodiscard]] std::optional<TimePoint> nextTimeout() const;

    [[nodiscard]] bool isDeadConnection() const { return dead_connection_; }
    void killConnection() { dead_connection_ = true; }
    [[nodiscard]] bool isOutgoingDataWaiting() const;
    [[nodiscard]] bool areAcksWaiting() const { return !acknowlegements_.empty(); }
    [[nodiscard]] bool ackTimeout(TimePoint now) const;
    [[nodiscard]] std::size_t getMessagesInResendBuffer() const { return resend_list_.size(); }
    [[nodiscard]] std::size_t getUnacknowledgedBytes() const { return unacknowledged_bytes_; }
    [[nodiscard]] const CCRakNetSlidingWindow &getCongestionManager() const { return congestion_manager_; }

private:
    struct DatagramHistoryNode {
        std::vector<std::uint32_t> message_numbers;
        TimePoint time_sent;
    };

    struct UnreliableWithAckReceiptNode {
        std::uint32_t datagram_number;
        std::uint32_t send_receipt_serial;
        TimePoint next_action_time;
    };

    struct OutgoingPacket {
        std::uint64_t weight;
        InternalPacket packet;
    };

    struct OrderedPacket {
        std::uint64_t weight;
        InternalPacket packet;
    };

    struct SplitPacketChannel {
        std::uint32_t split_packet_count;
        std::size_t bytes{0};
        FrameHeader first_header;
        std::vector<std::pair<std::uint32_t, ByteBuf>> parts;
    };

    /**
     * @see RakNet::ReliabilityLayer::hasReceivedPacketQueue
     */
    class HasReceivedPacketQueue {
    public:
        [[nodiscard]] std::size_t size() const { return size_; }
        [[nodiscard]] bool empty() const { return size_ == 0; }
        [[nodiscard]] bool operator[](std::size_t index) const;
        void reset(std::size_t index);
        void push(bool hole, std::size_t count);
        void pop();

    private:
        std::deque<std::uint64_t> words_;
        std::size_t first_{0};
        std::size_t size_{0};
    };

    void notify(std::string_view message, bool is_error)
    {
        listener_.onReliabilityLayerNotification(message, is_error);
    }

    void handleAcknowledgements(Reader &reader, TimePoint now);
    void handleNegativeAcknowledgements(Reader &reader, TimePoint now);
    void handleFrame(const FrameHeader &header, ByteBuf data, TimePoint now);
    [[nodiscard]] std::optional<InternalPacket> insertIntoSplitPacketList(const FrameHeader &header, ByteBuf data,
                                                                          TimePoint now);
    void deliver(InternalPacket packet);
    void removePacketFromResendList(std::uint32_t message_number);

    [[nodiscard]] std::size_t getMaxDatagramSizeExcludingMessageHeaderBytes() const;
    [[nodiscard]] std::deque<OutgoingPacket> *peekOutgoing();
    [[nodiscard]] std::uint64_t getNextWeight(PacketPriority priority);
    void initHeapWeights();
    void pushOutgoing(InternalPacket packet);
    void splitPacket(InternalPacket packet);

    void sendAcks();
    void pushPacket(const InternalPacket &packet);
    void pushDatagram(TimePoint now);
    [[nodiscard]] bool resendBufferOverflow() const;
    [[nodiscard]] DatagramHistoryNode *getDatagramHistoryNode(std::uint32_t datagram_number);

    Listener &listener_;
    CCRakNetSlidingWindow congestion_manager_;
    std::chrono::milliseconds timeout_time_{10000};
    std::chrono::milliseconds unreliable_timeout_{0};
    TimePoint time_last_datagram_arrived_;
    TimePoint last_update_time_;
    bool dead_connection_{false};

    std::deque<ByteBuf> output_queue_;
    RangeList acknowlegements_;
    RangeList naks_;
    bool remote_system_needs_b_and_as_{false};
    HasReceivedPacketQueue has_received_packet_queue_;
    std::uint32_t received_packets_base_index_{0};
    std::array<std::uint32_t, NUMBER_OF_ORDERED_STREAMS> ordered_read_index_{};
    std::array<std::uint32_t, NUMBER_OF_ORDERED_STREAMS> highest_sequenced_read_index_{};
    std::array<std::uint32_t, NUMBER_OF_ORDERED_STREAMS> heap_index_offsets_{};
    std::array<std::vector<OrderedPacket>, NUMBER_OF_ORDERED_STREAMS> ordering_heaps_;
    std::unordered_map<std::uint16_t, SplitPacketChannel> split_packet_channels_;
    std::size_t split_packet_bytes_{0};

    std::array<std::uint32_t, NUMBER_OF_ORDERED_STREAMS> ordered_write_index_{};
    std::array<std::uint32_t, NUMBER_OF_ORDERED_STREAMS> sequenced_write_index_{};
    std::uint16_t split_packet_id_{0};
    std::uint32_t send_reliable_message_number_index_{0};
    std::array<std::deque<OutgoingPacket>, NUMBER_OF_PRIORITIES> outgoing_packet_buffer_;
    std::array<std::uint64_t, NUMBER_OF_PRIORITIES> outgoing_packet_buffer_next_weights_{};
    std::list<InternalPacket> resend_list_;
    std::array<std::optional<std::list<InternalPacket>::iterator>, RESEND_BUFFER_ARRAY_LENGTH> resend_buffer_;
    std::size_t unacknowledged_bytes_{0};
    std::deque<DatagramHistoryNode> datagram_history_;
    std::uint32_t datagram_history_pop_count_{0};
    std::vector<UnreliableWithAckReceiptNode> unreliable_with_ack_receipt_history_;
    bool bandwidth_exceeded_statistic_{false};

    std::vector<const InternalPacket *> datagram_packets_;
    std::deque<InternalPacket> unreliable_packets_this_update_;
    std::size_t datagram_size_so_far_{0};
    std::size_t all_datagram_sizes_so_far_{0};
    std::size_t datagrams_this_update_{0};
    DatagramHeader datagram_header_;
    std::array<std::byte, MAXIMUM_MTU_SIZE> update_buffer_{};
};

}  // namespace endgate::raknet
