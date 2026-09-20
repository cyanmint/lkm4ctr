# lkm4ctr_checker

`lkm4ctr_checker` 是普通使用者空間診斷程式，不是核心模組，也不建立 `/dev` 節點。它實際執行 `unshare(2)`、`fork(2)`、`setns(2)`、mqueue、SysV IPC 和 `mount(2)`，回報 `PASS`（真實效果）、`STUB`（只有 bookkeeping）或 `FAIL`（系統呼叫失敗）。

```sh
./lkm4ctr_checker
./lkm4ctr_checker -q
echo $?       # 0 表示沒有 FAIL，1 表示至少一個 FAIL
```

請以 root 或 `CAP_SYS_ADMIN` 執行；非特權執行得到 `EPERM` 是預期現象。載入模組前後各執行一次有助於比較。它可獨立建置：

```sh
make -C lkm4ctr_checker CC=aarch64-linux-gnu-gcc
```

完整測試方法見英文版 [`../../en/developer/CHECKER.md`](../../en/developer/CHECKER.md)。
