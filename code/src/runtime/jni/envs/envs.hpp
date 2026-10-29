#pragma once

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{

class JavaWorld;

// Java environment supplements.
//
// Two layers, one direction of dependency:
//   src/runtime/jni/envs/   platform packs (java.lang, later android.*); no
//                           target-specific names are ever added here
//   code/user_code/<case>/  one target project; its main.cpp installs its own
//                           pack explicitly via
//                           RuntimeInstance::installJavaEnvironment()
//
// A pack is one function. It declares only the classes/methods/fields the
// target actually needs; nothing is created implicitly.
namespace envs
{

using Installer = Result<void> (*)(JavaWorld &world);

// Installs the built-in platform packs. Called once when the instance is
// created, before any module is loaded.
Result<void> installAll(JavaWorld &world);

Result<void> installJavaLang(JavaWorld &world);

} // namespace envs
} // namespace duckemu::runtime::jni
