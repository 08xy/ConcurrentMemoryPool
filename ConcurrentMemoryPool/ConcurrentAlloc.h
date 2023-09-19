#pragma once

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include "ObjectPool.h"

static void* ConcurrentAlloc(size_t size)
{
	// 大于MAX_BYTES(256kb = 32page)
	// 1. size > 32page && size < 128 page
	//    可以向PageCache申请空间
	// 2. size > 128 page
	//    这时，直接向系统堆申请空间
	// 上述的两种情况都在Page Cache的NewSpan函数中处理
	// 所以，下面代码使用统一的方式去解决
	if (size > MAX_BYTES)
	{
		size_t align_size = SizeClass::RoundUp(size);
		size_t page_num = align_size >> PAGE_SHIFT;

		PageCache::GetInstance()->_page_mtx.lock();
		Span* span = PageCache::GetInstance()->NewSpan(page_num);
		span->_obj_size = size;
		PageCache::GetInstance()->_page_mtx.unlock();

		void* ptr = (void*)(span->_page_id << PAGE_SHIFT);
		return ptr;
	}
	else
	{
		// 通过TLS每个线程无锁的获取自己的专属的ThreadCache对象
		if (nullptr == Ptr_TLS_ThreadCache)
		{
			static ObjectPool<ThreadCache> tc_pool;
			//Ptr_TLS_ThreadCache = new ThreadCache;
			Ptr_TLS_ThreadCache = tc_pool.New();
		}

		return Ptr_TLS_ThreadCache->Allocate(size);
	}
}

static void ConcurrentFree(void* ptr)
{
	Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = span->_obj_size;

	if (size > MAX_BYTES)
	{
		PageCache::GetInstance()->_page_mtx.lock();
		PageCache::GetInstance()->ReleaseSpanToPage(span);
		PageCache::GetInstance()->_page_mtx.unlock();
	}
	else
	{
		assert(Ptr_TLS_ThreadCache);
		Ptr_TLS_ThreadCache->Deallocate(ptr, size);
	}
}
