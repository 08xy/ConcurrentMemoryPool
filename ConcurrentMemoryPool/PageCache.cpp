#include "PageCache.h"

PageCache PageCache::_instance_page;

// 获取一个k页的span
Span* PageCache::NewSpan(size_t k)
{
	assert(k > 0);

	// 大于128 page的直接向堆申请
	if (k > NUM_PAGE - 1)
	{
		void* ptr = SystemAlloc(k);
		//Span* span = new Span;
		Span* span = _span_pool.New();

		span->_page_id = (PAGE_ID)ptr >> PAGE_SHIFT;
		span->_page_num = k;

		//_id_span_map[span->_page_id] = span;
		_id_span_map.set(span->_page_id, span);
		return span;
	}

	// 先检查第k个span桶有没有span
	if (!_span_lists[k].Empty())
	{
		Span* need_span = _span_lists[k].PopFront();

		// 建立id与span的映射，在Central Cache回收小块内存时，查找对应的span
		for (PAGE_ID i = 0; i < need_span->_page_num; ++i)
		{
			_id_span_map.set(need_span->_page_id + i, need_span);
		}
	}

	// 检查后面的桶里有没有span(比k大)，如果有将其切分
	for (size_t i = k + 1; i < NUM_PAGE; ++i)
	{
		if (!_span_lists[i].Empty())
		{
			Span* cleaved_span = _span_lists[i].PopFront();
			//Span* need_span = new Span;
			Span* need_span = _span_pool.New();

			// 在cleaved_span的头部切一个k页下来
			// k页返回
			// cleaved_span挂到对应映射的位置
			need_span->_page_id = cleaved_span->_page_id;
			need_span->_page_num = k;

			cleaved_span->_page_id += k;
			cleaved_span->_page_num -= k;

			_span_lists[cleaved_span->_page_num].PushFront(cleaved_span);

			// 存储cleaved_span的首尾页号与cleaved_span映射
			// 方便PageCache回收内存时，进行的合并查找
			//_id_span_map[cleaved_span->_page_id] = cleaved_span;
			//_id_span_map[cleaved_span->_page_id + cleaved_span->_page_num - 1] = cleaved_span;
			_id_span_map.set(cleaved_span->_page_id, cleaved_span);
			_id_span_map.set(cleaved_span->_page_id + cleaved_span->_page_num - 1, cleaved_span);

			
			// 建立id与审判的映射，方便CentralCache回收小块内存时，查找对应的span
			//注意：need_span的每一个页面都需要注册，因为 obj的ptr-> span的_page_id -> span
			for (PAGE_ID i = 0; i < need_span->_page_num; ++i)
			{
				_id_span_map.set(need_span->_page_id + i, need_span);
			}

			return need_span;
		}
	}

	// 走到这个位置了，就说明后面没有大页的span了
	// 这时，向堆要一个128页的span
	//Span* big_span = new Span;
	Span* big_span = _span_pool.New();
	void* ptr = SystemAlloc(NUM_PAGE - 1);
	big_span->_page_id = (PAGE_ID)ptr >> PAGE_SHIFT;
	big_span->_page_num = NUM_PAGE - 1;

	_span_lists[big_span->_page_num].PushFront(big_span);

	// 现在有128页span了，递归调用该函数
	return NewSpan(k);
}


Span* PageCache::MapObjectToSpan(void* obj)
{
	// 内存对象的地址>>13，就是其所在的页号
	PAGE_ID id = ((PAGE_ID)obj >> PAGE_SHIFT);

	/*std::unique_lock<std::mutex> lock(_page_mtx);

	auto ret = _id_span_map.find(id);
	if (ret != _id_span_map.end())
	{
		return ret->second;
	}
	else
	{
		assert(false);
		return nullptr;
	}*/
	auto ret = (Span*)_id_span_map.get(id);
	assert(ret != nullptr);
	return ret;
}

// 尝试对span前后的页，进行合并，缓解内存碎片问题
void PageCache::ReleaseSpanToPage(Span* span)
{
	// 大于128 page的直接还堆
	if (span->_page_num > NUM_PAGE - 1)
	{
		void* ptr = (void*)(span->_page_id << PAGE_SHIFT);
		SystemFree(ptr);

		return;
	}


	// 向前合并
	while (1)
	{
		PAGE_ID prev_id = span->_page_id - 1;

		//auto ret = _id_span_map.find(prev_id);
		//// 前面的页号没有(不属于这个进程)，不合并
		//if (ret == _id_span_map.end())
		//{
		//	break;
		//}
		Span* ret = (Span*)_id_span_map.get(prev_id);
		if (ret == nullptr) { break; }

		// 前面的相邻页的span正在被使用，不合并
		Span* prev_span = ret;
		if (prev_span->_is_use == true) { break; }

		// 合并出超过128页的span没办法管理，不合并
		if (span->_page_num + prev_span->_page_num > NUM_PAGE - 1) { break; }
		
		// 终于可以合并了
		span->_page_id = prev_span->_page_id;
		span->_page_num += prev_span->_page_num;

		// prev_span已经被span合并了，从它原有的span_list剔除
		_span_lists[prev_span->_page_num].Erase(prev_span);
		//delete prev_span;
		_span_pool.Delete(prev_span);
	}

	// 向后合并
	while (1)
	{
		PAGE_ID next_id = span->_page_id + span->_page_num;
		/*auto ret = _id_span_map.find(next_id);
		if (ret == _id_span_map.end()) { break; }*/
		auto ret = (Span*)_id_span_map.get(next_id);
		if (ret == nullptr) { break; }

		Span* next_span = ret;
		if (next_span->_is_use == true) { break; }

		// 合并出超过128页的span没办法管理，不合并
		if (span->_page_num + next_span->_page_num > NUM_PAGE - 1) { break; }

		// 终于可以合并了
		span->_page_num += next_span->_page_num;

		// prev_span已经被span合并了，从它原有的span_list剔除
		_span_lists[next_span->_page_num].Erase(next_span);
		//delete next_span;
		_span_pool.Delete(next_span);
	}

	_span_lists[span->_page_num].PushFront(span);
	span->_is_use = false;

	// 注册新span到_id_span_map 中，以便它与其它span融合
	//_id_span_map[span->_page_id] = span;
	//_id_span_map[span->_page_id + span->_page_num - 1] = span;
	_id_span_map.set(span->_page_id, span);
	_id_span_map.set(span->_page_id + span->_page_num - 1, span);
}