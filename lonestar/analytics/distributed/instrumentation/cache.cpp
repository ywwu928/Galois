#include <bits/stdc++.h>

template <typename T>
struct metadata
{
    T tag;
    bool dirty;
};

template <typename T>
struct access_info
{
    bool present;
    bool has_victim;
    bool victim_dirty;
    T victim;
};

template <typename T>
class LRUCache {
    uint64_t size; //maximum capacity of cache
    std::list<metadata<T>> lru; // LRU list of the metadata
    std::unordered_map<T, typename std::list<metadata<T>>::iterator> index_lookup; // map tag to the index in the LRU list

public:
    LRUCache() {
        size= 0;
    }
    
    LRUCache(uint64_t n) {
        size= n;
    }

    access_info<T> read(T x) {
        access_info<T> info;
        if (index_lookup.find(x) == index_lookup.end()) { // not present in cache
            info.present = false;
            
            if (lru.size() == size) { // cache is full
                info.has_victim = true;

                // delete LRU element
                metadata<T> last = lru.back();
                info.victim = last.tag;
                info.victim_dirty = last.dirty;
                lru.pop_back();
                index_lookup.erase(last.tag);
            } else { // cache has empty space
                info.has_victim = false;
            }

            metadata<T> new_entry = {x, false};
            lru.push_front(new_entry);
            index_lookup[x] = lru.begin();
        } else { // present in cache
            info.present = true;
            metadata<T> old_entry = *(index_lookup[x]);
            lru.erase(index_lookup[x]);
            lru.push_front(old_entry);
            index_lookup[x] = lru.begin();
        }

        return info;
    }
    
    access_info<T> write(T x) {
        access_info<T> info;
        if (index_lookup.find(x) == index_lookup.end()) { // not present in cache
            info.present = false;
            
            if (lru.size() == size) { // cache is full
                info.has_victim = true;

                // delete LRU element
                metadata<T> last = lru.back();
                info.victim = last.tag;
                info.victim_dirty = last.dirty;
                lru.pop_back();
                index_lookup.erase(last.tag);
            } else { // cache has empty space
                info.has_victim = false;
            }

            metadata<T> new_entry = {x, true};
            lru.push_front(new_entry);
            index_lookup[x] = lru.begin();
        } else { // present in cache
            info.present = true;
            lru.erase(index_lookup[x]);
            metadata<T> new_entry = {x, true};
            lru.push_front(new_entry);
            index_lookup[x] = lru.begin();
        }

        return info;
    }

    void display() {
        for (auto it=lru.begin(); it!=lru.end(); it++) {
            std::cout << "tag = " << it->tag << " dirty = " << it->dirty << std::endl;
        }
    }

    void clear() {
        lru.clear();
        index_lookup.clear();
    }
};

