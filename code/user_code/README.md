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
