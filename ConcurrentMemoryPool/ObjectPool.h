#pragma once

#include "Common.h"

#ifdef _WIN32
	#include <Windows.h>
#else
//	
#endif


//  一种定长内存池的实现思路
//template<size_t size>
//class ObjectPool {};


template<typename T>
class ObjectPool
{
private:
	char* _memory = nullptr;     // 向OS申请的一大块内存空间
	size_t _remain_bytes = 0;	 // 大块空间中在切分过程中剩余字节数
	void* _free_list = nullptr;	 // 管理被释放返还的空间的链表的头指针

public:
	// 去获得一个T类型的对象（他所需要的空间就是定长的）
	// 获取空间的思路： 优先从_free_list中获取；
	//					如果 _free_list==nullptr，从_memory中获取；如果_memory剩余空间不够一个T类型对象，再向OS申请空间
	T* New()
	{
		T* obj = nullptr;

		if (_free_list != nullptr)
		{										
			void* next = *((void**)_free_list); // 取下一个链表节点的指针
			obj = (T*)_free_list;
			_free_list = next;
		}
		else
		{
			if (_remain_bytes < sizeof(T))	//两种情况：1 程序刚启动，_memory==nullptr  2.剩余空间不足
			{
				_remain_bytes = 128 * 1024;
				_memory = (char*)SystemAlloc(_remain_bytes >> 13);
				if (nullptr == _memory)
				{
					throw std::bad_alloc();
				}
			}

			obj = (T*)_memory;

			// 确保一个T对象的所用空间一个能够存放一个指针
			size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			_memory += objSize;
			_remain_bytes -= objSize;
		}

		// 定位new，显示调用T的构造函数初始化
		new(obj)T;

		return obj;
	}

	void Delete(T* obj)
	{
		// 显示调用析构函数清理对象
		obj->~T();

		// push_front
		*(void**)obj = _free_list; //  *(void**) 使用链表节点的头4/8个字节存储指针
		_free_list = obj;
	}
};

