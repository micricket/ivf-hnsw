#pragma once
#include <mutex>
#include <string.h>

//#include <unordered_set>
#include <sparsehash/dense_hash_set>

using google::dense_hash_set;

namespace hnswlib{
typedef unsigned short vl_type;
class VisitedList {
public:
	vl_type curV;
	vl_type *mass;
    dense_hash_set<unsigned int> *vl_set;
	unsigned int numelements;

	VisitedList(unsigned int numelements1)
	{
		curV = -1;
		numelements = numelements1;
        //vl_set = new dense_hash_set<unsigned int>();
        //vl_set->set_empty_key(NULL);
		mass = new vl_type[numelements];
	}
    dense_hash_set<unsigned int> *getVisitedSet() {return vl_set;};
	void reset()
	{
		curV++;
		if (curV == 0) {
			memset(mass, 0, sizeof(vl_type) * numelements);
			curV++;
		}
        //vl_set->clear();
	};
	~VisitedList() { delete /*vl_set;*/mass; }
};
///////////////////////////////////////////////////////////
//
// Class for multi-threaded pool-management of VisitedLists
//
/////////////////////////////////////////////////////////

class VisitedListPool {
	deque<VisitedList *> pool;
	mutex poolguard;
	int maxpools;
	int numelements;

public:
	VisitedListPool(int initmaxpools, int numelements1)
	{
		numelements = numelements1;
		for (int i = 0; i < initmaxpools; i++)
			pool.push_front(new VisitedList(numelements));
	}
	VisitedList *getFreeVisitedList()
	{
		VisitedList *rez;
		{
			unique_lock<mutex> lock(poolguard);
			if (pool.size() > 0) {
				rez = pool.front();
				pool.pop_front();
			}
			else {
				rez = new VisitedList(numelements);
			}
		}
		rez->reset();
		return rez;
	};
	void releaseVisitedList(VisitedList *vl)
	{
		unique_lock<mutex> lock(poolguard);
		pool.push_front(vl);
	};
	~VisitedListPool()
	{
		while (pool.size()) {
			VisitedList *rez = pool.front();
			pool.pop_front();
			delete rez;
		}
	};
};
}

