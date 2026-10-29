# Java 环境补充指南

> 这份文档规定 `code/user_code/` 下目标项目的**写法**。平台级环境在
> `src/runtime/jni/envs/`，不在这里。
>
> 核心原则：**声明即存在**。没写的东西一律不存在；查询不到返回 null 并产生
> `JniEvent`，不会自动造类/方法/字段，也不会内置任何设备值。

---

## 1. 先决定放哪

| 内容 | 位置 | 规则 |
|---|---|---|
| 平台级 Java 行为（`java.lang`，以后 `android.os.Build`、`Context`） | `src/runtime/jni/envs/` | 跨目标复用；runtime 不能出现目标名字 |
| 目标类、目标专属方法/字段/常量、目标 so 与 runner | `code/user_code/<case>/` | 每个目标一个项目 |
| 导入符号（libc/libandroid 的 `malloc`/`fopen` 等） | `src/runtime/loader/` | 走 thunk/虚拟模块，**不写进 env** |
| 测试专用类（如 `env/Fixture`） | `tests/fixtures/envs/` | 测试 main 里显式安装 |

判断标准：**这段行为换一个目标还成立吗？** 成立 → 平台；只对当前 so/APK 成立
→ 项目。拿不准就先放项目，等第二个目标也需要时再上提。

---

## 2. 每个文件干什么

```text
<case>/
├── <case>_env.hpp       # 只声明安装函数；项目对外的唯一入口
├── <case>_env.cpp       # 目标专属的类/方法/字段声明与 shim；项目 Java 侧的全部内容
├── main.cpp             # 唯一的接线点：create → 装环境 → 加载 libc/so → 调用
└── assets/              # 目标 so、properties、证书、配置
```

| 文件 | 放什么 | 不放什么 |
|---|---|---|
| `<case>_env.hpp` | `installEnvironment(JavaWorld&)` 的声明、必要的类型前置声明 | 实现、常量表、业务逻辑 |
| `<case>_env.cpp` | `declareClass` / `declareMethod` / `declareField`、shim lambda、目标常量 | runner、文件加载、通用框架代码 |
| `main.cpp` | `RuntimeConfig`、`installJavaEnvironment`、`loadLibrary`、`callRegisteredNative`、输出 | 大段环境声明（那些在 env.cpp） |
| `assets/` | 目标二进制与数据 | 与目标无关的测试产物 |

**没有静态注册。** `main.cpp` 必须显式调用安装函数；不写 `REGISTER_*`、
不建名字表、不改 `RuntimeConfig`。

---

## 3. 格式与模板

### 3.1 `<case>_env.hpp`

```cpp
#pragma once

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::<case>
{

Result<void> installEnvironment(runtime::jni::JavaWorld &world);

} // namespace duckemu::user::<case>
```

### 3.2 `<case>_env.cpp`

```cpp
#include "<case>_env.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::user::<case>
{

using runtime::jni::JavaWorld;

Result<void> installEnvironment(JavaWorld &world)
{
	// 1) 类：只声明 so 会 FindClass 的类，以及解析方法真正需要的父类。
	//    父类不知道就传 ""，不要编一个。
	auto klass = world.declareClass("com/example/Target", "");
	if (!klass.ok())
	{
		return klass.error();
	}

	// 2) native 方法：只声明签名与静态性，shim 传 {}；方法体由 so 提供，
	//    JNI_OnLoad 的 RegisterNatives 会把地址绑进来。
	auto native = world.declareMethod(klass.value(), "data", "(I)Ljava/lang/String;",
		/*is_static=*/true, {});
	if (!native.ok())
	{
		return native.error();
	}

	// 3) host 方法：最小行为。常量、字段、宿主时钟优先；复杂逻辑再写 shim。
	auto version = world.declareMethod(klass.value(), "getVersion", "()Ljava/lang/String;",
		/*is_static=*/false,
		[](JavaWorld &java, runtime::jni::InvokeContext &) -> Result<uint64_t> {
			return java.newString("16.1.50");
		});
	if (!version.ok())
	{
		return version.error();
	}

	// 4) 字段：静态/实例都要给出签名；读写走 JavaWorld。
	auto counter = world.declareField(klass.value(), "counter", "I", /*is_static=*/true);
	if (!counter.ok())
	{
		return counter.error();
	}
	return {};
}

} // namespace duckemu::user::<case>
```

### 3.3 `main.cpp`（接线）

```cpp
#include "<case>_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"

RuntimeConfig config;
config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

auto created = RuntimeInstance::create(config);
if (!created.ok()) { /* 报错退出 */ }
RuntimeInstance &instance = *created.value();

// 唯一的接线点：create 之后、任何 loadLibrary 之前。
auto installed = instance.installJavaEnvironment(&user::<case>::installEnvironment);
if (!installed.ok()) { /* 报错退出 */ }

// 严格模式的失败要看得见。
instance.addJniListener([](const observer::JniEvent &event) {
	std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
		std::string(observer::jniEventKindName(event.kind)).c_str(),
		event.class_descriptor.c_str(), event.name.c_str(),
		event.signature.c_str(), event.slot, event.tid);
});

instance.loadLibrary(romLibc());
instance.loadLibrary(SO_PATH);

auto result = instance.callRegisteredNative(CLASS, "data", "(I)Ljava/lang/String;", { n });
if (result.ok() && result.value() != 0) {
	auto text = instance.jniStringUtf8(result.value());
	// 打印 text.value()
}
```

