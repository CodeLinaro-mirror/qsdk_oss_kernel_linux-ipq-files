/*
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: ISC
 */
#ifndef __ATHMEMDEBUG_H
#define __ATHMEMDEBUG_H

struct sk_buff;
struct sk_buff_head;
struct kmem_cache;
struct dma_pool;
struct net_device;
struct page_frag_cache;
struct device;
void ath_update_alloc(void *ptr, int len, int line, const char *func, int is_txskb);
void ath_upate_oom_panic(int is_panic);
void ath_update_free(const void *ptr);
void ath_update_free_skb_list(struct sk_buff_head *skb_list);


void *ath_kmalloc(size_t len, gfp_t flags, int line, const char *func);
void *ath_kmemdup(const void *src, size_t len, gfp_t flags, int line, const char *func);
void *ath_kzalloc(size_t len, gfp_t flags, int line, const char *func);
void *ath_kmalloc_array(size_t n, size_t size, gfp_t flags, int line, const char *func);
void *ath_kmalloc_node(size_t size, gfp_t flags, int node, int line, const char *func);
void *ath_kzalloc_node(size_t size, gfp_t flags, int node, int line, const char *func);
void *ath_krealloc_array(void *ptr, size_t new_n, size_t new_size, gfp_t flags,
			 int line, const char *func);
void *ath_vmalloc(unsigned long len, int line, const char *func);
void *ath_vzalloc(unsigned long len, int line, const char *func);
void *ath_kcalloc(size_t n, size_t len, gfp_t flags, int line, const char *func);

void ath_kfree(const void *ptr);
void ath_vfree(void *ptr);
void ath_kfree_sensitive(const void *ptr);

/* vmalloc family */
void *ath___vmalloc(unsigned long size, gfp_t gfp_mask, int line, const char *func);
void *ath_vmalloc_user(unsigned long size, int line, const char *func);
void *ath_vmalloc_node(unsigned long size, int node, int line, const char *func);
void *ath_vzalloc_node(unsigned long size, int node, int line, const char *func);
void *ath_vmalloc_32(unsigned long size, int line, const char *func);

/* String duplication tracking */
char *ath_kstrdup(const char *s, gfp_t gfp, int line, const char *func);
char *ath_kstrndup(const char *s, size_t len, gfp_t gfp, int line, const char *func);
char *ath_kmemdup_nul(const char *s, size_t len, gfp_t gfp, int line, const char *func);
void *ath_memdup_user(const void __user *src, size_t len, int line, const char *func);
void *ath_memdup_user_nul(const void __user *src, size_t len, int line, const char *func);

/* Per-CPU allocation tracking */
void __percpu *ath_alloc_percpu_gfp(size_t size, size_t align, gfp_t flags,
				    int line, const char *func);
void __percpu *ath_alloc_percpu(size_t size, size_t align, int line, const char *func);
void ath_free_percpu(void __percpu *ptr);

/* Page allocation tracking */
unsigned long ath_get_free_pages(gfp_t flags, unsigned int order,
				 int line, const char *func);
unsigned long ath_get_zeroed_page(gfp_t flags, int line, const char *func);
void ath_free_pages(unsigned long addr, unsigned int order);
void *ath_page_frag_alloc(struct page_frag_cache *nc, unsigned int fragsz,
			  gfp_t gfp_mask, int line, const char *func);
void *ath_page_frag_alloc_align(struct page_frag_cache *nc, unsigned int fragsz,
				gfp_t gfp_mask, unsigned int align_mask,
				int line, const char *func);
void ath_page_frag_free(void *addr);

/* kmem_cache allocation tracking */
void *ath_kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags,
			   int line, const char *func);
void *ath_kmem_cache_zalloc(struct kmem_cache *cachep, gfp_t flags,
			    int line, const char *func);
void ath_kmem_cache_free(struct kmem_cache *cachep, void *objp);
void *ath_kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags,
				int node, int line, const char *func);

/* krealloc tracking */
void *ath_krealloc(const void *ptr, size_t new_size, gfp_t flags,
		   int line, const char *func);

/* kvmalloc tracking */
void *ath_kvmalloc(size_t size, gfp_t flags, int line, const char *func);
void *ath_kvzalloc(size_t size, gfp_t flags, int line, const char *func);
void *ath_kvcalloc(size_t n, size_t size, gfp_t flags, int line, const char *func);
void ath_kvfree(const void *addr);
void *ath_kvmalloc_node(size_t size, gfp_t flags, int node, int line, const char *func);
void *ath_kvrealloc(const void *p, size_t oldsize, size_t newsize, gfp_t flags,
		    int line, const char *func);
void *ath_kvmemdup(const void *src, size_t len, gfp_t flags, int line, const char *func);

void *ath_netdev_alloc_skb_no_skb_reset(struct net_device *dev, unsigned int length,
					gfp_t flags, int line, const char *func);
void *ath_netdev_alloc_skb(struct net_device *dev, unsigned int len,
			   int line, const char *func);
void *ath_netdev_alloc_skb_fast(struct net_device *dev, unsigned int len,
				int line, const char *func);
void *ath_dev_alloc_skb(unsigned int len, int line, const char *func);
void *ath_skb_copy(const struct sk_buff *skb, gfp_t flags, int line, const char *func);
void *ath_skb_clone(struct sk_buff *skb, gfp_t flags, int line, const char *func);
void *ath_skb_clone_sk(struct sk_buff *skb, int line, const char *func);
void *ath_skb_share_check(struct sk_buff *skb, gfp_t flags, int line, const char *func);

void *ath_dma_alloc_coherent(struct device *dev, size_t len,
			     dma_addr_t *handle, gfp_t flags,
			     int line, const char *func);
void ath_dma_free_coherent(struct device *dev, size_t size,
			   void *cpu_addr, dma_addr_t dma_handle);


void *ath_nlmsg_new(size_t len, gfp_t flags, int line, const char *func);

/* DMA pool tracking */
void *ath_dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
			 dma_addr_t *handle, int line, const char *func);
void ath_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr);
void *ath_dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
			   gfp_t gfp, unsigned long attrs, int line, const char *func);

/* devm (device-managed) allocation tracking */
void *ath_devm_kmalloc(struct device *dev, size_t size, gfp_t gfp,
		       int line, const char *func);
void *ath_devm_kmalloc_array(struct device *dev, size_t n, size_t size, gfp_t flags,
			     int line, const char *func);
void ath_devm_kfree(struct device *dev, const void *p);
void *ath_devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp,
		       int line, const char *func);
unsigned long ath_devm_get_free_pages(struct device *dev, gfp_t gfp_mask,
				      unsigned int order, int line, const char *func);
void ath_devm_free_pages(struct device *dev, unsigned long addr);

#endif /* __ATHMEMDEBUG_H */
