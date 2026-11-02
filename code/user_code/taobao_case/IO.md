# 淘宝案例 I/O 备忘

> 2026-11-02。只记“跑 IO 时哪些现象正常、哪些是坑、怎么查”。
> 设备面与命令链见 `taobao_device.cpp` / `taobao_case.cpp`。

## 1. 锁文件：`EEXIST` 是锁协议的一部分，不是错误

SDK 每次受锁保护地读状态文件，都是这个序列（`DUCKEMU_IO_TRACE` 实测）：

```text
openat(lock, 0xc2 = O_CREAT|O_EXCL|O_RDWR)  -> 第一次成功；之后 -17
openat(lock, 0x42 = O_CREAT|O_RDWR)         -> 成功
读状态文件，close
```

`0xc2` 失败后的 `0x42` 是“锁已存在，按已有文件打开”的正常分支，**没有重试、
没有等待**。一次 10103 里 `sgFile.lock` 24 次、`rt_storage.lock` 37 次 `-17`，
全是这个分支，不要为了清零去改 `O_EXCL` 语义。

`main.cpp` 的 `StateRoot::create` 仍然在**每次启动**删掉残留锁：

- `data/user/0/com.taobao.taobao/files/*.lock`
- `data/user/0/com.taobao.taobao/app_SGLib/app_*/lock.lock`

原因只是 runner 从宿主拆进程，SDK 自己没机会走清理；删掉能让当次运行的第一次
获取走成功分支。**它不影响耗时**——清掉后 `-17` 只从 104 降到 61，10103 时长不变。

## 2. 首次 seed 要删的三个状态文件

`app_SGLib/SG_FAST_CONFIG`、`app_SGLib/.sf/.uf_wk`、`app_SGLib/.sf/.uf_sw`
会把 SDK 从“正常 ZIP 中央目录解析”（`base.apk` 约 16 次 open）推到从文件尾
每次退 1 KiB 的反向扫描（90 MB APK，约 9 万次）。只在 `first_run` 删除即可：
后续运行没有它们也不会影响 10101。

## 3. 这些 ENOENT 是探测，不是错误

`TAOBAO_SVC_DEBUG=1` 只打印失败的 openat/faccessat/newfstatat/readlinkat。
下面这些失败是 SDK 在探测设备环境，**不要**为了清零去预建文件/目录：

```text
/proc/mounts  /proc/net/unix  /proc/<pid>/{stat,wchan,task}
/dev/__properties__/u:object_r:*_prop:s0
/data/user/0/com.taobao.taobao/app_SGLib/lvm_*        # 缓存 miss，可接受
/data/user/0/com.taobao.taobao/app_SGLib/.ab394a9964/cfg/cr/<hash>/<n>
/data/user/0/com.taobao.taobao/app_SGLib/.<hash>      # 动态目录探测
```

真机同样会 miss；SDK 有回退链。要关注的是 **EROFS**（`-30`）：可写 `state/`
挂上后它应当是 0，出现就说明状态目录没挂对。

## 4. 怎么查

```sh
# 失败路径 + SDK 写到 fd 2 的内容
TAOBAO_SVC_DEBUG=1 ./build/user_code/duckemu_taobao_case --init

# 每一次 open/read/write/close/lseek（量大）
DUCKEMU_IO_TRACE=1 ./build/user_code/duckemu_taobao_case --init

# 按路径聚合失败
TAOBAO_SVC_DEBUG=1 ./build/user_code/duckemu_taobao_case --init 2>&1 |
  grep '^svc openat .*-> -' | sed 's/ -> .*//' | sort | uniq -c | sort -rn
```

`close` 偶发的 `-EBADF`（`-9`）是 bionic 重复 close 的正常现象，不处理。

## 5. 状态目录

- 首次运行从 `assets/seed/` 复制到 `state/`，之后一直复用。
- `TAOBAO_STATE_DIR` 可换目录；`state/` 已 gitignore。
- 想回到“全新安装”：删掉 `state/` 再跑 `--init`。
