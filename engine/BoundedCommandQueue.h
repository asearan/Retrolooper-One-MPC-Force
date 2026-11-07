// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace retrolooper {
template<class T, std::size_t Capacity> class BoundedCommandQueue {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    struct Slot { std::atomic<std::size_t> sequence{}; T value{}; };
    std::array<Slot, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::size_t read_{0}; // single consumer
    std::atomic<std::uint64_t> rejected_{0};
public:
    BoundedCommandQueue() { for(std::size_t i=0;i<Capacity;++i) slots_[i].sequence.store(i); }
    bool push(const T& value) noexcept {
        auto pos=write_.load(std::memory_order_relaxed);
        for(int retry=0;retry<64;++retry) {
            auto& slot=slots_[pos & (Capacity-1)];
            const auto seq=slot.sequence.load(std::memory_order_acquire);
            const auto diff=static_cast<std::intptr_t>(seq)-static_cast<std::intptr_t>(pos);
            if(diff==0) {
                if(write_.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed)) {
                    slot.value=value; slot.sequence.store(pos+1,std::memory_order_release); return true;
                }
            } else if(diff<0) break;
            else pos=write_.load(std::memory_order_relaxed);
        }
        rejected_.fetch_add(1,std::memory_order_relaxed); return false;
    }
    bool pop(T& value) noexcept {
        auto& slot=slots_[read_ & (Capacity-1)];
        if(slot.sequence.load(std::memory_order_acquire)!=read_+1) return false;
        value=slot.value; slot.sequence.store(read_+Capacity,std::memory_order_release); ++read_; return true;
    }
    std::uint64_t rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }
};
}
