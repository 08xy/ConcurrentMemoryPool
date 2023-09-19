#pragma once

#include <iostream>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include <thread>
#include <mutex>

#include <ctime>
#include <assert.h>

#ifdef _WIN32
	#include <Windows.h>
#else
	// linux
#endif


using std::cout;
using std::endl;

static const size_t MAX_BYTES = 256 * 1024;	 
static const size_t NUM_FREELIST = 208;		 // 自由链表最大个数
static const size_t NUM_PAGE = 129;			 // 0下标不使用
static const size_t PAGE_SHIFT = 13;		 // 8*1024 一页

#ifdef _WIN64
typedef unsigned long long PAGE_ID;
#elif _WIN32
typedef size_t PAGE_ID;
#elif
// linux
#endif

inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN32
	void* ptr = VirtualAlloc(0, kpage << 13, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	// linux下brk mmap等
#endif

	if (ptr == nullptr)
		throw std::bad_alloc();

	return ptr;
}

inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// sbrk unmmap等
#endif
}

static void*& NextObj(void* obj)
{
	return *(void**)obj;
}

class FreeList
{
private:
	void* _free_list = nullptr;
	size_t _max_size = 1;
	size_t _list_size = 0;
public:

	void Push(void* obj)
	{
		assert(obj);

		// 头插
		NextObj(obj) = _free_list;
		_free_list = obj;

		++_list_size;
	}

	void PushRange(void* start, void* end, size_t n)
	{
		NextObj(end) = _free_list;
		_free_list = start;

		_list_size += n;
	}

	void PopRange(void*& start, void*& end, size_t n)
	{
		assert(n <= _list_size);
		start = _free_list;
		end = start;

		for (size_t i = 0; i < n - 1; ++i)
		{
			end = NextObj(end);
		}

		// obj -> obj -> obj ->obj    n == 3
		// start         end 
		_free_list = NextObj(end);
		NextObj(end) = nullptr;

		_list_size -= n;
	}

	void* Pop()
	{
		assert(_free_list);

		// 头删
		void* obj = _free_list;
		_free_list = NextObj(obj);
		_list_size--;

		return obj;
	}

	bool Empty()
	{
		return nullptr == _free_list;
	}

	size_t& MaxSize()
	{
		return _max_size; // 返回&，需要通过该函数对其修改
	}

	size_t Size()
	{
		return _list_size;
	}
};

// 计算对象大小的对齐映射规则
class SizeClass
{
public:
	// 分段对齐：控制自由链表的数量
	// 整体控制在最多10%左右的内碎片浪费
	// [1,128]					8byte对齐	     freelist[0,16)
	// [128+1,1024]				16byte对齐	     freelist[16,72)
	// [1024+1,8*1024]			128byte对齐	     freelist[72,128)
	// [8*1024+1,64*1024]		1024byte对齐     freelist[128,184)
	// [64*1024+1,256*1024]		8*1024byte对齐   freelist[184,208)

	// 处理单次对齐：在对其数为 align_num 情况下，申请bytes大小的空间，计算实际返回的空间大小
	// 为什么这样做：内存对齐的需要
	static inline size_t _RoundUp(size_t bytes, size_t align_num)
	{
		return ((bytes + align_num - 1) & ~(align_num - 1));
	}

	// 处理分段对齐
	static inline size_t RoundUp(size_t size)
	{
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 8*1024)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 64*1024)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 256*1024)
		{
			return _RoundUp(size, 8*1024);
		}
		else
		{
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}

	// 计算在对其数为(1<<align_shift)的区间中所处的index
	static inline size_t _Index(size_t bytes, size_t align_shift)
	{
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
	}

	// 计算映射到哪一个自由链表桶
	static inline size_t Index(size_t bytes)
	{
		assert(bytes <= MAX_BYTES);

		// 每个区间有多少自由链表
		static int group_array[4] = { 16, 56, 56, 56 };

		if (bytes <= 128)
		{
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024)
		{
			return _Index(bytes - 128, 4) + group_array[0];
		}
		else if (bytes <= 8 * 1024)
		{
			return _Index(bytes - 1024, 7) + group_array[0] + group_array[1];
		}
		else if (bytes <= 64 * 1024)
		{
			return _Index(bytes - 8 * 1024, 10) + group_array[0] + group_array[1] + group_array[2];
		}
		else if (bytes <= 256 * 1024)
		{
			return _Index(bytes - 64 * 1024, 13) + group_array[0] + group_array[1] + group_array[2] + group_array[3];
		}
		else
		{
			assert(false);
		}

		return -1;
	}

	// 慢开始反馈调节 batch_num的上限值
	static size_t NumMoveSize(size_t size)
	{
		assert(size > 0);

		// [2, 512] 一次批量移动多少个对象的上限值
		int num = MAX_BYTES / size;
		if (num < 2)
			num = 2;

		if (num > 512)
			num = 512;

		return num;
	}

	// 计算一次向系统申请几个页
	static size_t NumMovePage(size_t size)
	{
		// ThreadCache一次向CentralCache申请大小为size对象个数的上限值
		// 那么申请几个页与其相关
		size_t num = NumMoveSize(size);
		size_t n_page = num * size;

		n_page >>= PAGE_SHIFT;
		if (n_page == 0)
			n_page = 1;

		return n_page;
	}

};


// 管理多个连续页大块内存的跨度结构
struct Span
{
	PAGE_ID _page_id = 0; // 大块内存的起始页号
	size_t _page_num = 0; // 页的数量

	Span* _next = nullptr;	// 双向链表的结构组织span
	Span* _prev = nullptr;

	size_t _use_count = 0;		// 切好小块内存，被分配给thread cache的计数
	void* _free_list = nullptr; // 切好的小块内存的自由链表
	size_t _obj_size = 0;       // 小块内存的大小

	bool _is_use = false;		// 是否正在被使用
};

// 带头双向循环链表
class SpanList
{
private:
	Span* _head;
public:
	std::mutex _mtx;  // 桶锁，减少锁的竞争

public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	bool Empty()
	{
		return _head->_next == _head;
	}

	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	Span* PopFront()
	{
		Span* pop_span = _head->_next;
		Erase(pop_span);
		return pop_span;
	}

	void Insert(Span* pos, Span* new_span)
	{
		assert(pos);
		assert(new_span);

		Span* prev = pos->_prev;
		// prev	new_span pos
		prev->_next = new_span;
		new_span->_prev = prev;
		new_span->_next = pos;
		pos->_prev = new_span;
	}

	// 只是将在pos位置的Span从链表中解开
	void Erase(Span* pos)
	{
		assert(pos);
		assert(pos != _head);

		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_next = next;
		next->_prev = prev;
	}
};
