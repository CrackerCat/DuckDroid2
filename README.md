# DuckDroid2

A C++ user-mode Android ARM64 emulation runtime (an unidbg-style reimplementation).

用 C++ 重写的 Android ARM64 用户态模拟执行运行时：在 x86_64 Linux 上直接运行目标 `.so`，
按 Linux arm64 ABI 补齐 syscall、bionic TLS、线程/futex、VFS 与 JNI 边界。

> 代码里的目标前缀仍是 `duckemu_*`（历史命名），对外名称统一为 DuckDroid2。
>
> 目标：让真实的 Android native 签名 so 稳定跑起来，输出与真机/参考实现一致，并能作为
> 多实例服务对外提供。与 [unidbg](https://github.com/zhkl0228/unidbg) 思路同源，但实现是
> 独立的 C++ 重写，不共享代码。

**当前状态（2026-10-29）：M0–M7 全部通过，34/34 tests。** libeleven 与京东
16.1.50 两个真实目标已接入；System → Worker → RuntimeInstance 的服务链路跑通。
路线图见 [docs/里程碑.md](docs/里程碑.md)。

---

## 它解决什么问题

Android 的 native 代码（签名、加固、风控算法等）以 `.so` 形式存在，依赖 bionic、
Linux 内核、JNI 与 ART。要脱离真机做分析或批量调用，通常有三条路：

1. 逆成伪代码重写 —— 成本高，行为容易失真；
2. 嵌一个真 Android/ART —— 重、慢、生命周期难管；
3. **模拟执行**：不启动 Android，只在宿主上实现目标代码所依赖的那部分语义。

DuckDroid2 走第 3 条。它加载 ROM 里的真 libc（LineageOS 21 / Android 14），补
syscall、TLS、线程/futex、VFS 与 JNI 边界；导入符号、JNI 表项、syscall 统一通过
thunk / SVC 回到宿主 C++。

和 unidbg 一样做“补环境”，不一样的地方在于：真 bionic、显式 Java 环境、进程隔离、
可服务化。两者的取舍写在
[docs/DuckDroid2与unidbg-四种取舍.md](docs/DuckDroid2与unidbg-四种取舍.md)。

## 已实现

- **执行后端**：Unicorn（arm64），每个 guest 线程一个 `uc_engine`，实例级执行锁
  保证同一时刻只有一个 engine 在跑 guest 代码（单核串行，保住 `LDXR/STXR` 语义）。
- **地址空间**：宿主 mmap 作为唯一权威，`mmap/munmap/mprotect/MAP_FIXED/brk`，
  所有 engine 共享同一份内存视图；写代码页触发跨 engine 的 TB 失效；scudo 需要的
  TBI alias（`1<<56` / `2<<56`）映射到同一块宿主页。
- **Guest → Host 调用桥**：只读 thunk 页 + `svc #(0x8000|index)`，Linux syscall、
  host stub、JNI 表项、导入符号共用一条分发路径。
- **线程**：`clone`（`CLONE_SETTLS` / `PARENT_SETTID` / `CHILD_CLEARTID`）、
  `futex WAIT/WAKE`、`SYS_exit` / `exit_group`、futex 驱动的阻塞完成式
  （`PendingSyscall`：不重放、不回退 PC）；主线程也有自己的宿主线程。
- **真 bionic**：从 ROM 加载 `libc/libm/libdl/libz/liblog/libc++`，处理
  `DT_NEEDED`、APS2 打包重定位、`DT_RELR`、`PT_TLS` / TLSDESC、IFUNC/IRELATIVE；
  运行 `__libc_preinit`，`malloc` 走真 scudo，`pthread_create/join` 走真 bionic。
- **JNI**：`JNIEnv` 233 项 / `JavaVM` 8 项函数表，`RegisterNatives`、`Call*Method`、
  数组、`*V` 变参、对象字段描述符；Java 世界严格声明，缺失返回 null + `JniEvent`。
- **服务化**：System（UDS 网关）→ Worker（按 app 的 supervisor）→ Instance
  （`RuntimeInstance` 子进程）；warm pool、会话亲和、超时 `SIGKILL`、崩溃重生。
- **确定性**：`RuntimeClock` 可注入墙钟/单调钟，注入后每次读 +1 ms；
  `clock_gettime`、`gettimeofday`、`java.lang.System` 共用同一个时钟。
- **可观测性**：`svc.log`（可解析、可 diff 的 SVC 语义日志）、`MemoryEvent`、
  `ThreadEvent`、`ModuleEvent`、`JniEvent`、每线程 SPSC 诊断环、x29 帧链回溯。
- **真实目标**：libeleven（`data(I)Ljava/lang/String;`）与京东 16.1.50
  （`Bridge.main`）；libeleven 对拍 `n:` 段，京东在同一注入时钟下与 DuckDroid
  参考 runner 逐字节一致。

## 尚未实现

- **信号语义**：只有 no-op；`EINTR` 是接口预留，没有真正的信号投递。
- **动态 TLS/DTV**：所有 `PT_TLS` 按静态槽处理；`__tls_get_addr` 的 DTV 扩容、
  `dlopen` 后的 TLS 访问没做；`tls_modules` 仍是空表。
- **futex 面**：timeout、`WAIT_BITSET`、PI、robust list、requeue 都没有。
- **随机数注入**：`getrandom` 和 `AT_RANDOM` 仍是宿主 `std::random_device`，
  完全可复现还需要实例级 `RuntimeRandom`。
- **指令级 trace / 回放**：只有语义级 `svc.log`，没有指令级 trace，也没有回放器。
- **调试与 hook**：没有 inline hook、Dobby/xHook、gdb/IDA stub；拦截点是
  SVC、GOT/导入符号和 JNI 表。
- **回溯**：x29 帧链不是 CFI/DWARF；release so 默认 omit-frame-pointer，链会很短。
- **16 KiB 页**：只在 4 KiB 页上验证。
- **更多目标**：快手、抖音还没接；`libsgmainso` 只验证了加载和绑定。

## 架构

```text
System（每 app 一个 UDS listener，网关/监督）
└── Worker（按 app 的 supervisor，不建 RuntimeInstance）
    └── Instance 子进程（exec 之后 RuntimeInstance::create）
        ├── RuntimeMemory              地址空间唯一权威（region 表 + 宿主后备）
        ├── ThreadScheduler            guest 线程 = 宿主线程 = uc_engine
        │   ├── RuntimeThread × N
        │   ├── PendingSyscall         阻塞 syscall 的延后完成
        │   └── WaitQueue              futex
        ├── UnicornBackend             实例级 engine 注册表 + 广播 + region 重放
        ├── Loader                     ELF / 重定位 / TLS / IFUNC / 真 bionic
        ├── VirtualFileSystem          /proc、host roots、只读文件 mmap
        ├── JniState / JavaWorld       JNIEnv/JavaVM、类/方法/字段、shim
        ├── RuntimeClock               可注入墙钟/单调钟
        ├── ThunkTable                 guest 内 host 函数 thunk 页
        └── EventBus                   语义事件 + 诊断通道
```

关键不变量：

1. **guest 线程 = 宿主线程 = `uc_engine`**，1:1:1。
2. **同一时刻只有一个 engine 执行 guest 代码**；线程阻塞时释放执行锁。
3. **地址空间只有一份权威**（`RuntimeMemory`），所有 engine 共享并保持权限/代码缓存同步。
4. **一个 SVC 只分发一次**：阻塞的 syscall 不重放，进入时分配 `seq`、完成时发布 `id`。
5. **fork 点早于任何 engine / 宿主线程**：实例进程在 `exec` 之后才创建
   `RuntimeInstance`，不在活实例上 fork。

## 目录

```text
code/
├── src/common/              # ID、Result/Error、日志、UDS 封装
├── src/protocol/            # 4B 长度帧、JSON 消息、路由
├── src/runtime/
│   ├── instance/            # RuntimeInstance 门面
│   ├── memory/              # 地址空间、region、thunk 页
│   ├── backend/unicorn/     # engine、backend、hook
│   ├── threads/             # RuntimeThread、调度器、futex、clone、TLS
│   ├── svc/                 # dispatcher + syscall handlers
│   ├── loader/              # ELF、重定位、TLS、IFUNC、bionic globals
│   ├── vfs/                 # /proc、host roots、文件 mmap
│   ├── jni/                 # JNIEnv/JavaVM、JavaWorld、refs、envs
│   ├── clock/               # RuntimeClock
│   └── observer/            # 语义事件、诊断环、svc.log、guest 栈回溯
├── src/worker/              # Worker supervisor、实例进程、AppPlugin
├── src/system/              # UDS 网关、WorkerPool
├── user_code/               # 目标项目（libeleven_case、jdgs_case）与注册表
├── tests/                   # integration + unit（34 个）
├── tools/spike/             # M0 的 Unicorn 九条探针
├── tools/trace_viewer/      # svc.log 查看器
└── tools/sign_client/       # UDS 客户端与压测
docs/
├── 里程碑.md                # 执行期唯一权威路线图（开工先读）
├── devlogs/                 # 设计与评审记录（每个里程碑一份）
├── DuckDroid2与unidbg-四种取舍.md
└── 日记.md、目录说明.md
```

## 构建与测试

要求：

- Linux x86_64（当前只在 Manjaro / GCC 16 上验证）
- CMake ≥ 3.20、Ninja、C++20 编译器
- `clang` 与 `llvm-objcopy`（把 arm64 汇编测试固件编成扁平 blob）
- `nlohmann_json` ≥ 3.11（M7 的线上格式）
- 仓库子模块：Unicorn（只启用 aarch64）
- 真 bionic / JNI / 服务 gate 需要本地 ROM 与样本；默认路径在
  `code/tests/CMakeLists.txt` 的 `DUCKEMU_*` cache 变量里，可用
  `-DDUCKEMU_ANDROID_ENV_ROOT=...` 改根目录

```bash
git clone --recursive <repo-url> DuckDroid2
cd DuckDroid2

cmake -S code -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

可选：

```bash
# 严格构建
cmake -S code -B build-werror -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUCKEMU_WARNINGS_AS_ERRORS=ON

# ASan + UBSan
cmake -S code -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DDUCKEMU_SANITIZE_ADDRESS=ON -DDUCKEMU_SANITIZE_UNDEFINED=ON

# 查看 svc trace
./build/trace_viewer summary build/tests/traces/02_pure_native.svc.log
```

当前 34 个测试，按阶段分组：

- M0：`m0_probes`
- 裸 blob：`01_tls_read`、`02_pure_native`、`02_stack_dump`、`03_pthread_join`
- loader / bionic：`04_so_load`、`04b_tlsdesc`、`04b_libc_map`、`04c_globals`、
  `04c_preinit`、`04c_vfs`、`04c_syscalls`、`04c_bionic`
- JNI：`05_jni_env`、`05_jni_stub`、`05_jni_arrays`、`05_jni_real`
- 时钟：`06_clock_injection`
- 服务化：`07a_instance_host`、`07b_worker_supervisor`、`07c_system_gateway`、
  `07d_session_reset`、`07_end_to_end`、`07e_jdgs_plugin`
- 单元：`elf_image_test`、`runtime_memory_test`、`ring_buffer_test`、
  `guest_stack_test`、`svc_trace_format_test`、`protocol_test`、`jni_world_test`、
  `jni_layout_test`、`runtime_clock_test`、`module_missing_dep_test`

### 跑真实目标

```bash
# libeleven：输出 n:<epoch_ms> 的结构
./build/user_code/duckemu_libeleven_case 1 5

# 京东 16.1.50：需要先把精简 APK 放到 assets/root/.../base.apk
# （APK 不入库，.gitignore 排除 *.apk）
./build/user_code/duckemu_jdgs_case
```

### 跑服务

```bash
ROM=/path/to/ROM/out/target/product/lemonade/symbols/system/lib64
SO=$PWD/code/user_code/libeleven_case/assets/libeleven.so

./build/duckemu_system \
  --socket-dir /tmp/duckemu \
  --worker-exec "$PWD/build/duckemu_worker" \
  --wall-time-ns 1792886400000000000 \
  --app "id=libeleven,library=$SO,rom-libc=$ROM/bootstrap/libc.so,library-path=$ROM/bootstrap:$ROM,instances=2"

python3 code/tools/sign_client/sign_client.py /tmp/duckemu/libeleven.sock 7
```

## 性能基线

只在本机测过，且**没有和 unidbg 做同目标对拍**：

| 目标 | 配置 | 结果 |
|---|---|---|
| libeleven | 1 Worker × 4 实例，8 客户端，10000 次 `data(7)` | 8176 req/s，p50 0.622 ms，p99 1.322 ms |
| 京东 16.1.50 | 2 实例 / 2 客户端 | 约 23 req/s，平均 87 ms |
| 京东 16.1.50 | 4 实例 / 8 客户端 | 约 39 req/s，平均 201 ms |

libeleven 很简单（基本是魔改 base64），这组数字只说明链路通；京东单次签名约
100 ms guest CPU，单实例串行。

## 路线图

| 里程碑 | 内容 | Gate | 状态 |
|---|---|---|---|
| M0 探针 | 用源码级事实钉死 Unicorn 语义（九条探针） | 九条全有实测数据 | ✅ |
| M1 最小闭环 | 裸 blob、TLS 正确、SVC 可分发 | `01_tls_read`、`02_pure_native` | ✅ |
| M2 可观测性 | svc trace、内存事件、帧链 dump | `02_pure_native`、`02_stack_dump` | ✅ |
| M3 线程 | clone/futex/阻塞完成式、每线程 engine | `03_pthread_join` | ✅ |
| M4 loader | ELF 加载、重定位、依赖递归 | `04_so_load` | ✅ |
| M4.5 loader 补全 | APS2/RELR、TLS/TLSDESC、IFUNC、真依赖 | `04b_libc_map` | ✅ |
| M4.6 真 bionic | preinit/scudo、VFS/`/proc`、真 pthread | `04c_bionic` | ✅ |
| M5 JNI | JNIEnv、`RegisterNatives`、严格 Java 环境 | `05_jni_stub` | ✅ |
| M6 真实目标 | libeleven / 京东签名结果一致 | `06_clock_injection`、`07e_jdgs_plugin`（reset 由 `07d_session_reset`） | ✅ |
| M7 服务化 | System/Worker/Instance、UDS、warm pool | `07_end_to_end` | ✅ |

执行规则：一个里程碑 = 一个可 review 的 PR；gate 是测试文件，不是口头结论；
不提前实现下一阶段的抽象。M6 的 reset 语义由 `07d_session_reset` 覆盖
（回收 = 换实例进程）。

## 设计文档

- [docs/里程碑.md](docs/里程碑.md) —— 唯一权威路线图与当前状态
- [docs/devlogs/](docs/devlogs/) —— 每个阶段的评审结论、设计、实测结果
  - [调用桥与线程模型决策](docs/devlogs/2026-10-27-02-调用桥与线程模型决策.md)
  - [M0 探针结果](docs/devlogs/2026-10-27-03-M0探针结果.md)
  - [M1 TLS 布局与门禁修正](docs/devlogs/2026-10-28-04-M1-TLS布局与门禁修正.md)
  - [M2 设计](docs/devlogs/2026-10-28-06-M2设计.md) / [M2 结果](docs/devlogs/2026-10-28-07-M2结果.md)
  - [M3 设计](docs/devlogs/2026-10-28-08-M3设计.md) / [M3 结果](docs/devlogs/2026-10-28-09-M3结果.md)
  - [M4 设计](docs/devlogs/2026-10-28-10-M4设计.md) / [M4 结果](docs/devlogs/2026-10-28-11-M4结果.md)
  - [路线修正：真 bionic](docs/devlogs/2026-10-28-12-路线修正-真bionic.md)
  - [M4.5 设计](docs/devlogs/2026-10-28-13-M4.5设计.md) / [M4.5 结果](docs/devlogs/2026-10-28-14-M4.5结果.md)
  - [M4.6 设计](docs/devlogs/2026-10-28-15-M4.6设计.md)
  - [M5 设计](docs/devlogs/2026-10-29-16-M5设计.md)
  - [M6 进度：libeleven 调用](docs/devlogs/2026-10-29-17-M6进度-libeleven调用.md)
  - [M6 时钟注入](docs/devlogs/2026-10-29-18-M6时钟注入.md)
  - [M7 设计](docs/devlogs/2026-10-29-19-M7设计-进程模型与网关.md) / [M7 结果](docs/devlogs/2026-10-29-20-M7结果.md)
  - [京东 16.1.50 接入](docs/devlogs/2026-10-29-20-JD接入与JNI面补全.md)
- [DuckDroid2 与 unidbg 的四种取舍](docs/DuckDroid2与unidbg-四种取舍.md)
- [docs/目录说明.md](docs/目录说明.md) —— 目标目录结构与分层约束
- [code/user_code/ENVIRONMENT.md](code/user_code/ENVIRONMENT.md) —— 目标项目的 Java 环境写法

## 已知限制

- 单核串行：父线程在 guest 里死循环会饿死子线程；没有抢占，生产超时靠
  System/Worker 对实例进程 `SIGKILL`。
- Unicorn 的 `timeout` 参数已弃用（其超时线程与下一轮运行存在竞态）；
  DuckDroid2 统一传 `timeout = 0`。
- 绑定 ROM：`libc_shared_globals` 偏移、静态 TLS 布局、clone 语义都跟当前
  LineageOS 21 / `lemonade` 走；换 ROM 要重新核对。
- 只验证了 Linux x86_64 宿主 + arm64 guest；16 KiB 页设备未覆盖。
- 真实目标只有 libeleven 和京东两个；libeleven 很简单，京东单次约 100 ms
  guest CPU，性能基线不代表复杂 so。
- 京东的精简 APK 不入库（`.gitignore` 排除 `*.apk`），新克隆需自行放置。
- 没有和 unidbg 做同目标、同输入、同机器的性能对拍。

## 致谢

- [Unicorn Engine](https://github.com/unicorn-engine/unicorn)（含 QEMU TCG）——
  当前的 ARM64 执行后端。
- [unidbg](https://github.com/zhkl0228/unidbg) —— 补环境思路的先行者与参照。
- AOSP bionic —— TLS 布局、线程语义与 `libc_shared_globals` 的来源。
- LineageOS / Android 14 ROM —— 真 libc 与系统库样本。
- QBDI trace 工具链 —— `svc.log` 继承了其文本格式的约定（tab 分隔、`0x` 十六进制、
  JSON 列），但索引体系不同。
- [nlohmann/json](https://github.com/nlohmann/json) —— M7 的线格式。

## 许可证

Copyright (C) 2026 DuckDroid2 contributors.

DuckDroid2 以 **GPL-2.0-only** 发布，完整文本见 [LICENSE](LICENSE)。

默认构建会静态链接 Unicorn Engine（GPL-2.0-only），因此分发包含 Unicorn 的二进制时，
整体按 GPLv2 处理。第三方依赖清单见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

新增源文件建议在文件头加 `SPDX-License-Identifier: GPL-2.0-only`。
