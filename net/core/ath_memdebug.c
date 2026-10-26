/*
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: ISC
 */

#include <linux/export.h>
#include <linux/mempool.h>
#include <linux/dmapool.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/percpu.h>
#include <linux/io.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <net/netlink.h>

#define FUNC_OBJ_ARR_SIZE 500
#define BUF_SIZE 10192

char athmem_stats_buf[BUF_SIZE];
unsigned int athmem_stats_num_nodes;
unsigned long long athmem_stats_dup_ptr;

bool athmem_flag_stop_tracking;
bool athmem_flag_track_only_txskb;
bool athmem_flag_print_only_fn_adds;
bool athmem_flag_limit_op;
bool athmem_flag_init_done;

static struct rb_root athmem_obj_tree_root = RB_ROOT;
spinlock_t athmem_spinlock;

struct athmem_func_obj {
	const char *func;
	int line;
	int count;
	unsigned long long size;
	unsigned long long actual_size;
};

struct athmem_debug_object {
	struct rb_node rb_node;
	unsigned long long pointer;
	size_t size;
	int line;
	size_t actual_size;
	const char *func;
};

struct athmem_func_obj athmem_func_obj_arr[FUNC_OBJ_ARR_SIZE];
int athmem_func_obj_arr_len;

static struct athmem_debug_object *lookup_object(unsigned long ptr)
{
	struct rb_node *rb = athmem_obj_tree_root.rb_node;
	struct athmem_debug_object *object;

	while (rb) {
		object = rb_entry(rb, struct athmem_debug_object, rb_node);
		if (ptr < object->pointer)
			rb = object->rb_node.rb_left;
		else if (ptr > object->pointer)
			rb = object->rb_node.rb_right;
		else if (object->pointer == ptr)
			return object;
	}
	return NULL;
}

static void athmem_remove_object(struct athmem_debug_object *object)
{
	rb_erase(&object->rb_node, &athmem_obj_tree_root);
}

static struct athmem_debug_object *athmem_find_and_remove_obj(unsigned long ptr)
{
	unsigned long flags;
	struct athmem_debug_object *object;

	spin_lock_irqsave(&athmem_spinlock, flags);
	object = lookup_object(ptr);
	if (object) {
		athmem_stats_num_nodes--;
		athmem_remove_object(object);
	}
	spin_unlock_irqrestore(&athmem_spinlock, flags);

	return object;
}

static void athmem_delete_obj_full(unsigned long ptr)
{
	struct athmem_debug_object *object;

	object = athmem_find_and_remove_obj(ptr);
	if (!object)
		return;
	kfree(object);
}

static void athmem_debug_print_rbtree(void)
{
	struct rb_node *node;
	unsigned long flags;
	struct athmem_debug_object *obj;

	pr_info("rb tree :\n");
	spin_lock_irqsave(&athmem_spinlock, flags);
	for (node = rb_first(&athmem_obj_tree_root); node; node = rb_next(node)) {
		obj = rb_entry(node, struct athmem_debug_object, rb_node);
		pr_info("func : %s line : %d addr : %llx\n", (char *)obj->func, obj->line,
			obj->pointer);
	}
	spin_unlock_irqrestore(&athmem_spinlock, flags);
}

static inline void athmem_remove_dup(struct rb_node *node)
{
	int i;
	struct athmem_debug_object *obj = rb_entry(node,
						   struct athmem_debug_object,
						   rb_node);

	for (i = 0; i < athmem_func_obj_arr_len; i++) {
		if (obj->func == athmem_func_obj_arr[i].func &&
		    obj->line == athmem_func_obj_arr[i].line) {
			athmem_func_obj_arr[i].size +=  obj->size;
			athmem_func_obj_arr[i].actual_size += obj->actual_size;
			athmem_func_obj_arr[i].count++;
			return;
		}
	}

	if (athmem_func_obj_arr_len < FUNC_OBJ_ARR_SIZE) {
		athmem_func_obj_arr[i].func =  obj->func;
		athmem_func_obj_arr[i].line =  obj->line;
		athmem_func_obj_arr[i].size =  obj->size;
		athmem_func_obj_arr[i].count = 1;
		athmem_func_obj_arr[i].actual_size = obj->actual_size;
		athmem_func_obj_arr_len++;
	} else {
		pr_err("functions stats buffer is full\n");
	}
}

