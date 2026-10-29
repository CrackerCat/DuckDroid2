# 第三方依赖与许可

DuckDroid2 本体以 GPL-2.0-only 发布，完整文本见 [LICENSE](LICENSE)。
下面是随仓库或构建一起使用的第三方组件。

## Unicorn Engine

- 路径：`code/third_party/unicorn`
- 许可：GPL-2.0-only（完整文本见 `code/third_party/unicorn/COPYING`）
- 用途：默认的 ARM64 执行后端，静态链接进 `duckemu_runtime`
- 说明：分发包含 Unicorn 的二进制时，整体按 GPLv2 处理

## nlohmann/json

- 许可：MIT
- 用途：M7 的 UDS 消息编解码；通过系统包引入，不随仓库分发
- 说明：MIT 与 GPLv2 兼容

## Dynarmic（可选，未默认启用）

- 路径：`code/third_party/dynarmic`
- 许可：0BSD-style permissive（见 `code/third_party/dynarmic/LICENSE.txt`）
- 用途：规划中的快速后端；当前构建不包含

其他系统库（pthread、libstdc++ 等）按其各自许可使用，不随仓库分发。
