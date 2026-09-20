# shadow_hook：共用 syscall hook ABI

`shadow_hook.h` 宣告 `struct shadow_hook`、`SHADOW_HOOK()` 以及安裝/移除函式，供 `vendor_kernel` 和合併模組使用。流程是解析目標符號、選擇 ftrace 或 kprobe、跳轉到替代函式，必要時再呼叫真實函式指標。

它只能重定向既有入口，不能補出目標 `vmlinux` 沒有的結構或完整子系統。UTS/PID/USER/IPC/mqueue 與 overlayfs 有實作；NET/MNT/CGROUP 有明確限制。不要與其他也 hook 相同 syscall 的模組同時使用。
