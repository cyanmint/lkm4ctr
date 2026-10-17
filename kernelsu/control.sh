#!/system/bin/sh
# KernelSU module control entry point for lkm4ctr.

MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
STATE_DIR=${LKM4CTR_STATE_DIR:-/data/adb/lkm4ctr}
AUTOLOAD_FILE=$STATE_DIR/auto_load
BUSYBOX=${LKM4CTR_BUSYBOX:-/data/adb/ksu/bin/busybox}

resolve_kmi() {
    release=$1
    kernel_series=$(printf '%s\n' "$release" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\)\..*/\1/p')
    android_version=$(printf '%s\n' "$release" | sed -n 's/.*android\([0-9][0-9]*\).*/\1/p')
    if [ -z "$kernel_series" ] || [ -z "$android_version" ]; then
        echo "Cannot determine Android KMI from kernel release: $release" >&2
        return 1
    fi

    kmi=android${android_version}-${kernel_series}
    if ! grep -Fqx "$kmi" "$MODDIR/supported_kmis.txt"; then
        echo "Unsupported or ambiguous Android KMI: $kmi (kernel release: $release)" >&2
        return 1
    fi
    printf '%s\n' "$kmi"
}

module_is_loaded() {
    [ -x "$BUSYBOX" ] || return 2
    modules=$("$BUSYBOX" lsmod 2>/dev/null) || return 2
    if printf '%s\n' "$modules" | grep -q '^lkm4ctr[[:space:]]'; then
        return 0
    else
        result=$?
        [ "$result" -eq 1 ] && return 1
        return 2
    fi
}

report_lsmod_error() {
    if [ ! -x "$BUSYBOX" ]; then
        echo "KernelSU BusyBox not found: $BUSYBOX" >&2
    else
        echo "Cannot determine lkm4ctr state using lsmod" >&2
    fi
}

load_module() {
    if module_is_loaded; then
        echo "lkm4ctr is already loaded"
        return 0
    else
        state_result=$?
        if [ "$state_result" -ne 1 ]; then
            report_lsmod_error
            return 1
        fi
    fi

    kmi=$(resolve_kmi "$(uname -r)") || return 1
    module=$MODDIR/kmi/lkm4ctr-${kmi}-arm64.ko
    if [ ! -s "$module" ]; then
        echo "Matching module file is missing: $module" >&2
        return 1
    fi
    if [ ! -x "$BUSYBOX" ]; then
        echo "KernelSU BusyBox not found: $BUSYBOX" >&2
        return 1
    fi
    "$BUSYBOX" insmod "$module" || {
        echo "insmod failed for $kmi; check kernel log and module-signature policy" >&2
        return 1
    }
    echo "Loaded lkm4ctr for $kmi"
}

unload_module() {
    if module_is_loaded; then
        :
    else
        state_result=$?
        if [ "$state_result" -eq 1 ]; then
            echo "lkm4ctr is not loaded"
            return 0
        fi
        report_lsmod_error
        return 1
    fi
    if [ ! -x "$BUSYBOX" ]; then
        echo "KernelSU BusyBox not found: $BUSYBOX" >&2
        return 1
    fi
    "$BUSYBOX" rmmod lkm4ctr || {
        echo "rmmod failed; stop users of lkm4ctr and retry" >&2
        return 1
    }
    echo "Unloaded lkm4ctr"
}

run_checker() {
    checker=$MODDIR/bin/lkm4ctr_checker
    if [ ! -x "$checker" ]; then
        echo "lkm4ctr_checker is missing or not executable: $checker" >&2
        return 1
    fi
    "$checker"
}

read_autoload() {
    if [ ! -f "$AUTOLOAD_FILE" ]; then
        printf '1\n'
        return 0
    fi
    value=$(cat "$AUTOLOAD_FILE") || return 1
    case "$value" in
        1|0) printf '%s\n' "$value" ;;
        *) echo "Invalid auto-load setting in $AUTOLOAD_FILE" >&2; return 1 ;;
    esac
}

set_autoload() {
    value=$1
    case "$value" in
        1|0) ;;
        *) echo "Invalid auto-load value: $value" >&2; return 2 ;;
    esac
    mkdir -p "$STATE_DIR" || return 1
    temporary=$AUTOLOAD_FILE.$$
    if ! printf '%s\n' "$value" > "$temporary"; then
        rm -f "$temporary"
        return 1
    fi
    mv "$temporary" "$AUTOLOAD_FILE" || {
        rm -f "$temporary"
        return 1
    }
    if [ "$value" = 1 ]; then
        echo "Boot auto-load enabled"
    else
        echo "Boot auto-load disabled"
    fi
}

toggle_load() {
    module_is_loaded
    state_result=$?
    case "$state_result" in
        0) unload_module ;;
        1) load_module ;;
        *) report_lsmod_error; return 1 ;;
    esac
}

toggle_autoload() {
    current=$(read_autoload) || return 1
    if [ "$current" = 1 ]; then
        set_autoload 0
    else
        set_autoload 1
    fi
}

show_status() {
    module_is_loaded
    state_result=$?
    case "$state_result" in
        0) state=loaded ;;
        1) state=unloaded ;;
        *) state=unknown ;;
    esac
    autoload=$(read_autoload) || return 1
    if [ "$autoload" = 1 ]; then autoload_state=enabled; else autoload_state=disabled; fi
    kmi=$(resolve_kmi "$(uname -r)" 2>/dev/null) || kmi=unsupported
    printf 'STATE=%s\nAUTOLOAD=%s\nKMI=%s\n' "$state" "$autoload_state" "$kmi"
}

command_name=${1:-status}
case "$command_name" in
    detect-kmi)
        if [ "$#" -gt 1 ]; then release=$2; else release=$(uname -r); fi
        resolve_kmi "$release"
        ;;
    load|insmod)
        load_module
        ;;
    unload|rmmod)
        unload_module
        ;;
    toggle-load)
        toggle_load
        ;;
    enable-autoload)
        set_autoload 1
        ;;
    disable-autoload)
        set_autoload 0
        ;;
    toggle-autoload)
        toggle_autoload
        ;;
    boot)
        autoload=$(read_autoload) || exit 1
        if [ "$autoload" = 1 ]; then load_module; else echo "Boot auto-load is disabled"; fi
        ;;
    checker)
        run_checker
        ;;
    status)
        show_status
        ;;
    *)
        echo "Usage: $0 {status|load|unload|toggle-load|enable-autoload|disable-autoload|toggle-autoload|boot|checker|detect-kmi [release]}" >&2
        exit 2
        ;;
esac
