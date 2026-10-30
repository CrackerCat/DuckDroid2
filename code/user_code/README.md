# code/user_code

每个子目录是一个**目标项目**（一个 so / 一个 app 的 Java 环境补充与调用方）。

- 补环境的完整规则、格式模板、每个文件的职责、补的顺序：
  **[ENVIRONMENT.md](ENVIRONMENT.md)**
- 平台级 Java 环境在 `src/runtime/jni/envs/`，目标专属内容只放在这里。
- runtime 不认识任何具体目标；项目通过 `main.cpp` 显式调用
  `RuntimeInstance::installJavaEnvironment` 接入，没有静态注册。

## 现有项目

- [`libeleven_case/`](libeleven_case/)：`libeleven.so` 的
  `ChallengeElevenFragment.data(I)Ljava/lang/String;` 声明与调用。

  ```bash
  cmake --build build --target duckemu_libeleven_case
  ./build/user_code/duckemu_libeleven_case 1 5
  # 确定性回放：注入墙钟，之后每次读 +1 ms
  ./build/user_code/duckemu_libeleven_case --wall-time-ns 1792886400000000000 1 5
  ```

- [`jdgs_case/`](jdgs_case/)：京东 16.1.50 `libjdg.so` 的
  `Bridge.main(I[Ljava/lang/Object;)[Ljava/lang/Object;` 签名调用；带一个只保留
  9 个条目的精简 APK，app id `jdgs16`。

  ```bash
  cmake --build build --target duckemu_jdgs_case
  ./build/user_code/duckemu_jdgs_case
  # 与 DuckDroid 参考 runner 逐字节一致的回放
  JDGS_WALL_TIME_MS=1792886400000 JDGS_REPEAT=2 ./build/user_code/duckemu_jdgs_case
  ```

- [`szstone_case/`](szstone_case/)：得物 6.1.5.10 `libszstone.so` 的
  `SzSdk.lf([BZZZ)[B` 调用；设备快照（`/proc`、build.prop）作为 guest 根，
  app id `szstone`。

  ```bash
  cmake --build build --target duckemu_szstone_case
  ./build/user_code/duckemu_szstone_case
  # 注入时钟 + 随机种子后输出逐字节可复现
  ./build/user_code/duckemu_szstone_case --random-seed 7 \
    --wall-time-ns 1792886400000000000
  ```
