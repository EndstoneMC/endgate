#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"
#include "endgate/network/pipeline.h"

namespace endgate {
namespace {

struct Recorder {
    std::vector<std::string> events;
};

struct RecordingSink {
    Recorder &recorder;

    void channelActive() { recorder.events.emplace_back("sink:active"); }
    void channelInactive() { recorder.events.emplace_back("sink:inactive"); }
    void channelRead(int message) { recorder.events.push_back("sink:read " + std::to_string(message)); }
    void channelRead(const std::string &message) { recorder.events.push_back("sink:read " + message); }
    void channelReadComplete() { recorder.events.emplace_back("sink:complete"); }
    void userEventTriggered(int event) { recorder.events.push_back("sink:event " + std::to_string(event)); }
    void exceptionCaught(const std::exception &cause)
    {
        recorder.events.push_back(std::string{"sink:exception "} + cause.what());
    }
    void write(const std::string &message) { recorder.events.push_back("head:write " + message); }
    void flush() { recorder.events.emplace_back("head:flush"); }
    void close() { recorder.events.emplace_back("head:close"); }
};

template <int Id>
class Inbound : public ChannelInboundHandlerAdapter {
public:
    explicit Inbound(Recorder &recorder) : recorder_(recorder) {}

    template <typename Context>
    void channelActive(Context &ctx)
    {
        recorder_.events.push_back("in" + std::to_string(Id) + ":active");
        ctx.fireChannelActive();
    }

    template <typename Context>
    void channelRead(Context &ctx, int message)
    {
        recorder_.events.push_back("in" + std::to_string(Id) + ":read " + std::to_string(message));
        ctx.fireChannelRead(message + 1);
    }

    template <typename Context>
    void channelReadComplete(Context &ctx)
    {
        recorder_.events.push_back("in" + std::to_string(Id) + ":complete");
        ctx.fireChannelReadComplete();
    }

private:
    Recorder &recorder_;
};

template <int Id>
class Outbound : public ChannelOutboundHandlerAdapter {
public:
    explicit Outbound(Recorder &recorder) : recorder_(recorder) {}

    template <typename Context>
    void write(Context &ctx, const std::string &message)
    {
        recorder_.events.push_back("out" + std::to_string(Id) + ":write " + message);
        ctx.write(message + std::to_string(Id));
    }

    template <typename Context>
    void flush(Context &ctx)
    {
        recorder_.events.push_back("out" + std::to_string(Id) + ":flush");
        ctx.flush();
    }

private:
    Recorder &recorder_;
};

class Throwing : public ChannelDuplexHandler {
public:
    template <typename Context>
    void channelRead(Context &, int)
    {
        throw std::runtime_error("read failed");
    }

    template <typename Context>
    void write(Context &, const std::string &)
    {
        throw std::runtime_error("write failed");
    }
};

class Swallowing : public ChannelInboundHandlerAdapter {
public:
    explicit Swallowing(Recorder &recorder) : recorder_(recorder) {}

    template <typename Context>
    void exceptionCaught(Context &, const std::exception &cause)
    {
        recorder_.events.push_back(std::string{"swallowed "} + cause.what());
    }

private:
    Recorder &recorder_;
};

struct Relay : ChannelDuplexHandler {
    template <typename Context>
    void channelRead(Context &ctx, int message)
    {
        ctx.write(std::string{"reply"});
        ctx.fireChannelRead(message);
        ctx.fireChannelRead(message);
    }
};

TEST(ChannelPipeline, InboundEventsRunHeadToTailAndSkipOutboundHandlers)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Inbound<1>{recorder}, Outbound<9>{recorder}, Inbound<2>{recorder}};

    pipeline.fireChannelActive();
    pipeline.fireChannelRead(10);
    pipeline.fireChannelReadComplete();
    pipeline.fireUserEventTriggered(7);
    pipeline.fireChannelInactive();

    EXPECT_EQ(recorder.events, (std::vector<std::string>{"in1:active", "in2:active", "sink:active", "in1:read 10",
                                                         "in2:read 11", "sink:read 12", "in1:complete", "in2:complete",
                                                         "sink:complete", "sink:event 7", "sink:inactive"}));
}

TEST(ChannelPipeline, OutboundOperationsRunTailToHeadAndSkipInboundHandlers)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Outbound<1>{recorder}, Inbound<9>{recorder}, Outbound<2>{recorder}};

    pipeline.writeAndFlush(std::string{"m"});
    pipeline.close();

    EXPECT_EQ(recorder.events, (std::vector<std::string>{"out2:write m", "out1:write m2", "head:write m21",
                                                         "out2:flush", "out1:flush", "head:flush", "head:close"}));
}

TEST(ChannelPipeline, ContextFiresFromItsOwnPosition)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Outbound<1>{recorder}, Relay{}, Outbound<2>{recorder}};

    pipeline.fireChannelRead(5);

    EXPECT_EQ(recorder.events,
              (std::vector<std::string>{"out1:write reply", "head:write reply1", "sink:read 5", "sink:read 5"}));
}

TEST(ChannelPipeline, AnExceptionInAHandlerReachesItsOwnExceptionCaught)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Throwing{}, Inbound<1>{recorder}};
    pipeline.fireChannelRead(1);
    EXPECT_EQ(recorder.events, (std::vector<std::string>{"sink:exception read failed"}));

    recorder.events.clear();
    ChannelPipeline caught{sink, Throwing{}, Swallowing{recorder}};
    caught.fireChannelRead(1);
    EXPECT_EQ(recorder.events, (std::vector<std::string>{"swallowed read failed"}));
}