static void athmem_sort_func_obj_arr(void)
{
	struct athmem_func_obj temp;
	int i;
	int j;

	for (i = 0 ; i < athmem_func_obj_arr_len; i++) {
		for (j = i + 1; j < athmem_func_obj_arr_len; j++) {
			if (athmem_func_obj_arr[i].size < athmem_func_obj_arr[j].size) {
				temp = athmem_func_obj_arr[i];
				athmem_func_obj_arr[i] = athmem_func_obj_arr[j];
				athmem_func_obj_arr[j] = temp;
			}
		}
	}
}

static void athmem_reset_func_obj(void)
{
	memset(athmem_func_obj_arr, 0, sizeof(athmem_func_obj_arr));
	athmem_func_obj_arr_len = 0;
}

static int athmem_copy_memory_stats(int is_panic)
{
	struct rb_node *node;
	int i;
	unsigned long flags;
	unsigned long long total_size = 0;
	int len = 0;
	unsigned long long rb_nodes_count;
	char *buf = athmem_stats_buf;
	int buf_len = BUF_SIZE;

	if (is_panic)
		athmem_flag_stop_tracking = 1;

	spin_lock_irqsave(&athmem_spinlock, flags);
	len += scnprintf(buf + len, buf_len - len, "node count :  %d\n", athmem_stats_num_nodes);

	for (node = rb_first(&athmem_obj_tree_root); node; node = rb_next(node))
		athmem_remove_dup(node);

	rb_nodes_count = athmem_stats_num_nodes;
	spin_unlock_irqrestore(&athmem_spinlock, flags);

	athmem_sort_func_obj_arr();

	for (i = 0; i < athmem_func_obj_arr_len; i++) {
		if (!athmem_flag_limit_op || i < 10) {
			if (!athmem_flag_print_only_fn_adds) {
				len += scnprintf(buf + len, buf_len - len,
						 "func : %s (%p) \t line : %d count : %d requested %llu actual %llu\n",
						 (char *)athmem_func_obj_arr[i].func,
						 (char *)athmem_func_obj_arr[i].func,
						 athmem_func_obj_arr[i].line,
						 athmem_func_obj_arr[i].count,
						 athmem_func_obj_arr[i].size,
						 athmem_func_obj_arr[i].actual_size);
			} else {
				len += scnprintf(buf + len, buf_len - len,
						 "func : %p \t line : %d count : %d requested %llu actual %llu\n",
						 (char *)athmem_func_obj_arr[i].func,
						 athmem_func_obj_arr[i].line,
						 athmem_func_obj_arr[i].count,
						 athmem_func_obj_arr[i].size,
						 athmem_func_obj_arr[i].actual_size);
			}
		}
		total_size += athmem_func_obj_arr[i].size;
	}

	len += scnprintf(buf + len, buf_len - len, "total allocated mem(bytes) : %llu\n",
			 total_size);
	len += scnprintf(buf + len, buf_len - len, "memory size of rb tree : %llu\n",
			 rb_nodes_count * sizeof(struct athmem_debug_object));
	len += scnprintf(buf + len, buf_len - len, "dup ptr count %llu\n",
			 athmem_stats_dup_ptr);

	athmem_reset_func_obj();

	return len;
}

static void athmem_delete_rb_tree(void)
{
	struct rb_node *node;
	unsigned long flags;

	pr_info("deleting rb tree\n");
	spin_lock_irqsave(&athmem_spinlock, flags);

	for (node = rb_first(&athmem_obj_tree_root); node; node = rb_next(node))
		rb_erase(node, &athmem_obj_tree_root);

	athmem_stats_num_nodes = 0;
	spin_unlock_irqrestore(&athmem_spinlock, flags);
}

