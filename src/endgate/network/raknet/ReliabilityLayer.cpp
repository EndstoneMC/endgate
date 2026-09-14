/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file
///

#include "ReliabilityLayer.h"

#include "RakPeer.h"

#include <math.h>

#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "RakNetDefines.h"

using namespace RakNet;

// Can't figure out which library has this function on the PS3
double Ceil(double d)
{
    if (((double)((int)d)) == d) {
        return d;
    }
    return (int)(d + 1.0);
}

static const CCTimeType MAX_TIME_BETWEEN_PACKETS = 350000;  // 350 milliseconds
static const CCTimeType STARTING_TIME_BETWEEN_PACKETS = MAX_TIME_BETWEEN_PACKETS;

typedef uint32_t BitstreamLengthEncoding;

#ifdef _MSC_VER
#pragma warning(push)
#endif

BPSTracker::TimeAndValue2::TimeAndValue2() {}
BPSTracker::TimeAndValue2::~TimeAndValue2() {}
BPSTracker::TimeAndValue2::TimeAndValue2(RakNet::TimeUS t, uint64_t v1) : value1(v1), time(t) {}
BPSTracker::BPSTracker()
{
    Reset();
}
BPSTracker::~BPSTracker() {}
void BPSTracker::Reset()
{
    total1 = lastSec1 = 0;
    dataQueue.clear();
}
uint64_t BPSTracker::GetTotal1(void) const
{
    return total1;
}

void BPSTracker::ClearExpired1(RakNet::TimeUS time)
{
    while (dataQueue.empty() == false && dataQueue.front().time + 1000000 < time) {
        lastSec1 -= dataQueue.front().value1;
        dataQueue.pop_front();
    }
}

struct DatagramHeaderFormat {
    DatagramSequenceNumberType datagramNumber;

    // Use floats to save bandwidth
    float AS;  // Data arrival rate
    bool isACK;
    bool isNAK;
    bool isPacketPair;
    bool hasBAndAS;
    bool isContinuousSend;
    bool needsBAndAs;
    bool isValid;  // To differentiate between what I serialized, and offline data

    static BitSize_t GetDataHeaderBitLength() { return BYTES_TO_BITS(GetDataHeaderByteLength()); }

    static unsigned int GetDataHeaderByteLength() { return 2 + 3 + sizeof(float) * 1; }

    void Serialize(RakNet::BitStream *b)
    {

        b->Write(true);  // IsValid
        if (isACK) {
            b->Write(true);
            b->Write(hasBAndAS);
            b->AlignWriteToByteBoundary();
            if (hasBAndAS) {
                b->Write(AS);
            }
        }
        else if (isNAK) {
            b->Write(false);
            b->Write(true);
        }
        else {
            b->Write(false);
            b->Write(false);
            b->Write(isPacketPair);
            b->Write(isContinuousSend);
            b->Write(needsBAndAs);
            b->AlignWriteToByteBoundary();
            b->Write(datagramNumber);
        }
    }
    void Deserialize(RakNet::BitStream *b)
    {

        b->Read(isValid);
        b->Read(isACK);
        if (isACK) {
            isNAK = false;
            isPacketPair = false;
            b->Read(hasBAndAS);
            b->AlignReadToByteBoundary();
            if (hasBAndAS) {
                b->Read(AS);
            }
        }
        else {
            b->Read(isNAK);
            if (isNAK) {
                isPacketPair = false;
            }
            else {
                b->Read(isPacketPair);
                b->Read(isContinuousSend);
                b->Read(needsBAndAs);
                b->AlignReadToByteBoundary();
                b->Read(datagramNumber);
            }
        }
    }
};

#if !defined(__GNUC__) && !defined(__ARMCC)
#pragma warning(disable : 4702)  // unreachable code
#endif

using namespace RakNet;

int SplitPacketIndexComp(SplitPacketIndexType const &key, InternalPacket *const &data)
{
    if (key < data->splitPacketIndex) {
        return -1;
    }
    if (key == data->splitPacketIndex) {
        return 0;
    }
    return 1;
}

// Constructor
// Add 21 to the default MTU so if we encrypt it can hold potentially 21 more bytes of extra data + padding.
ReliabilityLayer::ReliabilityLayer()
{
    timeoutTime = 10000;

    InitializeVariables();
    datagramHistoryMessagePool.SetPageSize(sizeof(MessageNumberNode) * 128);
    internalPacketPool.SetPageSize(sizeof(InternalPacket) * INTERNAL_PACKET_PAGE_SIZE);
    refCountedDataPool.SetPageSize(sizeof(InternalPacketRefCountedData) * 32);
}

// Destructor
ReliabilityLayer::~ReliabilityLayer()
{
    FreeMemory(true);  // Free all memory immediately
}
// Resets the layer for reuse
void ReliabilityLayer::Reset(bool resetVariables, int MTUSize)
{
    FreeMemory(true);  // true because making a memory reset pending in the update cycle causes resets after reconnects.
                       // Instead, just call Reset from a single thread
    if (resetVariables) {
        InitializeVariables();

        congestionManager.Init(RakNet::GetTimeUS(), MTUSize - UDP_HEADER_SIZE);
    }
}

// Set the time, in MS, to use before considering ourselves disconnected after not being able to deliver a reliable
// packet
void ReliabilityLayer::SetTimeoutTime(RakNet::TimeMS time)
{
    timeoutTime = time;
}

// Returns the value passed to SetTimeoutTime. or the default if it was never called
RakNet::TimeMS ReliabilityLayer::GetTimeoutTime(void)
{
    return timeoutTime;
}

// Initialize the variables
void ReliabilityLayer::InitializeVariables(void)
{
    memset(orderedWriteIndex, 0, NUMBER_OF_ORDERED_STREAMS * sizeof(OrderingIndexType));
    memset(sequencedWriteIndex, 0, NUMBER_OF_ORDERED_STREAMS * sizeof(OrderingIndexType));
    memset(orderedReadIndex, 0, NUMBER_OF_ORDERED_STREAMS * sizeof(OrderingIndexType));
    memset(highestSequencedReadIndex, 0, NUMBER_OF_ORDERED_STREAMS * sizeof(OrderingIndexType));
    memset(&statistics, 0, sizeof(statistics));
    memset(&heapIndexOffsets, 0, sizeof(heapIndexOffsets));

    statistics.connectionStartTime = RakNet::GetTimeUS();
    splitPacketId = 0;
    elapsedTimeSinceLastUpdate = 0;
    throughputCapCountdown = 0;
    sendReliableMessageNumberIndex = 0;
    internalOrderIndex = 0;
    timeToNextUnreliableCull = 0;
    unreliableLinkedListHead = 0;
    lastUpdateTime = RakNet::GetTimeUS();
    bandwidthExceededStatistic = false;
    unreliableTimeout = 0;
    lastBpsClear = 0;

    // Disable packet pairs
    countdownToNextPacketPair = 15;

    deadConnection = cheater = false;
    timeOfLastContinualSend = 0;

    timeLastDatagramArrived = RakNet::GetTimeMS();
    statistics.messagesInResendBuffer = 0;
    statistics.bytesInResendBuffer = 0;

    receivedPacketsBaseIndex = 0;
    resetReceivedPackets = true;
    receivePacketCount = 0;

    timeBetweenPackets = STARTING_TIME_BETWEEN_PACKETS;

    nextSendTime = lastUpdateTime;

    unacknowledgedBytes = 0;
    resendLinkedListHead = 0;
    totalUserDataBytesAcked = 0;

    datagramHistoryPopCount = 0;

    InitHeapWeights();
    for (int i = 0; i < NUMBER_OF_PRIORITIES; i++) {
        statistics.messageInSendBuffer[i] = 0;
        statistics.bytesInSendBuffer[i] = 0.0;
    }

    for (int i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++) {
        bpsMetrics[i].Reset();
    }
}

// Frees all allocated memory
void ReliabilityLayer::FreeMemory(bool freeAllImmediately)
{
    (void)freeAllImmediately;
    FreeThreadSafeMemory();
}

void ReliabilityLayer::FreeThreadSafeMemory(void)
{
    unsigned i, j;
    InternalPacket *internalPacket;

    ClearPacketsAndDatagrams();

    for (auto &[splitPacketId, splitPacketChannel] : splitPacketChannelList) {
        for (j = 0; j < splitPacketChannel.splitPacketList.size(); j++) {
            FreeInternalPacketData(splitPacketChannel.splitPacketList[j], _FILE_AND_LINE_);
            ReleaseToInternalPacketPool(splitPacketChannel.splitPacketList[j]);
        }
    }
    splitPacketChannelList.clear();

    while (outputQueue.empty() == false) {
        internalPacket = outputQueue.front();
        outputQueue.pop_front();
        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
        ReleaseToInternalPacketPool(internalPacket);
    }

    for (i = 0; i < NUMBER_OF_ORDERED_STREAMS; i++) {
        for (j = 0; j < orderingHeaps[i].Size(); j++) {
            FreeInternalPacketData(orderingHeaps[i][j], _FILE_AND_LINE_);
            ReleaseToInternalPacketPool(orderingHeaps[i][j]);
        }
        orderingHeaps[i].Clear(true, _FILE_AND_LINE_);
    }

    memset(resendBuffer, 0, sizeof(resendBuffer));
    statistics.messagesInResendBuffer = 0;
    statistics.bytesInResendBuffer = 0;

    if (resendLinkedListHead) {
        InternalPacket *prev;
        InternalPacket *iter = resendLinkedListHead;

#ifdef _MSC_VER
#pragma warning(disable : 4127)  // warning C4127: conditional expression is constant
#endif
        while (1) {
            if (iter->data) {
                FreeInternalPacketData(iter, _FILE_AND_LINE_);
            }
            prev = iter;
            iter = iter->resendNext;
            if (iter == resendLinkedListHead) {
                ReleaseToInternalPacketPool(prev);
                break;
            }
            ReleaseToInternalPacketPool(prev);
        }
        resendLinkedListHead = 0;
    }
    unacknowledgedBytes = 0;

    for (j = 0; j < outgoingPacketBuffer.Size(); j++) {
        if (outgoingPacketBuffer[j]->data) {
            FreeInternalPacketData(outgoingPacketBuffer[j], _FILE_AND_LINE_);
        }
        ReleaseToInternalPacketPool(outgoingPacketBuffer[j]);
    }

    outgoingPacketBuffer.Clear(true, _FILE_AND_LINE_);

    unreliableWithAckReceiptHistory.clear();

    packetsToSendThisUpdate.clear();
    packetsToSendThisUpdate.reserve(512);
    packetsToDeallocThisUpdate.clear();
    packetsToDeallocThisUpdate.reserve(512);
    packetsToSendThisUpdateDatagramBoundaries.clear();
    packetsToSendThisUpdateDatagramBoundaries.reserve(128);
    datagramSizesInBytes.clear();
    datagramSizesInBytes.reserve(128);

    internalPacketPool.Clear(_FILE_AND_LINE_);

    refCountedDataPool.Clear(_FILE_AND_LINE_);

    while (datagramHistory.empty() == false) {
        RemoveFromDatagramHistory(datagramHistoryPopCount);
        datagramHistory.pop_front();
        datagramHistoryPopCount++;
    }
    datagramHistoryMessagePool.Clear(_FILE_AND_LINE_);
    datagramHistoryPopCount = 0;

    acknowlegements.Clear();
    NAKs.Clear();

    unreliableLinkedListHead = 0;
}

