#include "CentralCache.h"
#include "PageCache.h"

CentralCache CentralCache::_instance_central;

// 获得一个非空的span
Span* CentralCache::GetNonNullOneSpan(SpanList& list, size_t size)
{
	 // & list 是 _span_lists[index]
	 // 查看当前的spanlist中还有没有 拥有未分配对象的span
	Span* it = list.Begin();
	while (it != list.End())
	{
		if (it->_free_list != nullptr)
		{
			return it;
		}
		else
		{
			it = it->_next;
		}
	}

	// 先把central cache的桶锁解掉，这样如果其它线程释放内存对象回来，不会被阻塞
	list._mtx.unlock();

	// 走到这说明没有空闲的span了，只能向page cache要
	PageCache::GetInstance()->_page_mtx.lock();
	Span* span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
	span->_is_use = true;
	span->_obj_size = size;
	PageCache::GetInstance()->_page_mtx.unlock();

	// 对获取span进行切分，不需要加锁，因为这会其它线程访问不到这个span（没挂到list上）

	// 计算span的大块内存的起始地址和大块内存的大小（bytes）
	char* start = (char*)(span->_page_id << PAGE_SHIFT);
	size_t bytes = span->_page_num << PAGE_SHIFT;
	char* end = start + bytes;

	// 把大块内存切成自由链表链接起来
	//	 先切下一块做头，方便尾插（分配自由链表的内存对象时，从start-》end方向
	span->_free_list = start;
	start += size;
	void* tail = span->_free_list;
	int debug_i = 1;	// 调试代码所用
	while (start < end)
	{
		++debug_i;
		NextObj(tail) = start;
		tail = start; // tail = NextObj(tail);
		start += size;
	}
	NextObj(tail) = nullptr;

	// 切好span后，需要把span挂到桶里，需要加锁了
	list._mtx.lock();
	list.PushFront(span);

	return span;
}

// 希望获取batch_num 个 size大小的对象，返回实际获取的个数
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batch_num, size_t size)
{
	size_t index = SizeClass::Index(size);
	_span_lists[index]._mtx.lock();

	Span* span = GetNonNullOneSpan(_span_lists[index], size);
	assert(span);
	assert(span->_free_list);

	// 从span中获取batch_num个对象
	// 如果不够batch_num个，有多少那多少
	start = span->_free_list;
	end = start;
	size_t i = 0;
	size_t actual_num = 1;
	while (i < batch_num - 1 && NextObj(end) != nullptr)
	{
		end = NextObj(end);
		++i;
		++actual_num;
	}
	span->_free_list = NextObj(end);
	NextObj(end) = nullptr;
	span->_use_count += actual_num;

	_span_lists[index]._mtx.unlock();

	return actual_num;
}

void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	size_t index = SizeClass::Index(size);
	
	_span_lists[index]._mtx.lock();

	while (start)
	{
		void* next = NextObj(start);

		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);

		// 头插
		NextObj(start) = span->_free_list;
		span->_free_list = start;
		span->_use_count--;

		// _use_count == 0时，说明切分的小块内存都回来了
		// 这个span就可以回收给page cache
		if (0 == span->_use_count)
		{
			_span_lists[index].Erase(span);
			span->_free_list = nullptr;
			span->_next = nullptr;
			span->_prev = nullptr;

			// 释放span给PageCache时，要使用Page Cache的锁
			// 这时把桶锁戒掉
			_span_lists[index]._mtx.unlock();

			PageCache::GetInstance()->_page_mtx.lock();
			PageCache::GetInstance()->ReleaseSpanToPage(span);
			PageCache::GetInstance()->_page_mtx.unlock();

			_span_lists[index]._mtx.lock();
		}

		start = next;
	}
	_span_lists[index]._mtx.unlock();
}