static ssize_t athmem_debug_read(struct file *file,
				 char __user *user_buf,
				 size_t count, loff_t *ppos)
{
	size_t len;
	int ret;
	char *buf = athmem_stats_buf;

	len = athmem_copy_memory_stats(0);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, len);
	return ret;
}

static void athmem_debug_help(void)
{
	pr_info("ATHMEMDEBUG: ------ HELP ------\n"
		"0:\t print help\n"
		"1:\t stop memory tracking\n"
		"2:\t resume memory tracking\n"
		"3:\t print node_count in rb_tree\n"
		"4:\t enable flag to print only function name address\n"
		"5:\t disable flag to print only function name address\n"
		"6:\t limit stats output\n"
		"7:\t enable track only TX SKB\n"
		"8:\t disable track only TX SKB\n"
		"9:\t delete rb tree\n"
		"10:\t print rb tree\n");
}

static ssize_t athmem_debug_write(struct file *file,
				  const char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	u8 type = 0;
	int ret;

	ret = kstrtou8_from_user(user_buf, count, 0, &type);
	if (ret) {
		pr_err("no input received\n");
		return ret;
	}

	switch (type) {
	case 0:
		athmem_debug_help();
		break;
	case 1:
		athmem_flag_stop_tracking = 1;
		pr_info("athmem: stopped tracking\n");
		break;
	case 2:
		athmem_flag_stop_tracking = 0;
		pr_info("athmem: resume tracking\n");
		break;
	case 3:
		pr_info("athmem: node count: %d\n", athmem_stats_num_nodes);
		break;
	case 4:
		athmem_flag_print_only_fn_adds = 1;
		pr_info("athmem: athmem_flag_print_only_fn_adds enabled\n");
		break;
	case 5:
		athmem_flag_print_only_fn_adds = 0;
		pr_info("athmem: athmem_flag_print_only_fn_adds disabled\n");
		break;
	case 6:
		athmem_flag_limit_op = 1;
		pr_info("athmem: mem stats output is limited\n");
		break;
	case 7:
		athmem_flag_track_only_txskb = 1;
		pr_info("athmem: track only TX SKB enabled\n");
		break;
	case 8:
		athmem_flag_track_only_txskb = 0;
		pr_info("athmem: track only TX SKB disabled\n");
		break;
	case 9:
		athmem_delete_rb_tree();
		break;
	case 10:
		athmem_debug_print_rbtree();
		break;
	default:
		pr_info("athmem: unknown option\n");
		athmem_debug_help();
		break;
	}

	ret = count;

	return ret;
}

