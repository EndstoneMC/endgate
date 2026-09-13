#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>

namespace endgate {

/**
 * A reference counted, chainable byte buffer.
 *
 * Segments share their storage, so a slice copies nothing, a header is prepended without moving the
 * payload, and the whole chain converts straight into a scatter-gather write.
 *
 * @see io.netty.buffer.ByteBuf
 * @see io.netty.buffer.CompositeByteBuf
 */
class ByteBuf {
public:
    ByteBuf() = default;

    static ByteBuf allocate(std::size_t capacity)
    {
        ByteBuf buf;
        buf.storage_ = std::make_shared<std::vector<std::byte>>(capacity);
        return buf;
    }

    static ByteBuf copyOf(std::span<const std::byte> data)
    {
        ByteBuf buf;
        buf.storage_ = std::make_shared<std::vector<std::byte>>(data.begin(), data.end());
        buf.segments_.push_back({buf.storage_, 0, data.size()});
        buf.readable_ = data.size();
        return buf;
    }

    [[nodiscard]] std::size_t readableBytes() const { return readable_ - reader_index_; }

    [[nodiscard]] bool empty() const { return readableBytes() == 0; }

    [[nodiscard]] std::span<std::byte> writable()
    {
        return {storage_->data() + readable_, storage_->size() - readable_};
    }

    void advanceWriter(std::size_t count)
    {
        segments_.push_back({storage_, readable_, count});
        readable_ += count;
    }

    [[nodiscard]] ByteBuf retainedSlice(std::size_t index, std::size_t length) const
    {
        ByteBuf slice;
        slice.storage_ = storage_;
        std::size_t skipped = 0;
        for (const auto &segment : segments_) {
            if (skipped + segment.length <= index) {
                skipped += segment.length;
                continue;
            }
            const auto offset = index > skipped ? index - skipped : 0;
            const auto take = std::min(segment.length - offset, length - slice.readable_);
            slice.segments_.push_back({segment.storage, segment.offset + offset, take});
            slice.readable_ += take;
            if (slice.readable_ == length) {
                break;
            }
            skipped += segment.length;
        }
        return slice;
    }

    void addComponent(ByteBuf component)
    {
        for (auto &segment : component.segments_) {
            readable_ += segment.length;
            segments_.push_back(std::move(segment));
        }
    }

    void prependComponent(ByteBuf component)
    {
        const auto added = component.readableBytes();
        segments_.insert(segments_.begin(), component.segments_.begin(), component.segments_.end());
        readable_ += added;
    }

    [[nodiscard]] std::span<const std::byte> contiguous() const
    {
        if (segments_.size() != 1) {
            return {};
        }
        const auto &segment = segments_.front();
        return {segment.storage->data() + segment.offset, segment.length};
    }

    void copyTo(std::span<std::byte> out) const
    {
        std::size_t written = 0;
        for (const auto &segment : segments_) {
            const auto take = std::min(segment.length, out.size() - written);
            std::copy_n(segment.storage->data() + segment.offset, take, out.begin() + written);
            written += take;
        }
    }

    [[nodiscard]] std::vector<asio::const_buffer> toBufferSequence() const
    {
        std::vector<asio::const_buffer> buffers;
        buffers.reserve(segments_.size());
        for (const auto &segment : segments_) {
            buffers.emplace_back(segment.storage->data() + segment.offset, segment.length);
        }
        return buffers;
    }

private:
    struct Segment {
        std::shared_ptr<std::vector<std::byte>> storage;
        std::size_t offset;
        std::size_t length;
    };

    std::shared_ptr<std::vector<std::byte>> storage_;
    std::vector<Segment> segments_;
    std::size_t readable_{0};
    std::size_t reader_index_{0};
};

}  // namespace endgate