// Packets are read directly from the socket layer and skip the reliability
// layer  because unconnected players do not use the reliability layer
// This function takes packet data after a player has been confirmed as
// connected.  The game should not use that data directly
// because some data is used internally, such as packet acknowledgment and
// split packets
// Mojang bounds acknowledgement processing so a malformed ACK/NAK cannot drive the datagram-number
// walk into a runaway loop. At most RESEND_BUFFER_ARRAY_LENGTH datagrams are ever outstanding.
#define MAX_ACKNOWLEDGEMENT_RANGES_PER_DATAGRAM 0x400
#define MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM       0x4000
bool ReliabilityLayer::HandleSocketReceiveFromConnectedPlayer(const char *buffer, unsigned int length,
                                                              SystemAddress &systemAddress, int MTUSize,
                                                              RakPeer *peer, CCTimeType timeRead,
                                                              BitStream &updateBitStream)
{

    bpsMetrics[(int)ACTUAL_BYTES_RECEIVED].Push1(timeRead, length);

    (void)MTUSize;

    if (length <= 2 || buffer == 0)  // Length of 1 is a connection request resend that we just ignore
    {
        peer->OnReliabilityLayerNotification(systemAddress, "length <= 2 || buffer == 0", true);
        return true;
    }

    timeLastDatagramArrived = RakNet::GetTimeMS();

    DatagramSequenceNumberType holeCount;
    unsigned i;

    RakNet::BitStream socketData((unsigned char *)buffer, length,
                                 false);  // Convert the incoming data to a bitstream for easy parsing

    DatagramHeaderFormat dhf;
    dhf.Deserialize(&socketData);
    if (dhf.isValid == false) {
        peer->OnReliabilityLayerNotification(systemAddress, "dhf.isValid==false", true);
        return true;
    }
    if (dhf.isACK) {
        DatagramSequenceNumberType datagramNumber;

        incomingAcks.Clear();
        if (incomingAcks.Deserialize(&socketData) == false) {
            peer->OnReliabilityLayerNotification(systemAddress, "incomingAcks.Deserialize failed", true);
            return false;
        }
        if (incomingAcks.ranges.size() > MAX_ACKNOWLEDGEMENT_RANGES_PER_DATAGRAM) {
            peer->OnReliabilityLayerNotification(systemAddress, "number of ACK ranges in this datagram exceeds the maximum allowed", true);
            return false;
        }
        uint32_t totalAcknowledgementsThisDatagram = 0;
        for (i = 0; i < incomingAcks.ranges.size(); i++) {
            if (incomingAcks.ranges[i].minIndex > incomingAcks.ranges[i].maxIndex ||
                (incomingAcks.ranges[i].maxIndex == (uint24_t)(0xFFFFFFFF))) {
                peer->OnReliabilityLayerNotification(systemAddress, "incomingAcks minIndex > maxIndex or maxIndex is too large", true);
                return false;
            }
            totalAcknowledgementsThisDatagram +=
                incomingAcks.ranges[i].maxIndex.val - incomingAcks.ranges[i].minIndex.val + 1;
            if (totalAcknowledgementsThisDatagram > MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM) {
                peer->OnReliabilityLayerNotification(systemAddress, "datagram contains too many ACKs, exceeding MAX_TOTAL_ACKS_PER_DATAGRAM",
                                             true);
                return false;
            }
            for (datagramNumber = incomingAcks.ranges[i].minIndex;
                 datagramNumber >= incomingAcks.ranges[i].minIndex && datagramNumber <= incomingAcks.ranges[i].maxIndex;
                 datagramNumber++) {
                CCTimeType whenSent;

                if (unreliableWithAckReceiptHistory.size() > 0) {
                    unsigned int k = 0;
                    while (k < unreliableWithAckReceiptHistory.size()) {
                        if (unreliableWithAckReceiptHistory[k].datagramNumber == datagramNumber) {
                            InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
                            AllocInternalPacketData(ackReceipt, 5, false, _FILE_AND_LINE_);
                            ackReceipt->dataBitLength = BYTES_TO_BITS(5);
                            ackReceipt->data[0] = (MessageID)ID_SND_RECEIPT_ACKED;
                            memcpy(ackReceipt->data + sizeof(MessageID),
                                   &unreliableWithAckReceiptHistory[k].sendReceiptSerial, sizeof(uint32_t));
                            outputQueue.push_back(ackReceipt);

                            // Remove, swap with last
                            unreliableWithAckReceiptHistory.erase(unreliableWithAckReceiptHistory.begin() + k);
                        }
                        else {
                            k++;
                        }
                    }
                }

                MessageNumberNode *messageNumberNode = GetMessageNumberNodeByDatagramIndex(datagramNumber, &whenSent);
                if (messageNumberNode) {
                    CCTimeType ping;
                    if (timeRead > whenSent) {
                        ping = timeRead - whenSent;
                    }
                    else {
                        ping = 0;
                    }
                    congestionManager.OnAck(timeRead, ping, dhf.hasBAndAS, 0, dhf.AS, totalUserDataBytesAcked,
                                            bandwidthExceededStatistic, datagramNumber);
                    while (messageNumberNode) {

                        RemovePacketFromResendListAndDeleteOlderReliableSequenced(messageNumberNode->messageNumber,
                                                                                  timeRead, systemAddress);
                        messageNumberNode = messageNumberNode->next;
                    }

                    RemoveFromDatagramHistory(datagramNumber);
                }
            }
        }
    }
    else if (dhf.isNAK) {
        DatagramSequenceNumberType messageNumber;
        DataStructures::RangeList<DatagramSequenceNumberType> incomingNAKs;
        if (incomingNAKs.Deserialize(&socketData) == false) {
            peer->OnReliabilityLayerNotification(systemAddress, "incomingNAKs.Deserialize failed", true);
            return false;
        }
        if (incomingNAKs.ranges.size() > MAX_ACKNOWLEDGEMENT_RANGES_PER_DATAGRAM) {
            peer->OnReliabilityLayerNotification(systemAddress, "number of NAK ranges in this datagram exceeds the maximum allowed", true);
            return false;
        }
        uint32_t totalAcknowledgementsThisDatagram = 0;
        for (i = 0; i < incomingNAKs.ranges.size(); i++) {
            if (incomingNAKs.ranges[i].minIndex > incomingNAKs.ranges[i].maxIndex ||
                (incomingNAKs.ranges[i].maxIndex == (uint24_t)(0xFFFFFFFF))) {
                peer->OnReliabilityLayerNotification(systemAddress, "incomingNAKs minIndex>maxIndex or maxIndex is too large", true);
                return false;
            }
            totalAcknowledgementsThisDatagram +=
                incomingNAKs.ranges[i].maxIndex.val - incomingNAKs.ranges[i].minIndex.val + 1;
            if (totalAcknowledgementsThisDatagram > MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM) {
                peer->OnReliabilityLayerNotification(systemAddress, "datagram contains too many NAKs, exceeding MAX_TOTAL_NAKS_PER_DATAGRAM",
                                             true);
                return false;
            }
            for (messageNumber = incomingNAKs.ranges[i].minIndex;
                 messageNumber >= incomingNAKs.ranges[i].minIndex && messageNumber <= incomingNAKs.ranges[i].maxIndex;
                 messageNumber++) {
                congestionManager.OnNAK(timeRead, messageNumber);

                CCTimeType timeSent;
                MessageNumberNode *messageNumberNode = GetMessageNumberNodeByDatagramIndex(messageNumber, &timeSent);
                while (messageNumberNode) {
                    // Update timers so resends occur immediately
                    InternalPacket *internalPacket =
                        resendBuffer[messageNumberNode->messageNumber & (uint32_t)RESEND_BUFFER_ARRAY_MASK];
                    if (internalPacket) {
                        if (internalPacket->nextActionTime != 0) {
                            internalPacket->nextActionTime = timeRead;
                        }
                    }

                    messageNumberNode = messageNumberNode->next;
                }
            }
        }
    }
    else {
        uint32_t skippedMessageCount;
        if (!congestionManager.OnGotPacket(dhf.datagramNumber, dhf.isContinuousSend, timeRead, length,
                                           &skippedMessageCount)) {
            peer->OnReliabilityLayerNotification(systemAddress, "congestionManager.OnGotPacket failed", true);
            return true;
        }
        if (dhf.isPacketPair) {
            congestionManager.OnGotPacketPair(dhf.datagramNumber, length, timeRead);
        }

        DatagramHeaderFormat dhfNAK;
        dhfNAK.isNAK = true;
        uint32_t skippedMessageOffset;
        for (skippedMessageOffset = skippedMessageCount; skippedMessageOffset > 0; skippedMessageOffset--) {
            NAKs.Insert(dhf.datagramNumber - skippedMessageOffset);
        }
        remoteSystemNeedsBAndAS = dhf.needsBAndAs;

        // Ack dhf.datagramNumber
        // Ack even unreliable messages for congestion control, just don't resend them on no ack
        SendAcknowledgementPacket(dhf.datagramNumber, 0);

        InternalPacket *internalPacket = CreateInternalPacketFromBitStream(&socketData, timeRead);
        if (internalPacket == 0) {
            peer->OnReliabilityLayerNotification(systemAddress, "CreateInternalPacketFromBitStream failed", true);
            return true;
        }

        while (internalPacket) {
            {

                // resetReceivedPackets is set from a non-threadsafe function.
                // We do the actual reset in this function so the data is not modified by multiple threads
                if (resetReceivedPackets) {
                    hasReceivedPacketQueue.clear();
                    receivedPacketsBaseIndex = 0;
                    resetReceivedPackets = false;
                }

                // Check for corrupt orderingChannel
                if (internalPacket->reliability == RELIABLE_SEQUENCED ||
                    internalPacket->reliability == UNRELIABLE_SEQUENCED ||
                    internalPacket->reliability == RELIABLE_ORDERED) {
                    if (internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS) {
                        peer->OnReliabilityLayerNotification(systemAddress, "internalPacket->orderingChannel >= NUMBER_OF_ORDERED_STREAMS",
                                                     true);
                        bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(
                            timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

                        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                        ReleaseToInternalPacketPool(internalPacket);
                        goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                    }
                }

                // 8/12/09 was previously not checking if the message was reliable. However, on packetloss this would
                // mean you'd eventually exceed the hole count because unreliable messages were never resent, and you'd
                // stop getting messages
                if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
                    internalPacket->reliability == RELIABLE_ORDERED) {
                    // If the following conditional is true then this either a duplicate packet
                    // or an older out of order packet
                    // The subtraction unsigned overflow is intentional
                    holeCount =
                        (DatagramSequenceNumberType)(internalPacket->reliableMessageNumber - receivedPacketsBaseIndex);
                    const DatagramSequenceNumberType typeRange = (DatagramSequenceNumberType)(const uint32_t)-1;

                    if (holeCount == (DatagramSequenceNumberType)0) {
                        // Got what we were expecting
                        if (hasReceivedPacketQueue.empty() == false) {
                            hasReceivedPacketQueue.pop_front();
                        }
                        ++receivedPacketsBaseIndex;
                    }
                    else if (holeCount > typeRange / (DatagramSequenceNumberType)2) {
                        bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(
                            timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

                        peer->OnReliabilityLayerNotification(systemAddress, "holeCount > typeRange/(DatagramSequenceNumberType) 2", false);
                        // Duplicate packet
                        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                        ReleaseToInternalPacketPool(internalPacket);

                        goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                    }
                    else if ((unsigned int)holeCount < hasReceivedPacketQueue.size()) {
                        // Got a higher count out of order packet that was missing in the sequence or we already got
                        if (hasReceivedPacketQueue[holeCount] != false)  // non-zero means this is a hole
                        {
#ifdef LOG_TRIVIAL_NOTIFICATIONS
                            peer->OnReliabilityLayerNotification(systemAddress, "Higher count pushed to hasReceivedPacketQueue", false);
#endif

                            // Fill in the hole
                            hasReceivedPacketQueue[holeCount] = false;  // We got the packet at holeCount
                        }
                        else {
                            bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(
                                timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

#ifdef LOG_TRIVIAL_NOTIFICATIONS
                            peer->OnReliabilityLayerNotification(systemAddress, "Duplicate packet ignored", false);
#endif

                            // Duplicate packet
                            FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                            ReleaseToInternalPacketPool(internalPacket);

                            goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                        }
                    }
                    else  // holeCount>=receivedPackets.Size()
                    {
                        if (holeCount > (DatagramSequenceNumberType)1000000) {
                            peer->OnReliabilityLayerNotification(systemAddress, "holeCount > 1000000", true);
                            bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_IGNORED].Push1(
                                timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

                            // Would crash due to out of memory!
                            FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                            ReleaseToInternalPacketPool(internalPacket);

                            goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                        }

#ifdef LOG_TRIVIAL_NOTIFICATIONS
                        peer->OnReliabilityLayerNotification(systemAddress, "Adding to hasReceivedPacketQueue later ordered message", false);
#endif

                        // Fix - sending on a higher priority gives us a very very high received packets base index if
                        // we formerly had pre-split a lot of messages and used that as the message number.  Because of
                        // this, a lot of time is spent in this linear loop and the timeout time expires because not all
                        // of the message is sent in time. Fixed by late assigning message IDs on the sender

                        // Add 0 times to the queue until (reliableMessageNumber - baseIndex) < queue size.
                        while ((unsigned int)(holeCount) > hasReceivedPacketQueue.size()) {
                            hasReceivedPacketQueue.push_back(
                                true);  // time+(CCTimeType)60 * (CCTimeType)1000 * (CCTimeType)1000);
                                        // // Didn't get this packet - set the time to give up waiting
                        }
                        hasReceivedPacketQueue.push_back(false);  // Got the packet
                    }

                    while (hasReceivedPacketQueue.empty() == false && hasReceivedPacketQueue.front() == false) {
                        hasReceivedPacketQueue.pop_front();
                        ++receivedPacketsBaseIndex;
                    }
                }

                // Is this a split packet? If so then reassemble
                if (internalPacket->splitPacketCount > 0) {
                    // Check for a rebuilt packet
                    if (internalPacket->reliability != RELIABLE_ORDERED &&
                        internalPacket->reliability != RELIABLE_SEQUENCED &&
                        internalPacket->reliability != UNRELIABLE_SEQUENCED) {
                        internalPacket->orderingChannel = 255;  // Use 255 to designate not sequenced and not ordered
                    }

                    InsertIntoSplitPacketList(internalPacket, timeRead);

                    internalPacket = BuildPacketFromSplitPacketList(internalPacket->splitPacketId, timeRead, peer,
                                                                    systemAddress, updateBitStream);

                    if (internalPacket == 0) {
#ifdef LOG_TRIVIAL_NOTIFICATIONS
                        peer->OnReliabilityLayerNotification(systemAddress, "BuildPacketFromSplitPacketList did not return anything.", false);
#endif

                        // Don't have all the parts yet
                        goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                    }
                }

                if (internalPacket->reliability == RELIABLE_SEQUENCED ||
                    internalPacket->reliability == UNRELIABLE_SEQUENCED ||
                    internalPacket->reliability == RELIABLE_ORDERED) {

                    if (internalPacket->orderingIndex == orderedReadIndex[internalPacket->orderingChannel]) {
                        // Has current ordering index
                        if (internalPacket->reliability == RELIABLE_SEQUENCED ||
                            internalPacket->reliability == UNRELIABLE_SEQUENCED) {
                            // Is sequenced
                            if (IsOlderOrderedPacket(internalPacket->sequencingIndex,
                                                     highestSequencedReadIndex[internalPacket->orderingChannel]) ==
                                false) {
                                // Expected or highest known value

                                // Update highest sequence
                                // 6/26/2012 - Did not have the +1 in the next statement
                                // Means a duplicated RELIABLE_SEQUENCED or UNRELIABLE_SEQUENCED packet would be
                                // returned to the user
                                highestSequencedReadIndex[internalPacket->orderingChannel] =
                                    internalPacket->sequencingIndex + (OrderingIndexType)1;

                                // Fallthrough, returned to user below
                            }
                            else {

#ifdef LOG_TRIVIAL_NOTIFICATIONS
                                peer->OnReliabilityLayerNotification(systemAddress, "Sequenced rejected: lower than highest known value",
                                                             false);
#endif

                                // Lower than highest known value
                                FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                                ReleaseToInternalPacketPool(internalPacket);

                                goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                            }
                        }
                        else {
                            // Push to output buffer immediately
                            bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(
                                timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));
                            outputQueue.push_back(internalPacket);

                            orderedReadIndex[internalPacket->orderingChannel]++;
                            highestSequencedReadIndex[internalPacket->orderingChannel] = 0;

                            // Return off heap until order lost
                            while (orderingHeaps[internalPacket->orderingChannel].Size() > 0 &&
                                   orderingHeaps[internalPacket->orderingChannel].Peek()->orderingIndex ==
                                       orderedReadIndex[internalPacket->orderingChannel]) {
                                internalPacket = orderingHeaps[internalPacket->orderingChannel].Pop(0);

                                bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(
                                    timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));
                                outputQueue.push_back(internalPacket);

                                if (internalPacket->reliability == RELIABLE_ORDERED) {
                                    orderedReadIndex[internalPacket->orderingChannel]++;
                                }
                                else {
                                    highestSequencedReadIndex[internalPacket->orderingChannel] =
                                        internalPacket->sequencingIndex;
                                }
                            }

                            // Done
                            goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                        }
                    }
                    else if (IsOlderOrderedPacket(internalPacket->orderingIndex,
                                                  orderedReadIndex[internalPacket->orderingChannel]) == false) {
                        // internalPacket->_orderingIndex is greater
                        // If a message has a greater ordering index, and is sequenced or ordered, buffer it
                        // Sequenced has a lower heap weight, ordered has max sequenced weight

                        // Keep orderedHoleCount count small
                        if (orderingHeaps[internalPacket->orderingChannel].Size() == 0) {
                            heapIndexOffsets[internalPacket->orderingChannel] =
                                orderedReadIndex[internalPacket->orderingChannel];
                        }

                        reliabilityHeapWeightType orderedHoleCount =
                            internalPacket->orderingIndex - heapIndexOffsets[internalPacket->orderingChannel];
                        reliabilityHeapWeightType weight = orderedHoleCount * 1048576;
                        if (internalPacket->reliability == RELIABLE_SEQUENCED ||
                            internalPacket->reliability == UNRELIABLE_SEQUENCED) {
                            weight += internalPacket->sequencingIndex;
                        }
                        else {
                            weight += (1048576 - 1);
                        }
                        orderingHeaps[internalPacket->orderingChannel].Push(weight, internalPacket, _FILE_AND_LINE_);

#ifdef LOG_TRIVIAL_NOTIFICATIONS
                        peer->OnReliabilityLayerNotification(systemAddress, "Larger number ordered packet leaving holes", false);
#endif

                        // Buffered, nothing to do
                        goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                    }
                    else {
                        // Out of order
                        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
                        ReleaseToInternalPacketPool(internalPacket);

#ifdef LOG_TRIVIAL_NOTIFICATIONS
                        peer->OnReliabilityLayerNotification(systemAddress, "Rejected older resend", false);
#endif

                        // Ignored, nothing to do
                        goto CONTINUE_SOCKET_DATA_PARSE_LOOP;
                    }
                }

                bpsMetrics[(int)USER_MESSAGE_BYTES_RECEIVED_PROCESSED].Push1(
                    timeRead, BITS_TO_BYTES(internalPacket->dataBitLength));

                // Nothing special about this packet.  Add it to the output queue
                outputQueue.push_back(internalPacket);

                internalPacket = 0;
            }

            // Used for a goto to jump to the resendNext packet immediately

        CONTINUE_SOCKET_DATA_PARSE_LOOP:
            // Parse the bitstream to create an internal packet
            internalPacket = CreateInternalPacketFromBitStream(&socketData, timeRead);
        }
    }

    receivePacketCount++;

    return true;
}

