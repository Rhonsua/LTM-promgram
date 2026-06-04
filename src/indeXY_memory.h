#pragma once
#ifndef INDEXY_MEMORY_H
#define INDEXY_MEMORY_H

#include "common_def.h"

#include "bplus_tree.h"
#include "r_tree.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>
#include "log.h"   // 新增日志头文件

enum class IndexType {
    B_PLUS_TREE,
    R_TREE
};

enum class MemoryState {
    HOT,
    COLD,
    DISK
};

struct IndexEntry {
    uint64_t key;
    std::vector<double> dims;
    PhyAddr phy_addr;
    MemoryState state;
    IndexType type;

    bool isValid() const {
        return phy_addr.isValid() && (type == IndexType::B_PLUS_TREE || !dims.empty());
    }
};

class LRUCache {
private:
    uint64_t max_memory_bytes;
    uint64_t current_usage;
    std::mutex mtx;

    using CacheList = std::list<uint64_t>;
    using CacheMap = std::unordered_map<uint64_t, std::pair<IndexEntry, CacheList::iterator>>;
    CacheList lru_list;
    CacheMap cache_map;

    uint64_t calcEntrySize(const IndexEntry& entry) {
        return sizeof(uint64_t) + entry.dims.size() * sizeof(double) + sizeof(PhyAddr);
    }

    void evictColdData() {
        // 调用者已持有锁
        while (current_usage > max_memory_bytes && !lru_list.empty()) {
            uint64_t evict_key = lru_list.back();
            auto& entry_pair = cache_map[evict_key];
            IndexEntry& entry = entry_pair.first;

            entry.state = MemoryState::DISK;
            current_usage -= calcEntrySize(entry);

            cache_map.erase(evict_key);
            lru_list.pop_back();

            LOG_INFO << "[LRU] 淘汰冷数据：key=" << evict_key << "，剩余内存=" << current_usage << "字节" << std::endl;
        }
    }

public:
    explicit LRUCache(uint64_t max_mem) : max_memory_bytes(max_mem), current_usage(0) {}

    bool put(const IndexEntry& entry) {
        std::lock_guard<std::mutex> lock(mtx);
        uint64_t entry_size = calcEntrySize(entry);

        while (current_usage + entry_size > max_memory_bytes && !lru_list.empty()) {
            evictColdData();
        }

        if (current_usage + entry_size > max_memory_bytes) {
            LOG_INFO << "[LRU] 内存不足，标记key=" << entry.key << "为冷数据（落盘）" << std::endl;
            return false;
        }

        lru_list.remove(entry.key);
        lru_list.push_front(entry.key);
        cache_map[entry.key] = { entry, lru_list.begin() };
        current_usage += entry_size;
        return true;
    }

    IndexEntry get(uint64_t key, IndexType type) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cache_map.find(key);
        if (it == cache_map.end() || it->second.first.type != type) {
            return IndexEntry{};  // 空条目（无效）
        }

        lru_list.erase(it->second.second);
        lru_list.push_front(key);
        it->second.second = lru_list.begin();

        return it->second.first;
    }

    uint64_t getMemoryUsage() const { return current_usage; }
};

class IndeXYMemoryIndex {
private:
    std::unique_ptr<BPTree> bplus_tree;
    std::unique_ptr<RTree> r_tree;
    std::shared_ptr<LRUCache> lru_cache;
    uint32_t r_tree_dim;
    uint32_t default_order;

public:
    IndeXYMemoryIndex(uint64_t max_mem_bytes, uint32_t r_dim = 2, uint32_t order = 5)
        : r_tree_dim(r_dim), default_order(order) {
        bplus_tree = std::make_unique<BPTree>(default_order);
        bplus_tree->init();
        r_tree = std::make_unique<RTree>(r_tree_dim, default_order);
        r_tree->init();

        lru_cache = std::make_shared<LRUCache>(max_mem_bytes);
        LOG_INFO << "[IndeXY] 初始化完成：内存上限=" << max_mem_bytes << "字节，R树维度=" << r_dim << std::endl;
    }

    bool insert(const IndexEntry& entry) {
        if (!entry.isValid()) {
            LOG_ERROR << "[IndeXY] 插入失败：无效索引项" << std::endl;
            return false;
        }

        bool is_hot = lru_cache->put(entry);

        if (entry.type == IndexType::B_PLUS_TREE) {
            return bplus_tree->insert(entry.key, entry.phy_addr);
        }
        else if (entry.type == IndexType::R_TREE) {
            SpatialEntry r_entry;
            r_entry.id = entry.key;
            r_entry.phy_addr = entry.phy_addr;
            r_entry.mbr.dimension = entry.dims.size();
            r_entry.mbr.min_vals = entry.dims;
            r_entry.mbr.max_vals = entry.dims;
            return r_tree->insert(r_entry);
        }

        return false;
    }

    std::vector<IndexEntry> query(const IndexEntry& query_entry) {
        std::vector<IndexEntry> results;

        if (query_entry.type == IndexType::B_PLUS_TREE) {
            PhyAddr phy_addr = bplus_tree->search(query_entry.key);
            if (phy_addr.isValid()) {
                IndexEntry res;
                res.key = query_entry.key;
                res.phy_addr = phy_addr;
                res.state = lru_cache->get(query_entry.key, IndexType::B_PLUS_TREE).state;
                res.type = IndexType::B_PLUS_TREE;
                results.push_back(res);
            }
        }
        else if (query_entry.type == IndexType::R_TREE) {
            MBR query_mbr;
            query_mbr.dimension = query_entry.dims.size();
            query_mbr.min_vals = query_entry.dims;
            query_mbr.max_vals = query_entry.dims;

            std::vector<SpatialEntry> r_results;
            if (r_tree->search(query_mbr, r_results)) {
                for (const auto& r_entry : r_results) {
                    IndexEntry res;
                    res.key = r_entry.id;
                    res.dims = r_entry.mbr.min_vals;
                    res.phy_addr = r_entry.phy_addr;
                    res.state = lru_cache->get(r_entry.id, IndexType::R_TREE).state;
                    res.type = IndexType::R_TREE;
                    results.push_back(res);
                }
            }
        }

        return results;
    }

    void clear() {
        bplus_tree->clear();
        r_tree->clear();
        LOG_INFO << "[IndeXY] 所有索引已清空" << std::endl;
    }

    uint64_t getMemoryUsage() const {
        return lru_cache->getMemoryUsage();
    }
};

#endif // INDEXY_MEMORY_H