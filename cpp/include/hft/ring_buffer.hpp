// Lock-free single-producer / single-consumer ring buffer.
//
// This is the C++ port of hft/core/ringbuffer.py, done properly: the Python
// version relies on the GIL and an asyncio.Event for synchronisation, which is
// fine inside one event loop but is not a real concurrent queue. Here the
// producer and consumer run on different threads with no locks at all.
//
// How it is safe without locks
// ----------------------------
//  * Only the producer ever writes `head_`; only the consumer ever writes
//    `tail_`. Neither index is ever contended for writes.
//  * The producer writes the slot, then publishes `head_` with release
//    ordering. The consumer acquires `head_` before reading the slot. That
//    pairing is what guarantees the consumer sees the fully-written element,
//    not a torn one.
//  * `head_` and `tail_` sit on separate cache lines. Without the padding the
//    two threads would ping-pong the same line between cores on every single
//    operation ("false sharing") and throughput collapses.
//  * Each side keeps a *cached* copy of the other's index and only re-reads
//    the real (shared) atomic when its cached value says the queue looks
//    full/empty. In the common case a push or pop touches zero shared state.
//
// Overflow behaviour differs deliberately from the Python version
// --------------------------------------------------------------
// ringbuffer.py drops the *oldest* item on overflow by advancing the read
// index from the producer side. In a lock-free SPSC queue the producer must
// not touch the read index -- doing so would race with the consumer mid-read.
// So this version drops the *newest* item (try_push returns false) and counts
// it. The intent is the same: never block ingestion. If drop-oldest semantics
// are genuinely required, that has to be the consumer's policy (drain and
// discard stale ticks), not the producer's.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace hft {

// Conservative cache line size. std::hardware_destructive_interference_size is
// available in C++17 but is a hard error on some libstdc++ configurations when
// combined with -mtune defaults, so we hardcode the x86-64 value.
inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class SpscRingBuffer {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRingBuffer is designed for POD market data messages so a "
                "slot can be published with a plain store; use a pointer or an "
                "index if you need a non-trivial payload.");

 public:
  // `capacity` is rounded up to a power of two so the modulo becomes a mask.
  explicit SpscRingBuffer(std::size_t capacity)
      : capacity_(round_up_pow2(capacity)), mask_(capacity_ - 1), slots_(capacity_) {}

  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }

  // Producer side. Returns false when the queue is full, without recording a
  // drop -- a caller that retries has not lost anything, and counting failed
  // attempts as drops would make the metric meaningless.
  bool try_push(const T& item) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = head + 1;

    if (next - cached_tail_ > capacity_) {
      // Cached view says full -- pay for the real load only now.
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next - cached_tail_ > capacity_) return false;
    }

    slots_[head & mask_] = item;
    head_.store(next, std::memory_order_release);  // publishes the slot write
    return true;
  }

  // Producer side with an explicit drop-on-overflow policy: the message is
  // discarded and counted rather than retried. This is the "never stall
  // ingestion" behaviour the Python ring buffer had -- use it when a stale
  // tick is worth less than the latency of blocking the feed.
  bool push_or_drop(const T& item) noexcept {
    if (try_push(item)) return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Consumer side. Returns false when the queue is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);

    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;
    }

    out = slots_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Approximate; both indices are read independently so this is a snapshot,
  // useful for metrics but never for control flow.
  std::size_t size_approx() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }

  bool empty_approx() const noexcept { return size_approx() == 0; }

  std::uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

 private:
  static std::size_t round_up_pow2(std::size_t n) {
    if (n < 2) return 2;
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::vector<T> slots_;

  // Producer-owned line: the write index and the consumer index it has cached.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  std::atomic<std::uint64_t> dropped_{0};

  // Consumer-owned line: the read index and the producer index it has cached.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;

  char pad_[kCacheLine];  // keeps the next object out of tail_'s line
};

}  // namespace hft
