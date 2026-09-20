# shadow_hook：共享 syscall hook ABI

`shadow_hook.h` 声明 `struct shadow_hook`、`SHADOW_HOOK()` 以及安装/移除函数，供 `vendor_kernel` 和合并模块中的其他代码使用。

典型生命周期如下：

1. 用 kprobe 技巧解析目标符号地址；
2. 按编译期配置选择 ftrace 或 kprobe；
3. 将保存的程序计数器改写到替代函数；
4. 替代函数可调用保存的真实函数指针，或直接返回 vendored 实现的结果。

它只能重定向已有入口，不能凭空增加目标 `vmlinux` 中不存在的结构布局或完整子系统。当前 `vendor_kernel` 的 UTS/PID/USER/IPC/mqueue 路径与 overlayfs 有真实实现，但 NET/MNT/CGROUP 仍有明确限制。

只能加载一个 hook 到同一目标符号；不要与其他也 hook 相同 syscall 的模块叠加使用。