// This gets an end-user packet already parsed out. Returns number of BITS put into the buffer
BitSize_t ReliabilityLayer::Receive(unsigned char **data)
{
    InternalPacket *internalPacket;

    if (outputQueue.empty() == false) {
        internalPacket = outputQueue.front();
        outputQueue.pop_front();

        BitSize_t bitLength;
        *data = internalPacket->data;
        bitLength = internalPacket->dataBitLength;
        ReleaseToInternalPacketPool(internalPacket);
        return bitLength;
    }

    else {
        return 0;
    }
}

// Puts data on the send queue
// bitStream contains the data to send
// priority is what priority to send the data at
// reliability is what reliability to use
// ordering channel is from 0 to 255 and specifies what stream to use
bool ReliabilityLayer::Send(char *data, BitSize_t numberOfBitsToSend, PacketPriority priority,
                            PacketReliability reliability, unsigned char orderingChannel, bool makeDataCopy,
                            int MTUSize, CCTimeType currentTime, uint32_t receipt)
{

    (void)MTUSize;

    // Fix any bad parameters
    if (reliability > RELIABLE_ORDERED_WITH_ACK_RECEIPT || reliability < 0) {
        reliability = RELIABLE;
    }

    if (priority > NUMBER_OF_PRIORITIES || priority < 0) {
        priority = HIGH_PRIORITY;
    }

    if (orderingChannel >= NUMBER_OF_ORDERED_STREAMS) {
        orderingChannel = 0;
    }

    unsigned int numberOfBytesToSend = (unsigned int)BITS_TO_BYTES(numberOfBitsToSend);
    if (numberOfBitsToSend == 0) {
        return false;
    }
    InternalPacket *internalPacket = AllocateFromInternalPacketPool();
    if (internalPacket == 0) {
        notifyOutOfMemory(_FILE_AND_LINE_);
        return false;  // Out of memory
    }

    bpsMetrics[(int)USER_MESSAGE_BYTES_PUSHED].Push1(currentTime, numberOfBytesToSend);

    internalPacket->creationTime = currentTime;

    if (makeDataCopy) {
        AllocInternalPacketData(internalPacket, numberOfBytesToSend, true, _FILE_AND_LINE_);
        memcpy(internalPacket->data, data, numberOfBytesToSend);
    }
    else {
        // Allocated the data elsewhere, delete it in here
        AllocInternalPacketData(internalPacket, (unsigned char *)data);
    }

    internalPacket->dataBitLength = numberOfBitsToSend;
    internalPacket->messageInternalOrder = internalOrderIndex++;
    internalPacket->priority = priority;
    internalPacket->reliability = reliability;
    internalPacket->sendReceiptSerial = receipt;

    // Calculate if I need to split the packet
    unsigned int maxDataSizeBytes =
        GetMaxDatagramSizeExcludingMessageHeaderBytes() - BITS_TO_BYTES(GetMaxMessageHeaderLengthBits());

    bool splitPacket = numberOfBytesToSend > maxDataSizeBytes;

    // If a split packet, we might have to upgrade the reliability
    if (splitPacket) {
        // Split packets cannot be unreliable, in case that one part doesn't arrive and the whole cannot be reassembled.
        // One part could not arrive either due to packetloss or due to unreliable discard
        if (internalPacket->reliability == UNRELIABLE) {
            internalPacket->reliability = RELIABLE;
        }
        else if (internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT) {
            internalPacket->reliability = RELIABLE_WITH_ACK_RECEIPT;
        }
        else if (internalPacket->reliability == UNRELIABLE_SEQUENCED) {
            internalPacket->reliability = RELIABLE_SEQUENCED;
        }
    }

    if (internalPacket->reliability == RELIABLE_SEQUENCED || internalPacket->reliability == UNRELIABLE_SEQUENCED) {
        // Assign the sequence stream and index
        internalPacket->orderingChannel = orderingChannel;
        internalPacket->orderingIndex = orderedWriteIndex[orderingChannel];
        internalPacket->sequencingIndex = sequencedWriteIndex[orderingChannel]++;
    }
    else if (internalPacket->reliability == RELIABLE_ORDERED ||
             internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        // Assign the ordering channel and index
        internalPacket->orderingChannel = orderingChannel;
        internalPacket->orderingIndex = orderedWriteIndex[orderingChannel]++;
        sequencedWriteIndex[orderingChannel] = 0;
    }

    if (splitPacket)  // If it uses a secure header it will be generated here
    {
        // Must split the packet.  This will also generate the SHA1 if it is required. It also adds it to the send list.
        SplitPacket(internalPacket);
        return true;
    }

    RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
    AddToUnreliableLinkedList(internalPacket);

    RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
    RakAssert(internalPacket->messageNumberAssigned == false);
    outgoingPacketBuffer.Push(GetNextWeight(internalPacket->priority), internalPacket, _FILE_AND_LINE_);
    RakAssert(outgoingPacketBuffer.Size() == 0 ||
              outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
    statistics.messageInSendBuffer[(int)internalPacket->priority]++;
    statistics.bytesInSendBuffer[(int)internalPacket->priority] += (double)BITS_TO_BYTES(internalPacket->dataBitLength);

    return true;
}
// Run this once per game cycle.  Handles internal lists and actually does the send
void ReliabilityLayer::Update(RakPeer *peer, SystemAddress &systemAddress, int MTUSize, CCTimeType time,
                              unsigned bitsPerSecondLimit, BitStream &updateBitStream)

{
    (void)MTUSize;

    // This line is necessary because the timer isn't accurate
    if (time <= lastUpdateTime) {
        // Always set the last time in case of overflow
        lastUpdateTime = time;
        return;
    }

    CCTimeType timeSinceLastTick = time - lastUpdateTime;
    lastUpdateTime = time;
    if (timeSinceLastTick > 100000) {
        timeSinceLastTick = 100000;
    }

    if (unreliableTimeout > 0) {
        if (timeSinceLastTick >= timeToNextUnreliableCull) {
            if (unreliableLinkedListHead) {
                // Cull out all unreliable messages that have exceeded the timeout
                InternalPacket *cur = unreliableLinkedListHead;
                InternalPacket *end = unreliableLinkedListHead->unreliablePrev;

#ifdef _MSC_VER
#pragma warning(disable : 4127)  // warning C4127: conditional expression is constant
#endif
                while (1) {
                    if (time > cur->creationTime + (CCTimeType)unreliableTimeout) {
                        // Flag invalid, and clear the memory. Still needs to be removed from the sendPacketSet later
                        // This fixes a problem where a remote system disconnects, but we don't know it yet, and memory
                        // consumption increases to a huge value
                        FreeInternalPacketData(cur, _FILE_AND_LINE_);
                        cur->data = 0;
                        InternalPacket *next = cur->unreliableNext;
                        RemoveFromUnreliableLinkedList(cur);

                        if (cur == end) {
                            break;
                        }

                        cur = next;
                    }
                    else {

                        // They should be inserted in-order, so no need to iterate past the first failure
                        break;
                    }
                }
            }

            timeToNextUnreliableCull = unreliableTimeout / (CCTimeType)2;
        }
        else {
            timeToNextUnreliableCull -= timeSinceLastTick;
        }
    }

    // Due to thread vagarities and the way I store the time to avoid slow calls to RakNet::GetTime
    // time may be less than lastAck
    if (statistics.messagesInResendBuffer != 0 && AckTimeout(RakNet::TimeMS(time / (CCTimeType)1000))) {
        // SHOW - dead connection
        // We've waited a very long time for a reliable packet to get an ack and it never has
        deadConnection = true;
        return;
    }

    if (congestionManager.ShouldSendACKs(time, timeSinceLastTick)) {
        SendACKs(peer, systemAddress, time, updateBitStream);
    }

    if (NAKs.Size() > 0) {
        updateBitStream.Reset();
        DatagramHeaderFormat dhfNAK;
        dhfNAK.isNAK = true;
        dhfNAK.isACK = false;
        dhfNAK.isPacketPair = false;
        dhfNAK.Serialize(&updateBitStream);
        NAKs.Serialize(&updateBitStream, GetMaxDatagramSizeExcludingMessageHeaderBits(), true);
        SendBitStream(peer, systemAddress, &updateBitStream, time);
    }

    DatagramHeaderFormat dhf;
    dhf.needsBAndAs = congestionManager.GetIsInSlowStart();
    dhf.isContinuousSend = bandwidthExceededStatistic;
    bandwidthExceededStatistic = outgoingPacketBuffer.Size() > 0;

    const bool hasDataToSendOrResend = IsResendQueueEmpty() == false || bandwidthExceededStatistic;
    RakAssert(NUMBER_OF_PRIORITIES == 4);
    congestionManager.Update(time, hasDataToSendOrResend);

    statistics.BPSLimitByOutgoingBandwidthLimit = BITS_TO_BYTES(bitsPerSecondLimit);
    statistics.BPSLimitByCongestionControl = congestionManager.GetBytesPerSecondLimitByCongestionControl();

    unsigned int i;
    if (time > lastBpsClear + 100000) {
        for (i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++) {
            bpsMetrics[i].ClearExpired1(time);
        }

        lastBpsClear = time;
    }

    if (unreliableWithAckReceiptHistory.size() > 0) {
        i = 0;
        while (i < unreliableWithAckReceiptHistory.size()) {
            if (time - unreliableWithAckReceiptHistory[i].nextActionTime < (((CCTimeType)-1) / 2)) {
                InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
                AllocInternalPacketData(ackReceipt, 5, false, _FILE_AND_LINE_);
                ackReceipt->dataBitLength = BYTES_TO_BITS(5);
                ackReceipt->data[0] = (MessageID)ID_SND_RECEIPT_LOSS;
                memcpy(ackReceipt->data + sizeof(MessageID), &unreliableWithAckReceiptHistory[i].sendReceiptSerial,
                       sizeof(uint32_t));
                outputQueue.push_back(ackReceipt);

                // Remove, swap with last
                unreliableWithAckReceiptHistory.erase(unreliableWithAckReceiptHistory.begin() + i);
            }
            else {
                i++;
            }
        }
    }

    if (hasDataToSendOrResend == true) {
        InternalPacket *internalPacket;
        bool pushedAnything;
        BitSize_t nextPacketBitLength;
        dhf.isACK = false;
        dhf.isNAK = false;
        dhf.hasBAndAS = false;
        ResetPacketsAndDatagrams();

        int transmissionBandwidth = congestionManager.GetTransmissionBandwidth(
            time, timeSinceLastTick, unacknowledgedBytes, dhf.isContinuousSend);
        int retransmissionBandwidth = congestionManager.GetRetransmissionBandwidth(
            time, timeSinceLastTick, unacknowledgedBytes, dhf.isContinuousSend);
        if (retransmissionBandwidth > 0 || transmissionBandwidth > 0) {
            statistics.isLimitedByCongestionControl = false;

            allDatagramSizesSoFar = 0;

            // Keep filling datagrams until we exceed retransmission bandwidth
            while ((int)BITS_TO_BYTES(allDatagramSizesSoFar) < retransmissionBandwidth) {
                pushedAnything = false;

                // Fill one datagram, then break
                while (IsResendQueueEmpty() == false) {
                    internalPacket = resendLinkedListHead;
                    RakAssert(internalPacket->messageNumberAssigned == true);

                    if (time - internalPacket->nextActionTime < (((CCTimeType)-1) / 2)) {
                        nextPacketBitLength = internalPacket->headerLength + internalPacket->dataBitLength;
                        if (datagramSizeSoFar + nextPacketBitLength > GetMaxDatagramSizeExcludingMessageHeaderBits()) {
                            // Gathers all PushPackets()
                            PushDatagram();
                            break;
                        }

                        PopListHead(false);

                        CC_DEBUG_PRINTF_2("Rs %i ", internalPacket->reliableMessageNumber.val);

                        bpsMetrics[(int)USER_MESSAGE_BYTES_RESENT].Push1(time,
                                                                         BITS_TO_BYTES(internalPacket->dataBitLength));

                        PushPacket(time, internalPacket, true);  // Affects GetNewTransmissionBandwidth()
                        internalPacket->timesSent++;
                        congestionManager.OnResend(time, internalPacket->nextActionTime);
                        internalPacket->retransmissionTime =
                            congestionManager.GetRTOForRetransmission(internalPacket->timesSent);
                        internalPacket->nextActionTime = internalPacket->retransmissionTime + time;

                        pushedAnything = true;

                        // Put the packet back into the resend list at the correct spot
                        // Don't make a copy since I'm reinserting an allocated struct
                        InsertPacketIntoResendList(internalPacket, time, false, false);
                    }
                    else {
                        // Filled one datagram.
                        // If the 2nd and it's time to send a datagram pair, will be marked as a pair
                        PushDatagram();
                        break;
                    }
                }

                if (pushedAnything == false) {
                    break;
                }
            }
        }
        else {
            statistics.isLimitedByCongestionControl = true;
        }

        if ((int)BITS_TO_BYTES(allDatagramSizesSoFar) < transmissionBandwidth) {
            allDatagramSizesSoFar = 0;

            // Keep filling datagrams until we exceed transmission bandwidth
            while (ResendBufferOverflow() == false &&
                   ((int)BITS_TO_BYTES(allDatagramSizesSoFar) < transmissionBandwidth ||
                    // This condition means if we want to send a datagram pair, and only have one datagram buffered,
                    // exceed bandwidth to add another
                    (countdownToNextPacketPair == 0 && datagramsToSendThisUpdateIsPair.size() == 1))) {
                // Fill with packets until MTU is reached
                pushedAnything = false;

                statistics.isLimitedByOutgoingBandwidthLimit =
                    bitsPerSecondLimit != 0 &&
                    BITS_TO_BYTES(bitsPerSecondLimit) < bpsMetrics[USER_MESSAGE_BYTES_SENT].GetBPS1(time);

                while (outgoingPacketBuffer.Size() && statistics.isLimitedByOutgoingBandwidthLimit == false) {
                    internalPacket = outgoingPacketBuffer.Peek();
                    RakAssert(internalPacket->messageNumberAssigned == false);
                    RakAssert(outgoingPacketBuffer.Size() == 0 ||
                              outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));

                    if (internalPacket->data == 0) {
                        outgoingPacketBuffer.Pop(0);
                        RakAssert(outgoingPacketBuffer.Size() == 0 ||
                                  outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
                        statistics.messageInSendBuffer[(int)internalPacket->priority]--;
                        statistics.bytesInSendBuffer[(int)internalPacket->priority] -=
                            (double)BITS_TO_BYTES(internalPacket->dataBitLength);
                        ReleaseToInternalPacketPool(internalPacket);
                        continue;
                    }

                    internalPacket->headerLength = GetMessageHeaderLengthBits(internalPacket);
                    nextPacketBitLength = internalPacket->headerLength + internalPacket->dataBitLength;
                    if (datagramSizeSoFar + nextPacketBitLength > GetMaxDatagramSizeExcludingMessageHeaderBits()) {
                        // Hit MTU. May still push packets if smaller ones exist at a lower priority
                        RakAssert(datagramSizeSoFar != 0);
                        RakAssert(internalPacket->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
                        break;
                    }

                    bool isReliable;
                    if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
                        internalPacket->reliability == RELIABLE_ORDERED ||
                        internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT ||
                        //						internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT  ||
                        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
                        isReliable = true;
                    }
                    else {
                        isReliable = false;
                    }

                    outgoingPacketBuffer.Pop(0);
                    RakAssert(outgoingPacketBuffer.Size() == 0 ||
                              outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
                    RakAssert(internalPacket->messageNumberAssigned == false);
                    statistics.messageInSendBuffer[(int)internalPacket->priority]--;
                    statistics.bytesInSendBuffer[(int)internalPacket->priority] -=
                        (double)BITS_TO_BYTES(internalPacket->dataBitLength);
                    if (isReliable
                        /*
                        I thought about this and agree that UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT and
                        RELIABLE_SEQUENCED_WITH_ACK_RECEIPT is not useful unless you also know if the message was
                        discarded.

                        The problem is that internally, message numbers are only assigned to reliable messages, because
                        message numbers are only used to discard duplicate message receipt and only reliable messages
                        get sent more than once. However, without message numbers getting assigned and transmitted,
                        there is no way to tell the sender about which messages were discarded. In fact, in looking this
                        over I realized that UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT introduced a bug, because the remote
                        system assumes all message numbers are used (no holes). With that send type, on packetloss, a
                        permanent hole would have been created which eventually would cause the system to discard all
                        further packets.

                        So I have two options. Either do not support ack receipts when sending sequenced, or write
                        complex and major new systems. UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT would need to send the
                        message ID number on a special channel which allows for non-delivery. And both of them would
                        need to have a special range list to indicate which message numbers were not delivered, so when
                        acks are sent that can be indicated as well. A further problem is that the ack itself can be
                        lost - it is possible that the message can arrive but be discarded, yet the ack is lost. On
                        resend, the resent message would be ignored as duplicate, and you'd never get the discard
                        message either (unless I made a special buffer for that case too).
*/
                        //						||
                        // If needs an ack receipt, keep the internal packet around in the list
                        //						internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT ||
                        //						internalPacket->reliability == UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
                    ) {
                        internalPacket->messageNumberAssigned = true;
                        internalPacket->reliableMessageNumber = sendReliableMessageNumberIndex;
                        internalPacket->retransmissionTime =
                            congestionManager.GetRTOForRetransmission(internalPacket->timesSent + 1);
                        internalPacket->nextActionTime = internalPacket->retransmissionTime + time;
                        const CCTimeType threshhold = 10000000;
                        if (internalPacket->nextActionTime - time > threshhold) {
                            RakAssert(time - internalPacket->nextActionTime < threshhold);
                        }
                        if (resendBuffer[internalPacket->reliableMessageNumber & (uint32_t)RESEND_BUFFER_ARRAY_MASK] !=
                            0) {
                            RakAssert(0);
                        }
                        resendBuffer[internalPacket->reliableMessageNumber & (uint32_t)RESEND_BUFFER_ARRAY_MASK] =
                            internalPacket;
                        statistics.messagesInResendBuffer++;
                        statistics.bytesInResendBuffer += BITS_TO_BYTES(internalPacket->dataBitLength);

                        InsertPacketIntoResendList(internalPacket, time, true, isReliable);

                        sendReliableMessageNumberIndex++;
                    }
                    else if (internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT) {
                        unreliableWithAckReceiptHistory.push_back(UnreliableWithAckReceiptNode(
                            congestionManager.GetNextDatagramSequenceNumber() +
                                (unsigned int)packetsToSendThisUpdateDatagramBoundaries.size(),
                            internalPacket->sendReceiptSerial,
                            congestionManager.GetRTOForRetransmission(internalPacket->timesSent + 1) + time));
                    }

                    // If isReliable is false, the packet and its contents will be added to a list to be freed in
                    // ClearPacketsAndDatagrams However, the internalPacket structure will remain allocated and be in
                    // the resendBuffer list if it requires a receipt
                    bpsMetrics[(int)USER_MESSAGE_BYTES_SENT].Push1(time, BITS_TO_BYTES(internalPacket->dataBitLength));

                    PushPacket(time, internalPacket, isReliable);
                    internalPacket->timesSent++;

                    pushedAnything = true;

                    if (ResendBufferOverflow()) {
                        break;
                    }
                }

                // No datagrams pushed?
                if (datagramSizeSoFar == 0) {
                    break;
                }

                // Filled one datagram.
                // If the 2nd and it's time to send a datagram pair, will be marked as a pair
                PushDatagram();
            }
        }

        for (unsigned int datagramIndex = 0; datagramIndex < packetsToSendThisUpdateDatagramBoundaries.size();
             datagramIndex++) {
            if (datagramIndex > 0) {
                dhf.isContinuousSend = true;
            }
            MessageNumberNode *messageNumberNode = 0;
            dhf.datagramNumber = congestionManager.GetAndIncrementNextDatagramSequenceNumber();
            dhf.isPacketPair = datagramsToSendThisUpdateIsPair[datagramIndex];

            bool isSecondOfPacketPair =
                dhf.isPacketPair && datagramIndex > 0 && datagramsToSendThisUpdateIsPair[datagramIndex - 1];
            unsigned int msgIndex, msgTerm;
            if (datagramIndex == 0) {
                msgIndex = 0;
                msgTerm = packetsToSendThisUpdateDatagramBoundaries[0];
            }
            else {
                msgIndex = packetsToSendThisUpdateDatagramBoundaries[datagramIndex - 1];
                msgTerm = packetsToSendThisUpdateDatagramBoundaries[datagramIndex];
            }

            // More accurate time to reset here
            updateBitStream.Reset();
            dhf.Serialize(&updateBitStream);
            CC_DEBUG_PRINTF_2("S%i ", dhf.datagramNumber.val);

            while (msgIndex < msgTerm) {
                // If reliable or needs receipt
                if (packetsToSendThisUpdate[msgIndex]->reliability != UNRELIABLE &&
                    packetsToSendThisUpdate[msgIndex]->reliability != UNRELIABLE_SEQUENCED) {
                    if (messageNumberNode == 0) {
                        messageNumberNode = AddFirstToDatagramHistory(
                            dhf.datagramNumber, packetsToSendThisUpdate[msgIndex]->reliableMessageNumber, time);
                    }
                    else {
                        messageNumberNode = AddSubsequentToDatagramHistory(
                            messageNumberNode, packetsToSendThisUpdate[msgIndex]->reliableMessageNumber);
                    }
                }

                RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
                WriteToBitStreamFromInternalPacket(&updateBitStream, packetsToSendThisUpdate[msgIndex], time);
                RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
                msgIndex++;
            }

            if (isSecondOfPacketPair) {
                // Pad to size of first datagram
                RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
                updateBitStream.PadWithZeroToByteLength(datagramSizesInBytes[datagramIndex - 1]);
                RakAssert(updateBitStream.GetNumberOfBytesUsed() <= MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
            }

            if (messageNumberNode == 0) {
                // Unreliable, add dummy node
                AddFirstToDatagramHistory(dhf.datagramNumber, time);
            }

            congestionManager.OnSendBytes(time, UDP_HEADER_SIZE + DatagramHeaderFormat::GetDataHeaderByteLength());

            SendBitStream(peer, systemAddress, &updateBitStream, time);

            bandwidthExceededStatistic = outgoingPacketBuffer.Size() > 0;

            if (bandwidthExceededStatistic == true) {
                timeOfLastContinualSend = time;
            }
            else {
                timeOfLastContinualSend = 0;
            }
        }

        ClearPacketsAndDatagrams();

        // Any data waiting to send after attempting to send, then bandwidth is exceeded
        bandwidthExceededStatistic = outgoingPacketBuffer.Size() > 0;
    }
}

// Writes a bitstream to the socket
void ReliabilityLayer::SendBitStream(RakPeer *peer, SystemAddress &systemAddress, RakNet::BitStream *bitStream,
                                     CCTimeType currentTime)
{
    (void)systemAddress;

    unsigned int length;

    length = (unsigned int)bitStream->GetNumberOfBytesUsed();

    bpsMetrics[(int)ACTUAL_BYTES_SENT].Push1(currentTime, length);

    RakAssert(length <= congestionManager.GetMTU());

    peer->SendDatagram(systemAddress, (const char *)bitStream->GetData(), length);
}

// Are we waiting for any data to be sent out or be processed by the player?
bool ReliabilityLayer::IsOutgoingDataWaiting(void)
{
    if (outgoingPacketBuffer.Size() > 0) {
        return true;
    }

    return statistics.messagesInResendBuffer != 0;
}
bool ReliabilityLayer::AreAcksWaiting(void)
{
    return acknowlegements.Size() > 0;
}
void ReliabilityLayer::SetSplitMessageProgressInterval(int interval)
{
    splitMessageProgressInterval = interval;
}
void ReliabilityLayer::SetUnreliableTimeout(RakNet::TimeMS timeoutMS)
{
    unreliableTimeout = (CCTimeType)timeoutMS * (CCTimeType)1000;
}

// This will return true if we should not send at this time
bool ReliabilityLayer::IsSendThrottled(int MTUSize)
{
    (void)MTUSize;

    return false;
}

// We lost a packet
void ReliabilityLayer::UpdateWindowFromPacketloss(CCTimeType time)
{
    (void)time;
}

// Increase the window size
void ReliabilityLayer::UpdateWindowFromAck(CCTimeType time)
{
    (void)time;
}

// Does what the function name says
unsigned ReliabilityLayer::RemovePacketFromResendListAndDeleteOlderReliableSequenced(
    const MessageNumberType messageNumber, CCTimeType time, const SystemAddress &systemAddress)
{
    (void)time;
    (void)messageNumber;
    InternalPacket *internalPacket;

    internalPacket = resendBuffer[messageNumber & RESEND_BUFFER_ARRAY_MASK];
    // May ask to remove twice, for example resend twice, then second ack
    if (internalPacket && internalPacket->reliableMessageNumber == messageNumber) {
        resendBuffer[messageNumber & RESEND_BUFFER_ARRAY_MASK] = 0;
        CC_DEBUG_PRINTF_2("AckRcv %i ", messageNumber);

        statistics.messagesInResendBuffer--;
        statistics.bytesInResendBuffer -= BITS_TO_BYTES(internalPacket->dataBitLength);

        totalUserDataBytesAcked += (double)BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);

        // Return receipt if asked for
        if (internalPacket->reliability >= RELIABLE_WITH_ACK_RECEIPT &&
            (internalPacket->splitPacketCount == 0 ||
             internalPacket->splitPacketIndex + 1 == internalPacket->splitPacketCount)) {
            InternalPacket *ackReceipt = AllocateFromInternalPacketPool();
            AllocInternalPacketData(ackReceipt, 5, false, _FILE_AND_LINE_);
            ackReceipt->dataBitLength = BYTES_TO_BITS(5);
            ackReceipt->data[0] = (MessageID)ID_SND_RECEIPT_ACKED;
            memcpy(ackReceipt->data + sizeof(MessageID), &internalPacket->sendReceiptSerial,
                   sizeof(internalPacket->sendReceiptSerial));
            outputQueue.push_back(ackReceipt);
        }

        bool isReliable;
        if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
            internalPacket->reliability == RELIABLE_ORDERED ||
            internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT ||
            //			internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT  ||
            internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
            isReliable = true;
        }
        else {
            isReliable = false;
        }

        RemoveFromList(internalPacket, isReliable);
        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
        ReleaseToInternalPacketPool(internalPacket);

        return 0;
    }
    else {
    }

    return (unsigned)-1;
}

// Acknowledge receipt of the packet with the specified messageNumber
void ReliabilityLayer::SendAcknowledgementPacket(const DatagramSequenceNumberType messageNumber, CCTimeType time)
{

    nextAckTimeToSend = time;
    acknowlegements.Insert(messageNumber);

    CC_DEBUG_PRINTF_2("AckPush %i ", messageNumber);
}

// Parse an internalPacket and figure out how many header bits would be
// written.  Returns that number
BitSize_t ReliabilityLayer::GetMaxMessageHeaderLengthBits(void)
{
    InternalPacket ip;
    ip.reliability = RELIABLE_SEQUENCED;
    ip.splitPacketCount = 1;
    return GetMessageHeaderLengthBits(&ip);
}
BitSize_t ReliabilityLayer::GetMessageHeaderLengthBits(const InternalPacket *const internalPacket)
{
    BitSize_t bitLength;

    bitLength = 8 * 1;

    bitLength += 8 * 2;

    if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED || internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT ||
        //		internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT ||
        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        bitLength += 8 * 3;  // bitStream->Write(internalPacket->reliableMessageNumber); // Message sequence number
    }
    // bitStream->AlignWriteToByteBoundary(); // Potentially nothing else to write

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED) {
        bitLength += 8 * 3;
        ;  // bitStream->Write(internalPacket->_sequencingIndex); // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED,
           // RELIABLE_ORDERED.
    }

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED ||
        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        bitLength += 8 * 3;  // bitStream->Write(internalPacket->orderingIndex); // Used for UNRELIABLE_SEQUENCED,
                             // RELIABLE_SEQUENCED, RELIABLE_ORDERED.
        bitLength +=
            8 *
            1;  // tempChar=internalPacket->orderingChannel; bitStream->WriteAlignedVar8((const char*)& tempChar); //
                // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED. 5 bits needed, write one byte
    }
    if (internalPacket->splitPacketCount > 0) {
        bitLength += 8 * 4;  // bitStream->WriteAlignedVar32((const char*)& internalPacket->splitPacketCount);
        bitLength +=
            8 *
            sizeof(SplitPacketIdType);  // bitStream->WriteAlignedVar16((const char*)& internalPacket->splitPacketId);
        bitLength += 8 * 4;  // bitStream->WriteAlignedVar32((const char*)& internalPacket->splitPacketIndex); // Only
                             // needed if splitPacketCount>0. 4 bytes
    }

    return bitLength;
}

