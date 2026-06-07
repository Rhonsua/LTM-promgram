#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#pragma once
#include "hybrid_index.h"
#include "lru_cache.h"
#include "cold_hot_manager.h"
#include "index_interface.h"
#include <mutex>
#include <vector>
#include <unordered_map>

class MemoryManager {
private:
    HybridIndex                    index_;
    LRUCache<uint64_t, IndexEntry>  cache_;
    ColdHotManager<uint64_t>        hotCold_;
    bool                            enableCache_;
    mutable IndexStats              stats_;
    mutable std::mutex              statsMutex_;
    mutable std::mutex              cacheMutex_;
    bool                            initialized_ = false;

    // key -> id 映射，用于等值查询时快速定位缓存
    std::unordered_map<uint64_t, uint64_t> key_to_id_;

    void updateCache(const IndexEntry& entry);
    std::optional<IndexEntry> getFromCache(uint64_t id);

public:
    MemoryManager(IndexType type = IndexType::HYBRID,
        bool useCache = true,
        size_t cacheSize = 10000,
        uint32_t btreeOrder = 5,
        uint32_t rtreeDim = 2,
        uint32_t coldThreshold = 2,
        double   decayFactor = 0.5);

    void init();
    bool isInitialized() const { return initialized_; }

    bool insert(const IndexEntry& entry);
    size_t bulkInsert(const std::vector<IndexEntry>& entries);

    QueryIterator query(const QueryCondition& condition);

    std::optional<IndexEntry> findById(uint64_t id);
    std::optional<PhyAddr> findByKey(uint64_t key) const;
    std::vector<std::pair<uint64_t, PhyAddr>> rangeSearchByKey(uint64_t start, uint64_t end) const;

    bool remove(uint64_t key);

    size_t precleanColdData();
    void decayAccessFrequency();
    void clear();

    IndexStats getStats() const;
    void printStats() const;

    std::vector<uint64_t> getAllKeys() const;
};
#endif