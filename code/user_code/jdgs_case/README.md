# jdgs_case

京东 16.1.50（`com.jingdong.app.mall`，versionCode 102918）`libjdg.so` 的
`Bridge.main` 签名调用。移植自 `DuckDroid/code/user_code/jdgs_16`，行为、常量与
Java 环境一致；差别是按 DuckEmu 的写法组织：只声明目标真正需要的成员，宿主
方法用 shim，没有静态注册。

```text
jdgs_case/
├── jdgs_env.hpp          # installEnvironment + 参数/结果辅助函数
├── jdgs_env.cpp          # Bridge、Context、PackageManager、签名链、Integer、File
├── main.cpp              # 本地 runner：装环境 → 加载 libc/libjdg → init → sign
├── plugin.{hpp,cpp}      # worker 的 AppPlugin 适配（app id: jdgs16）
├── jdgs_cert.h           # APK 签名证书（META-INF/JINGDONG.RSA）
└── assets/
    ├── libjdg.so
    └── root/data/app/com.jingdong.app.mall-NMPcDqBzdYwQrAm-23cOyQ==/base.apk
```

## 运行

```bash
cmake --build build --target duckemu_jdgs_case
./build/user_code/duckemu_jdgs_case
```

成功时输出 git 请求头值（`b1`–`b7` 的 JSON），例如：

```text
{"b1":"68dd5803-...","b2":"3.6.5_0","b3":"3.0","b4":"...","b5":"503299c6...","b7":"1793...","b6":"..."}
```

- `b1`/`b2`/`b3`/`b5` 是确定性的；
- `b4`/`b6`/`b7` 含每次调用的时间/随机分量。注入墙钟后整个输出逐字节可复现：

  ```bash
  JDGS_WALL_TIME_MS=1792886400000 JDGS_REPEAT=2 ./build/user_code/duckemu_jdgs_case
  ```

  与 DuckDroid 参考 runner 在同一注入时钟下逐字节一致。

## 可注入项

| 环境变量 | 作用 | 默认 |
| --- | --- | --- |
| `JDGS_REPEAT` | 连续签名次数 | `1` |
| `JDGS_WALL_TIME_MS` | 注入墙钟（ms；不设置则跟随宿主） | 宿主墙钟 |
| `JDGS_MONOTONIC_NS` | 注入单调时钟（ns） | 宿主单调钟 |
| `JDGS_REQUEST` | `main(101)` 的请求串 | 内置 coral 样本 |
| `JDGS_DEVICE_PAYLOAD` | 设备载荷 | 内置 coral 载荷 |
| `JDGS_DFP_EID` | dfp eid | `""` |
| `JDGS_PAY_VERSION` | 支付版本 | `1.0` |
| `JDGS_OPENUDID` | openudid | 全 0 UUID |
| `JDGS_SDK_CONFIG` | SDK 配置 | `83` |
| `JDGS_BAD_CERT` | 用假证书验证签名链确实参与运算 | 未设置 |
| `JDGS_DEBUG` | 打印 init(103) 的阶段状态 | 未设置 |

时钟也可以写成 `--wall-time-ns` / `--monotonic-time-ns`。

## 初始化与签名

```text
Bridge.main(103, ["0"])
Bridge.main(103, ["1", null])
Bridge.main(101, [byte[] request, payload, dfp eid, "1.0", openudid, "83", null])
```

返回 `[Ljava/lang/Object;` 的第 0 个元素是签名字符串；若是
`java/lang/Integer`，表示 native 返回了错误码。

## 目标环境（jdgs_env.cpp）

- `Bridge`：`main`（native，由 `JNI_OnLoad` 绑定）、`getAppContext`、
  `getAppKey`、`getJDGVN`、`getPicName`、`getSecName`、`getBizNum`（null）。
  全部静态。
- `Context`：`getPackageName`、`getPackageCodePath`、`getPackageManager`、
  `getFilesDir`。
- `PackageManager.getPackageInfo`、`PackageInfo.versionName`/`versionCode`/
  `signingInfo`/`signatures`、`SigningInfo.getApkContentsSigners`、
  `Signature.toByteArray`、`CertificateFactory.getInstance`/
  `generateCertificate`、`Certificate.getEncoded`；证书内容内嵌在
  `jdgs_cert.h`。
- `java/lang/Integer.<init>(I)` 用实例字段保存状态；`java/io/File.getAbsolutePath`
  返回一个绝对路径（当前没有可写 guest home）。
- `java.lang.String` 的 `([BLjava/lang/String;)V` 构造器与
  `getBytes(Ljava/lang/String;)[B` 在平台包里，UTF-8 固定。