// Parse an internalPacket and create a bitstream to represent this data
BitSize_t ReliabilityLayer::WriteToBitStreamFromInternalPacket(RakNet::BitStream *bitStream,
                                                               const InternalPacket *const internalPacket,
                                                               CCTimeType curTime)
{
    (void)curTime;

    BitSize_t start = bitStream->GetNumberOfBitsUsed();
    unsigned char tempChar;

    // (Incoming data may be all zeros due to padding)
    bitStream->AlignWriteToByteBoundary();  // Potentially unaligned
    if (internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT) {
        tempChar = UNRELIABLE;
    }
    else if (internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT) {
        tempChar = RELIABLE;
    }
    else if (internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        tempChar = RELIABLE_ORDERED;
    }
    else {
        tempChar = (unsigned char)internalPacket->reliability;
    }

    bitStream->WriteBits((const unsigned char *)&tempChar, 3, true);  // 3 bits to write reliability.

    bool hasSplitPacket = internalPacket->splitPacketCount > 0;
    bitStream->Write(hasSplitPacket);  // Write 1 bit to indicate if splitPacketCount>0
    bitStream->AlignWriteToByteBoundary();
    RakAssert(internalPacket->dataBitLength < 65535);
    unsigned short s;
    s = (unsigned short)internalPacket->dataBitLength;
    bitStream->WriteAlignedVar16((const char *)&s);
    if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED || internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT ||
        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        bitStream->Write(internalPacket->reliableMessageNumber);  // Used for all reliable types
    }
    bitStream->AlignWriteToByteBoundary();  // Potentially nothing else to write

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED) {
        bitStream->Write(
            internalPacket->sequencingIndex);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED.
    }

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED ||
        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        bitStream->Write(
            internalPacket->orderingIndex);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED.
        tempChar = internalPacket->orderingChannel;
        bitStream->WriteAlignedVar8((const char *)&tempChar);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED,
                                                               // RELIABLE_ORDERED. 5 bits needed, write one byte
    }

    if (internalPacket->splitPacketCount > 0) {

        bitStream->WriteAlignedVar32((const char *)&internalPacket->splitPacketCount);
        RakAssert(sizeof(SplitPacketIndexType) == 4);  // Only needed if splitPacketCount>0. 4 bytes
        bitStream->WriteAlignedVar16((const char *)&internalPacket->splitPacketId);
        RakAssert(sizeof(SplitPacketIdType) == 2);  // Only needed if splitPacketCount>0.
        bitStream->WriteAlignedVar32(
            (const char *)&internalPacket->splitPacketIndex);  // Only needed if splitPacketCount>0. 4 bytes
    }

    // Write the actual data.
    bitStream->WriteAlignedBytes((unsigned char *)internalPacket->data, BITS_TO_BYTES(internalPacket->dataBitLength));

    return bitStream->GetNumberOfBitsUsed() - start;
}