static const struct file_operations athmem_debug_fops = {
	.read = athmem_debug_read,
	.write = athmem_debug_write,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static int athmem_debug_debugfs_add(void)
{
	struct dentry *subdir;

	subdir = debugfs_create_dir("ath_memdebug", NULL);
	debugfs_create_file("meminfo", 0644, subdir,
			    &athmem_obj_tree_root, &athmem_debug_fops);
	return 0;
}

static void athmem_create_object(unsigned long long ptr, size_t size,
				 size_t actual_size, gfp_t gfp,
				 int line, const char *func)
{
	unsigned long flags;
	struct athmem_debug_object *object, *parent;
	struct rb_node **link, *rb_parent;

	if (!athmem_flag_init_done) {
		athmem_debug_debugfs_add();
		athmem_flag_init_done = 1;
		spin_lock_init(&athmem_spinlock);
	}

	object = kmalloc(sizeof(*object), (gfp));
	if (!object) {
		pr_warn("Allocation of athmem_debug_object failed\n");
		return;
	}

	object->pointer = ptr;
	object->size = size;
	object->actual_size = actual_size;
	object->line = line;
	object->func = func;

	spin_lock_irqsave(&athmem_spinlock, flags);

	link = &athmem_obj_tree_root.rb_node;
	rb_parent = NULL;
	while (*link) {
		rb_parent = *link;
		parent = rb_entry(rb_parent, struct athmem_debug_object, rb_node);
		if (ptr < parent->pointer) {
			link = &parent->rb_node.rb_left;
		} else if (ptr > parent->pointer) {
			link = &parent->rb_node.rb_right;
		} else {
			athmem_stats_dup_ptr++;
			spin_unlock_irqrestore(&athmem_spinlock, flags);
			pr_debug("dup ptr incrementing %s %d ptr = %p", (char *)func,
				 line, (void *)ptr);
			kfree(object);
			return;
		}
	}
	athmem_stats_num_nodes++;
	rb_link_node(&object->rb_node, rb_parent, link);
	rb_insert_color(&object->rb_node, &athmem_obj_tree_root);
	spin_unlock_irqrestore(&athmem_spinlock, flags);
}

void ath_upate_oom_panic(int is_panic)
{
	if (is_panic)
		athmem_flag_stop_tracking = 1;
}
EXPORT_SYMBOL(ath_upate_oom_panic);

void ath_update_free(const void *ptr)
{
	if (athmem_flag_stop_tracking || !athmem_flag_init_done)
		return;

	athmem_delete_obj_full((unsigned long)(uintptr_t)ptr);
}
EXPORT_SYMBOL(ath_update_free);

void ath_update_free_skb_list(struct sk_buff_head *skb_list)
{
	struct sk_buff *skb = NULL, *next = NULL;

	skb_queue_walk_safe(skb_list, skb, next) {
		if (skb)
			ath_update_free(skb);
	}
}
EXPORT_SYMBOL(ath_update_free_skb_list);

void ath_update_alloc(void *ptr, int len, int line, const char *func, int is_txskb)
{
	if (!ptr || athmem_flag_stop_tracking ||
	    (athmem_flag_track_only_txskb && !is_txskb))
		return;

	athmem_create_object((unsigned long)ptr, len, len, GFP_ATOMIC, line, func);
}
EXPORT_SYMBOL(ath_update_alloc);

void *ath_kmalloc(size_t len, gfp_t flags, int line, const char *func)
{
	void *addr = kmalloc(len, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmalloc);

void *ath_kmemdup(const void *src, size_t len, gfp_t flags, int line,
		  const char *func)
{
	void *addr = (void *)kmemdup(src, len, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmemdup);

void *ath_kzalloc(size_t len, gfp_t flags, int line, const char *func)
{
	void *addr = kzalloc(len, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kzalloc);

void *ath_netdev_alloc_skb(struct net_device *dev, unsigned int len,
			   int line, const char *func)
{
	struct sk_buff *skb = netdev_alloc_skb(dev, len);

	if (skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)skb, len, skb->truesize,
				     GFP_KERNEL, line, func);
	return skb;
}
EXPORT_SYMBOL(ath_netdev_alloc_skb);

void *ath_netdev_alloc_skb_fast(struct net_device *dev, unsigned int len,
				int line, const char *func)
{
	struct sk_buff *skb = netdev_alloc_skb_fast(dev, len);

	if (skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)skb, len, skb->truesize,
				     GFP_ATOMIC, line, func);
	return skb;
}
EXPORT_SYMBOL(ath_netdev_alloc_skb_fast);

void *ath_netdev_alloc_skb_no_skb_reset(struct net_device *dev, unsigned int len,
					gfp_t flags, int line, const char *func)
{
	struct sk_buff *skb = __netdev_alloc_skb_no_skb_reset(dev, len, flags);

	if (skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)skb, len, skb->truesize,
				     flags, line, func);
	return skb;
}
EXPORT_SYMBOL(ath_netdev_alloc_skb_no_skb_reset);

void *ath_dev_alloc_skb(unsigned int len, int line, const char *func)
{
	struct sk_buff *skb = dev_alloc_skb(len);

	if (skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)skb, len, skb->truesize,
				     GFP_ATOMIC, line, func);
	return skb;
}
EXPORT_SYMBOL(ath_dev_alloc_skb);