`installJavaEnvironment` 在**第一个模块加载之后**调用会返回
`InvalidArgument`：`JNI_OnLoad` 在 `loadLibrary` 里执行，晚装等于没装。

---

## 4. 常用 API 与行为

### 4.1 声明

| 调用 | 用途 | 注意 |
|---|---|---|
| `world.declareClass(descriptor, super)` | 声明类 | descriptor 用 `/`；`super` 未知时传 `""`；重复声明且父类一致时幂等 |
| `world.declareMethod(class, name, signature, is_static, shim)` | 声明方法 | native 方法传 `{}`；host 方法给 shim |
| `world.declareField(class, name, signature, is_static)` | 声明字段 | signature 是单字符 Java 类型（`I`/`J`/`L...;` 等） |

`is_static` **必须和 Java 侧一致**，因为 `RegisterNatives` 不携带静态性：

- runner 用 `callRegisteredNative`（传 jclass）→ native 声明为 `true`；
- so 里用 `CallObjectMethod` 调它 → 声明为 `false`；
- 不确定时看 Java 声明或 dex，不要猜。

### 4.2 shim 的输入与返回

```cpp
[](JavaWorld &world, InvokeContext &call) -> Result<uint64_t>
```

- `call.receiver`：实例方法是对象地址，静态方法是类对象地址。
- `call.args`：**对象参数是对象地址，不是 JNI 句柄**；基本类型是原始值。
- 返回值：基本类型直接返回；对象结果返回对象地址（`world.newString(...)`、
  `world.findClass(...)`、`call.receiver`）；void 返回 0。
- 读取字符串参数：`world.stringUtf8(call.args[0])`。
- 读取/写入字段：`world.readInstanceField(obj, field_id)`、
  `world.readStaticField(class_object, field_id)`，字段 id 来自 `declareField`。

### 4.3 结果与错误

- 声明失败（父类不存在、签名非法、重复声明）直接 `return xxx.error()`，
  让实例创建失败；不要吞掉。
- shim 里真的无法回答时返回 `Error{ ErrorCode::Unsupported, "..." }`，
  由 JNI 层记录并返回 null/0；不要编一个默认值。
- 正常路径不应该出现 `JniEvent`；出现了就是环境没补完。

### 4.4 host → guest

`RuntimeInstance::callRegisteredNative` 由 runtime 填 `JNIEnv*`（x0）和
`jclass`（x1），参数从 x2 开始，最多 6 个。因此它调用的是**静态** native。
返回值是 x0；对象结果是 JNI 句柄，用 `jniStringUtf8` 等辅助函数解析。

---

## 5. 怎么补：严格模式驱动

1. **先什么都不加**，构建并跑目标。
2. 看 stderr：
   - `missing-class class=...` → 补类；
   - `missing-method class=... name=... sig=...` → 补方法；
   - `missing-field ...` → 补字段；
   - `unsupported-function slot=...` → 该 JNI 表项还没实现，属于 runtime 的活。
3. **只补事件里的那一个**，不要顺手声明“以后可能用到”的成员。
4. 父类/接口不知道就留空；等真实调用需要继承关系时再补。
5. `FindClass` 找不到的类不要声明；先确认 so 真的调了它。
6. 重新跑到**正常路径 `JniEvent` 计数为 0**。
7. 缺失场景单独写测试，断言事件类型与内容（不能只看返回值）。
8. 通用部分稳定后，再上提到 `src/runtime/jni/envs/`。

---

## 6. 硬规则

- 不自动造类/方法/字段；查询不到就是 null + 事件。
- 不内置设备值：包名、型号、证书、时间必须由项目声明。
- 不编父类、不编方法体、不为了让调用“先过”而返回假对象。
- runtime 不出现目标名字；项目不写通用框架。
- 不引入 DSL、数据文件、静态注册器；不新增 `RuntimeConfig` 必填参数。
- 目标专属资源（so、properties、证书）放项目 `assets/`，不写死在代码路径里。

---

## 7. 新项目检查清单

- [ ] `code/user_code/<case>/` 目录与 `assets/`
- [ ] `<case>_env.hpp/.cpp` 只包含目标需要的声明与 shim
- [ ] `main.cpp` 在**任何 loadLibrary 之前**调用 `installJavaEnvironment`
- [ ] 正常路径 `JniEvent` 为 0
- [ ] 缺失路径有测试并断言事件
- [ ] `<case>_env.cpp` 里没有 runner/文件加载/框架代码
- [ ] `runtime/jni/envs/` 没有出现目标名字
- [ ] CMake target 与运行命令写进本目录 README

---

## 8. 现有例子

`libeleven_case/` 是最小完整项目：

```text
libeleven_case/
├── libeleven_env.hpp    # installEnvironment 声明
├── libeleven_env.cpp    # ChallengeElevenFragment + data(I) 的声明
├── main.cpp             # 装环境 → 加载 ROM libc 与 assets/libeleven.so → 调 data(n)
└── assets/libeleven.so
```

```bash
cmake --build build --target duckemu_libeleven_case
./build/user_code/duckemu_libeleven_case 1 5
```
