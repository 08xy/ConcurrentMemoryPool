#include "ThreadCache.h"
#include "CentralCache.h"


void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{
	// 慢开始反馈调节算法
	// 1、最开始不会一次向central cache批量要太多，因为太多了，可能用不完
	// 2、如果不停有这个size大小的需求，batch_num会不断增长，直到上限
	// 3、size越大，一次向central cache要的batchNum就越小
	// 4、size越小，一次向central cache要的batchNum就越大
	size_t batch_num = min(_free_lists[index].MaxSize(), SizeClass::NumMoveSize(size));
	if (_free_lists[index].MaxSize() == batch_num)
	{
		_free_lists[index].MaxSize() += 1;
	}

	void* start = nullptr;
	void* end = nullptr;
	size_t actual_num = CentralCache::GetInstance()->FetchRangeObj(start, end, batch_num, size);
	assert(actual_num > 0);

	// 如果向CentralCache申请的对象有1-bitch_num个，返回第一个，余下的挂接到_free_list
	if (1 == actual_num)
	{
		assert(start == end); // 确保只有一个对象
		return start;
	}
	else
	{
		_free_lists[index].PushRange(NextObj(start), end, actual_num-1);
		return start;
	}
}

//  优先从_free_lists[index]中获取空间
void* ThreadCache::Allocate(size_t size)
{
	assert(size <= MAX_BYTES);

	size_t align_size = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);

	if (!_free_lists[index].Empty())
	{
		return _free_lists[index].Pop();
	}
	else
	{
		return FetchFromCentralCache(index, align_size);
	}
}

void ThreadCache::Deallocate(void* ptr, size_t size)
{
	assert(ptr);
	assert(size <= MAX_BYTES);

	//  找到映射的自由链表桶，被回收的对象空间插入
	size_t index = SizeClass::Index(size);
	_free_lists[index].Push(ptr);

	// 当链表长度大于一次批量申请的内存时，就开始还一段list给central cache
	if (_free_lists[index].Size() >= _free_lists[index].MaxSize())
	{
		ListTooLong(_free_lists[index], size);
	}
}

void ThreadCache::ListTooLong(FreeList& list, size_t size)
{
	void* start = nullptr;
	void* end = nullptr;
	list.PopRange(start, end, list.MaxSize());  // 一个桶就会留取少量资源

	CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}