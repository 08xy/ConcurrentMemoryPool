#pragma once

#include "Common.h"
#include "ObjectPool.h"
#include "PageMap.h"


// 单例模式（饿汉版）
class PageCache
{
private:
	SpanList _span_lists[NUM_PAGE];  // span的页数对应桶的下标
	ObjectPool<Span> _span_pool;

	static PageCache _instance_page;
	TCMalloc_PageMap1<32 - PAGE_SHIFT> _id_span_map;
public:
	std::mutex _page_mtx;			// 用一整个锁,不是不用桶锁，而是它更有性价比(效率更高)

private:
	PageCache() {}
	PageCache(const PageCache&) = delete;
	PageCache& operator=(const PageCache&) = delete;
public:
	static PageCache* GetInstance()
	{
		return &_instance_page;
	}

	// 返回 k页 大小的 span
	Span* NewSpan(size_t k);

	// 获取从内存对象到span的映射
	Span* MapObjectToSpan(void* obj);

	// 释放空闲span到PageCache，并尝试合并相邻的span
	void ReleaseSpanToPage(Span* span);
};
