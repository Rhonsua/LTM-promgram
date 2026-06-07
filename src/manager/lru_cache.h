#pragma once
#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include <list>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <cstddef>

template<typename Key, typename Value>
class LRUCache {
private:
    size_t capacity;
    std::list<std::pair<Key, Value>> items;
    std::unordered_map<Key, typename decltype(items)::iterator> map;
    mutable std::mutex mtx;

    std::function<void(const Key&, const Value&)> evict_callback;

public:
    explicit LRUCache(size_t cap, std::function<void(const Key&, const Value&)> cb = nullptr)
        : capacity(cap), evict_callback(cb) {
    }

    void put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = map.find(key);
        if (it != map.end()) {
            it->second->second = value;
            items.splice(items.begin(), items, it->second);
            return;
        }
        if (items.size() >= capacity) {
            auto last = items.back();
            if (evict_callback) evict_callback(last.first, last.second);
            map.erase(last.first);
            items.pop_back();
        }
        items.emplace_front(key, value);
        map[key] = items.begin();
    }

    bool get(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = map.find(key);
        if (it == map.end()) return false;
        items.splice(items.begin(), items, it->second);
        value = it->second->second;
        return true;
    }

    bool erase(const Key& key) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = map.find(key);
        if (it == map.end()) return false;
        items.erase(it->second);
        map.erase(it);
        return true;
    }

    bool contains(const Key& key) {
        std::lock_guard<std::mutex> lock(mtx);
        return map.find(key) != map.end();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return items.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        items.clear();
        map.clear();
    }
};

#endif // LRU_CACHE_H