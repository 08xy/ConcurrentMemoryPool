#pragma once

#include "Common.h"

// 整个程序一个CentralCache就行——》单例模式
class CentralCache
{
private:
	SpanList _span_lists[NUM_FREELIST];   // 与ThreadCache相同的映射规则
	static CentralCache _instance_central;

private:
	CentralCache() {}
	CentralCache(const CentralCache& ) = delete;
	CentralCache& operator=(const CentralCache& ) = delete;

public:
	static CentralCache* GetInstance()
	{
		return &_instance_central;
	}

	// 获得一个非空的span
	Span* GetNonNullOneSpan(SpanList& list, size_t size);

	//  从中心缓存中获取一部分对象给ThreadCache
	size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

	void ReleaseListToSpans(void* start, size_t size);
};