// Parse a bitstream and create an internal packet to represent this data
InternalPacket *ReliabilityLayer::CreateInternalPacketFromBitStream(RakNet::BitStream *bitStream, CCTimeType time)
{
    bool bitStreamSucceeded;
    InternalPacket *internalPacket;
    unsigned char tempChar;
    bool hasSplitPacket = false;
    bool readSuccess;

    if (bitStream->GetNumberOfUnreadBits() < (int)sizeof(internalPacket->reliableMessageNumber) * 8) {
        return 0;  // leftover bits
    }

    internalPacket = AllocateFromInternalPacketPool();
    if (internalPacket == 0) {
        // Out of memory
        RakAssert(0);
        return 0;
    }
    internalPacket->creationTime = time;

    // (Incoming data may be all zeros due to padding)
    bitStream->AlignReadToByteBoundary();  // Potentially unaligned
    bitStream->ReadBits((unsigned char *)(&(tempChar)), 3);
    internalPacket->reliability = (const PacketReliability)tempChar;
    readSuccess = bitStream->Read(hasSplitPacket);  // Read 1 bit to indicate if splitPacketCount>0
    bitStream->AlignReadToByteBoundary();
    unsigned short s;
    bitStream->ReadAlignedVar16((char *)&s);
    internalPacket->dataBitLength = s;  // Length of message (2 bytes)
    if (internalPacket->reliability == RELIABLE || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED
        // I don't write ACK_RECEIPT to the remote system
        // 		||
        // 		internalPacket->reliability == RELIABLE_WITH_ACK_RECEIPT ||
        // 		internalPacket->reliability == RELIABLE_SEQUENCED_WITH_ACK_RECEIPT ||
        // 		internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT
    ) {
        bitStream->Read(internalPacket->reliableMessageNumber);  // Message sequence number
    }
    else {
        internalPacket->reliableMessageNumber = (MessageNumberType)(const uint32_t)-1;
    }
    bitStream->AlignReadToByteBoundary();  // Potentially nothing else to Read

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED) {
        bitStream->Read(
            internalPacket->sequencingIndex);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED.
    }

    if (internalPacket->reliability == UNRELIABLE_SEQUENCED || internalPacket->reliability == RELIABLE_SEQUENCED ||
        internalPacket->reliability == RELIABLE_ORDERED ||
        internalPacket->reliability == RELIABLE_ORDERED_WITH_ACK_RECEIPT) {
        bitStream->Read(
            internalPacket
                ->orderingIndex);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED, RELIABLE_ORDERED. 4 bytes.
        readSuccess = bitStream->ReadAlignedVar8(
            (char *)&internalPacket->orderingChannel);  // Used for UNRELIABLE_SEQUENCED, RELIABLE_SEQUENCED,
                                                        // RELIABLE_ORDERED. 5 bits needed, Read one byte
    }
    else {
        internalPacket->orderingChannel = 0;
    }

    if (hasSplitPacket) {

        bitStream->ReadAlignedVar32(
            (char *)&internalPacket->splitPacketCount);  // Only needed if splitPacketCount>0. 4 bytes
        bitStream->ReadAlignedVar16((char *)&internalPacket->splitPacketId);  // Only needed if splitPacketCount>0.
        readSuccess = bitStream->ReadAlignedVar32(
            (char *)&internalPacket->splitPacketIndex);  // Only needed if splitPacketCount>0. 4 bytes
        RakAssert(readSuccess);
    }
    else {
        internalPacket->splitPacketCount = 0;
    }

    if (readSuccess == false || internalPacket->dataBitLength == 0 ||
        internalPacket->reliability >= NUMBER_OF_RELIABILITIES || internalPacket->orderingChannel >= 32 ||
        (hasSplitPacket && (internalPacket->splitPacketIndex >= internalPacket->splitPacketCount))) {
        // If this assert hits, encoding is garbage
        RakAssert("Encoding is garbage" && 0);
        ReleaseToInternalPacketPool(internalPacket);
        return 0;
    }

    // Allocate memory to hold our data
    AllocInternalPacketData(internalPacket, BITS_TO_BYTES(internalPacket->dataBitLength), false, _FILE_AND_LINE_);
    RakAssert(BITS_TO_BYTES(internalPacket->dataBitLength) < MAXIMUM_MTU_SIZE);

    if (internalPacket->data == 0) {
        RakAssert("Out of memory in ReliabilityLayer::CreateInternalPacketFromBitStream" && 0);
        notifyOutOfMemory(_FILE_AND_LINE_);
        ReleaseToInternalPacketPool(internalPacket);
        return 0;
    }

    // Set the last byte to 0 so if ReadBits does not read a multiple of 8 the last bits are 0'ed out
    internalPacket->data[BITS_TO_BYTES(internalPacket->dataBitLength) - 1] = 0;

    // Read the data the packet holds
    bitStreamSucceeded = bitStream->ReadAlignedBytes((unsigned char *)internalPacket->data,
                                                     BITS_TO_BYTES(internalPacket->dataBitLength));

    if (bitStreamSucceeded == false) {
        // If this hits, most likely the variable buff is too small in RunUpdateCycle in RakPeer.cpp
        RakAssert("Couldn't read all the data" && 0);

        FreeInternalPacketData(internalPacket, _FILE_AND_LINE_);
        ReleaseToInternalPacketPool(internalPacket);
        return 0;
    }

    return internalPacket;
}

