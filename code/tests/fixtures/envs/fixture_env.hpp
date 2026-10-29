#pragma once

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::test
{

// Test-only Java environment for the M5 fixtures. Installed explicitly by the
// test binaries that need env/Fixture; the runtime library does not know it.
Result<void> installFixtureEnvironment(runtime::jni::JavaWorld &world);

} // namespace duckemu::test
