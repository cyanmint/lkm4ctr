// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>

#include "lkm4ctr_log.h"

int shadow_hijack_init(void);
void shadow_hijack_exit(void);
int vendor_kernel_init(void);
void vendor_kernel_exit(void);
int lkm4ctr_diagfs_init(void);
void lkm4ctr_diagfs_exit(void);
int lkm4ctr_hotreload_init(void);
void lkm4ctr_hotreload_exit(void);

#define LKM4CTR_VERSION "4.0.1"
#define LKM4CTR_TAG	"lkm4ctr"

/*
 * vendor_kernel now auto-loads at insmod time alongside shadow_hijack's
 * shared hook-engine bookkeeping. lkm4ctr_diagfs_init() still only registers
 * the "lkm4ctr" filesystem type, so diagfs remains the runtime control path
 * for manual unload/reload of vendor_kernel via ./mnt/control ("echo load")
 * or the equivalent global alias.
 * See lkm4ctr_diagfs.c for the full control surface.
 */
static int __init lkm4ctr_init(void)
{
	int ret;

	ret = shadow_hijack_init();
	if (ret)
		return ret;

	ret = vendor_kernel_init();
	if (ret) {
		shadow_hijack_exit();
		return ret;
	}

	ret = lkm4ctr_diagfs_init();
	if (ret) {
		LKM4CTR_WARN(LKM4CTR_TAG,
			     "diagfs registration failed: %d (mount -t lkm4ctr, including control/status and load/unload control, will be unavailable)",
			     ret);
	}

	lkm4ctr_hotreload_init();

	LKM4CTR_INFO(LKM4CTR_TAG,
		     "loaded unified module (vendor_kernel auto-started; mount -t lkm4ctr diag <mountpoint> to inspect it)");
	return 0;
}

/*
 * lkm4ctr_exit() unconditionally calls every subsystem's own _exit(), which
 * is idempotent-safe and force-frees all of its resources even if it was
 * never loaded (shadow_hook_remove_all() on hooks that were never installed
 * is a no-op; vendor_kernel's own resource-registry teardown is likewise a
 * no-op on an already-empty registry) or was already manually unloaded via
 * diagfs. This is what guarantees rmmod is never blocked by that state:
 * whatever the diagfs left active is force-cleaned up right here,
 * unconditionally, on the way out.
 */
static void __exit lkm4ctr_exit(void)
{
	lkm4ctr_diagfs_exit();
	lkm4ctr_hotreload_exit();
	vendor_kernel_exit();
	shadow_hijack_exit();
	LKM4CTR_INFO(LKM4CTR_TAG, "unloaded unified module");
}

module_init(lkm4ctr_init);
module_exit(lkm4ctr_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
/*
 * lkm4ctr statically links vendored kernel-common code (see
 * ../README.md): fs/overlayfs (Miklos Szeredi <miklos@szeredi.hu>,
 * credited again by its own MODULE_AUTHOR() in each
 * ../fs/overlayfs variant's super.c) plus ipc/ and kernel
 * namespace files
 * (originally by Krishna Balasubramanian, Manfred Spraul, Krzysztof
 * Benedyczak, Jakub Jelinek, David S. Miller, Eric Schenk, Bruno Haible,
 * Arun Sharma, and the wider upstream Linux kernel community -- see each
 * vendored file's own header comment for the exact original copyright).
 * This MODULE_AUTHOR() credits those vendored subsystems as a whole; it is
 * additive to (not a replacement for) the per-file copyright headers and
 * fs/overlayfs's own MODULE_AUTHOR().
 */
MODULE_AUTHOR("Linux kernel contributors (vendored ipc/, kernel/*_namespace.c, kernel/nsproxy.c, fs/overlayfs; see AGENTS.md and ../README.md)");
MODULE_AUTHOR("cyanmint <https://github.com/cyanmint>");
MODULE_AUTHOR("GitHub Copilot <https://github.com/copilot>");
MODULE_DESCRIPTION("Unified lkm4ctr module: shared hook engine plus vendored namespace/IPC/mqueue/overlayfs compatibility");
MODULE_VERSION(LKM4CTR_VERSION);