void *ath_skb_copy(const struct sk_buff *skb, gfp_t flags, int line,
		   const char *func)
{
	struct sk_buff *new_skb = skb_copy(skb, flags);

	if (new_skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)new_skb, skb->truesize,
				     new_skb->truesize, flags, line, func);
	return new_skb;
}
EXPORT_SYMBOL(ath_skb_copy);

void *ath_skb_clone(struct sk_buff *skb, gfp_t flags, int line,
		    const char *func)
{
	struct sk_buff *new_skb = skb_clone(skb, flags);
	size_t clone_sz = SKB_DATA_ALIGN(sizeof(struct sk_buff));

	if (new_skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)new_skb, clone_sz,
				     clone_sz, flags, line, func);
	return new_skb;
}
EXPORT_SYMBOL(ath_skb_clone);

void *ath_skb_clone_sk(struct sk_buff *skb, int line, const char *func)
{
	struct sk_buff *new_skb = skb_clone_sk(skb);
	size_t clone_sz = SKB_DATA_ALIGN(sizeof(struct sk_buff));

	if (new_skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)new_skb, clone_sz,
				     clone_sz, GFP_ATOMIC, line, func);
	return new_skb;
}
EXPORT_SYMBOL(ath_skb_clone_sk);

void *ath_skb_share_check(struct sk_buff *skb, gfp_t flags, int line,
			  const char *func)
{
	struct sk_buff *new_skb = skb_share_check(skb, flags);
	size_t clone_sz = SKB_DATA_ALIGN(sizeof(struct sk_buff));

	if (new_skb && new_skb != skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)new_skb, clone_sz,
				     clone_sz, flags, line, func);
	return new_skb;
}
EXPORT_SYMBOL(ath_skb_share_check);

void *ath_nlmsg_new(size_t len, gfp_t flags, int line, const char *func)
{
	struct sk_buff *skb = nlmsg_new(len, flags);

	if (skb && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)skb, nlmsg_total_size(len),
				     skb->truesize, flags, line, func);
	return skb;
}
EXPORT_SYMBOL(ath_nlmsg_new);

void *ath_vmalloc(unsigned long len, int line, const char *func)
{
	void *addr = vmalloc(len);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, PAGE_ALIGN(len),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vmalloc);

void *ath_vzalloc(unsigned long len, int line, const char *func)
{
	void *addr = vzalloc(len);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, PAGE_ALIGN(len),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vzalloc);

