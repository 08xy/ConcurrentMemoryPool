﻿#pragma once

// 之前使用 unordered_map管理 page_id 与 span 的映射关系
// 通过VS自带的性能测试工具发现，性能主要消耗与锁的竞争，和哈希表的查找中

// 改使用基数树优化这一部分性能消耗
// 其优点在于：
//

#include "Common.h"

// Single-level array
template <int BITS>
class TCMalloc_PageMap1
{
private:
	static const int LENGTH = 1 << BITS;
	void** array_;

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap1()
	{
		size_t size = sizeof(void*) << BITS;
		size_t align_size = SizeClass::_RoundUp(size, 1 << PAGE_SHIFT);
		array_ = (void**)SystemAlloc(align_size >> PAGE_SHIFT);
		memset(array_, 0, sizeof(void*) << BITS);
	}

	// Return the current value for KEY.  Returns NULL if not yet set,
	// or if k is out of range
	void* get(Number k) const
	{
		if ((k >> BITS) > 0)
		{
			return nullptr;
		}
		return array_[k];
	}

	void set(Number k, void* v)
	{
		array_[k] = v;
	}
};

// Two-level radix tree
template<int BITS>
class TCMalloc_PageMap2
{
private:
	// Put 32 entries in the root and (2^BITS)/32 entries in each leaf.
	static const int ROOT_BITS = 5;
	static const int ROOT_LENGTH = 1 << ROOT_BITS;

	static const int LEAF_BITS = BITS - ROOT_BITS;
	static const int LEAF_LENGTH = 1 << LEAF_BITS;

	// Leaf node
	struct Leaf
	{
		void* values[LEAF_LENGTH];
	};

	Leaf* root_[ROOT_LENGTH];      // Pointer to 32 child nodes

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap2()
	{
		memset(root_, 0, sizeof(root_));

		PreallocateMoreMemory();
	}

	void* get(Number k) const 
	{
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		if ((k >> BITS) > 0 || root_[i1] == NULL) {
			return NULL;
		}
		return root_[i1]->values[i2];
	}

	void set(Number k, void* v) 
	{
		const Number i1 = k >> LEAF_BITS;
		const Number i2 = k & (LEAF_LENGTH - 1);
		ASSERT(i1 < ROOT_LENGTH);
		root_[i1]->values[i2] = v;
	}

	bool Ensure(Number start, size_t n) 
	{
		for (Number key = start; key <= start + n - 1;) 
		{
			const Number i1 = key >> LEAF_BITS;

			// Check for overflow
			if (i1 >= ROOT_LENGTH)
				return false;

			// Make 2nd level node if necessary
			if (root_[i1] == NULL) 
			{
				//Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
				//if (leaf == NULL) return false;
				static ObjectPool<Leaf>	leafPool;
				Leaf* leaf = (Leaf*)leafPool.New();

				memset(leaf, 0, sizeof(*leaf));
				root_[i1] = leaf;
			}

			// Advance key past whatever is covered by this leaf node
			key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
		}
		return true;
	}

	void PreallocateMoreMemory() 
	{
		// Allocate enough to keep track of all possible pages
		Ensure(0, 1 << BITS);
	}
};