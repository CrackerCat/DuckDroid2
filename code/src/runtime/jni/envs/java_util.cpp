// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni::envs
{
namespace
{

Result<void> declare(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	ShimHandler shim)
{
	auto declared = world.declareMethod(klass, name, signature, is_static, std::move(shim));
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

// java.util collections are host state keyed by the guest object address. One
// RuntimeInstance owns one JavaWorld, and object addresses are not reused
// while the instance lives, so a process-local table is enough; install clears
// it so a second instance in the same process cannot inherit entries.
struct MapEntries
{
	std::vector<std::pair<uint64_t, uint64_t> > entries;
};

std::unordered_map<uint64_t, MapEntries> g_maps;
std::unordered_map<uint64_t, uint64_t> g_key_sets; // set object -> map object
uint64_t g_hash_map_class = 0;
uint64_t g_hash_set_class = 0;
uint64_t g_map_class = 0;
uint64_t g_set_class = 0;

bool sameKey(JavaWorld &world, uint64_t first, uint64_t second)
{
	if (first == second)
	{
		return true;
	}
	if (first == 0 || second == 0)
	{
		return false;
	}
	if (world.classOf(first) != world.stringClass() ||
		world.classOf(second) != world.stringClass())
	{
		return false;
	}
	auto equal = world.stringEquals(first, second);
	return equal.ok() && equal.value();
}

uint64_t findKey(JavaWorld &world, const MapEntries &map, uint64_t key)
{
	for (const auto &entry : map.entries)
	{
		if (sameKey(world, entry.first, key))
		{
			return entry.first;
		}
	}
	return 0;
}

Result<uint64_t> mapConstructor(JavaWorld &, InvokeContext &call)
{
	g_maps[call.receiver].entries.clear();
	return uint64_t{ 0 };
}

Result<uint64_t> mapPut(JavaWorld &world, InvokeContext &call)
{
	if (call.args.size() != 2)
	{
		return Error{ ErrorCode::InvalidArgument, "HashMap.put expects key and value" };
	}
	auto &entries = g_maps[call.receiver].entries;
	for (auto &entry : entries)
	{
		if (sameKey(world, entry.first, call.args[0]))
		{
			const uint64_t previous = entry.second;
			entry.second = call.args[1];
			return previous;
		}
	}
	entries.emplace_back(call.args[0], call.args[1]);
	return uint64_t{ 0 };
}

Result<uint64_t> mapGet(JavaWorld &world, InvokeContext &call)
{
	if (call.args.size() != 1)
	{
		return Error{ ErrorCode::InvalidArgument, "HashMap.get expects one key" };
	}
	const auto found = g_maps.find(call.receiver);
	if (found == g_maps.end())
	{
		return uint64_t{ 0 };
	}
	for (const auto &entry : found->second.entries)
	{
		if (sameKey(world, entry.first, call.args[0]))
		{
			return entry.second;
		}
	}
	return uint64_t{ 0 };
}

Result<uint64_t> mapContainsKey(JavaWorld &world, InvokeContext &call)
{
	if (call.args.size() != 1)
	{
		return Error{ ErrorCode::InvalidArgument, "HashMap.containsKey expects one key" };
	}
	const auto found = g_maps.find(call.receiver);
	if (found == g_maps.end())
	{
		return uint64_t{ 0 };
	}
	return findKey(world, found->second, call.args[0]) != 0 ? uint64_t{ 1 } : uint64_t{ 0 };
}

Result<uint64_t> mapSize(JavaWorld &, InvokeContext &call)
{
	const auto found = g_maps.find(call.receiver);
	return found == g_maps.end() ? uint64_t{ 0 } : found->second.entries.size();
}

Result<uint64_t> mapKeySet(JavaWorld &world, InvokeContext &call)
{
	auto set = world.allocateObject(g_hash_set_class);
	if (!set.ok())
	{
		return set.error();
	}
	g_key_sets[set.value()] = call.receiver;
	return set.value();
}

Result<uint64_t> mapToString(JavaWorld &world, InvokeContext &call)
{
	std::string text = "{";
	const auto found = g_maps.find(call.receiver);
	if (found != g_maps.end())
	{
		bool first = true;
		for (const auto &entry : found->second.entries)
		{
			if (!first)
			{
				text += ", ";
			}
			first = false;
			if (world.classOf(entry.first) == world.stringClass())
			{
				auto key = world.stringUtf8(entry.first);
				text += key.ok() ? key.value() : std::string("<key>");
			}
			else
			{
				text += "<key>";
			}
			text += "=";
			if (world.classOf(entry.second) == world.stringClass())
			{
				auto value = world.stringUtf8(entry.second);
				text += value.ok() ? value.value() : std::string("<value>");
			}
			else
			{
				text += "<obj>";
			}
		}
	}
	text += "}";
	return world.newString(text);
}

Result<uint64_t> setToArray(JavaWorld &world, InvokeContext &call)
{
	const auto set = g_key_sets.find(call.receiver);
	const uint64_t map = set != g_key_sets.end() ? set->second : 0;
	const auto entries = g_maps.find(map);
	const uint64_t count = entries != g_maps.end() ? entries->second.entries.size() : 0;
	auto array = world.newArray(ArrayElementKind::Object, count, world.objectClass());
	if (!array.ok())
	{
		return array.error();
	}
	for (uint64_t index = 0; index < count; ++index)
	{
		auto written = world.writeArrayElement(array.value(), index,
			entries->second.entries[index].first);
		if (!written.ok())
		{
			return written.error();
		}
	}
	return array.value();
}

} // namespace

Result<void> installJavaUtil(JavaWorld &world)
{
	auto set = world.declareClass("java/util/Set", "");
	if (!set.ok())
	{
		return set.error();
	}
	g_set_class = set.value();

	auto hash_map = world.declareClass("java/util/HashMap", "java/lang/Object");
	if (!hash_map.ok())
	{
		return hash_map.error();
	}
	g_hash_map_class = hash_map.value();

	auto hash_set = world.declareClass("java/util/HashSet", "java/util/Set");
	if (!hash_set.ok())
	{
		return hash_set.error();
	}
	g_hash_set_class = hash_set.value();

	auto map = world.declareClass("java/util/Map", "");
	if (!map.ok())
	{
		return map.error();
	}
	g_map_class = map.value();

	// Android APIs return List parameters; device-info probes only resolve the
	// type and call the read-only members on lists they got elsewhere.
	auto list = world.declareClass("java/util/List", "");
	if (!list.ok())
	{
		return list.error();
	}

	g_maps.clear();
	g_key_sets.clear();

	// The SDK builds its result map with NewObject(HashMap, <init>(I)) and
	// fills it through the interface method ids it resolved on Map.
	const auto result = declare(world, g_hash_map_class, "<init>", "()V", false,
		mapConstructor);
	if (!result.ok())
	{
		return result;
	}
	const auto sized_ctor = declare(world, g_hash_map_class, "<init>", "(I)V", false,
		mapConstructor);
	if (!sized_ctor.ok())
	{
		return sized_ctor;
	}
	const auto set_ctor = declare(world, g_hash_set_class, "<init>", "()V", false,
		mapConstructor);
	if (!set_ctor.ok())
	{
		return set_ctor;
	}
	const auto set_sized_ctor = declare(world, g_hash_set_class, "<init>", "(I)V", false,
		mapConstructor);
	if (!set_sized_ctor.ok())
	{
		return set_sized_ctor;
	}

	const struct
	{
		uint64_t klass;
		const char *name;
		const char *signature;
		ShimHandler shim;
	} methods[] = {
		{ g_map_class, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
			mapPut },
		{ g_map_class, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", mapGet },
		{ g_map_class, "containsKey", "(Ljava/lang/Object;)Z", mapContainsKey },
		{ g_map_class, "size", "()I", mapSize },
		{ g_map_class, "keySet", "()Ljava/util/Set;", mapKeySet },
		{ g_map_class, "toString", "()Ljava/lang/String;", mapToString },
		{ g_hash_map_class, "put",
			"(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", mapPut },
		{ g_hash_map_class, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", mapGet },
		{ g_hash_map_class, "containsKey", "(Ljava/lang/Object;)Z", mapContainsKey },
		{ g_hash_map_class, "size", "()I", mapSize },
		{ g_hash_map_class, "keySet", "()Ljava/util/Set;", mapKeySet },
		{ g_hash_map_class, "toString", "()Ljava/lang/String;", mapToString },
		{ g_set_class, "toArray", "()[Ljava/lang/Object;", setToArray },
		{ g_hash_set_class, "toArray", "()[Ljava/lang/Object;", setToArray },
	};
	for (const auto &method : methods)
	{
		auto declared = declare(world, method.klass, method.name, method.signature, false,
			method.shim);
		if (!declared.ok())
		{
			return declared;
		}
	}
	const auto list_size = declare(world, list.value(), "size", "()I", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!list_size.ok())
	{
		return list_size;
	}
	const auto list_empty = declare(world, list.value(), "isEmpty", "()Z", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 1 }; });
	if (!list_empty.ok())
	{
		return list_empty;
	}
	const auto list_get = declare(world, list.value(), "get",
		"(I)Ljava/lang/Object;", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!list_get.ok())
	{
		return list_get;
	}
	return declare(world, list.value(), "toArray", "()[Ljava/lang/Object;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newArray(ArrayElementKind::Object, 0, java.objectClass());
		});
}

} // namespace duckemu::runtime::jni::envs
