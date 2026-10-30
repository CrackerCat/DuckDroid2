# szstone_case — 得物 6.1.5.10 libszstone.so sk 注册 body

`com.shizhuang.stone.main.SzSdk.lf([BZZZ)[B` 是 sk 注册的本地一半：
入参是设备画像 blob（`key\x1cvalue\x1d...`），出参是 `POST dav.dewu.com/call`
的 body（base64url 解码后的字节）。服务端回包里的 `k` 再解一次就是 92 字符 sk。

## 文件

```text
szstone_case/
├── szstone_env.hpp/.cpp   # SzSdk 的 10 个 native + lf 走到的 Android 类
├── szstone_cert.h         # APK 签名证书 DER（PackageInfo.signatures 的字节）
├── szstone_codec.hpp      # runner/plugin 共用的 hex 与 z 参数解析
├── szstone.properties     # 抓取设备的系统属性（只放 lf 读到的键）
├── main.cpp               # 本地 runner：装环境 → 加载 libc + so → 调 lf → 对比
├── plugin.hpp/.cpp        # worker 适配，app id "szstone"（op "lf"）
└── assets/
    ├── libszstone.so      # 6.1.5.10 的 UPX 打包 so（框架内自解包）
    ├── lf_in.bin          # 真机 hook dump 的 2980 字节设备画像
    ├── lf_out.bin         # 同一台真机的 lf 输出（3263 字节），用于对比
    ├── capture/           # call_0 抓包：原始 HTTP 请求 + 解码后的 3263 字节输出
    └── root/              # 真机文件快照：proc/、etc/hosts、system|vendor/build.prop
```

## 运行

```bash
cmake --build build --target duckemu_szstone_case
./build/user_code/duckemu_szstone_case
# 指定画像 / 期望输出 / 三个 boolean / 文件系统根
./build/user_code/duckemu_szstone_case --blob in.bin --expect out.bin --z 1,1,1 --root DIR
# 逐字节可复现的回放（lf 的 nonce 与耗时字段被钉住）
./build/user_code/duckemu_szstone_case --random-seed 7 \
  --wall-time-ns 1792886400000000000 --monotonic-time-ns 1000000000
```

`z` 默认是 `1,1,1`，即真机 sk 注册时的 `lf(blob, true, true, true)`。

输出：

```text
lf in=2980 z=(1,1,1) out=<n>
<hex>
jni_events=0
match_expect=true|false
```

`jni_events > 0` 说明 Java 环境还有洞。

## 长度

lf 输出带随机 nonce，与真机抓包之间不能逐字节比较；同一注入时钟 + 种子下
可以跨进程复现。能跨环境对齐的是长度：

| 环境 | 长度 |
| --- | --- |
| 设备快照 + `szstone.properties`，宿主时钟 | 3199 |
| 同上，注入 `--wall-time-ns` + `--monotonic-time-ns` + `--random-seed` | **3183，逐字节可复现** |
| 同上再挂真机 APK（`call_0` 抓包的同配置） | **3263** |

> 旧文档里的 3215 是 `gsm.version.baseband` 还在属性画像里时的长度；那个值
> 属于另一次抓取，`call_0` 对应的真机调用里 baseband 是空的。去掉它以后挂
> APK 正好是抓包的 3263。

`match_expect=false` 是预期的：`lf_out.bin` 来自真机，nonce 与本次不同，且
默认没有挂 APK。gate 因此只断言长度与两次实例间的逐字节一致性。

### 与 `call_0` 抓包对拍

`assets/capture/call_0_3263.req.txt` 是抓到的原始 HTTP 请求，body 是
`base64url(lf_out)`，解码后 3263 字节（等于 `assets/capture/lf_out_0_3263.bin`）。
把真机 APK 按画像里的 `dadir` 路径放进一个 guest 根：

```bash
APK_ROOT=/tmp/szs-apk-root
mkdir -p "$APK_ROOT/data/app/~~jKTCowhdea931IMmF_a-DA==/com.shizhuang.duapp-3R6fyRkBVCZKmrfOBdhqrA=="
ln /path/to/du.apk \
  "$APK_ROOT/data/app/~~jKTCowhdea931IMmF_a-DA==/com.shizhuang.duapp-3R6fyRkBVCZKmrfOBdhqrA==/base.apk"

./build/user_code/duckemu_szstone_case --root "$APK_ROOT" > /tmp/run.txt
# lf in=2980 z=(1,1,1) out=3263
```

实测：长度与抓包完全一致；body 尾部有 336 字节的加密区块与抓包相同（AES
块对齐），但因为 nonce 与真机的实际耗时不同，字节不会逐位相等。

