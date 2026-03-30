#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x02f9bbf0, "timer_init_key" },
	{ 0x058c185a, "jiffies" },
	{ 0x32feeafc, "mod_timer" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x0bc5fb0d, "unregister_chrdev_region" },
	{ 0x1595e410, "device_destroy" },
	{ 0xa1dacb42, "class_destroy" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfaabfe5e, "kmalloc_caches" },
	{ 0xc064623f, "__kmalloc_cache_noprof" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x2352b148, "timer_delete_sync" },
	{ 0x4e54d6ac, "cdev_del" },
	{ 0xd272d446, "__rcu_read_lock" },
	{ 0xc13edbcb, "find_vpid" },
	{ 0x848a0d8d, "pid_task" },
	{ 0xd272d446, "__rcu_read_unlock" },
	{ 0xbf2c538b, "get_task_mm" },
	{ 0xcf46e6bd, "mmput" },
	{ 0x1cf09ab5, "__put_task_struct_rcu_cb" },
	{ 0xb9fcd065, "call_rcu" },
	{ 0x2520ea93, "refcount_warn_saturate" },
	{ 0x8ffd462b, "send_sig" },
	{ 0xd272d446, "__fentry__" },
	{ 0x9f222e1e, "alloc_chrdev_region" },
	{ 0x653aa194, "class_create" },
	{ 0xe486c4b7, "device_create" },
	{ 0xd5f66efd, "cdev_init" },
	{ 0x8ea73856, "cdev_add" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x02f9bbf0,
	0x058c185a,
	0x32feeafc,
	0xe8213e80,
	0xd272d446,
	0x0bc5fb0d,
	0x1595e410,
	0xa1dacb42,
	0xbd03ed67,
	0x092a35a2,
	0xf46d5bf3,
	0xcb8b6ec6,
	0xf46d5bf3,
	0xbd03ed67,
	0xfaabfe5e,
	0xc064623f,
	0xd272d446,
	0x2352b148,
	0x4e54d6ac,
	0xd272d446,
	0xc13edbcb,
	0x848a0d8d,
	0xd272d446,
	0xbf2c538b,
	0xcf46e6bd,
	0x1cf09ab5,
	0xb9fcd065,
	0x2520ea93,
	0x8ffd462b,
	0xd272d446,
	0x9f222e1e,
	0x653aa194,
	0xe486c4b7,
	0xd5f66efd,
	0x8ea73856,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"timer_init_key\0"
	"jiffies\0"
	"mod_timer\0"
	"_printk\0"
	"__x86_return_thunk\0"
	"unregister_chrdev_region\0"
	"device_destroy\0"
	"class_destroy\0"
	"__ref_stack_chk_guard\0"
	"_copy_from_user\0"
	"mutex_lock\0"
	"kfree\0"
	"mutex_unlock\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"__stack_chk_fail\0"
	"timer_delete_sync\0"
	"cdev_del\0"
	"__rcu_read_lock\0"
	"find_vpid\0"
	"pid_task\0"
	"__rcu_read_unlock\0"
	"get_task_mm\0"
	"mmput\0"
	"__put_task_struct_rcu_cb\0"
	"call_rcu\0"
	"refcount_warn_saturate\0"
	"send_sig\0"
	"__fentry__\0"
	"alloc_chrdev_region\0"
	"class_create\0"
	"device_create\0"
	"cdev_init\0"
	"cdev_add\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "BCB22994EBA9869B5DC77B2");
