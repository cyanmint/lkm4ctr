# lkm4ctr_checker

`lkm4ctr_checker` 是普通用户空间诊断程序，不是内核模块，也不创建 `/dev` 节点。它实际执行 `unshare(2)`、`fork(2)`、`setns(2)`、mqueue、SysV IPC 和 `mount(2)`，报告：

- `PASS`：观察到真实、可验证的隔离或功能效果；
- `STUB`：调用成功，但只有 bookkeeping，没有真实隔离；
- `FAIL`：系统调用失败，例如 `EINVAL`、`ENOSYS` 或 `EPERM`。

用法：

```sh
./lkm4ctr_checker
./lkm4ctr_checker -q
echo $?       # 0 表示没有 FAIL，1 表示至少一个 FAIL
```

请以 root 或 `CAP_SYS_ADMIN` 运行。非特权运行得到 `EPERM` 是预期现象。建议在 `insmod` 前后各运行一次并比较结果。

程序可独立构建，不依赖内核头文件或 KDIR：

```sh
make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
```

完整测试方法和结果含义见英文版 [`../../en/developer/CHECKER.md`](../../en/developer/CHECKER.md)。
