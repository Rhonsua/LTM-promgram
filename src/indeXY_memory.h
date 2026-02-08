#pragma once
#ifndef INDEXY_MEMORY_H
#define INDEXY_MEMORY_H

#include "bplus_tree.h"
#include "r_tree.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>
#include <vector>
#include <cstdint>

// ========== 1. 统一索引元数据（复用你的结构体） ==========
// 索引类型（区分B+树/R树）
enum class IndexType {
    B_PLUS_TREE,  // 一维
    R_TREE        // 多维
};

// 内存状态（larger than memory 核心）
enum class MemoryState {
    HOT,    // 热点：驻留内存
    COLD,   // 冷数据：标记落盘（可淘汰）
    DISK    // 已落盘：仅保留元数据
};

// 统一索引项（关联你的B+树/R树元数据）
struct IndexEntry {
    uint64_t key;               // 一维主键（B+树）/ 多维数据ID（R树）
    std::vector<double> dims;   // 多维坐标（R树用，一维为空）
    PhyAddr phy_addr;           // 复用你的物理地址结构体
    MemoryState state;          // 内存状态
    IndexType type;             // 索引类型

    // 有效性校验（复用你的PhyAddr::isValid()）
    bool isValid() const {
        return phy_addr.isValid() && (type == IndexType::B_PLUS_TREE || !dims.empty());
    }
};

// ========== 2. LRU内存池（适配larger than memory） ==========
class LRUCache {
private:
    uint64_t max_memory_bytes;  // 内存上限（如1GB）
    uint64_t current_usage;     // 当前内存使用量
    std::mutex mtx;             // 线程安全锁

    // LRU核心结构：key -> （IndexEntry + 链表迭代器）
    using CacheList = std::list<uint64_t>;
    using CacheMap = std::unordered_map<uint64_t, std::pair<IndexEntry, CacheList::iterator>>;
    CacheList lru_list;         // 访问顺序链表（头部=最新，尾部=最久）
    CacheMap cache_map;         // 索引项映射

    // 计算单个索引项的内存占用
    uint64_t calcEntrySize(const IndexEntry& entry) {
        return sizeof(uint64_t) + entry.dims.size() * sizeof(double) + sizeof(PhyAddr);
    }

    // 淘汰最久未使用的冷数据
    void evictColdData() {
        std::lock_guard<std::mutex> lock(mtx);
        while (current_usage > max_memory_bytes && !lru_list.empty()) {
            // 淘汰链表尾部（最久未使用）
            uint64_t evict_key = lru_list.back();
            auto& entry_pair = cache_map[evict_key];
            IndexEntry& entry = entry_pair.first;

            // 标记为已落盘，释放内存
            entry.state = MemoryState::DISK;
            current_usage -= calcEntrySize(entry);

            // 从缓存中移除（仅保留元数据可扩展）
            cache_map.erase(evict_key);
            lru_list.pop_back();

            std::cout << "[LRU] 淘汰冷数据：key=" << evict_key << "，剩余内存=" << current_usage << "字节" << std::endl;
        }
    }

public:
    // 初始化：指定内存上限（字节）
    explicit LRUCache(uint64_t max_mem) : max_memory_bytes(max_mem), current_usage(0) {}

    // 添加/更新索引项（标记为热点）
    bool put(const IndexEntry& entry) {
        std::lock_guard<std::mutex> lock(mtx);
        uint64_t entry_size = calcEntrySize(entry);

        // 内存不足时先淘汰冷数据
        if (current_usage + entry_size > max_memory_bytes) {
            evictColdData();
        }

        // 仍不足则标记为冷数据（落盘）
        if (current_usage + entry_size > max_memory_bytes) {
            std::cout << "[LRU] 内存不足，标记key=" << entry.key << "为冷数据（落盘）" << std::endl;
            return false;
        }

        // 加入LRU缓存（标记为热点）
        lru_list.remove(entry.key);  // 移除旧位置
        lru_list.push_front(entry.key);  // 加入头部（最新访问）
        cache_map[entry.key] = { entry, lru_list.begin() };
        current_usage += entry_size;
        return true;
    }

