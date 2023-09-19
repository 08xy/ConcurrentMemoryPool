#pragma once

#include "Common.h"

class ThreadCache
{
private:
	FreeList _free_lists[NUM_FREELIST];

public:
	// 申请和释放内存对象
	void* Allocate(size_t size);
	void Deallocate(void* ptr, size_t size);

	// 从中心缓存获取对象
	void* FetchFromCentralCache(size_t index, size_t size);

	// 释放对象时，链表过长时，回收内存到CentralCache
	void ListTooLong(FreeList& list, size_t size);
};

// TLS thread local storage
static _declspec(thread) ThreadCache* Ptr_TLS_ThreadCache = nullptr;

// 两个问题：
//	1.什么时候为需要ThreadCache的线程创建ThreadCache对象
//	2.以什么方法创建Thread Cache对象：TLS方法（优点：无锁的获取-》减少锁的竞争）
