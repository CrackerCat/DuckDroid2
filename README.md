# DuckEmu

A C++ user-mode Android ARM64 emulation runtime (an unidbg-style reimplementation).

用 C++ 重写的 Android ARM64 用户态模拟执行运行时：在 x86_64 Linux 上直接运行目标 `.so`，
按 Linux arm64 ABI 补齐 syscall、bionic TLS、线程/futex、JNI 环境等语义。

> 目标：让真实的 Android native 签名 so 稳定跑起来，输出与真机一致，并能作为多实例
> 服务对外提供。与 [unidbg](https://github.com/zhkl0228/unidbg) 思路同源，但实现是
> 独立的 C++ 重写，不共享代码。

**当前状态（2026-10-28）：早期开发中。** M0–M3 已完成，能跑裸 arm64 blob；
真实 `.so` 加载与 JNI 尚未实现。路线图见 [docs/里程碑.md](docs/里程碑.md)。

---

## 它解决什么问题

Android 的 native 代码（签名、加固、风控算法等）以 `.so` 形式存在，依赖 bionic、
Linux 内核、JNI 与 ART。要脱离真机做分析或批量调用，通常有三条路：

1. 逆成伪代码重写 —— 成本高，行为容易失真；
2. 嵌一个真 Android/ART —— 重、慢、生命周期难管；
3. **模拟执行**：不启动 Android，只在宿主上实现目标代码所依赖的那部分语义。

DuckEmu 走第 3 条。它不加载 libc 的真实实现，而是用 SVC 陷阱把
“内核 + libc + JNI” 的入口统一截获，在宿主 C++ 里实现，再把结果写回 guest。

和 unidbg 一样做“补环境”，不一样的地方在于：从一开始就按可观测、可多线程、
可服务化的目标设计，并且把每个里程碑的验收标准固化成 integration test。

## 已实现

- **执行后端**：Unicorn（arm64），每个 guest 线程一个 `uc_engine`，实例级执行锁
  保证同一时刻只有一个 engine 在跑 guest 代码（单核串行，保住 `LDXR/STXR` 语义）。
- **地址空间**：宿主 mmap 作为唯一权威，`mmap/munmap/mprotect/MAP_FIXED/brk`，
  所有 engine 共享同一份内存视图；写代码页触发跨 engine 的 TB 失效。
- **Guest → Host 调用桥**：只读 thunk 页 + `svc #(0x8000|index)`，Linux syscall 与
  host stub 共用一条分发路径。
- **线程**：`clone`（`CLONE_SETTLS` / `PARENT_SETTID` / `CHILD_CLEARTID`）、
  `futex WAIT/WAKE`、`SYS_exit` / `exit_group`、futex 驱动的阻塞完成式
  （`PendingSyscall`：不重放、不回退 PC）。
- **bionic arm64 TLS 布局**（Android 13）：`[TP]` = DTV，`[TP+8]` = pthread_internal_t*，
  errno 在 pthread_internal_t 内；clone 的 TLS 由 guest 提供，runtime 只 adopt。
- **可观测性**：`svc.log`（可解析、可 diff 的 SVC 语义日志）、`MemoryEvent`、
  `ThreadEvent`、可选的高频诊断环形缓冲（每线程一个 SPSC 环）。
- **测试**：M0 的九条 Unicorn 探针，以及 01–03 三个 gate 的裸 blob integration test。

## 尚未实现

- ELF loader / 重定位 / 符号解析（M4）；加载真实 `.so`
- 虚拟 bionic 符号表（M4）
- JNIEnv 与 `RegisterNatives`（M5）
- 真实目标 so 的一致性验收（M6）、多实例与 UDS 服务（M7）
- 信号的完整语义、futex 超时/`WAIT_BITSET`/PI、抢占/watchdog
- 指令级 trace（设计上默认关闭，留给分析模式）

## 架构

```text
WorkerProcess / System（M7，规划中）
└── RuntimeInstance                     = 一个被模拟的 Android 进程
    ├── RuntimeMemory                  地址空间唯一权威（region 表 + 宿主后备）
    ├── RuntimeThread × N              线程 = 宿主 std::thread + uc_engine
    │   ├── SvcDispatcher              SVC 拦截 + syscall/host stub 分发
    │   ├── TlsManager（仅引导）       裸 blob 模式下的主线程 TCB
    │   └── PendingSyscall             阻塞 syscall 的延后完成
    ├── UnicornBackend                 实例级 engine 注册表 + 广播 + region 重放
    ├── ThunkTable                     guest 内 host 函数 thunk 页
    └── EventBus                       语义事件（Svc/Memory/Thread）+ 诊断通道
```

关键不变量：

1. **guest 线程 = 宿主线程 = `uc_engine`**，1:1:1。
2. **同一时刻只有一个 engine 执行 guest 代码**；线程阻塞时释放执行锁。
3. **地址空间只有一份权威**（`RuntimeMemory`），所有 engine 共享并保持权限/代码缓存同步。
4. **一个 SVC 只分发一次**：阻塞的 syscall 不重放，进入时分配 `seq`、完成时发布 `id`。

## 目录

```text
code/
├── src/common/              # ID、Result/Error、日志
├── src/runtime/
│   ├── instance/            # RuntimeInstance 门面
│   ├── memory/              # 地址空间、region、thunk 页
│   ├── backend/unicorn/     # engine、backend、hook
│   ├── threads/             # RuntimeThread、调度器、futex、clone、TLS
│   ├── svc/                 # dispatcher + syscall handlers
│   └── observer/            # 语义事件、诊断环、svc.log、guest 栈回溯
├── tests/                   # integration（裸 blob）+ unit
├── tools/spike/             # M0 的 Unicorn 九条探针
└── tools/trace_viewer/      # svc.log 查看器
docs/
├── 里程碑.md                # 执行期唯一权威路线图（开工先读）
├── devlogs/                 # 设计与评审记录（每个里程碑一份）
└── 日记.md、目录说明.md
```

## 构建与测试

要求：

- Linux x86_64（当前只在 Manjaro / GCC 16 上验证）
- CMake ≥ 3.20、Ninja、C++20 编译器
- `clang` 与 `llvm-objcopy`（把 arm64 汇编测试固件编成扁平 blob）
- 仓库子模块：Unicorn（只启用 aarch64）

```bash
git clone --recursive <repo-url> DuckEmu
cd DuckEmu

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

当前 9 个测试：`m0_probes`、`01_tls_read`、`02_pure_native`、`02_stack_dump`、
`03_pthread_join`、`runtime_memory_test`、`ring_buffer_test`、`guest_stack_test`、
`svc_trace_format_test`。

## 路线图

| 里程碑 | 内容 | Gate | 状态 |
|---|---|---|---|
| M0 探针 | 用源码级事实钉死 Unicorn 语义（九条探针） | 九条全有实测数据 | ✅ |
| M1 最小闭环 | 裸 blob、TLS 正确、SVC 可分发 | `01_tls_read`、`02_pure_native` | ✅ |
| M2 可观测性 | svc trace、内存事件、帧链 dump | `02_pure_native`、`02_stack_dump` | ✅ |
| M3 线程 | clone/futex/阻塞完成式、每线程 engine | `03_pthread_join` | ✅ |
| M4 loader | ELF 加载、重定位、虚拟 bionic | `04_so_load` | ⬜ |
| M5 JNI | JNIEnv、`RegisterNatives` | `05_jni_stub` | ⬜ |
| M6 真实目标 | 北极星 so 输出与真机一致 | 真实签名一致 | ⬜ |
| M7 服务化 | fork 隔离、WorkerPool、UDS | `07_end_to_end` | ⬜ |

执行规则：一个里程碑 = 一个可 review 的 PR；gate 是测试文件，不是口头结论；
不提前实现下一阶段的抽象。

## 设计文档

- [docs/里程碑.md](docs/里程碑.md) —— 唯一权威路线图与当前状态
- [docs/devlogs/](docs/devlogs/) —— 每个阶段的评审结论、设计、实测结果
  - [调用桥与线程模型决策](docs/devlogs/2026-10-27-02-调用桥与线程模型决策.md)
  - [M0 探针结果](docs/devlogs/2026-10-27-03-M0探针结果.md)
  - [M1 TLS 布局与门禁修正](docs/devlogs/2026-10-28-04-M1-TLS布局与门禁修正.md)
  - [M2 设计](docs/devlogs/2026-10-28-06-M2设计.md) / [M2 结果](docs/devlogs/2026-10-28-07-M2结果.md)
  - [M3 设计](docs/devlogs/2026-10-28-08-M3设计.md) / [M3 结果](docs/devlogs/2026-10-28-09-M3结果.md)
- [docs/目录说明.md](docs/目录说明.md) —— 目标目录结构与分层约束

## 已知限制

- 单核串行：父线程在 guest 里死循环会饿死子线程；尚无抢占/watchdog。
- Unicorn 的 `timeout` 参数已弃用（其超时线程与下一轮运行存在竞态）；
  guest 死循环目前由测试框架超时兜底。
- 不支持新进程 `clone`、futex 超时/PI/`WAIT_BITSET`、信号语义。
- 只验证了 Linux x86_64 宿主 + arm64 guest。

## 致谢

- [Unicorn Engine](https://github.com/unicorn-engine/unicorn)（含 QEMU TCG）——
  当前的 ARM64 执行后端。
- [unidbg](https://github.com/zhkl0228/unidbg) —— 补环境思路的先行者与参照。
- AOSP bionic —— TLS 布局与线程语义的来源。
- QBDI trace 工具链 —— `svc.log` 继承了其文本格式的约定（tab 分隔、`0x` 十六进制、
  JSON 列），但索引体系不同。

## 许可证

尚未确定。正式开源前会补上 `LICENSE`（倾向 Apache-2.0 或 MIT）。
在此之前，请勿直接再分发。