    // 查询索引项（更新访问顺序）
    IndexEntry get(uint64_t key, IndexType type) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cache_map.find(key);
        if (it == cache_map.end() || it->second.first.type != type) {
            return IndexEntry{};  // 空条目（无效）
        }

        // 更新LRU顺序（移到头部）
        lru_list.erase(it->second.second);
        lru_list.push_front(key);
        it->second.second = lru_list.begin();

        return it->second.first;
    }

    // 获取当前内存使用量
    uint64_t getMemoryUsage() const { return current_usage; }
};

// ========== 3. indeXY内存层（整合B+树/R树） ==========
class IndeXYMemoryIndex {
private:
    std::unique_ptr<BPTree> bplus_tree;  // 你的B+树实例
    std::unique_ptr<RTree> r_tree;        // 你的R树实例
    std::shared_ptr<LRUCache> lru_cache;  // LRU内存池
    uint32_t r_tree_dim;                 // R树维度（默认2）
    uint32_t default_order;              // 默认阶数（5）

public:
    // 构造函数：初始化内存池+索引
    IndeXYMemoryIndex(uint64_t max_mem_bytes, uint32_t r_dim = 2, uint32_t order = 5)
        : r_tree_dim(r_dim), default_order(order) {
        // 初始化你的B+树/R树
        bplus_tree = std::make_unique<BPTree>(default_order);
        bplus_tree->init();
        r_tree = std::make_unique<RTree>(r_tree_dim, default_order);
        r_tree->init();

        // 初始化LRU内存池（限制内存上限）
        lru_cache = std::make_shared<LRUCache>(max_mem_bytes);
        std::cout << "[IndeXY] 初始化完成：内存上限=" << max_mem_bytes << "字节，R树维度=" << r_dim << std::endl;
    }

    // ========== 统一插入接口 ==========
    bool insert(const IndexEntry& entry) {
        if (!entry.isValid()) {
            std::cerr << "[IndeXY] 插入失败：无效索引项" << std::endl;
            return false;
        }

        // 1. 内存池标记热点/冷数据
        bool is_hot = lru_cache->put(entry);

        // 2. 维度分流：插入底层索引
        if (entry.type == IndexType::B_PLUS_TREE) {
            // 一维：插入你的B+树（复用PhyAddr）
            return bplus_tree->insert(entry.key, entry.phy_addr);
        }
        else if (entry.type == IndexType::R_TREE) {
            // 多维：转换为你的SpatialEntry，插入R树
            SpatialEntry r_entry;
            r_entry.id = entry.key;
            r_entry.phy_addr = entry.phy_addr;
            r_entry.mbr.dimension = entry.dims.size();
            r_entry.mbr.min_vals = entry.dims;  // 简化：点数据（实际可传范围）
            r_entry.mbr.max_vals = entry.dims;
            return r_tree->insert(r_entry);
        }

        return false;
    }

    // ========== 统一查询接口 ==========
    std::vector<IndexEntry> query(const IndexEntry& query_entry) {
        std::vector<IndexEntry> results;

        if (query_entry.type == IndexType::B_PLUS_TREE) {
            // 一维：查询你的B+树
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
            // 多维：查询你的R树（MBR范围）
            MBR query_mbr;
            query_mbr.dimension = query_entry.dims.size();
            query_mbr.min_vals = query_entry.dims;  // 简化：点查询（实际可传范围）
            query_mbr.max_vals = query_entry.dims;

            auto r_results = r_tree->search(query_mbr);
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

        return results;
    }

    // ========== 辅助接口 ==========
    // 清空所有索引
    void clear() {
        bplus_tree->clear();
        r_tree->clear();
        std::cout << "[IndeXY] 所有索引已清空" << std::endl;
    }

    // 获取当前内存使用量
    uint64_t getMemoryUsage() const {
        return lru_cache->getMemoryUsage();
    }
};

#endif // INDEXY_MEMORY_H