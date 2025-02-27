#ifndef _GALOIS_entry_H_
#define _GALOIS_entry_H_

#include <unordered_map>
#include <list>

#include "llvm/Support/CommandLine.h"

/******************************************************************************/
/* Declaration of command line arguments */
/******************************************************************************/
namespace cll = llvm::cl;

extern cll::opt<unsigned int> cacheEntry;

namespace galois {
namespace graphs {

/**
 * Cache: fully associative, LRU, read-only
 */
template <typename KeyType, typename ValueType>
class Cache {
public:
    Cache(int capacity) : capacity(capacity) { entry.reserve(capacity); }

    bool find(const KeyType& key) {
        if (entry.find(key) == entry.end()) {
            return false;
        } else {
            return true;
        }
    }

    ValueType get(const KeyType& key) {
        // Move the accessed node to the front (most recent)
        moveToFront(key);
        return entry[key].first;
    }

    void put(const KeyType& key, const ValueType& value) {
        if (entry.find(key) != entry.end()) {
            // Update the value and move the node to the front
            entry[key].first = value;
            moveToFront(key);
            return;
        }
        
        if (entry.size() >= capacity) {
            evict();
        }
        entry[key] = {value, list.begin()};
        list.push_front(key);
    }

    void clear() {
        entry.clear();
        list.clear();
    }

private:
    int capacity;
    std::unordered_map<KeyType, std::pair<ValueType, typename std::list<KeyType>::iterator>> entry;
    std::list<KeyType> list;

    void moveToFront(const KeyType& key) {
        list.erase(entry[key].second);
        list.push_front(key);
        entry[key].second = list.begin();
    }

    void evict() {
        KeyType oldKey = list.back();
        list.pop_back();
        entry.erase(oldKey);
    }
};

} // end namespace graphs
} // end namespace galois

#endif
