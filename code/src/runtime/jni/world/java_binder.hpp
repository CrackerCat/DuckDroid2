// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "common/error/result.hpp"
#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni
{

// Sequential JavaWorld setup with the first error remembered instead of
// returned at each step. Environment installers chain declarations and check
// once with done(); after a failure every later call is skipped, so a failed
// declaration never feeds a zero id into a later call.
class JavaBinder
{
    public:
	explicit JavaBinder(JavaWorld &world) : world_(world) {}

	JavaBinder &declareClass(std::string_view descriptor,
		uint64_t *slot,
		std::string_view super_descriptor = "")
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(world_.declareClass(descriptor, super_descriptor));
		return *this;
	}

	JavaBinder &declareMethod(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static,
		ShimHandler shim,
		uint64_t *slot = nullptr)
	{
		if (!ok())
		{
			return *this;
		}
		const uint64_t method = take(
			world_.declareMethod(class_object, name, signature, is_static, std::move(shim)));
		if (slot != nullptr)
		{
			*slot = method;
		}
		return *this;
	}

	JavaBinder &declareField(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static,
		uint64_t *slot)
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(world_.declareField(class_object, name, signature, is_static));
		return *this;
	}

	JavaBinder &allocateObject(uint64_t class_object, uint64_t *slot)
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(world_.allocateObject(class_object));
		return *this;
	}

	JavaBinder &newArray(ArrayElementKind kind,
		uint64_t length,
		uint64_t element_class,
		uint64_t initial_element,
		uint64_t *slot)
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(world_.newArray(kind, length, element_class, initial_element));
		return *this;
	}

	JavaBinder &newString(std::string_view utf8, uint64_t *slot)
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(world_.newString(utf8));
		return *this;
	}

	JavaBinder &writeInstanceField(uint64_t object, uint64_t field_id, uint64_t value)
	{
		if (!ok())
		{
			return *this;
		}
		take(world_.writeInstanceField(object, field_id, value));
		return *this;
	}

	JavaBinder &writeStaticField(uint64_t class_object, uint64_t field_id, uint64_t value)
	{
		if (!ok())
		{
			return *this;
		}
		take(world_.writeStaticField(class_object, field_id, value));
		return *this;
	}

	// Any other world operation the installer wants to fold in, such as a local
	// helper that builds an array.
	template <typename T>
	JavaBinder &assign(Result<T> result, T *slot)
	{
		if (!ok())
		{
			return *this;
		}
		*slot = take(std::move(result));
		return *this;
	}

	[[nodiscard]] bool ok() const
	{
		return error_.ok();
	}

	// The first error, or success. Consumes the binder.
	Result<void> done() &&
	{
		return std::move(error_);
	}

    private:
	template <typename T>
	T take(Result<T> result)
	{
		if (!result.ok())
		{
			error_ = result.error();
			return T{};
		}
		return std::move(result).value();
	}

	void take(Result<void> result)
	{
		if (!result.ok())
		{
			error_ = result.error();
		}
	}

	JavaWorld &world_;
	Result<void> error_;
};

} // namespace duckemu::runtime::jni
