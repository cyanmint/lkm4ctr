#!/system/bin/sh
# Public Domain since creation (AI-generated; no copyright subsists; not CC0).
# KernelSU sources this after extracting the module during installation.
if [ -z "$MODPATH" ] || [ ! -d "$MODPATH" ]; then
    abort "Invalid module path: $MODPATH"
fi

chmod -R a+x "$MODPATH" || abort "Failed to make lkm4ctr module files executable"