// Returns true if newPacketOrderingIndex is older than the waitingForPacketOrderingIndex
bool ReliabilityLayer::IsOlderOrderedPacket(OrderingIndexType newPacketOrderingIndex,
                                            OrderingIndexType waitingForPacketOrderingIndex)
{
    OrderingIndexType maxRange = (OrderingIndexType)(const uint32_t)-1;

    if (waitingForPacketOrderingIndex > maxRange / (OrderingIndexType)2) {
        if (newPacketOrderingIndex >=
                waitingForPacketOrderingIndex - maxRange / (OrderingIndexType)2 + (OrderingIndexType)1 &&
            newPacketOrderingIndex < waitingForPacketOrderingIndex) {
            return true;
        }
    }

    else if (newPacketOrderingIndex >=
                 (OrderingIndexType)(waitingForPacketOrderingIndex -
                                     ((OrderingIndexType)maxRange / (OrderingIndexType)2 + (OrderingIndexType)1)) ||
             newPacketOrderingIndex < waitingForPacketOrderingIndex) {
        return true;
    }

    // Old packet
    return false;
}

// Split the passed packet into chunks under MTU_SIZEbytes (including headers) and save those new chunks
// Optimized version
void ReliabilityLayer::SplitPacket(InternalPacket *internalPacket)
{
    // Doing all sizes in bytes in this function so I don't write partial bytes with split packets
    internalPacket->splitPacketCount =
        1;  // This causes GetMessageHeaderLengthBits to account for the split packet header
    unsigned int headerLength = (unsigned int)BITS_TO_BYTES(GetMessageHeaderLengthBits(internalPacket));
    unsigned int dataByteLength = (unsigned int)BITS_TO_BYTES(internalPacket->dataBitLength);
    int maximumSendBlockBytes, byteOffset, bytesToSend;
    SplitPacketIndexType splitPacketIndex;
    int i;
    InternalPacket **internalPacketArray;

    maximumSendBlockBytes =
        GetMaxDatagramSizeExcludingMessageHeaderBytes() - BITS_TO_BYTES(GetMaxMessageHeaderLengthBits());

    // Calculate how many packets we need to create
    internalPacket->splitPacketCount = ((dataByteLength - 1) / (maximumSendBlockBytes) + 1);

    internalPacketArray =
        (InternalPacket **)rakMalloc_Ex(sizeof(InternalPacket *) * internalPacket->splitPacketCount, _FILE_AND_LINE_);

    for (i = 0; i < (int)internalPacket->splitPacketCount; i++) {
        internalPacketArray[i] = AllocateFromInternalPacketPool();

        *internalPacketArray[i] = *internalPacket;
        internalPacketArray[i]->messageNumberAssigned = false;

        if (i != 0) {
            internalPacket->messageInternalOrder = internalOrderIndex++;
        }
    }

    // This identifies which packet this is in the set
    splitPacketIndex = 0;

    InternalPacketRefCountedData *refCounter = 0;

    // Do a loop to send out all the packets
    do {
        byteOffset = splitPacketIndex * maximumSendBlockBytes;
        bytesToSend = dataByteLength - byteOffset;

        if (bytesToSend > maximumSendBlockBytes) {
            bytesToSend = maximumSendBlockBytes;
        }

        // Copy over our chunk of data

        AllocInternalPacketData(internalPacketArray[splitPacketIndex], &refCounter, internalPacket->data,
                                internalPacket->data + byteOffset);

        if (bytesToSend != maximumSendBlockBytes) {
            internalPacketArray[splitPacketIndex]->dataBitLength =
                internalPacket->dataBitLength - splitPacketIndex * (maximumSendBlockBytes << 3);
        }
        else {
            internalPacketArray[splitPacketIndex]->dataBitLength = bytesToSend << 3;
        }

        internalPacketArray[splitPacketIndex]->splitPacketIndex = splitPacketIndex;
        internalPacketArray[splitPacketIndex]->splitPacketId = splitPacketId;
        internalPacketArray[splitPacketIndex]->splitPacketCount = internalPacket->splitPacketCount;
        RakAssert(internalPacketArray[splitPacketIndex]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
    } while (++splitPacketIndex < internalPacket->splitPacketCount);

    splitPacketId++;  // It's ok if this wraps to 0

    // Tell the heap we are going to push a list of elements where each element in the list follows the heap order
    RakAssert(outgoingPacketBuffer.Size() == 0 ||
              outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
    outgoingPacketBuffer.StartSeries();

    // Copy all the new packets into the split packet list
    for (i = 0; i < (int)internalPacket->splitPacketCount; i++) {
        internalPacketArray[i]->headerLength = headerLength;
        RakAssert(internalPacketArray[i]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
        AddToUnreliableLinkedList(internalPacketArray[i]);
        RakAssert(internalPacketArray[i]->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
        RakAssert(internalPacketArray[i]->messageNumberAssigned == false);
        outgoingPacketBuffer.PushSeries(GetNextWeight(internalPacketArray[i]->priority), internalPacketArray[i],
                                        _FILE_AND_LINE_);
        RakAssert(outgoingPacketBuffer.Size() == 0 ||
                  outgoingPacketBuffer.Peek()->dataBitLength < BYTES_TO_BITS(MAXIMUM_MTU_SIZE));
        statistics.messageInSendBuffer[(int)internalPacketArray[i]->priority]++;
        statistics.bytesInSendBuffer[(int)(int)internalPacketArray[i]->priority] +=
            (double)BITS_TO_BYTES(internalPacketArray[i]->dataBitLength);
    }

    // Do not delete, original is referenced by all split packets to avoid numerous allocations. See
    // AllocInternalPacketData above
    ReleaseToInternalPacketPool(internalPacket);

    rakFree_Ex(internalPacketArray, _FILE_AND_LINE_);
}

// Insert a packet into the split packet list
void ReliabilityLayer::InsertIntoSplitPacketList(InternalPacket *internalPacket, CCTimeType time)
{
    // Find in splitPacketChannelList if a SplitPacketChannel with this splitPacketId was already allocated. If not,
    // allocate and insert the channel into the list.
    auto [entry, inserted] = splitPacketChannelList.try_emplace(internalPacket->splitPacketId);
    SplitPacketChannel &splitPacketChannel = entry->second;
    if (inserted) {
        splitPacketChannel.firstPacket = 0;
        // Preallocate to the final size, to avoid runtime copies
        splitPacketChannel.splitPacketList.reserve(internalPacket->splitPacketCount);
    }

    // Insert the packet into the SplitPacketChannel
    splitPacketChannel.splitPacketList.push_back(internalPacket);
    splitPacketChannel.lastUpdateTime = time;

    // If the index is 0, then this is the first packet. Record this so it can be returned to the user with download
    // progress
    if (internalPacket->splitPacketIndex == 0) {
        splitPacketChannel.firstPacket = internalPacket;
    }

    // Return download progress if we have the first packet, the list is not complete, and there are enough packets to
    // justify it
    if (splitMessageProgressInterval && splitPacketChannel.firstPacket &&
        splitPacketChannel.splitPacketList.size() != splitPacketChannel.firstPacket->splitPacketCount &&
        (splitPacketChannel.splitPacketList.size() % splitMessageProgressInterval) == 0) {
        // Return ID_DOWNLOAD_PROGRESS
        // Write splitPacketIndex (SplitPacketIndexType)
        // Write splitPacketCount (SplitPacketIndexType)
        // Write byteLength (4)
        // Write data, splitPacketChannel.splitPacketList[0]->data
        InternalPacket *progressIndicator = AllocateFromInternalPacketPool();
        unsigned int length = sizeof(MessageID) + sizeof(unsigned int) * 2 + sizeof(unsigned int) +
                              (unsigned int)BITS_TO_BYTES(splitPacketChannel.firstPacket->dataBitLength);
        AllocInternalPacketData(progressIndicator, length, false, __FILE__, __LINE__);
        progressIndicator->dataBitLength = BYTES_TO_BITS(length);
        progressIndicator->data[0] = (MessageID)ID_DOWNLOAD_PROGRESS;
        unsigned int temp;
        temp = (unsigned int)splitPacketChannel.splitPacketList.size();
        memcpy(progressIndicator->data + sizeof(MessageID), &temp, sizeof(unsigned int));
        temp = (unsigned int)internalPacket->splitPacketCount;
        memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 1, &temp, sizeof(unsigned int));
        temp = (unsigned int)BITS_TO_BYTES(splitPacketChannel.firstPacket->dataBitLength);
        memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 2, &temp, sizeof(unsigned int));

        memcpy(progressIndicator->data + sizeof(MessageID) + sizeof(unsigned int) * 3,
               splitPacketChannel.firstPacket->data,
               (size_t)BITS_TO_BYTES(splitPacketChannel.firstPacket->dataBitLength));
        outputQueue.push_back(progressIndicator);
    }
}

// Take all split chunks with the specified splitPacketId and try to
// reconstruct a packet.  If we can, allocate and return it.  Otherwise return 0
// Optimized version
InternalPacket *ReliabilityLayer::BuildPacketFromSplitPacketList(SplitPacketChannel &splitPacketChannel,
                                                                 CCTimeType time)
{
    unsigned int j;
    InternalPacket *internalPacket, *splitPacket;

    // Reconstruct
    internalPacket = CreateInternalPacketCopy(splitPacketChannel.splitPacketList[0], 0, 0, time);
    internalPacket->dataBitLength = 0;
    for (j = 0; j < splitPacketChannel.splitPacketList.size(); j++) {
        internalPacket->dataBitLength += splitPacketChannel.splitPacketList[j]->dataBitLength;
    }

    internalPacket->data =
        (unsigned char *)rakMalloc_Ex((size_t)BITS_TO_BYTES(internalPacket->dataBitLength), _FILE_AND_LINE_);
    internalPacket->allocationScheme = InternalPacket::NORMAL;

    BitSize_t offset = 0;
    for (j = 0; j < splitPacketChannel.splitPacketList.size(); j++) {
        splitPacket = splitPacketChannel.splitPacketList[j];
        memcpy(internalPacket->data + BITS_TO_BYTES(offset), splitPacket->data,
               (size_t)BITS_TO_BYTES(splitPacketChannel.splitPacketList[j]->dataBitLength));
        offset += splitPacketChannel.splitPacketList[j]->dataBitLength;
    }

    for (j = 0; j < splitPacketChannel.splitPacketList.size(); j++) {
        FreeInternalPacketData(splitPacketChannel.splitPacketList[j], _FILE_AND_LINE_);
        ReleaseToInternalPacketPool(splitPacketChannel.splitPacketList[j]);
    }

    return internalPacket;
}
InternalPacket *ReliabilityLayer::BuildPacketFromSplitPacketList(SplitPacketIdType splitPacketId, CCTimeType time,
                                                                 RakPeer *peer, SystemAddress &systemAddress,
                                                                 BitStream &updateBitStream)
{
    InternalPacket *internalPacket;

    // Find in splitPacketChannelList the SplitPacketChannel with this splitPacketId
    auto entry = splitPacketChannelList.find(splitPacketId);
    SplitPacketChannel &splitPacketChannel = entry->second;

    if (splitPacketChannel.splitPacketList.size() == splitPacketChannel.splitPacketList[0]->splitPacketCount) {
        // Ack immediately, because for large files this can take a long time
        SendACKs(peer, systemAddress, time, updateBitStream);
        internalPacket = BuildPacketFromSplitPacketList(splitPacketChannel, time);
        splitPacketChannelList.erase(entry);
        return internalPacket;
    }
    else {
        return 0;
    }
}

// Creates a copy of the specified internal packet with data copied from the original starting at dataByteOffset for
// dataByteLength bytes. Does not copy any split data parameters as that information is always generated does not have
// any reason to be copied
InternalPacket *ReliabilityLayer::CreateInternalPacketCopy(InternalPacket *original, int dataByteOffset,
                                                           int dataByteLength, CCTimeType time)
{
    InternalPacket *copy = AllocateFromInternalPacketPool();
    // Copy over our chunk of data

    if (dataByteLength > 0) {
        AllocInternalPacketData(copy, BITS_TO_BYTES(dataByteLength), false, _FILE_AND_LINE_);
        memcpy(copy->data, original->data + dataByteOffset, dataByteLength);
    }
    else {
        copy->data = 0;
    }

    copy->dataBitLength = dataByteLength << 3;
    copy->creationTime = time;
    copy->nextActionTime = 0;
    copy->orderingIndex = original->orderingIndex;
    copy->sequencingIndex = original->sequencingIndex;
    copy->orderingChannel = original->orderingChannel;
    copy->reliableMessageNumber = original->reliableMessageNumber;
    copy->priority = original->priority;
    copy->reliability = original->reliability;

    return copy;
}

// Inserts a packet into the resend list in order
void ReliabilityLayer::InsertPacketIntoResendList(InternalPacket *internalPacket, CCTimeType time, bool firstResend,
                                                  bool modifyUnacknowledgedBytes)
{
    (void)firstResend;
    (void)time;
    (void)internalPacket;

    AddToListTail(internalPacket, modifyUnacknowledgedBytes);
    RakAssert(internalPacket->nextActionTime != 0);
}

//  Were you ever unable to deliver a packet despite retries?
bool ReliabilityLayer::IsDeadConnection(void) const
{
    return deadConnection;
}

//  Causes IsDeadConnection to return true
void ReliabilityLayer::KillConnection(void)
{
    deadConnection = true;
}

// Statistics
RakNetStatistics *ReliabilityLayer::GetStatistics(RakNetStatistics *rns)
{
    unsigned i;
    RakNet::TimeUS time = RakNet::GetTimeUS();
    uint64_t uint64Denominator;
    double doubleDenominator;

    for (i = 0; i < RNS_PER_SECOND_METRICS_COUNT; i++) {
        statistics.valueOverLastSecond[i] = bpsMetrics[i].GetBPS1Threadsafe(time);
        statistics.runningTotal[i] = bpsMetrics[i].GetTotal1();
    }

    memcpy(rns, &statistics, sizeof(statistics));

    if (rns->valueOverLastSecond[USER_MESSAGE_BYTES_SENT] + rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT] > 0) {
        rns->packetlossLastSecond = (float)((double)rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT] /
                                            ((double)rns->valueOverLastSecond[USER_MESSAGE_BYTES_SENT] +
                                             (double)rns->valueOverLastSecond[USER_MESSAGE_BYTES_RESENT]));
    }
    else {
        rns->packetlossLastSecond = 0.0f;
    }

    rns->packetlossTotal = 0.0f;
    uint64Denominator = (rns->runningTotal[USER_MESSAGE_BYTES_SENT] + rns->runningTotal[USER_MESSAGE_BYTES_RESENT]);
    if (uint64Denominator != 0 && rns->runningTotal[USER_MESSAGE_BYTES_SENT] / uint64Denominator > 0) {
        doubleDenominator =
            ((double)rns->runningTotal[USER_MESSAGE_BYTES_SENT] + (double)rns->runningTotal[USER_MESSAGE_BYTES_RESENT]);
        if (doubleDenominator != 0) {
            rns->packetlossTotal = (float)((double)rns->runningTotal[USER_MESSAGE_BYTES_RESENT] / doubleDenominator);
        }
    }

    rns->isLimitedByCongestionControl = statistics.isLimitedByCongestionControl;
    rns->BPSLimitByCongestionControl = statistics.BPSLimitByCongestionControl;
    rns->isLimitedByOutgoingBandwidthLimit = statistics.isLimitedByOutgoingBandwidthLimit;
    rns->BPSLimitByOutgoingBandwidthLimit = statistics.BPSLimitByOutgoingBandwidthLimit;

    return rns;
}

