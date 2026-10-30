// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/random/random_source.hpp"

#include <algorithm>
#include <cstring>

namespace duckemu::runtime
{
namespace
{

uint64_t hostSeed()
{
	std::random_device device;
	return (static_cast<uint64_t>(device()) << 32) ^ device();
}

} // namespace

RandomSource::RandomSource()
	: engine_(hostSeed())
{
}

RandomSource::RandomSource(uint64_t seed)
	: engine_(seed)
{
}

void RandomSource::fill(void *buffer, uint64_t size)
{
	auto *out = static_cast<uint8_t *>(buffer);
	std::lock_guard<std::mutex> lock(mutex_);
	while (size > 0)
	{
		const uint64_t word = engine_();
		const uint64_t take = std::min<uint64_t>(size, sizeof(word));
		std::memcpy(out, &word, static_cast<size_t>(take));
		out += take;
		size -= take;
	}
}

uint64_t RandomSource::next()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return engine_();
}

} // namespace duckemu::runtime
