// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <mutex>
#include <random>

namespace duckemu::runtime
{

// The instance-wide entropy stream. getrandom(2) and /dev/{u,}random read from
// the same source, so the two observation surfaces cannot drift; a replay can
// later pin it by constructing with an explicit seed.
//
// Thread-safe: guest threads may call getrandom concurrently while another
// thread reads /dev/urandom.
class RandomSource
{
    public:
	RandomSource();
	explicit RandomSource(uint64_t seed);

	RandomSource(const RandomSource &) = delete;
	RandomSource &operator=(const RandomSource &) = delete;

	// Fills the whole buffer. Sizes beyond one word are built word by word.
	void fill(void *buffer, uint64_t size);

	[[nodiscard]] uint64_t next();

    private:
	std::mutex mutex_;
	std::mt19937_64 engine_;
};

} // namespace duckemu::runtime