TEST(ChannelPipeline, AnExceptionFromAnOutboundHandlerEntersAtTheHead)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Swallowing{recorder}, Throwing{}};

    pipeline.write(std::string{"m"});

    EXPECT_EQ(recorder.events, (std::vector<std::string>{"swallowed write failed"}));
}

TEST(ChannelPipeline, GetFindsAHandlerByType)
{
    Recorder recorder;
    RecordingSink sink{recorder};
    ChannelPipeline pipeline{sink, Inbound<1>{recorder}, Outbound<2>{recorder}};
    static_assert(std::same_as<decltype(pipeline.get<Outbound<2>>()), Outbound<2> &>);
}

class Splitter : public ByteToMessageDecoder {
public:
    template <typename Context>
    void decode(Context &ctx, ByteBufView &in)
    {
        if (in.readableBytes() < 2) {
            return;
        }
        const auto length = in.getUnsignedByte(in.readerIndex());
        if (length == 0xFF) {
            throw std::out_of_range("bad length");
        }
        if (in.readableBytes() < 1U + length) {
            return;
        }
        in.skipBytes(1);
        ctx.fireChannelRead(in.readSlice(length));
    }
};

struct BytesSink {
    std::vector<std::string> messages;
    std::vector<std::string> errors;

    void channelRead(ByteBufView message)
    {
        const auto bytes = message.nioBuffer();
        messages.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    void exceptionCaught(const std::exception &cause) { errors.emplace_back(cause.what()); }
};

ByteBuf copyOf(std::string_view text)
{
    return Unpooled::copiedBuffer(std::as_bytes(std::span{text}));
}

TEST(ByteToMessageDecoder, FiresOncePerMessageAndKeepsTheRemainderForTheNextRead)
{
    BytesSink sink;
    ChannelPipeline<BytesSink, Splitter> pipeline{sink};

    const auto first = copyOf("\x02"
                              "ab"
                              "\x03"
                              "cd");
    pipeline.fireChannelRead(first.slice());
    EXPECT_EQ(sink.messages, (std::vector<std::string>{"ab"}));

    const auto second = copyOf("e\x01"
                               "f");
    pipeline.fireChannelRead(second.slice());
    EXPECT_EQ(sink.messages, (std::vector<std::string>{"ab", "cde", "f"}));
}

TEST(ByteToMessageDecoder, WrapsAFailureInDecoderExceptionAndForgetsTheRemainder)
{
    BytesSink sink;
    ChannelPipeline<BytesSink, Splitter> pipeline{sink};

    const auto bad = copyOf("\x01"
                            "a"
                            "\xFF"
                            "zz");
    pipeline.fireChannelRead(bad.slice());
    EXPECT_EQ(sink.messages, (std::vector<std::string>{"a"}));
    EXPECT_EQ(sink.errors, (std::vector<std::string>{"bad length"}));

    const auto good = copyOf("\x01"
                             "x");
    pipeline.fireChannelRead(good.slice());
    EXPECT_EQ(sink.messages, (std::vector<std::string>{"a", "x"}));
}

class Halver : public MessageToMessageDecoder {
public:
    template <typename Context>
    void decode(Context &ctx, ByteBufView batch)
    {
        if (!batch.isReadable()) {
            throw std::runtime_error("empty");
        }
        const auto half = batch.readableBytes() / 2;
        ctx.fireChannelRead(batch.readSlice(half));
        ctx.fireChannelRead(batch.readSlice(batch.readableBytes()));
    }
};

TEST(MessageToMessageDecoder, FansOutAndPassesOnWhatDecodeDoesNotTake)
{
    struct Sink : BytesSink {
        using BytesSink::channelRead;
        void channelRead(int number) { numbers.push_back(number); }
        std::vector<int> numbers;
    };

    Sink sink;
    ChannelPipeline<Sink, Halver> pipeline{sink};

    const auto batch = copyOf("abcd");
    pipeline.fireChannelRead(batch.slice());
    pipeline.fireChannelRead(42);
    pipeline.fireChannelRead(ByteBufView{});

    EXPECT_EQ(sink.messages, (std::vector<std::string>{"ab", "cd"}));
    EXPECT_EQ(sink.numbers, (std::vector<int>{42}));
    EXPECT_EQ(sink.errors, (std::vector<std::string>{"empty"}));
}

class Prefixer : public MessageToByteEncoder {
public:
    template <typename Context>
    void encode(Context &, const std::string &message, ByteBuf &out)
    {
        if (message.empty()) {
            throw std::runtime_error("nothing to encode");
        }
        out.writeUnsignedVarInt(static_cast<std::uint32_t>(message.size()))
            .writeBytes(std::as_bytes(std::span{message}));
    }
};

struct BufferSink {
    std::vector<std::string> written;
    std::vector<std::string> errors;

    void write(ByteBuf buf) { written.push_back(ByteBufUtil::hexDump(buf)); }
    void exceptionCaught(const std::exception &cause) { errors.emplace_back(cause.what()); }
};

TEST(MessageToByteEncoder, EncodesIntoAFreshBufferAndPassesOnWhatEncodeDoesNotTake)
{
    BufferSink sink;
    ChannelPipeline<BufferSink, Prefixer> pipeline{sink};

    pipeline.write(std::string{"hi"});
    pipeline.write(Unpooled::copiedBuffer(std::as_bytes(std::span{std::string_view{"raw"}})));
    pipeline.write(std::string{});

    EXPECT_EQ(sink.written, (std::vector<std::string>{"026869", "726177"}));
    EXPECT_EQ(sink.errors, (std::vector<std::string>{"nothing to encode"}));
}

}  // namespace
}  // namespace endgate