// Returns the number of packets in the resend queue, not counting holes
unsigned int ReliabilityLayer::GetResendListDataSize(void) const
{
    return statistics.messagesInResendBuffer;
}

bool ReliabilityLayer::AckTimeout(RakNet::Time curTime)
{
    // I check timeLastDatagramArrived-curTime because with threading it is possible that timeLastDatagramArrived is
    // slightly greater than curTime, in which case this is NOT an ack timeout
    return (timeLastDatagramArrived - curTime) > 10000 && curTime - timeLastDatagramArrived > timeoutTime;
}
CCTimeType ReliabilityLayer::GetNextSendTime(void) const
{
    return nextSendTime;
}
CCTimeType ReliabilityLayer::GetTimeBetweenPackets(void) const
{
    return timeBetweenPackets;
}
void ReliabilityLayer::ResetPacketsAndDatagrams(void)
{
    packetsToSendThisUpdate.clear();
    packetsToDeallocThisUpdate.clear();
    packetsToSendThisUpdateDatagramBoundaries.clear();
    datagramsToSendThisUpdateIsPair.clear();
    datagramSizesInBytes.clear();
    datagramSizeSoFar = 0;
}
void ReliabilityLayer::PushPacket(CCTimeType time, InternalPacket *internalPacket, bool isReliable)
{
    BitSize_t bitsForThisPacket =
        BYTES_TO_BITS(BITS_TO_BYTES(internalPacket->dataBitLength) + BITS_TO_BYTES(internalPacket->headerLength));
    datagramSizeSoFar += bitsForThisPacket;
    RakAssert(BITS_TO_BYTES(datagramSizeSoFar) < MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
    allDatagramSizesSoFar += bitsForThisPacket;
    packetsToSendThisUpdate.push_back(internalPacket);
    packetsToDeallocThisUpdate.push_back(isReliable == false);
    RakAssert(internalPacket->headerLength == GetMessageHeaderLengthBits(internalPacket));

    congestionManager.OnSendBytes(time, BITS_TO_BYTES(internalPacket->dataBitLength) +
                                            BITS_TO_BYTES(internalPacket->headerLength));
}
void ReliabilityLayer::PushDatagram(void)
{
    if (datagramSizeSoFar > 0) {
        packetsToSendThisUpdateDatagramBoundaries.push_back((unsigned int)packetsToSendThisUpdate.size());
        datagramsToSendThisUpdateIsPair.push_back(false);
        RakAssert(BITS_TO_BYTES(datagramSizeSoFar) < MAXIMUM_MTU_SIZE - UDP_HEADER_SIZE);
        datagramSizesInBytes.push_back(BITS_TO_BYTES(datagramSizeSoFar));
        datagramSizeSoFar = 0;
    }
}
bool ReliabilityLayer::TagMostRecentPushAsSecondOfPacketPair(void)
{
    if (datagramsToSendThisUpdateIsPair.size() >= 2) {
        datagramsToSendThisUpdateIsPair[datagramsToSendThisUpdateIsPair.size() - 2] = true;
        datagramsToSendThisUpdateIsPair[datagramsToSendThisUpdateIsPair.size() - 1] = true;
        return true;
    }
    return false;
}
void ReliabilityLayer::ClearPacketsAndDatagrams(void)
{
    unsigned int i;
    for (i = 0; i < packetsToDeallocThisUpdate.size(); i++) {
        // packetsToDeallocThisUpdate holds a boolean indicating if packetsToSendThisUpdate at this index should be
        // freed
        if (packetsToDeallocThisUpdate[i]) {
            RemoveFromUnreliableLinkedList(packetsToSendThisUpdate[i]);
            FreeInternalPacketData(packetsToSendThisUpdate[i], _FILE_AND_LINE_);
            ReleaseToInternalPacketPool(packetsToSendThisUpdate[i]);
        }
    }
    packetsToDeallocThisUpdate.clear();
}
void ReliabilityLayer::MoveToListHead(InternalPacket *internalPacket)
{
    if (internalPacket == resendLinkedListHead) {
        return;
    }
    if (resendLinkedListHead == 0) {
        internalPacket->resendNext = internalPacket;
        internalPacket->resendPrev = internalPacket;
        resendLinkedListHead = internalPacket;
        return;
    }
    internalPacket->resendPrev->resendNext = internalPacket->resendNext;
    internalPacket->resendNext->resendPrev = internalPacket->resendPrev;
    internalPacket->resendNext = resendLinkedListHead;
    internalPacket->resendPrev = resendLinkedListHead->resendPrev;
    internalPacket->resendPrev->resendNext = internalPacket;
    resendLinkedListHead->resendPrev = internalPacket;
    resendLinkedListHead = internalPacket;
    RakAssert(internalPacket->headerLength + internalPacket->dataBitLength > 0);
}
void ReliabilityLayer::RemoveFromList(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes)
{
    InternalPacket *newPosition;
    internalPacket->resendPrev->resendNext = internalPacket->resendNext;
    internalPacket->resendNext->resendPrev = internalPacket->resendPrev;
    newPosition = internalPacket->resendNext;
    if (internalPacket == resendLinkedListHead) {
        resendLinkedListHead = newPosition;
    }
    if (resendLinkedListHead == internalPacket) {
        resendLinkedListHead = 0;
    }

    if (modifyUnacknowledgedBytes) {
        RakAssert(unacknowledgedBytes >= BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength));
        unacknowledgedBytes -= BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);
    }
}
void ReliabilityLayer::AddToListTail(InternalPacket *internalPacket, bool modifyUnacknowledgedBytes)
{
    if (modifyUnacknowledgedBytes) {
        unacknowledgedBytes += BITS_TO_BYTES(internalPacket->headerLength + internalPacket->dataBitLength);
    }

    if (resendLinkedListHead == 0) {
        internalPacket->resendNext = internalPacket;
        internalPacket->resendPrev = internalPacket;
        resendLinkedListHead = internalPacket;
        return;
    }
    internalPacket->resendNext = resendLinkedListHead;
    internalPacket->resendPrev = resendLinkedListHead->resendPrev;
    internalPacket->resendPrev->resendNext = internalPacket;
    resendLinkedListHead->resendPrev = internalPacket;
}
void ReliabilityLayer::PopListHead(bool modifyUnacknowledgedBytes)
{
    RakAssert(resendLinkedListHead != 0);
    RemoveFromList(resendLinkedListHead, modifyUnacknowledgedBytes);
}
bool ReliabilityLayer::IsResendQueueEmpty(void) const
{
    return resendLinkedListHead == 0;
}
void ReliabilityLayer::SendACKs(RakPeer *peer, SystemAddress &systemAddress, CCTimeType time,
                                BitStream &updateBitStream)
{
    BitSize_t maxDatagramPayload = GetMaxDatagramSizeExcludingMessageHeaderBits();

    while (acknowlegements.Size() > 0) {
        // Send acks
        updateBitStream.Reset();
        DatagramHeaderFormat dhf;
        dhf.isACK = true;
        dhf.isNAK = false;
        dhf.isPacketPair = false;
        double B;
        double AS;
        bool hasBAndAS;
        if (remoteSystemNeedsBAndAS) {
            congestionManager.OnSendAckGetBAndAS(time, &hasBAndAS, &B, &AS);
            dhf.AS = (float)AS;
            dhf.hasBAndAS = hasBAndAS;
        }
        else {
            dhf.hasBAndAS = false;
        }
        updateBitStream.Reset();
        dhf.Serialize(&updateBitStream);
        CC_DEBUG_PRINTF_1("AckSnd ");
        acknowlegements.Serialize(&updateBitStream, maxDatagramPayload, true);
        SendBitStream(peer, systemAddress, &updateBitStream, time);
        congestionManager.OnSendAck(time, updateBitStream.GetNumberOfBytesUsed());
    }
}
InternalPacket *ReliabilityLayer::AllocateFromInternalPacketPool(void)
{
    InternalPacket *ip = internalPacketPool.Allocate(_FILE_AND_LINE_);
    ip->reliableMessageNumber = (MessageNumberType)(const uint32_t)-1;
    ip->messageNumberAssigned = false;
    ip->nextActionTime = 0;
    ip->splitPacketCount = 0;
    ip->splitPacketIndex = 0;
    ip->splitPacketId = 0;
    ip->allocationScheme = InternalPacket::NORMAL;
    ip->data = 0;
    ip->timesSent = 0;
    return ip;
}
void ReliabilityLayer::ReleaseToInternalPacketPool(InternalPacket *ip)
{
    internalPacketPool.Release(ip, _FILE_AND_LINE_);
}
void ReliabilityLayer::RemoveFromUnreliableLinkedList(InternalPacket *internalPacket)
{
    if (internalPacket->reliability == UNRELIABLE || internalPacket->reliability == UNRELIABLE_SEQUENCED ||
        internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT
        //		||
        //		internalPacket->reliability==UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
    ) {
        InternalPacket *newPosition;
        internalPacket->unreliablePrev->unreliableNext = internalPacket->unreliableNext;
        internalPacket->unreliableNext->unreliablePrev = internalPacket->unreliablePrev;
        newPosition = internalPacket->unreliableNext;
        if (internalPacket == unreliableLinkedListHead) {
            unreliableLinkedListHead = newPosition;
        }
        if (unreliableLinkedListHead == internalPacket) {
            unreliableLinkedListHead = 0;
        }
    }
}
void ReliabilityLayer::AddToUnreliableLinkedList(InternalPacket *internalPacket)
{
    if (internalPacket->reliability == UNRELIABLE || internalPacket->reliability == UNRELIABLE_SEQUENCED ||
        internalPacket->reliability == UNRELIABLE_WITH_ACK_RECEIPT
        //		||
        //		internalPacket->reliability==UNRELIABLE_SEQUENCED_WITH_ACK_RECEIPT
    ) {
        if (unreliableLinkedListHead == 0) {
            internalPacket->unreliableNext = internalPacket;
            internalPacket->unreliablePrev = internalPacket;
            unreliableLinkedListHead = internalPacket;
            return;
        }
        internalPacket->unreliableNext = unreliableLinkedListHead;
        internalPacket->unreliablePrev = unreliableLinkedListHead->unreliablePrev;
        internalPacket->unreliablePrev->unreliableNext = internalPacket;
        unreliableLinkedListHead->unreliablePrev = internalPacket;
    }
}
void ReliabilityLayer::ValidateResendList(void) const {}
bool ReliabilityLayer::ResendBufferOverflow(void) const
{
    int index1 = sendReliableMessageNumberIndex & (uint32_t)RESEND_BUFFER_ARRAY_MASK;
    RakAssert(index1 < RESEND_BUFFER_ARRAY_LENGTH);
    return resendBuffer[index1] != 0;  // || resendBuffer[index2]!=0;
}
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::GetMessageNumberNodeByDatagramIndex(
    DatagramSequenceNumberType index, CCTimeType *timeSent)
{
    if (datagramHistory.empty()) {
        return 0;
    }

    if (congestionManager.LessThan(index, datagramHistoryPopCount)) {
        return 0;
    }

    DatagramSequenceNumberType offsetIntoList = index - datagramHistoryPopCount;
    if (offsetIntoList >= datagramHistory.size()) {
        return 0;
    }

    *timeSent = datagramHistory[offsetIntoList].timeSent;
    return datagramHistory[offsetIntoList].head;
}
void ReliabilityLayer::RemoveFromDatagramHistory(DatagramSequenceNumberType index)
{
    DatagramSequenceNumberType offsetIntoList = index - datagramHistoryPopCount;
    MessageNumberNode *mnm = datagramHistory[offsetIntoList].head;
    MessageNumberNode *next;
    while (mnm) {
        next = mnm->next;
        datagramHistoryMessagePool.Release(mnm, _FILE_AND_LINE_);
        mnm = next;
    }
    datagramHistory[offsetIntoList].head = 0;
}
void ReliabilityLayer::AddFirstToDatagramHistory(DatagramSequenceNumberType datagramNumber, CCTimeType timeSent)
{
    (void)datagramNumber;
    if (datagramHistory.size() > DATAGRAM_MESSAGE_ID_ARRAY_LENGTH) {
        RemoveFromDatagramHistory(datagramHistoryPopCount);
        datagramHistory.pop_front();
        datagramHistoryPopCount++;
    }

    datagramHistory.push_back(DatagramHistoryNode(0, timeSent));
}
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::AddFirstToDatagramHistory(
    DatagramSequenceNumberType datagramNumber, DatagramSequenceNumberType messageNumber, CCTimeType timeSent)
{
    (void)datagramNumber;
    if (datagramHistory.size() > DATAGRAM_MESSAGE_ID_ARRAY_LENGTH) {
        RemoveFromDatagramHistory(datagramHistoryPopCount);
        datagramHistory.pop_front();
        datagramHistoryPopCount++;
    }

    MessageNumberNode *mnm = datagramHistoryMessagePool.Allocate(_FILE_AND_LINE_);
    mnm->next = 0;
    mnm->messageNumber = messageNumber;
    datagramHistory.push_back(DatagramHistoryNode(mnm, timeSent));
    return mnm;
}
ReliabilityLayer::MessageNumberNode *ReliabilityLayer::AddSubsequentToDatagramHistory(
    MessageNumberNode *messageNumberNode, DatagramSequenceNumberType messageNumber)
{
    messageNumberNode->next = datagramHistoryMessagePool.Allocate(_FILE_AND_LINE_);
    messageNumberNode->next->messageNumber = messageNumber;
    messageNumberNode->next->next = 0;
    return messageNumberNode->next;
}
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket,
                                               InternalPacketRefCountedData **refCounter,
                                               unsigned char *externallyAllocatedPtr, unsigned char *ourOffset)
{
    internalPacket->allocationScheme = InternalPacket::REF_COUNTED;
    internalPacket->data = ourOffset;
    if (*refCounter == 0) {
        *refCounter = refCountedDataPool.Allocate(_FILE_AND_LINE_);
        (*refCounter)->refCount = 1;
        (*refCounter)->sharedDataBlock = externallyAllocatedPtr;
    }
    else {
        (*refCounter)->refCount++;
    }
    internalPacket->refCountedData = (*refCounter);
}
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket, unsigned char *externallyAllocatedPtr)
{
    internalPacket->allocationScheme = InternalPacket::NORMAL;
    internalPacket->data = externallyAllocatedPtr;
}
void ReliabilityLayer::AllocInternalPacketData(InternalPacket *internalPacket, unsigned int numBytes, bool allowStack,
                                               const char *file, unsigned int line)
{
    if (allowStack && numBytes <= sizeof(internalPacket->stackData)) {
        internalPacket->allocationScheme = InternalPacket::STACK;
        internalPacket->data = internalPacket->stackData;
    }
    else {
        internalPacket->allocationScheme = InternalPacket::NORMAL;
        internalPacket->data = (unsigned char *)rakMalloc_Ex(numBytes, file, line);
    }
}
void ReliabilityLayer::FreeInternalPacketData(InternalPacket *internalPacket, const char *file, unsigned int line)
{
    if (internalPacket == 0) {
        return;
    }

    if (internalPacket->allocationScheme == InternalPacket::REF_COUNTED) {
        if (internalPacket->refCountedData == 0) {
            return;
        }

        internalPacket->refCountedData->refCount--;
        if (internalPacket->refCountedData->refCount == 0) {
            rakFree_Ex(internalPacket->refCountedData->sharedDataBlock, file, line);
            internalPacket->refCountedData->sharedDataBlock = 0;
            refCountedDataPool.Release(internalPacket->refCountedData, file, line);
            internalPacket->refCountedData = 0;
        }
    }
    else if (internalPacket->allocationScheme == InternalPacket::NORMAL) {
        if (internalPacket->data == 0) {
            return;
        }

        rakFree_Ex(internalPacket->data, file, line);
        internalPacket->data = 0;
    }
    else {
        // Data was on stack
        internalPacket->data = 0;
    }
}
unsigned int ReliabilityLayer::GetMaxDatagramSizeExcludingMessageHeaderBytes(void)
{
    unsigned int val = congestionManager.GetMTU() - DatagramHeaderFormat::GetDataHeaderByteLength();

    return val;
}
BitSize_t ReliabilityLayer::GetMaxDatagramSizeExcludingMessageHeaderBits(void)
{
    return BYTES_TO_BITS(GetMaxDatagramSizeExcludingMessageHeaderBytes());
}
void ReliabilityLayer::InitHeapWeights(void)
{
    for (int priorityLevel = 0; priorityLevel < NUMBER_OF_PRIORITIES; priorityLevel++) {
        outgoingPacketBufferNextWeights[priorityLevel] = (1 << priorityLevel) * priorityLevel + priorityLevel;
    }
}
reliabilityHeapWeightType ReliabilityLayer::GetNextWeight(int priorityLevel)
{
    uint64_t next = outgoingPacketBufferNextWeights[priorityLevel];
    if (outgoingPacketBuffer.Size() > 0) {
        int peekPL = outgoingPacketBuffer.Peek()->priority;
        reliabilityHeapWeightType weight = outgoingPacketBuffer.PeekWeight();
        reliabilityHeapWeightType min = weight - (1 << peekPL) * peekPL + peekPL;
        if (next < min) {
            next = min + (1 << priorityLevel) * priorityLevel + priorityLevel;
        }
        outgoingPacketBufferNextWeights[priorityLevel] =
            next + (1 << priorityLevel) * (priorityLevel + 1) + priorityLevel;
    }
    else {
        InitHeapWeights();
    }
    return next;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
