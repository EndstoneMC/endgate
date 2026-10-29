#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>

namespace endgate {

/**
 * A reference counted, chainable byte buffer.
 *
 * Each element of the chain lays its storage out as headroom, data, tailroom, so a header goes in
 * front without moving the payload and a slice copies nothing. The whole chain converts straight into
 * a scatter-gather write. folly keeps the chain in a linked list of buffers; ours holds it in one
 * object, the way Netty's composite does.
 *
 * @see folly::IOBuf
 * @see io.netty.buffer.CompositeByteBuf
 */
class LegacyByteBuf {
public:
    LegacyByteBuf() = default;

    /**
     * @see folly::IOBuf::create
     */
    static LegacyByteBuf create(std::size_t capacity)
    {
        LegacyByteBuf buf;
        buf.elements_.push_back(
            {.storage = std::make_shared<std::vector<std::byte>>(capacity), .offset = 0, .length = 0});
        return buf;
    }

    /**
     * @see folly::IOBuf::copyBuffer
     */
    static LegacyByteBuf copyBuffer(std::span<const std::byte> data)
    {
        LegacyByteBuf buf;
        buf.elements_.push_back({.storage = std::make_shared<std::vector<std::byte>>(data.begin(), data.end()),
                                 .offset = 0,
                                 .length = data.size()});
        buf.length_ = data.size();
        return buf;
    }

    /**
     * @see folly::IOBuf::computeChainDataLength
     */
    [[nodiscard]] std::size_t length() const { return length_; }

    [[nodiscard]] bool empty() const { return length_ == 0; }

    /**
     * @see folly::IOBuf::writableTail
     */
    [[nodiscard]] std::span<std::byte> writableTail()
    {
        if (elements_.empty()) {
            return {};
        }
        const auto &last = elements_.back();
        const auto tail = last.offset + last.length;
        return {last.storage->data() + tail, last.storage->size() - tail};
    }

    /**
     * @see folly::IOBuf::tailroom
     */
    [[nodiscard]] std::size_t tailroom() const
    {
        if (elements_.empty()) {
            return 0;
        }
        const auto &last = elements_.back();
        return last.storage->size() - last.offset - last.length;
    }

    /**
     * @see folly::IOBuf::append
     */
    void append(std::size_t count)
    {
        elements_.back().length += count;
        length_ += count;
    }

    /**
     * @see folly::IOBuf::appendToChain
     */
    void appendToChain(LegacyByteBuf buf)
    {
        for (auto &element : buf.elements_) {
            length_ += element.length;
            elements_.push_back(std::move(element));
        }
    }

    /**
     * @see folly::IOBuf::prependChain
     */
    void prependChain(LegacyByteBuf buf)
    {
        length_ += buf.length_;
        elements_.insert(elements_.begin(), buf.elements_.begin(), buf.elements_.end());
    }

    /**
     * Shares the storage rather than copying it, so a batch fans out into packets for free.
     *
     * @see io.netty.buffer.LegacyByteBuf.retainedSlice
     */
    [[nodiscard]] LegacyByteBuf retainedSlice(std::size_t index, std::size_t length) const
    {
        LegacyByteBuf slice;
        std::size_t skipped = 0;
        for (const auto &element : elements_) {
            if (skipped + element.length <= index) {
                skipped += element.length;
                continue;
            }
            const auto offset = index > skipped ? index - skipped : 0;
            const auto take = std::min(element.length - offset, length - slice.length_);
            slice.elements_.push_back({.storage = element.storage, .offset = element.offset + offset, .length = take});
            slice.length_ += take;
            if (slice.length_ == length) {
                break;
            }
            skipped += element.length;
        }
        return slice;
    }

    /**
     * @see folly::IOBuf::coalesce
     */
    [[nodiscard]] std::span<const std::byte> coalesce() const
    {
        if (elements_.size() != 1) {
            return {};
        }
        const auto &element = elements_.front();
        return {element.storage->data() + element.offset, element.length};
    }

    void copyTo(std::span<std::byte> out) const
    {
        std::size_t written = 0;
        for (const auto &element : elements_) {
            const auto take = std::min(element.length, out.size() - written);
            std::copy_n(element.storage->data() + element.offset, take,
                        out.begin() + static_cast<std::ptrdiff_t>(written));
            written += take;
        }
    }

    [[nodiscard]] std::vector<asio::const_buffer> toBufferSequence() const
    {
        std::vector<asio::const_buffer> buffers;
        buffers.reserve(elements_.size());
        for (const auto &element : elements_) {
            buffers.emplace_back(element.storage->data() + element.offset, element.length);
        }
        return buffers;
    }

private:
    struct Element {
        std::shared_ptr<std::vector<std::byte>> storage;
        std::size_t offset;
        std::size_t length;
    };

    std::vector<Element> elements_;
    std::size_t length_{0};
};

}  // namespace endgate
