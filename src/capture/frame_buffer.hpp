#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "frame_packet.hpp"

// Single-producer, multi-reader latest-frame buffer. The producer never waits:
// publishing frame N simply replaces the slot used by frame N-4.
class FrameBuffer {
public:
    static constexpr std::size_t kCapacity = 4;

    void push(std::shared_ptr<const FramePacket> packet) noexcept
    {
        if (packet == nullptr) {
            return;
        }

        const std::uint64_t sequence = published_count_.load(std::memory_order_relaxed);
        auto entry = std::make_shared<const Entry>(Entry{sequence, std::move(packet)});
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        slots_[sequence % kCapacity].store(std::move(entry), std::memory_order_release);
#else
        std::atomic_store_explicit(
            &slots_[sequence % kCapacity], std::move(entry), std::memory_order_release);
#endif
        published_count_.store(sequence + 1, std::memory_order_release);
        if (sequence >= kCapacity) {
            overwritten_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::shared_ptr<const FramePacket> latest() const noexcept
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::uint64_t count = published_count_.load(std::memory_order_acquire);
            if (count == 0) {
                return {};
            }
            const std::uint64_t expected = count - 1;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            const auto entry = slots_[expected % kCapacity].load(std::memory_order_acquire);
#else
            const auto entry = std::atomic_load_explicit(
                &slots_[expected % kCapacity], std::memory_order_acquire);
#endif
            if (entry != nullptr && entry->sequence == expected) {
                return entry->packet;
            }
        }
        return {};
    }

    std::vector<std::shared_ptr<const FramePacket>> snapshot() const
    {
        const std::uint64_t end = published_count_.load(std::memory_order_acquire);
        const std::uint64_t begin = end > kCapacity ? end - kCapacity : 0;

        std::vector<std::shared_ptr<const FramePacket>> frames;
        frames.reserve(static_cast<std::size_t>(end - begin));
        for (std::uint64_t sequence = begin; sequence < end; ++sequence) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            const auto entry = slots_[sequence % kCapacity].load(std::memory_order_acquire);
#else
            const auto entry = std::atomic_load_explicit(
                &slots_[sequence % kCapacity], std::memory_order_acquire);
#endif
            if (entry != nullptr && entry->sequence == sequence) {
                frames.push_back(entry->packet);
            }
        }
        return frames;
    }

    std::size_t size() const noexcept
    {
        const auto count = published_count_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(std::min<std::uint64_t>(count, kCapacity));
    }

    std::uint64_t publishedCount() const noexcept
    {
        return published_count_.load(std::memory_order_relaxed);
    }

    std::uint64_t overwrittenCount() const noexcept
    {
        return overwritten_count_.load(std::memory_order_relaxed);
    }

    // Only call after the single producer has stopped.
    void clear() noexcept
    {
        for (auto &slot : slots_) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            slot.store({}, std::memory_order_relaxed);
#else
            std::atomic_store_explicit(
                &slot, std::shared_ptr<const Entry>{}, std::memory_order_relaxed);
#endif
        }
        published_count_.store(0, std::memory_order_relaxed);
        overwritten_count_.store(0, std::memory_order_relaxed);
    }

private:
    struct Entry {
        std::uint64_t sequence;
        std::shared_ptr<const FramePacket> packet;
    };

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::array<std::atomic<std::shared_ptr<const Entry>>, kCapacity> slots_{};
#else
    // JetPack's GCC 11/libstdc++ lacks atomic<shared_ptr>, but supports these
    // slots through the standard shared_ptr atomic free functions above.
    std::array<std::shared_ptr<const Entry>, kCapacity> slots_{};
#endif
    std::atomic<std::uint64_t> published_count_{0};
    std::atomic<std::uint64_t> overwritten_count_{0};
};