`FindClass("java/lang/string")` 这种大小写探针不声明即为失败，符合严格模式；
正常路径只应看到这一条 `missing-class` 事件。

## 精简 APK

没有搬 82 MiB 的完整 APK，只保留 native 真正读取的 9 个条目：

```text
assets/68dd5803-07c3-44d7-8881-0b213bf81b51.jdg.jpg
assets/68dd5803-07c3-44d7-8881-0b213bf81b51.jdg.xbt
assets/jdjdg-appkey.txt
assets/jdmp/jdg-evn-scnr.{bdex,edex,jdex,json}
assets/rootcert/secure_root.cer
META-INF/JINGDONG.RSA
```

压缩后约 106 KiB。与完整 APK 在同一注入时钟下签出的 `b1`/`b2`/`b3`/`b5`
相同；runner 通过 `filesystem_roots` 把它挂在
`/data/app/com.jingdong.app.mall-NMPcDqBzdYwQrAm-23cOyQ==/base.apk`。

APK 文件不入库（`.gitignore` 排除 `*.apk`）；新克隆的仓库里没有这个文件，
本地运行前需要把精简 APK 放到上面的路径。

## worker / AppPlugin

app id 是 `jdgs16`。除了库路径和 ROM libc，还要把项目 `assets/root` 作为
guest 文件系统根传给 worker：

```bash
duckemu_worker --supervisor --ipc-fd N \
  --app jdgs16 \
  --library code/user_code/jdgs_case/assets/libjdg.so \
  --rom-libc ROM/.../bootstrap/libc.so \
  --library-path ROM/.../bootstrap \
  --library-path ROM/.../lib64 \
  --filesystem-root code/user_code/jdgs_case/assets/root \
  --property-file code/user_code/jdgs_case/jdgs.properties
```

`jdgs.properties` 是这个包被验证过的设备画像（小米 8，SDK 32，来自
`adb shell getprop`，去掉 `cache_key.*` 与含换行的条目）。签名器在
`ro.build.version.sdk >= 33` 时会走另一条平台路径并返回 -4210，所以这个文件
不是可选项。

`libandroid.so` / `libmediandk.so` 只有 `DT_NEEDED` 依赖，签名路径不调用；
loader 把它们的导入符号绑定为返回 0 的桩，避免拉入框架的完整依赖链。

## 压测（System → Worker → RuntimeInstance）

`tools/sign_client/bench.py` 支持 `--app`，对 `jdgs16` 会把每个结果解析成
JSON 并检查 `b1`–`b7`，缺字段按错误计数。

```bash
cd /home/duck/NewCode/Android/environment/DuckDroid2
ENV=/home/duck/NewCode/Android/environment
ROM=$ENV/ROM/out/target/product/lemonade/symbols/system/lib64
CASE=$PWD/code/user_code/jdgs_case

# 终端 A：起 System（Ctrl-C 结束）
./build/duckemu_system --socket-dir /tmp/duckemu-jdgs \
  --worker-exec "$PWD/build/duckemu_worker" \
  --app "id=jdgs16,library=$CASE/assets/libjdg.so,rom-libc=$ROM/bootstrap/libc.so,library-path=$ROM/bootstrap:$ROM,instances=8,workers=1,filesystem-root=$CASE/assets/root,property-file=$CASE/jdgs.properties"

# 终端 B：8000 次签名，8 客户端，等 8 个实例预热完再计时
python3 code/tools/sign_client/bench.py /tmp/duckemu-jdgs/jdgs16.sock \
  --app jdgs16 --clients 8 --requests 8000 --instances 8
```

换参数用可重复的 `--param`（不传时用内置 coral 样本）：

```bash
python3 code/tools/sign_client/bench.py /tmp/duckemu-jdgs/jdgs16.sock \
  --app jdgs16 --clients 4 --requests 400 --instances 4 \
  --param sdk_config=83 --param pay_version=1.0
```

实测基线（20 核机型；一次签名约 100 ms guest CPU，单实例串行）：

| 实例 | 客户端 | 吞吐 | 平均延迟 | 说明 |
| --- | --- | --- | --- | --- |
| 2 | 2 | 23 req/s | 87 ms | 1:1 |
| 4 | 8 | 39 req/s | 201 ms | 每实例 2 客户端排队 |
| 8 | 8 | 57 req/s | 123 ms | 1:1，约 27 MB RSS/实例 |

吞吐 ≈ 实例数 / 单次签名时间；客户端多于实例只会抬高延迟，不提高吞吐。
提高并发就加 `instances`（每个实例是一个进程），压满 CPU 前基本线性。
`sdk_config` 等参数对耗时影响很小。
