# shadow_hijack：hook 引擎實作

`shadow_hijack.c` 與 `shadow_hook.h` 組成 `lkm4ctr.ko` 的共用 hook 引擎。它用 `register_kprobe()` 解析核心符號，依目標核心設定選擇 ftrace `IPMODIFY` 或 kprobe 後端，把系統呼叫入口轉到模組實作。

每個 hook 保存候選符號名稱、替代函式、原函式指標和所屬模組。遞迴保護允許替代函式安全呼叫原始實作；正在執行的重定向呼叫會持有模組參照，避免執行中卸載。

Android GKI 裝置常關閉 function tracer，因此生產裝置通常使用 kprobe 後端。同一目標符號同一時間只能有一個 shadow hook。