端到端实测（2026-10-30）：同一配置向 `https://dav.dewu.com/call` 发一次
`POST`（`cst` 取当前毫秒、`hk = sha256(body)`），HTTP 200，`c=10000`；
返回的 `k` 解出 92 字符 sk，与设备记录的值完全一致，`sm` 也相同。
只发一次，不重试。

把输出拼成 `/call` 的 body 时注意协议细节（与 `call_0` 一致）：

```bash
# 第 2 行 hex → 二进制 lf 输出
sed -n '2p' /tmp/run.txt | xxd -r -p > /tmp/lf_out.bin
# lf 输出 → base64url，保留 = padding：3263 字节 → 4352 字符
base64 -w0 /tmp/lf_out.bin | tr '+/' '-_' > /tmp/body.txt
wc -c /tmp/body.txt                    # 4352 == Content-Length
sha256sum /tmp/body.txt                # 就是 dsn 里的 hk（对 ASCII body 求哈希）
```

## 环境常量（可覆盖）

| 常量 | 默认值 | 覆盖 |
| --- | --- | --- |
| 包名 | `com.shizhuang.duapp` | — |
| APK 路径 | 画像 `dadir` 字段 | `SZSTONE_APK_PATH` |
| android_id | `7a1c2569212074a4` | `SZSTONE_ANDROID_ID` |
| 蓝牙地址 | `A4:50:46:9D:44:1E` | `SZSTONE_BT_ADDRESS` |
| 证书大小写 | 大写（Android `HEX_DIGITS`） | `SZSTONE_CERT_LOWER` |
| 实例 pid | 1000 | `SZSTONE_PID` |

## 系统属性

`szstone.properties` 只放 lf 通过 `__system_property_get` 问到的键（共 37 个）：

- 快照里有值的型号、品牌、fingerprint、SDK 32、ABI、build id 等；
- 少数设备标识键（`ro.product.device`、`ro.serialno`、`ro.sf.lcd_density` 等），
  保证属性画像与 `assets/root` 的 Mi 8 快照一致；
- 快照里本来就不存在的键（`ro.kernel.qemu`、`ro.boot.flash.locked` 等）故意
  不写，真机上它们也返回空。
- `gsm.version.baseband` 故意不写：`call_0` 对应的真机调用里它是空的，
  `prop.default` 里的 baseband 属于另一次抓取。写进去会多一个 16 字节块
  （挂 APK 后是 3279 而不是 3263）。

**不要直接挂全量 `assets/root/system/etc/prop.default`**：它有 1098 个键，
超过框架 128 KiB 的属性区（`property area is full`）。设备快照保留它是为了
可追溯，运行时只加载上面这份精简表。

## 依赖的 runtime 面

这个 case 用到了几项在它接入时才补的平台能力（与目标无关）：

- `/proc` 快照优先：`filesystem_roots` 里有 `/proc/...` 就先用快照，没有再用
  合成器；`/proc/<本实例 pid>/x` 与 `thread-local` 映射到快照的 `self/x`；
  `/proc/self/mem` 是可打开的空文件。
- CFI shadow：虚拟 linker 在加载 libdl 后分配 shadow 表并调用 `__cfi_init`。
  否则 libdl 的 `__cfi_slowpath` 会从地址 0 取表项，任何经过 CFI 检查的间接
  调用都会崩（`guest read unmapped address=0x3006`）。

## 诊断

```bash
DUCKEMU_IO_TRACE=1     # open/read/pread/write/close/lseek 的路径与字节数
DUCKEMU_LOAD_TRACE=1   # 模块加载基址与 span
DUCKEMU_VFS_TRACE=1    # guest 路径解析结果
SZSTONE_DEBUG=1        # 每个 Java shim 的调用
```

## worker

`make_app_plugin({app="szstone", library=..., rom_libc=..., ...})`，
`init` 装环境并加载 libc/so；`invoke` 的 `op="lf"`，参数
`blob`（hex，必填）、`z`（如 `0,1,0`），结果 `body_hex` / `body_len`。

worker 除了库路径还要把设备快照和属性画像传进去：

```bash
duckemu_worker --supervisor --ipc-fd N \
  --app szstone \
  --library code/user_code/szstone_case/assets/libszstone.so \
  --rom-libc ROM/.../bootstrap/libc.so \
  --library-path ROM/.../bootstrap \
  --library-path ROM/.../lib64 \
  --filesystem-root code/user_code/szstone_case/assets/root \
  --property-file code/user_code/szstone_case/szstone.properties
```

## gate

`07f_szstone_plugin`：两个全新实例在相同时钟/随机种子下各调用一次 `lf`，
断言首次 body 非空、长度 3183、`JniEvent=0`，且两次 body 逐字节相同。

```bash
ctest --test-dir build -R 07f_szstone_plugin --output-on-failure
```