void *ath_dma_alloc_coherent(struct device *dev, size_t len,
			     dma_addr_t *handle, gfp_t flags,
			     int line, const char *func)
{
	void *addr = dma_alloc_coherent(dev, len, handle, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, PAGE_ALIGN(len),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_dma_alloc_coherent);

void *ath_kcalloc(size_t n, size_t len, gfp_t flags, int line,
		  const char *func)
{
	void *addr = kcalloc(n, len, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, n * len, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kcalloc);

void ath_kfree(const void *ptr)
{
	ath_update_free(ptr);
	kfree(ptr);
}
EXPORT_SYMBOL(ath_kfree);

void ath_vfree(void *ptr)
{
	ath_update_free(ptr);
	vfree(ptr);
}
EXPORT_SYMBOL(ath_vfree);

void ath_dma_free_coherent(struct device *dev, size_t size,
			   void *cpu_addr, dma_addr_t dma_handle)
{
	ath_update_free(cpu_addr);
	dma_free_coherent(dev, size, cpu_addr, dma_handle);
	return;
}
EXPORT_SYMBOL(ath_dma_free_coherent);

void ath_kfree_sensitive(const void *ptr)
{
	ath_update_free(ptr);
	kfree_sensitive(ptr);
}
EXPORT_SYMBOL(ath_kfree_sensitive);

/* Per-CPU allocation tracking */
void __percpu *ath_alloc_percpu_gfp(size_t size, size_t align, gfp_t flags,
				    int line, const char *func)
{
	void __percpu *addr = __alloc_percpu_gfp(size, align, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size,
				     size * num_possible_cpus(),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_alloc_percpu_gfp);

void __percpu *ath_alloc_percpu(size_t size, size_t align, int line, const char *func)
{
	void __percpu *addr = __alloc_percpu(size, align);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size,
				     size * num_possible_cpus(),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_alloc_percpu);

void ath_free_percpu(void __percpu *ptr)
{
	ath_update_free(ptr);
	free_percpu(ptr);
}
EXPORT_SYMBOL(ath_free_percpu);

/* Page allocation tracking */
unsigned long ath_get_free_pages(gfp_t flags, unsigned int order,
				 int line, const char *func)
{
	unsigned long addr = __get_free_pages(flags, order);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object(addr, PAGE_SIZE << order, PAGE_SIZE << order,
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_get_free_pages);

unsigned long ath_get_zeroed_page(gfp_t flags, int line, const char *func)
{
	unsigned long addr = get_zeroed_page(flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object(addr, PAGE_SIZE, PAGE_SIZE,
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_get_zeroed_page);

void ath_free_pages(unsigned long addr, unsigned int order)
{
	ath_update_free((void *)addr);
	free_pages(addr, order);
}
EXPORT_SYMBOL(ath_free_pages);

void *ath_page_frag_alloc(struct page_frag_cache *nc, unsigned int fragsz,
			  gfp_t gfp_mask, int line, const char *func)
{
	void *addr = page_frag_alloc(nc, fragsz, gfp_mask);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, fragsz, fragsz,
				     gfp_mask, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_page_frag_alloc);

/* kmem_cache allocation tracking */
void *ath_kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags,
			   int line, const char *func)
{
	void *addr = kmem_cache_alloc(cachep, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, kmem_cache_size(cachep),
				     kmem_cache_size(cachep), flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmem_cache_alloc);

void *ath_kmem_cache_zalloc(struct kmem_cache *cachep, gfp_t flags,
			    int line, const char *func)
{
	void *addr = kmem_cache_zalloc(cachep, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, kmem_cache_size(cachep),
				     kmem_cache_size(cachep), flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmem_cache_zalloc);

void ath_kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	ath_update_free(objp);
	kmem_cache_free(cachep, objp);
}
EXPORT_SYMBOL(ath_kmem_cache_free);

/* krealloc tracking */
void *ath_krealloc(const void *ptr, size_t new_size, gfp_t flags,
		   int line, const char *func)
{
	void *addr;

	addr = krealloc(ptr, new_size, flags);
	if (!addr)
		return NULL;

	if (ptr)
		ath_update_free(ptr);
	if (!athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, new_size, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_krealloc);

/* kvmalloc tracking */
void *ath_kvmalloc(size_t size, gfp_t flags, int line, const char *func)
{
	void *addr = kvmalloc(size, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(size)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvmalloc);

void *ath_kvzalloc(size_t size, gfp_t flags, int line, const char *func)
{
	void *addr = kvzalloc(size, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(size)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvzalloc);

void *ath_kvcalloc(size_t n, size_t size, gfp_t flags, int line, const char *func)
{
	void *addr = kvcalloc(n, size, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, n * size,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(n * size)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvcalloc);

void ath_kvfree(const void *addr)
{
	ath_update_free(addr);
	kvfree(addr);
}
EXPORT_SYMBOL(ath_kvfree);

/* ------------------------------------------------------------------ */
/* kmalloc family - additional variants                                 */
/* ------------------------------------------------------------------ */

void *ath_kmalloc_array(size_t n, size_t size, gfp_t flags, int line, const char *func)
{
	void *addr = kmalloc_array(n, size, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, n * size, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmalloc_array);

void *ath_kmalloc_node(size_t size, gfp_t flags, int node, int line, const char *func)
{
	void *addr = kmalloc_node(size, flags, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmalloc_node);

void *ath_kzalloc_node(size_t size, gfp_t flags, int node, int line, const char *func)
{
	void *addr = kzalloc_node(size, flags, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kzalloc_node);

void *ath_krealloc_array(void *ptr, size_t new_n, size_t new_size, gfp_t flags,
			 int line, const char *func)
{
	void *addr = krealloc_array(ptr, new_n, new_size, flags);

	if (!addr)
		return NULL;
	if (ptr)
		ath_update_free(ptr);
	if (!athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, new_n * new_size,
				     ksize(addr), flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_krealloc_array);

/* ------------------------------------------------------------------ */
/* vmalloc family - additional variants                                 */
/* ------------------------------------------------------------------ */

void *ath___vmalloc(unsigned long size, gfp_t gfp_mask, int line, const char *func)
{
	void *addr = __vmalloc(size, gfp_mask);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     gfp_mask, line, func);
	return addr;
}
EXPORT_SYMBOL(ath___vmalloc);

void *ath_vmalloc_user(unsigned long size, int line, const char *func)
{
	void *addr = vmalloc_user(size);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vmalloc_user);

void *ath_vmalloc_node(unsigned long size, int node, int line, const char *func)
{
	void *addr = vmalloc_node(size, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vmalloc_node);

void *ath_vzalloc_node(unsigned long size, int node, int line, const char *func)
{
	void *addr = vzalloc_node(size, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vzalloc_node);

void *ath_vmalloc_32(unsigned long size, int line, const char *func)
{
	void *addr = vmalloc_32(size);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_vmalloc_32);

/* ------------------------------------------------------------------ */
/* String duplication tracking                                          */
/* ------------------------------------------------------------------ */

char *ath_kstrdup(const char *s, gfp_t gfp, int line, const char *func)
{
	char *addr = kstrdup(s, gfp);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, strlen(s) + 1,
				     ksize(addr), gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kstrdup);

char *ath_kstrndup(const char *s, size_t len, gfp_t gfp, int line, const char *func)
{
	char *addr = kstrndup(s, len, gfp);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len + 1,
				     ksize(addr), gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kstrndup);

char *ath_kmemdup_nul(const char *s, size_t len, gfp_t gfp, int line, const char *func)
{
	char *addr = kmemdup_nul(s, len, gfp);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len + 1,
				     ksize(addr), gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmemdup_nul);

void *ath_memdup_user(const void __user *src, size_t len, int line, const char *func)
{
	void *addr = memdup_user(src, len);

	if (!IS_ERR_OR_NULL(addr) && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, ksize(addr),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_memdup_user);

void *ath_memdup_user_nul(const void __user *src, size_t len, int line, const char *func)
{
	void *addr = memdup_user_nul(src, len);

	if (!IS_ERR_OR_NULL(addr) && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len + 1, ksize(addr),
				     GFP_KERNEL, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_memdup_user_nul);

/* ------------------------------------------------------------------ */
/* page_frag - additional variants                                      */
/* ------------------------------------------------------------------ */

void *ath_page_frag_alloc_align(struct page_frag_cache *nc, unsigned int fragsz,
				gfp_t gfp_mask, unsigned int align_mask,
				int line, const char *func)
{
	void *addr = page_frag_alloc_align(nc, fragsz, gfp_mask, align_mask);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, fragsz, fragsz,
				     gfp_mask, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_page_frag_alloc_align);

void ath_page_frag_free(void *addr)
{
	ath_update_free(addr);
	page_frag_free(addr);
}
EXPORT_SYMBOL(ath_page_frag_free);

/* ------------------------------------------------------------------ */
/* kmem_cache - node variant                                            */
/* ------------------------------------------------------------------ */

void *ath_kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags,
				int node, int line, const char *func)
{
	void *addr = kmem_cache_alloc_node(cachep, flags, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, kmem_cache_size(cachep),
				     kmem_cache_size(cachep), flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kmem_cache_alloc_node);

/* ------------------------------------------------------------------ */
/* kvmalloc family - additional variants                                */
/* ------------------------------------------------------------------ */

void *ath_kvmalloc_node(size_t size, gfp_t flags, int node, int line, const char *func)
{
	void *addr = kvmalloc_node(size, flags, node);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(size)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvmalloc_node);

void *ath_kvrealloc(const void *p, size_t oldsize, size_t newsize, gfp_t flags,
		    int line, const char *func)
{
	void *addr = kvrealloc(p, oldsize, newsize, flags);

	if (!addr)
		return NULL;
	if (p)
		ath_update_free(p);
	if (!athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, newsize,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(newsize)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvrealloc);

void *ath_kvmemdup(const void *src, size_t len, gfp_t flags, int line, const char *func)
{
	void *addr = kvmemdup(src, len, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len,
				     is_vmalloc_addr(addr) ? PAGE_ALIGN(len)
							   : ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_kvmemdup);

/* ------------------------------------------------------------------ */
/* DMA pool tracking                                                    */
/* ------------------------------------------------------------------ */

void *ath_dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
			 dma_addr_t *handle, int line, const char *func)
{
	void *addr = dma_pool_alloc(pool, mem_flags, handle);

	if (addr && !athmem_flag_stop_tracking)
		/*
		 * dma_pool element size is an internal field not exposed by
		 * the API; record size=0 so the entry is still tracked for
		 * leak detection via caller/line info.
		 */
		athmem_create_object((unsigned long)addr, 0, 0,
				     mem_flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_dma_pool_alloc);

void ath_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr)
{
	ath_update_free(vaddr);
	dma_pool_free(pool, vaddr, addr);
}
EXPORT_SYMBOL(ath_dma_pool_free);

void *ath_dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
			   gfp_t gfp, unsigned long attrs, int line, const char *func)
{
	void *addr = dmam_alloc_attrs(dev, size, dma_handle, gfp, attrs);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, PAGE_ALIGN(size),
				     gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_dmam_alloc_attrs);

/* ------------------------------------------------------------------ */
/* devm (device-managed) allocation tracking                            */
/* ------------------------------------------------------------------ */

void *ath_devm_kmalloc(struct device *dev, size_t size, gfp_t gfp,
		       int line, const char *func)
{
	void *addr = devm_kmalloc(dev, size, gfp);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, size, ksize(addr),
				     gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_devm_kmalloc);

void *ath_devm_kmalloc_array(struct device *dev, size_t n, size_t size,
			     gfp_t flags, int line, const char *func)
{
	void *addr = devm_kmalloc_array(dev, n, size, flags);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, n * size, ksize(addr),
				     flags, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_devm_kmalloc_array);

void ath_devm_kfree(struct device *dev, const void *p)
{
	ath_update_free(p);
	devm_kfree(dev, p);
}
EXPORT_SYMBOL(ath_devm_kfree);

void *ath_devm_kmemdup(struct device *dev, const void *src, size_t len,
		       gfp_t gfp, int line, const char *func)
{
	void *addr = devm_kmemdup(dev, src, len, gfp);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object((unsigned long)addr, len, ksize(addr),
				     gfp, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_devm_kmemdup);

unsigned long ath_devm_get_free_pages(struct device *dev, gfp_t gfp_mask,
				      unsigned int order, int line, const char *func)
{
	unsigned long addr = devm_get_free_pages(dev, gfp_mask, order);

	if (addr && !athmem_flag_stop_tracking)
		athmem_create_object(addr, PAGE_SIZE << order, PAGE_SIZE << order,
				     gfp_mask, line, func);
	return addr;
}
EXPORT_SYMBOL(ath_devm_get_free_pages);

void ath_devm_free_pages(struct device *dev, unsigned long addr)
{
	ath_update_free((void *)addr);
	devm_free_pages(dev, addr);
}
EXPORT_SYMBOL(ath_devm_free_pages);
