# DeviceProfile：一份设备画像喂满可观测面

> 日期：2026-10-30
> 审计顺序 4 → 5 → 1 → 6 → **3** 的最后一项；真机取值来自已连接的小米 8。

## 1. 为什么需要它

此前设备相关的值散在三处：`RuntimeConfig` 里的 pid/tid、VFS 的
uid/gid、`BionicGlobalsConfig` 的 progname，彼此独立。而目标 so 会
交叉核对：`uname` 的 release 要能和 `/proc/version` 对上，`getuid()`
要和 `/proc/self/status` 的 Uid 一致，`prctl(PR_GET_NAME)` 要和
`status` 的 Name 一致。任何一处硬编码不同都会暴露。

`runtime/device/device_profile.hpp` 把它们收成一个结构：
产品身份（model/brand/manufacturer/device/fingerprint/serial/hardware/
abi/sdk/release/incremental/build_type/debuggable）、`uname` 五个字段、
`/proc/cpuinfo` 的 implementer/part/revision/features、uid/gid、
进程名、android_id、屏幕参数、`AT_HWCAP/AT_HWCAP2`。

## 2. 谁在消费它

| 观测面 | 来源 |
|---|---|
| `uname(2)` | `device.sysname/nodename/kernel_release/kernel_version/machine` |
| `getuid/getgid/geteuid/getegid` | `device.uid/gid` |
| `prctl(PR_GET_NAME)` | `device.process_name` |
| `/proc/self/status` | Name、Uid/Gid、Threads；补了 Umask/Groups/Vm*/Cap*/Seccomp |
| `/proc/self/cmdline`、`comm` | `device.process_name` |
| `/proc/version` | `Linux version <kernel_release> (...) <kernel_version>` |
| `/proc/cpuinfo` | 按 `cpu_count` 生成，implementer/part/Features 来自画像 |
| `stat(2)` 的 uid/gid | `device.uid/gid`（时间戳仍由注入时钟决定） |
| `AT_HWCAP/AT_HWCAP2` | `device.hwcap/hwcap2`，经 `BionicGlobalsConfig` 写入 auxv |
| 进程名（`/proc/self/cmdline` 等） | `device.process_name` |

`/proc/version` 与 `/proc/cpuinfo` 是**进程无关**的条目，`makeGenerator`
以前只认 `/proc/<pid>/x`，这次在 pid 判断之前先处理它们。

## 3. 真机样本（小米 8，adb 采）

```text
model=MI 8  brand=Xiaomi  device=dipper  hardware=qcom
fingerprint=Xiaomi/dipper/dipper:8.1.0/OPM1.171019.011/V9.5.5.0.OEAMIFA:user/release-keys
sdk=32  release=12  incremental=eng.duck.20251229.113057  type=userdebug
uname: Linux localhost 4.9.327-perf-g194aea10fc23 #1 SMP PREEMPT ... aarch64
cpu: implementer 0x51 (Qualcomm Kryo), part 0x803, rev 12,
     features fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
display: 1080x2248, density 440
```

这些值不写进默认画像（默认仍是中性的 duckemu），由目标包按需覆盖。
默认值刻意保持与改造前一致的对外表现，避免既有测试与目标突然变脸。

## 4. 测试

`04h_device_profile`：fixture 用裸 syscall 读 `uname`、`getuid/getgid`、
`prctl(PR_GET_NAME)`、`/proc/version`、`/proc/cpuinfo`、
`/proc/self/status`，宿主注入一组独特的画像值（testproc / testnode /
4.9.327-perf-test / uid 10042 / implementer 0x51）并逐项核对一致。

全量 **39/39** 通过。

## 5. 仍未覆盖（原审计 8 等）

**属性（`ro.*` / `__system_property_get`）没有任何基础设施**。DuckDroid2
加载的是真实 libc，属性实现也在 libc 里，两个方向可选：

1. **符号 interpose**：在 `SymbolResolver` 里对
   `__system_property_get/find/read_callback` 先查宿主覆盖表，返回 thunk，
   由 runtime 用 `DeviceProfile` + 目标包属性表作答。改动小，但只覆盖
   "app so 从 libc 导入" 的调用，libc 内部自调用不受影响。
2. **合成属性区**：在 guest 里建 `/dev/__properties__` 与
   `prop_area` 格式，让 libc 原样读。保真度高，工作量也大。

其余的 7（io 变体）、9（/dev 节点）、10（RNG 注入）、11（AT_RANDOM 语义）、
12（TLS 动态面）、13（JNI 剩余）按原清单继续。
