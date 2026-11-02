# DuckDroid2

A C++ user-mode Android ARM64 emulation runtime (an unidbg-style reimplementation).

用 C++ 重写的 Android ARM64 用户态模拟执行运行时：在 x86_64 Linux 上直接运行目标 `.so`，
按 Linux arm64 ABI 补齐 syscall、bionic TLS、线程/futex、VFS、属性区与 JNI 边界。

> 代码里的目标前缀仍是 `duckemu_*`（历史命名），对外名称统一为 DuckDroid2。
>
> 目标：让真实的 Android native 签名 so 稳定跑起来，输出与真机/参考实现一致，并能作为
> 多实例服务对外提供。与 [unidbg](https://github.com/zhkl0228/unidbg) 思路同源，但实现是
> 独立的 C++ 重写，不共享代码。

**当前状态（2026-11-02）：M0–M8 与 M10 全部通过，M9 调试面垂直切片已通。**
生产构建 `57/57`，`-DDUCKEMU_WARNINGS_AS_ERRORS=ON` 也是 `57/57`；
`-DDUCKEMU_ENABLE_TRACE=ON` 为 `60/60`；`-DDUCKEMU_ENABLE_DEBUG=ON` 为 `57/57`
（`09_gdb_*` gate 尚未加入）。五个真实目标已接入：libeleven、京东 16.1.50、
得物 6.1.5.10、抖音 40.8.0、淘宝 10.68.0（SecurityGuard 6.8.260901）。
System → Worker → RuntimeInstance 的服务链路、FastAPI 案例、指令级 trace、
绝对地址 hook 与 GDB RSP 调试面均已落地。路线图见 [docs/里程碑.md](docs/里程碑.md)。

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

### 执行与地址空间

- **执行后端**：Unicorn（arm64），每个 guest 线程一个 `uc_engine`，实例级执行锁
  保证同一时刻只有一个 engine 在跑 guest 代码（单核串行，保住 `LDXR/STXR` 语义）。
- **地址空间**：宿主 mmap 作为唯一权威，支持 `mmap/munmap/mprotect/MAP_FIXED/brk`，
  所有 engine 共享同一份内存视图；写代码页触发跨 engine 的 TB 失效；scudo 需要的
  TBI alias（`1<<56` / `2<<56`）映射到同一块宿主页。
- **自修改代码**：写钩子只在地址空间存在 W+X 区时安装。绝大多数目标只有 `r-x` /
  `rw-` 映射，不必为每个 store 付一次 host 回调；guest 用 `mmap/mprotect` 造出
  W+X 区后，在 `uc_emu_start` 返回点补装（不能在 SVC 回调里直接加钩子）。淘宝
  init 的 10101 实测从 12.8 s 降到 9.1 s。
- **Guest → Host 调用桥**：只读 thunk 页 + `svc #(0x8000|index)`，Linux syscall、
  host stub、JNI 表项、导入符号共用一条分发路径；嵌套 `call_guest` 带上下文
  save/restore（上限 32 层）。

### 线程与执行模型

- `clone`（`CLONE_SETTLS` / `PARENT_SETTID` / `CHILD_CLEARTID`，arm64 的
  `CONFIG_CLONE_BACKWARDS` 参数顺序）、`futex WAIT/WAKE`（含 timespec 超时）、
  `WAIT_BITSET` / `WAKE_BITSET`（绝对超时按实例时钟换算）、`SYS_exit` / `exit_group`。
- futex 驱动的阻塞完成式（`PendingSyscall`：不重放、不回退 PC）；主线程也有
  自己的宿主线程；每线程 TLS、errno 与栈隔离，主线程栈默认 4 MiB。
- `sched_yield(2)` 真正释放执行锁并让出宿主线程；调度仍是合作式，没有抢占。
- `RuntimeConfig::default_timeout_us` 是主机发起的单次 guest 调用预算（默认 5 s）；
  超时后停止所有 engine，实例进入 Faulted（与 fault / `exit_group` 相同的终态）。
  单个 case 可以覆盖（淘宝 70102 给 900 s）。

### 真 bionic 与 loader

- 从 ROM 加载 `libc/libm/libdl/libz/liblog/libc++`，处理 `DT_NEEDED`、APS2 打包
  重定位、`DT_RELR`、`PT_TLS` / TLSDESC、IFUNC/IRELATIVE；运行 `__libc_preinit`，
  `malloc` 走真 scudo，`pthread_create/join` 走真 bionic。
- **TLS/DTV 完整语义**：guest `TlsModules` 表、静态/动态 TLS 分界、generation
  与 libc 副本同步、动态 TLSDESC 与 `__tls_get_addr` 的 DTV 扩容、
  `dlsym(STT_TLS)` 返回当前线程地址。
- **动态加载**：ld-android 的 `__loader_dlopen/android_dlopen_ext/dlsym/dlvsym/
  dlerror/dlclose/dladdr/dl_iterate_phdr` 是真实实现；构造器与 IFUNC 在嵌套 guest
  帧里跑完才返回，`dl_iterate_phdr` 带真实 program header。`dlclose` 仍是 no-op
  （见“尚未实现”）。
- **CFI shadow**：虚拟 linker 在依赖加载后调用一次 `__cfi_init`，映射 64 MiB
  零表，经过 `__cfi_slowpath` 的间接调用不再读地址 0。

### 系统调用与 VFS（M10）

- syscall handler 按子系统分文件（memory / fs / thread / event / misc），覆盖
  preinit、scudo、pthread、文件 IO、事件循环与目标实际用到的面。
- **挂载表**：实例固定命名空间 `/` = `OverlayFs(upper=可写沙箱, lower=只读宿主根…)`，
  另有 `/proc`、`/dev`、`/dev/__properties__` 合成挂载；按 guest 路径最长前缀匹配。
- **覆盖层**：读按 upper → lower；变更只落 upper；只在 lower 的变更回 `EROFS`；
  删除 upper 且 lower 同名时记录内存白障，lower 副本不再出现；跨挂载 `rename`
  回 `EXDEV`，同挂载 rename 不改 `st_ino`。
- **目录与 dirfd**：目录 `open` 得到快照描述，`getdents64` 带索引游标
  （`d_off = index+1`、部分 buffer 不丢条目、首条放不下回 `EINVAL`、`lseek` 复位、
  `dup` 共享游标）；`openat/fstatat/faccessat/unlinkat/renameat/mkdirat/readlinkat`
  接受目录 fd，非目录 fd 回 `ENOTDIR`；`pwrite64` 与 `renameat2(flags=0)` 已实现。
- **`/proc`**：优先使用目标自带的真机快照，缺失时合成 `/proc/{version,cpuinfo,
  self/status,self/cmdline,self/comm,self/maps,self/mem}` 等；`readlinkat` 支持
  `/proc/self/{exe,cwd,root,fd/N}`。
- **合成挂载**：`/dev/{null,zero,random,urandom}`（含 `rdev` 与读写模式语义）、
  `/dev/__properties__`。
- **属性区**：runtime 生成 Android 14 的三份二进制镜像（`property_info`、
  `properties_serial`、`u:object_r:<ctx>:s0`），由真 libc 在 preinit 里自己解析，
  runtime 不 interpose 任何属性符号；`--property-file` 按 build.prop 语法覆盖。
- **fd 与事件循环**：fd 表共享打开描述，支持 `dup/dup3/fcntl`；stdio 0/1/2 是
  描述符表里的永久条目；`pipe2`、`eventfd2`、`timerfd`、`poll/ppoll`、`pselect6`、
  `epoll` 由 deadline、timerfd 压缩截止与 activity 唤醒驱动；`statfs/fstatfs`
  按挂载点分类。可写 open 不再把宿主错误折叠成 `EACCES`。

### 设备画像与确定性

- **`DeviceProfile`** 统一喂 `uname`、`getuid/getgid`、`prctl(PR_GET_NAME)`、
  `/proc/{version,cpuinfo,self/status}`、`stat` 的 uid/gid、`AT_HWCAP`、
  `/proc/self/exe` 与 `/data` 容量；目标按需覆盖，默认保持中性画像。
- **`RuntimeClock`** 可注入墙钟/单调钟，注入后每次读 +1 ms；
  `clock_gettime`、`gettimeofday`、`java.lang.System` 共用同一个时钟；
  `nanosleep` / `clock_nanosleep` 在注入模式下推进实例时钟而不是真睡。
- **`RandomSource`** 可由 `--random-seed` 播种，`getrandom(2)`、`/dev/{u,}random`
  与 `AT_RANDOM`（进而 `__stack_chk_guard`）共用一条流。注入时钟 + 种子后，
  同一初始状态下的同一输入可跨进程复现。

### JNI

- `JNIEnv` 233 项 / `JavaVM` 8 项函数表；`RegisterNatives`、`Call*Method`、
  `Call*MethodV` 的 AAPCS64 va_list、数组（`New*Array` / `*ArrayRegion` /
  `NewObjectArray`）、对象字段描述符、`void` 与浮点返回值签名。
- **Java 世界严格声明**：类/方法/字段未由环境声明时返回 null + `JniEvent`，
  不自动创建。平台包在 `code/src/runtime/jni/envs/`，目标专属内容在
  `code/user_code/<case>/`。
- **反射与集合**：`Class.forName/getDeclaredMethods/getDeclaredMethod/
  getDeclaredFields`、`java.lang.reflect.{Method,Field,AccessibleObject}`、
  `Method.invoke`、`Field.get*`、原始类型 `TYPE`；`Map.values` →
  `Collection`/`Iterator`（活视图）、`Enumeration`、`ArrayList`；
  `java.net.{NetworkInterface,InetAddress}` 与 `FileDescriptor`。
- **引用生命周期**：local/global/weak global 三类引用；handle 是 guest 可见的
  8 字节 cell，对象表留在宿主侧；`Delete*Ref` 回收 cell，线程 detach/退出归还
  它持有的全部 cell；`PushLocalFrame/PopLocalFrame` 按 ART 的栈式语义工作；
  `GetObjectRefType` 可区分三类引用。
- **`JavaBinder`** 链式声明助手：首个错误记录后短路后续调用，失败时不会把 0
  当作 class/field id 继续建图；目标环境包（jdgs/szstone/dy7/taobao）已改用它。

### 服务化与 Web 案例

- **System**（每个 app 一个 UDS listener，网关/监督）→ **Worker**（按 app 的
  supervisor，不建 RuntimeInstance）→ **Instance 子进程**（`exec` 之后
  `RuntimeInstance::create`）；warm pool、会话亲和、超时 `SIGKILL`、崩溃重生。
- 目标以插件接入：`code/user_code/<case>/` + worker registry 里的 app id
  （当前 `libeleven`、`jdgs16`、`szstone`、`dy7`）；runtime / worker / system
  不随目标改动。淘宝目前是独立 runner（`duckemu_taobao_case`），没有注册
  worker 插件。
- **Web**：`code/src/web/` 是 FastAPI 服务，把 System 的 UDS 帧协议包成
  `/health`、`/sign`、`/jd/sign`，并在签名之上提供抖音数据接口
  （`/dy/sign`、`/dy/search`、`/dy/video`、`/dy/comments`、`/dy/user` 等）；
  另带只用标准库的 Python 客户端。不含鉴权、限流与任务队列，不是生产服务。

### 可观测性

- `svc.log`（9 列、tab 分隔：`id seq tid svc kind name args ret note`，
  args/note 为 JSON，可解析、可 diff）、`MemoryEvent`、`ThreadEvent`、
  `ModuleEvent`、`JniEvent`、每线程 SPSC 诊断环、x29 帧链回溯与 `trace_viewer`。
- `svc.log` 的 note 列区分三态：`{}`（完整实现）、
  `{"degraded":1,"reason":"..."}`（返回了值但跳过部分语义：信号投递、
  非线程 clone、futex bitset 掩码、文件锁、未建模的 prctl、robust list）、
  `{"unsupported":1,...}`（没有 handler，返回 ENOSYS）。接入新目标时先
  grep 这两类，再决定冻结、补最小语义还是升级成设计决策；清单见
  [`docs/runtime-contract.md`](docs/runtime-contract.md)。
- **指令级 trace**（`-DDUCKEMU_ENABLE_TRACE=ON`）：`code.log` / `bl.log` /
  `rw.log` 严格沿用 QBDI 四件套的列结构、行号和 JSON 约定，另加 `threads.tsv`
  记线程归属。目标 runner 用 `--trace DIR` 一个参数：`svc.log` 写 `DIR/svc.log`，
  指令 trace 写 `DIR/insn/`。
  - `stop_at_return` 记录到进入时的 LR 为止，不猜函数大小；hook 覆盖区间通常
    是目标模块。区间内实际执行的指令进 `code.log`；跳出覆盖区间的 `bl/blr`
    进 `bl.log`，落在 thunk 页时解析成 `JNIEnv::NewStringUTF` 这类宿主函数名。
  - `arm_address` 把 trace 定点到指定入口。init 之后仍有后台 guest 线程时，
    “窗口内第一条指令”可能属于别的线程；指定入口后，只有 PC 等于该地址才
    arm，其它执行流直接忽略。淘宝用 `lookupRegisteredNative()` 取入口，不写死偏移。
  - 装 trace code hook 后会对覆盖区间 `invalidateCode()`。入口 TB 在 init 阶段
    已按“无 hook”缓存，不 flush 会漏掉入口指令。
  - `rw.log` 的 `w` 条目在指令 POST 阶段落盘，hexdump 是写后内容；
    `max_instructions` 可以截断记录而不停止模拟。
- **地址 hook**（生产构建也编译）：绝对 guest 地址 + 回调；回调在指令执行前
  运行，且在 trace 的 PRE 采样之前。可读/写 X/W 寄存器、读/写/强写 guest
  内存、`setPc` 跳过、按顶层调用归零的 `hit_count`。用于观测和故障注入
  （白盒 AES/DFA 这类需要精确改寄存器或内存的场景）。
- **两条分析边界**：不做运行时内存 dump/回放——时钟与随机源固定后，
  同一输入 + 同一初始状态已经可复现，额外做 replay 不增加确定性；
  hook 只做原生层（绝对地址、JNI/导入符号），不把 hook 下沉成改写 guest
  执行流。

### 调试面（GDB RSP，M9）

- **模型 A（调试驱动）**：runner 加 `--debug`（默认 `127.0.0.1:31337`，
  `--debug-port` 覆盖）后停在目标入口，由 gdb / Binary Ninja / lldb 通过
  GDB Remote Serial Protocol 驱动；一次会话 = 一次目标调用。
- 已实现：`g`/`G`/`p`/`P`、`m`/`M`/`X`、`Z0` 断点、`s`/`c`/`vCont`、
  线程枚举（`qfThreadInfo`/`qsThreadInfo`/`qThreadExtraInfo`）、
  `qXfer:{features,libraries,memory-map}`、`qMemoryRegionInfo`、detach。
- 断点复用绝对地址 hook：Unicorn 的 code hook 停在指令执行**之前**
  （Probe 12A），所以不需要回写 PC，也不需要用 `BRK` 改写指令——后者在
  Unicorn 里会卡在原地址反复重试（Probe 12B）。
- 单步用 `uc_emu_start(count=1)`，不装逐指令 hook。计数运行和新增断点前会
  失效 TB 缓存：Unicorn 不会为后加的 hook 自动重译已缓存的 TB。
- 包日志默认写 `/tmp/<可执行文件名>-gdb.log`，控制台只留一行监听提示。
- **默认不编译**：`-DDUCKEMU_ENABLE_DEBUG=ON` 才编入；生产构建里
  `startDebugServer()` 返回 `Unsupported`。只在 `--debug` 时监听，只绑
  `127.0.0.1`，不经过 Worker/System。
- 已在 libeleven 上手工验收：入口断点、寄存器/内存、单步、任意地址断点、
  continue、detach 全通（gdb 客户端）。Binary Ninja 端、`09_gdb_*` gate、
  watchpoint 与线程切换尚未完成。

### 真实目标

| 目标          | 入口                                                | 结果                                                                                                                                   |
| ------------- | --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| libeleven     | `ChallengeElevenFragment.data(I)Ljava/lang/String;` | 输出 `n:<epoch_ms>`；`data(1..100)` 的 `n:` 段与参考 runner 逐值一致                                                                   |
| 京东 16.1.50  | `Bridge.main(101)`                                  | 同一注入时钟下与 DuckDroid 参考 runner 逐字节一致；带 9 条目精简 APK 与 Mi 8 属性画像（SDK 32）                                        |
| 得物 6.1.5.10 | `SzSdk.lf([BZZZ)[B`                                 | 挂真机 APK 后 body 长度 3263 与抓包一致；注入时钟 + 种子后 3183 跨进程逐字节复现；端到端 `/call` HTTP 200 / `c=10000`，解出 92 字符 sk |
| 抖音 40.8.0   | `k3.a`（`god7(url, headers)`）                      | 输出七组头；同一 `DY_WALL_TIME_MS` 下 X-Argus / X-Khronos 与参考 runner 一致                                                            |
| 淘宝 10.68.0  | `JNICLibrary.doCommandNative(I,[Object])`           | init 链 `10102→10101→10103→10104` 全 0；`--jni-events` 0 missing、EROFS 0；`70102` 返回含 x-sign / x-mini-wua / x-sgext / x-gst / x-umt 的 HashMap |

淘宝说明：目标 so 是 `libsgmainso-6.8.260901.so`，runner 是独立的
`duckemu_taobao_case`（`--init`、`--stage 10102|10101|70102`、`--trace DIR`），
状态目录跨运行持久化。`TAOBAO_RANDOM_SEED` / `TAOBAO_WALL_TIME_NS` 可以钉住
随机源与时钟，但状态目录会随运行演化，目前没有做逐字节复现断言。`10107` 按
设计不执行：它的返回值只喂给 Java 侧的 `nativeSetOsmssFunctionBridge`，而这条
Java 链不在 runner 内。运行中的 `EEXIST`/`ENOENT` 哪些是锁协议和探测、
哪些是错误，见 [`code/user_code/taobao_case/IO.md`](code/user_code/taobao_case/IO.md)。

## 尚未实现

- **信号语义**：只有 no-op；`EINTR` 是接口预留，没有真正的信号投递
  （`rt_sigaction`/`rt_sigprocmask`/`sigaltstack`/`kill` 均标 degraded）。
- **进程语义**：`fork`/`vfork`/`execve`/`wait4` 等没有 handler；进程级
  `clone`（缺 `CLONE_VM|CLONE_THREAD`）返回 `EINVAL` 并标 degraded。
- **网络**：`socket`/`bind`/`connect`/listen/accept 等返回 ENOSYS。
  得物实测只调一次 `socket(AF_INET, SOCK_STREAM, 0)`，结果无关。
- **futex 其余操作**：PI、requeue、robust list 没有实现；`WAIT_BITSET` /
  `WAKE_BITSET` 已支持，但 mask ≠ `0xffffffff` 时按 degraded 忽略 bit 匹配。
- **io 变体**：`readv/writev/preadv/pwritev/sendfile/splice/tee` 返回 ENOSYS；
  `pread64`/`pwrite64` 已实现。`ioctl(2)` 本身可用（普通/合成 fd 回 `ENOTTY`，
  未知 fd 回 `EBADF`），但没有任何具体请求被建模。
- **TLS 卸载语义**：`dlclose` 是 no-op，模块表与 DTV 槽不会因卸载回收。
- **严格 CFI**：shadow 是零表，`__loader_cfi_fail` 被绑成返回桩，检查被跳过
  而不是按真机终止。
- **回溯**：x29 帧链不是 CFI/DWARF；release so 默认 omit-frame-pointer，
  链会很短。
- **16 KiB 页**：只在 4 KiB 页上验证。
- **JNI 浮点参数**：浮点返回值已按 ABI 使用 s0/d0，浮点字段读写已放开；
  浮点**参数**（s0-s7）尚未读取，目前到达的浮点方法都是无参 getter。
- **调试面**：只支持调试驱动模型（不能在目标跑一半时附加）；watchpoint
  （Z2–Z4）回 `E01`；`H` 包一律回 OK，没有真正的线程切换；thunk 页断点、
  嵌套帧断点、信号投递与 DWARF 回溯都还没有；`09_gdb_*` gate 待补；
  `--debug` 需要 `-DDUCKEMU_ENABLE_DEBUG=ON` 构建。
- **淘宝**：70102 只覆盖一组抓取入参；未做与参考实现的逐字节对拍；
  init 链依赖持久化状态目录，首次运行与后续运行的耗时不具代表性。

## 架构

```text
System（每 app 一个 UDS listener，网关/监督）
└── Worker（按 app 的 supervisor，不建 RuntimeInstance）
    └── Instance 子进程（exec 之后 RuntimeInstance::create）
        ├── RuntimeMemory              地址空间唯一权威（region 表 + 宿主后备）
        ├── ThreadScheduler            guest 线程 = 宿主线程 = uc_engine
        │   ├── RuntimeThread × N
        │   ├── PendingSyscall         阻塞 syscall 的延后完成（含 deadline）
        │   └── WaitQueue              futex
        ├── UnicornBackend             实例级 engine 注册表 + 广播 + region 重放
        │   └── UnicornEngine          每线程 engine；按需 SMC 写钩子
        ├── Loader                     ELF / 重定位 / TLS / IFUNC / 真 bionic
        │   └── DlService              __loader_dl*、构造器、dl_iterate_phdr
        ├── VirtualFileSystem          MountTable + OverlayFs / ProcFs / 合成挂载
        │   ├── FdTable                共享打开描述（dup/fcntl、stdio）
        │   └── EventFiles             pipe/eventfd/timerfd/epoll
        ├── DeviceProfile              设备身份、uname、/proc、uid/gid、HWCAP
        ├── PropertyTable              三份属性区镜像（真 libc 解析）
        ├── RandomSource               实例随机流（getrandom / /dev/*random / AT_RANDOM）
        ├── JniState / JavaWorld       JNIEnv/JavaVM、类/方法/字段、RefTable
        ├── RuntimeClock               可注入墙钟/单调钟
        ├── ThunkTable                 guest 内 host 函数 thunk 页
        ├── InsnTrace / HookContext    M8：指令 trace 与绝对地址 hook
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
│   ├── backend/unicorn/     # engine、backend、hook、SMC 写钩子
│   ├── threads/             # RuntimeThread、调度器、futex、clone、TLS
│   ├── svc/                 # dispatcher + handlers（memory/fs/thread/event/misc）
│   ├── loader/              # ELF、重定位、TLS/DTV、IFUNC、bionic globals、DlService
│   ├── vfs/                 # MountTable、Overlay/Proc/合成后端、fd 表、目录游标
│   ├── props/               # property_info / 属性区镜像 / PropertyTable
│   ├── device/              # DeviceProfile
│   ├── random/              # RandomSource
│   ├── jni/                 # JNIEnv/JavaVM、JavaWorld、RefTable、平台 envs
│   ├── clock/               # RuntimeClock
│   ├── debug/               # M9：GDB RSP 调试面（默认不编译）
│   └── observer/            # 语义事件、诊断环、svc.log、trace、hook
├── src/worker/              # Worker supervisor、实例进程、AppPlugin
├── src/system/              # UDS 网关、WorkerPool
├── src/web/                 # FastAPI 案例（签名 + 抖音数据接口）
├── user_code/               # 五个目标项目与 app registry
├── tests/                   # integration + unit（57 个，TRACE 构建 60 个）
├── tools/spike/             # M0/M9 探针（Unicorn 语义、gdb_spike）
├── tools/trace_viewer/      # svc.log 查看器
└── tools/sign_client/       # UDS 客户端与压测
docs/
├── 里程碑.md                # 执行期唯一权威路线图（开工先读）
├── devlogs/                 # 设计与评审记录（每个里程碑/审计项一份）
├── DuckDroid2与unidbg-四种取舍.md
├── runtime-contract.md      # syscall 三态清单
└── 日记.md、目录说明.md
```

`code/user_code/` 下当前是 `libeleven_case`、`jdgs_case`、`szstone_case`、
`dy7_case`、`taobao_case`；前四个在 `user_code/registry.cpp` 里注册为 worker
app，淘宝是独立 runner。

## 构建与测试

要求：

- Linux x86_64（当前只在 Manjaro / GCC 16 上验证）
- CMake ≥ 3.20、Ninja、C++20 编译器
- `clang` 与 `llvm-objcopy`（把 arm64 汇编测试固件编成扁平 blob）
- `nlohmann_json` ≥ 3.11（UDS 线格式）
- 仓库子模块：Unicorn（只启用 aarch64）；也可用
  `-DDUCKEMU_USE_SYSTEM_UNICORN=ON` 链接系统 Unicorn
- 真 bionic / JNI / 服务 gate 需要本地 ROM 与样本；默认路径在
  `code/tests/CMakeLists.txt` 的 `DUCKEMU_*` cache 变量里，可用
  `-DDUCKEMU_ANDROID_ENV_ROOT=...` 改根目录
- Web 案例额外需要 Python 3、FastAPI、uvicorn（可选）
- 指令级 trace 额外需要 capstone >= 5（仅 `-DDUCKEMU_ENABLE_TRACE=ON` 时）

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

# 分析构建：指令级 trace（生产构建不链接 capstone）
cmake -S code -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUCKEMU_ENABLE_TRACE=ON
cmake --build build-trace -j"$(nproc)"

# libeleven：从函数入口记到进入时的 LR
./build-trace/user_code/duckemu_libeleven_case --trace /tmp/le_insn 1
# 京东：Bridge.main 是跳板，hook 覆盖目标模块，结尾同样是 LR
./build-trace/user_code/duckemu_jdgs_case --trace ./trace_logs/jd
# 淘宝：只 trace 70102 这一次调用，按注册 native 的入口 arm
TAOBAO_RANDOM_SEED=1 TAOBAO_WALL_TIME_NS=1793014979000000000 \
  ./build-trace/user_code/duckemu_taobao_case --stage 70102 --trace /tmp/tb70102

# 产出 DIR/svc.log + DIR/insn/{code,bl,rw}.log + threads.tsv
# 一次京东 main(101) 实测：code.log 1,171,888 行 / 129 MiB，
# rw.log 2,155,668 行 / 132 MiB，合计约 261 MiB（约 5.2 s）。
# 一次淘宝 70102 实测：code.log 19,179,756 行 / 2.2 GiB，
# rw.log 51,711,312 行 / 3.2 GiB，bl.log 24,524 行 / 1.5 MiB。
# trace_logs/ 已在 .gitignore 中。

# 分析构建：GDB RSP 调试面（生产构建不含）
cmake -S code -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUCKEMU_ENABLE_DEBUG=ON
cmake --build build-debug -j"$(nproc)"

# libeleven 停在入口，等 gdb / Binary Ninja 连 127.0.0.1:31337
./build-debug/user_code/duckemu_libeleven_case --debug
# gdb -q -ex 'set architecture aarch64' -ex 'target remote 127.0.0.1:31337'
# 包日志：/tmp/duckemu_libeleven_case-gdb.log
```

当前 57 个测试（生产构建；`DUCKEMU_ENABLE_TRACE=ON` 为 60，
`DUCKEMU_ENABLE_DEBUG=ON` 仍为 57），按阶段分组：

- 探针：`m0_probes`
- 裸 blob：`01_tls_read`、`02_pure_native`、`02_stack_dump`、`03_pthread_join`
- 执行模型：`09_sched_yield`、`09_call_timeout`
- loader / bionic：`04_so_load`、`04b_libc_map`、`04b_tlsdesc`、`04c_globals`、
  `04c_preinit`、`04c_vfs`、`04c_syscalls`、`04c_bionic`、`04n_tls_dtv`
- 运行时面：`04d_dlopen`、`04e_dup_fcntl`、`04f_vfs_write`、`04g_event_loop`、
  `04h_device_profile`、`04i_properties`、`04j_dev_nodes`、`04k_readlink`、
  `04l_random_seed`、`04m_statfs`
- JNI：`05_jni_env`、`05_jni_stub`、`05_jni_arrays`、`05_jni_real`
- 时钟：`06_clock_injection`
- 服务化：`07a_instance_host`、`07b_worker_supervisor`、`07c_system_gateway`、
  `07d_session_reset`、`07_end_to_end`、`07e_jdgs_plugin`、`07f_szstone_plugin`、
  `07g_dy7_plugin`
- 分析面：`08_address_hook`；TRACE=ON 另有 `08_insn_trace`、
  `08_insn_trace_threads`、`insn_decode_test`
- VFS 重构：`10a_vfs_contract`、`10b_vfs_mounts`、`10c_vfs_open_description`、
  `10d_vfs_dirent`、`10e_vfs_dirfd`
- 单元：`elf_image_test`、`runtime_memory_test`、`ring_buffer_test`、
  `guest_stack_test`、`svc_trace_format_test`、`protocol_test`、`jni_world_test`、
  `jni_platform_env_test`、`jni_layout_test`、`ref_table_test`、
  `runtime_clock_test`、`module_missing_dep_test`

### 跑真实目标

```bash
# libeleven：输出 n:<epoch_ms>
./build/user_code/duckemu_libeleven_case 1 5
# 确定性回放
./build/user_code/duckemu_libeleven_case --wall-time-ns 1792886400000000000 1 5

# 京东 16.1.50：需要精简 APK（APK 不入库，.gitignore 排除 *.apk）
./build/user_code/duckemu_jdgs_case

# 得物 6.1.5.10：设备快照 + 属性画像，注入时钟 + 种子后逐字节复现
./build/user_code/duckemu_szstone_case --random-seed 7 \
  --wall-time-ns 1792886400000000000 --monotonic-time-ns 1000000000

# 抖音 40.8.0：七组头；同一墙钟下 X-Argus / X-Khronos 与参考 runner 一致
DY_WALL_TIME_MS=1792886400000 ./build/user_code/duckemu_dy7_case --print-headers

# 淘宝 10.68.0：SecurityGuard init + 70102，打印头 map
./build/user_code/duckemu_taobao_case
# 只跑 init 诊断
./build/user_code/duckemu_taobao_case --init --jni-events
# 分段排查
./build/user_code/duckemu_taobao_case --stage 10101
```

各目标自己的运行参数、输入面与对拍方法见
[`code/user_code/README.md`](code/user_code/README.md) 及各自 case 下的 README；
淘宝的 IO 现象与排查方法见
[`code/user_code/taobao_case/IO.md`](code/user_code/taobao_case/IO.md)。

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

app 规格还支持 `filesystem-write-root=`、`property-file=`、`random-seed=`、
`workers=`；worker/system 会把它拆成对应子进程参数。

### 跑 Web 案例

先按上面的方式启动 `duckemu_system`，再：

```bash
cd code/src/web

# 启动 FastAPI
uv run python main.py
# 或
uv run uvicorn app.main:app --host 127.0.0.1 --port 8000
```

配置来自环境变量：`DUCKEMU_APP`（默认 `dy7`）、`DUCKEMU_SOCKET_DIR`
（默认 `/tmp/duckemu-dy7`）、`DUCKEMU_SOCKET`（显式指定 socket 时优先）、
`DUCKEMU_DEADLINE_MS`、`DUCKEMU_CONNECT_TIMEOUT_S`；抖音接口另有
`DOUYIN_*` 一组变量。接口、客户端与配置见
[`code/src/web/README.md`](code/src/web/README.md)；这是接入样板，不是生产服务。

## 性能基线

只在本机测过，且**没有和 unidbg 做同目标对拍**：

| 目标         | 配置                                            | 结果                                   |
| ------------ | ----------------------------------------------- | -------------------------------------- |
| libeleven    | 1 Worker × 4 实例，8 客户端，10000 次 `data(7)` | 8176 req/s，p50 0.622 ms，p99 1.322 ms |
| 京东 16.1.50 | 2 实例 / 2 客户端                               | 约 23 req/s，平均 87 ms                |
| 京东 16.1.50 | 4 实例 / 8 客户端                               | 约 39 req/s，平均 201 ms               |

libeleven 很简单（基本是魔改 base64），这组数字只说明链路通；京东单次签名约
100 ms guest CPU，单实例串行。得物与淘宝尚未做吞吐基线。淘宝 init 链里的
10101 约 9.1 s（SMC 写钩子按需安装后，此前 12.8 s），单次 70102 约 1–2 min
量级；首次运行还要从 APK 解出 `lvm_*` 模块，耗时更高。

## 路线图

| 里程碑           | 内容                                      | Gate                                                                            | 状态 |
| ---------------- | ----------------------------------------- | ------------------------------------------------------------------------------- | ---- |
| M0 探针          | 用源码级事实钉死 Unicorn 语义（九条探针） | 九条全有实测数据                                                                | ✅   |
| M1 最小闭环      | 裸 blob、TLS 正确、SVC 可分发             | `01_tls_read`、`02_pure_native`                                                 | ✅   |
| M2 可观测性      | svc trace、内存事件、帧链 dump            | `02_pure_native`、`02_stack_dump`                                               | ✅   |
| M3 线程          | clone/futex/阻塞完成式、每线程 engine     | `03_pthread_join`                                                               | ✅   |
| M4 loader        | ELF 加载、重定位、依赖递归                | `04_so_load`                                                                    | ✅   |
| M4.5 loader 补全 | APS2/RELR、TLS/TLSDESC、IFUNC、真依赖     | `04b_libc_map`                                                                  | ✅   |
| M4.6 真 bionic   | preinit/scudo、VFS/`/proc`、真 pthread    | `04c_bionic`                                                                    | ✅   |
| M5 JNI           | JNIEnv、`RegisterNatives`、严格 Java 环境 | `05_jni_stub`                                                                   | ✅   |
| M6 真实目标      | libeleven / 京东 / 得物 / 抖音 / 淘宝     | `06_clock_injection`、`07e_jdgs_plugin`、`07f_szstone_plugin`、`07g_dy7_plugin` | ✅   |
| M7 服务化        | System/Worker/Instance、UDS、warm pool    | `07_end_to_end`                                                                 | ✅   |
| M8 分析面        | 指令级 trace（LR 结尾）、绝对地址 hook    | `insn_decode_test`、`08_insn_trace`、`08_insn_trace_threads`、`08_address_hook` | ✅   |
| M9 调试面        | GDB RSP（Binary Ninja / gdb）             | `09_gdb_*`（待补）；libeleven 手工验收已过                                      | 🟨   |
| M10 VFS 重构     | 挂载表 / Overlay / 目录 fd / `getdents64` | `10a_vfs_contract` → `10e_vfs_dirfd`；四个既有目标输出不变                       | ✅   |

执行规则：一个里程碑 = 一个可 review 的 PR；gate 是测试文件，不是口头结论；
不提前实现下一阶段的抽象。M6 的 reset 语义由 `07d_session_reset` 覆盖
（回收 = 换实例进程）。

M7 之后的工作按“下一个真实目标缺什么补什么”推进。2026-10-30 完成动态
`dlopen` 与 fd 基础、可写 VFS 沙箱与事件循环、`DeviceProfile`、属性区、
`/dev` 字符节点、审计 #3/#5/#7（readlinkat/RNG/statfs）、得物接入与 CFI
shadow。2026-10-31 接入抖音（futex bitset、`GetObjectRefType`、4 MiB 栈）。
2026-11-01 起 M9 调试面：GDB RSP 在 libeleven 上跑通（入口停、单步、任意地址
断点、continue、detach）。2026-11-02 完成 M10 VFS 重构，接入淘宝
SecurityGuard（10102→10101→10103→10104，70102 mtop 签名头），并把
SMC 写钩子改成按 W+X 页按需安装。下一步是快手；M9 剩余项按需补
（BN 端、thunk/线程/watchpoint gate）。

## 设计文档

- [docs/里程碑.md](docs/里程碑.md) —— 唯一权威路线图与当前状态
- [docs/runtime-contract.md](docs/runtime-contract.md) —— syscall 三态清单与 VFS 语义
- [docs/devlogs/](docs/devlogs/) —— 每个阶段的评审结论、设计、实测结果
  - [架构评审结论](docs/devlogs/2026-10-27-01-架构评审结论.md)
  - [调用桥与线程模型决策](docs/devlogs/2026-10-27-02-调用桥与线程模型决策.md)
  - [M0 探针结果](docs/devlogs/2026-10-27-03-M0探针结果.md)
  - [M1 TLS 布局与门禁修正](docs/devlogs/2026-10-28-04-M1-TLS布局与门禁修正.md) /
    [M1 结果](docs/devlogs/2026-10-28-05-M1结果.md)
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
  - [动态 dlopen 与 fd 基础](docs/devlogs/2026-10-30-21-动态dlopen与fd基础.md)
  - [可写沙箱与事件循环](docs/devlogs/2026-10-30-22-可写沙箱与事件循环.md)
  - [DeviceProfile](docs/devlogs/2026-10-30-23-DeviceProfile.md)
  - [属性区](docs/devlogs/2026-10-30-24-属性区.md)
  - [dev 节点](docs/devlogs/2026-10-30-25-dev节点.md)
  - [审计 #3/#5/#7](docs/devlogs/2026-10-30-26-审计3-5-7.md)
  - [得物 szstone 接入与 CFI](docs/devlogs/2026-10-30-27-得物szstone接入与CFI.md)
  - [TLS/DTV 完整语义](docs/devlogs/2026-10-30-29-TLS-DTV完整语义.md)
  - [指令级 trace 与 hook 设计](docs/devlogs/2026-10-30-30-指令级trace与hook设计.md)
  - [抖音 dy7 接入](docs/devlogs/2026-10-31-31-抖音dy7接入.md)
  - [抖音错误面清理](docs/devlogs/2026-10-31-32-抖音错误面清理.md)
  - [M9 设计：GDB 远程调试](docs/devlogs/2026-11-01-33-M9设计-GDB远程调试.md)
  - [JNI 引用生命周期](docs/devlogs/2026-11-02-34-JNI引用生命周期.md)
  - [M10 设计：VFS 重构](docs/devlogs/2026-11-02-35-M10设计-VFS重构.md) /
    [M10 结果](docs/devlogs/2026-11-02-36-M10结果-VFS重构.md)
  - [淘宝 10103 设备面与 JNI 反射](docs/devlogs/2026-11-02-37-淘宝10103设备面.md)
- [DuckDroid2 与 unidbg 的四种取舍](docs/DuckDroid2与unidbg-四种取舍.md)
- [docs/目录说明.md](docs/目录说明.md) —— 目标目录结构与分层约束
- [code/user_code/ENVIRONMENT.md](code/user_code/ENVIRONMENT.md) —— 目标项目的 Java 环境写法

## 已知限制

- 单核串行：父线程在 guest 里死循环会饿死子线程；`sched_yield(2)` 现在会
  真正释放执行锁，但调度仍是合作式，没有抢占。
- `RuntimeConfig::default_timeout_us` 是主机发起的单次 guest 调用预算（默认
  5 s）；超时后所有 engine 被停止，实例进入 Faulted。生产路径另外由
  System/Worker 对实例进程 `SIGKILL` 兜底。
- 只验证了 Linux x86_64 宿主 + arm64 guest；16 KiB 页设备未覆盖。
- 淘宝首次运行会从 90 MB 的 `base.apk` 解出 `lvm_*` 模块，耗时明显高于后续
  运行；状态目录被删掉后相当于“全新安装”。

## 致谢

- [Unicorn Engine](https://github.com/unicorn-engine/unicorn)（含 QEMU TCG）——
  当前的 ARM64 执行后端。
- [unidbg](https://github.com/zhkl0228/unidbg) —— 补环境思路的先行者与参照。
- AOSP bionic —— TLS 布局、线程语义、属性区与 `libc_shared_globals` 的来源。
- LineageOS / Android 14 ROM —— 真 libc 与系统库样本。
- QBDI trace 工具链 —— `code.log`/`bl.log`/`rw.log` 继承了其文本格式的约定
  （tab 分隔、`0x` 十六进制、JSON 列），但索引体系不同。
- [nlohmann/json](https://github.com/nlohmann/json) —— UDS 线格式。
- [FastAPI](https://fastapi.tiangolo.com/) —— Web 案例的 HTTP 封装。

## 许可证

Copyright (C) 2026 DuckDroid2 contributors.

DuckDroid2 以 **GPL-2.0-only** 发布，完整文本见 [LICENSE](LICENSE)。

默认构建会静态链接 Unicorn Engine（GPL-2.0-only），因此分发包含 Unicorn 的二进制时，
整体按 GPLv2 处理。第三方依赖清单见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
`code/third_party/dynarmic` 已作为可选子模块预留，当前构建不包含。

新增源文件建议在文件头加 `SPDX-License-Identifier: GPL-2.0-only`。
