# shadow_hijack：hook 引擎实现

`shadow_hijack.c` 与 `shadow_hook.h` 组成 `lkm4ctr.ko` 的共享 hook 引擎。它通过 `register_kprobe()` 解析内核符号，并根据目标内核配置选择 ftrace `IPMODIFY` 或 kprobe 后端，把系统调用入口转到模块实现。

每个 hook 保存候选符号名、替代函数、原函数指针和所属模块。递归保护允许替代函数安全地调用原始实现；正在执行的重定向调用会持有模块引用，避免代码在执行中被卸载。

Android GKI 设备常关闭 function tracer，因此生产设备通常使用 kprobe 后端。一个目标符号同一时间只能有一个兼容的 shadow hook。

这是实现说明，不是用户安装步骤；用户操作请先看 [`../USER_GUIDE.md`](../USER_GUIDE.md)。
