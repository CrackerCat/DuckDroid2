#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckemu::runtime::observer
{

// Fixed-capacity single-producer / single-consumer ring. The producer never
// blocks: a full ring drops the newest item and counts it. Capacity is rounded
// up to a power of two so the index arithmetic is a mask.
//
// HARD CONSTRAINT: exactly one producer and one consumer. M3 gives every
// RuntimeThread its own engine, so the diagnostic feed must become one ring per
// thread (drained by iterating threads) -- never a shared ring with a lock,
// because the producer runs on the guest memory-access path.
template <typename T>
class RingBuffer
{
    public:
	explicit RingBuffer(size_t capacity)
		: slots_(roundUpPow2(capacity))
		, mask_(slots_.size() - 1)
	{
	}

	bool tryPush(const T &item) noexcept
	{
		const size_t head = head_.load(std::memory_order_relaxed);
		const size_t tail = tail_.load(std::memory_order_acquire);
		if (head - tail >= slots_.size())
		{
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		slots_[head & mask_] = item;
		head_.store(head + 1, std::memory_order_release);
		return true;
	}

	bool tryPop(T &out) noexcept
	{
		const size_t tail = tail_.load(std::memory_order_relaxed);
		const size_t head = head_.load(std::memory_order_acquire);
		if (tail == head)
		{
			return false;
		}
		out = slots_[tail & mask_];
		tail_.store(tail + 1, std::memory_order_release);
		return true;
	}

	[[nodiscard]] size_t capacity() const noexcept
	{
		return slots_.size();
	}

	[[nodiscard]] size_t size() const noexcept
	{
		const size_t head = head_.load(std::memory_order_acquire);
		const size_t tail = tail_.load(std::memory_order_acquire);
		return head >= tail ? head - tail : 0;
	}

	[[nodiscard]] uint64_t dropped() const noexcept
	{
		return dropped_.load(std::memory_order_relaxed);
	}

    private:
	static size_t roundUpPow2(size_t value)
	{
		size_t result = 2;
		while (result < value)
		{
			result <<= 1;
		}
		return result;
	}

	std::vector<T> slots_;
	size_t mask_ = 0;
	std::atomic<size_t> head_{ 0 };
	std::atomic<size_t> tail_{ 0 };
	std::atomic<uint64_t> dropped_{ 0 };
};

} // namespace duckemu::runtime::observer
