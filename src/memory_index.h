#ifndef __MEMORY_INDEX_H__
#define __MEMORY_INDEX_H__

#include "index_interface.h"
#include "r_tree.h"
#include "bplus_tree.h"
#include <iostream>
#include <iomanip>
#include <mutex>

template<uint32_t DIM>
class MemoryIndex {
private:
    // 基础索引结构
    std::unique_ptr<BPTree> btree_index;
    std::unique_ptr<RTree<DIM>> rtree_index;
    
    // ID到条目的映射缓存（用于快速访问）
    std::unordered_map<uint64_t, IndexEntry<DIM>> id_cache;
    
    // 索引配置
    IndexType index_type;
    bool enable_cache;
    size_t max_cache_size;
    
    // 统计信息
    mutable IndexStats stats;
    mutable std::mutex stats_mutex;
    mutable std::mutex cache_mutex;
    
    // 初始化标记
    bool isinited_;
    
    // 更新缓存
    void updateCache(const IndexEntry<DIM>& entry) {
        if (!enable_cache) return;
        
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        // 简单的缓存淘汰策略（如果满了，删除最早的一个）
        if (id_cache.size() >= max_cache_size && id_cache.find(entry.id) == id_cache.end()) {
            auto it = id_cache.begin();
            if (it != id_cache.end()) {
                id_cache.erase(it);
            }
        }
        
        id_cache[entry.id] = entry;
    }
    
    // 从缓存获取
    std::optional<IndexEntry<DIM>> getFromCache(uint64_t id) const {
        if (!enable_cache) return std::nullopt;
        
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        auto it = id_cache.find(id);
        if (it != id_cache.end()) {
            stats.cache_hits++;
            return it->second;
        }
        
        stats.cache_misses++;
        return std::nullopt;
    }
    
    // 从R树条目构建IndexEntry（需要从B+树获取key）
    IndexEntry<DIM> buildFromSpatialEntry(const SpatialEntry<DIM>& spatial_entry) const {
        IndexEntry<DIM> entry;
        entry.id = spatial_entry.id;
        entry.mbr = spatial_entry.mbr;
        entry.phy_addr = spatial_entry.phy_addr;
        
        // 如果有B+树索引，尝试获取key值
        if (btree_index) {
            // 注意：这里假设B+树使用id作为key来存储完整条目
            // 实际可能需要调整
            auto addr = btree_index->search(spatial_entry.id);
            if (addr.isValid()) {
                // 如果找到了，但我们无法从地址重建完整条目
                // 这是一个简化处理
                entry.key = spatial_entry.id; // 临时使用id作为key
            }
        }
        
        return entry;
    }

public:
    // 构造函数
    explicit MemoryIndex(IndexType type = IndexType::HYBRID, 
                        bool use_cache = true,
                        size_t cache_size = 10000,
                        uint32_t btree_order = 5)
        : index_type(type)
        , enable_cache(use_cache)
        , max_cache_size(cache_size)
        , isinited_(false) {
        
        // 根据类型初始化相应的索引
        if (type == IndexType::BTREE_ONLY || type == IndexType::HYBRID) {
            btree_index = std::make_unique<BPTree>(btree_order);
        }
        
        if (type == IndexType::RTREE_ONLY || type == IndexType::HYBRID) {
            rtree_index = std::make_unique<RTree<DIM>>();
        }
    }
    
    // 初始化
    void init() {
        if (isinited_) {
            std::cerr << "[MemoryIndex] 已经初始化" << std::endl;
            return;
        }
        
        if (btree_index) {
            btree_index->init();
        }
        
        if (rtree_index) {
            rtree_index->init();
        }
        
        id_cache.clear();
        isinited_ = true;
        
        std::cout << "[MemoryIndex] 初始化成功，维度: " << DIM 
                  << ", 索引类型: " << (index_type == IndexType::BTREE_ONLY ? "B+树" :
                      index_type == IndexType::RTREE_ONLY ? "R树" : "混合索引")
                  << ", 缓存: " << (enable_cache ? "开启" : "关闭")
                  << ", 缓存大小: " << max_cache_size
                  << std::endl;
    }
    
    // 检查是否初始化
    bool isinited() const { return isinited_; }
    
    // 插入数据
    bool insert(const IndexEntry<DIM>& entry) {
        if (!isinited_) {
            std::cerr << "[MemoryIndex] 错误：索引未初始化" << std::endl;
            return false;
        }
        
        if (!entry.isValid()) {
            std::cerr << "[MemoryIndex] 错误：无效的数据条目" << std::endl;
            return false;
        }
        
        bool success = true;
        
        // 插入B+树索引（使用key作为B+树的主键）
        if (btree_index) {
            // 这里我们需要将完整的条目存储到B+树
            // 由于B+树的search返回的是PhyAddr，我们需要额外的映射
            // 简化方案：使用一个全局ID到条目的映射，B+树只存储ID到地址的映射
            // 但这里为了简化，我们假设B+树的values可以直接存储IndexEntry
            // 注意：实际B+树的values存储的是PhyAddr，这里需要调整
            // 临时方案：使用entry.id作为key，PhyAddr指向实际数据
            success = success && btree_index->insert(entry.key, entry.phy_addr);
        }
        
        // 插入R树索引
        if (rtree_index) {
            success = success && rtree_index->insert(entry.toSpatialEntry());
        }
        
        // 更新缓存
        if (success && enable_cache) {
            updateCache(entry);
        }
        
        // 更新统计信息
        if (success) {
            std::lock_guard<std::mutex> lock(stats_mutex);
            stats.total_entries++;
        }
        
        return success;
    }
    
    // 批量插入
    size_t bulkInsert(const std::vector<IndexEntry<DIM>>& entries) {
        size_t success_count = 0;
        
        for (const auto& entry : entries) {
            if (insert(entry)) {
                success_count++;
            }
        }
        
        std::cout << "[MemoryIndex] 批量插入完成: " << success_count 
                  << "/" << entries.size() << " 成功" << std::endl;
        return success_count;
    }
    
    // 根据查询条件执行查询
    QueryIterator<DIM> query(const QueryCondition& condition) const {
        QueryIterator<DIM> iterator;
        
        if (!isinited_) {
            std::cerr << "[MemoryIndex] 错误：索引未初始化" << std::endl;
            return iterator;
        }
        
        switch (condition.type) {
            case QueryType::EQUAL: {
                // 等值查询（使用B+树）
                const auto& eq_cond = static_cast<const EqualCondition&>(condition);
                if (btree_index) {
                    auto addr = btree_index->search(eq_cond.key);
                    if (addr.isValid()) {
                        // 这里需要根据地址获取完整条目
                        // 简化：从缓存查找
                        auto cached = getFromCache(eq_cond.key); // 假设key等于id
                        if (cached) {
                            iterator.addResult(*cached);
                        }
                    }
                }
                break;
            }
            
            case QueryType::RANGE: {
                // 范围查询（使用B+树）
                const auto& range_cond = static_cast<const RangeCondition&>(condition);
                if (btree_index) {
                    auto results = btree_index->rangeSearch(range_cond.start_key, range_cond.end_key);
                    for (const auto& [key, addr] : results) {
                        auto cached = getFromCache(key);
                        if (cached) {
                            iterator.addResult(*cached);
                        }
                    }
                }
                break;
            }
            
            case QueryType::SPATIAL: {
                // 空间查询（使用R树）
                const auto& spatial_cond = static_cast<const SpatialCondition<DIM>&>(condition);
                if (rtree_index) {
                    auto results = rtree_index->search(spatial_cond.query_mbr);
                    for (const auto& spatial_entry : results) {
                        // 从缓存获取完整条目
                        auto cached = getFromCache(spatial_entry.id);
                        if (cached) {
                            iterator.addResult(*cached);
                        } else {
                            // 如果不在缓存中，从空间条目构建
                            auto entry = buildFromSpatialEntry(spatial_entry);
                            iterator.addResult(entry);
                        }
                    }
                }
                break;
            }
        }
        
        return iterator;
    }
    
    // 根据ID查询
    std::optional<IndexEntry<DIM>> findById(uint64_t id) const {
        // 先查缓存
        auto cached = getFromCache(id);
        if (cached) {
            return cached;
        }
        
        // 如果不在缓存中，需要从索引查询
        // 简化实现：如果有关联的B+树，可以通过id查询
        if (btree_index) {
            auto addr = btree_index->search(id);
            if (addr.isValid()) {
                // 这里需要从地址加载完整条目
                // 暂时返回一个部分填充的条目
                IndexEntry<DIM> entry;
                entry.id = id;
                entry.key = id;
                entry.phy_addr = addr;
                return entry;
            }
        }
        
        return std::nullopt;
    }
    
    // 根据key查询（使用B+树）
    std::optional<PhyAddr> findByKey(uint64_t key) const {
        if (!btree_index) return std::nullopt;
        auto addr = btree_index->search(key);
        if (addr.isValid()) {
            return addr;
        }
        return std::nullopt;
    }
    
    // 范围查询（返回物理地址）
    std::vector<std::pair<uint64_t, PhyAddr>> rangeSearchByKey(uint64_t start, uint64_t end) const {
        if (!btree_index) return {};
        return btree_index->rangeSearch(start, end);
    }
    
    // 删除条目
    bool remove(uint64_t key) {
        if (!isinited_ || !btree_index) return false;
        
        // 先从B+树删除
        btree_index->remove(key);
        
        // 从缓存删除
        if (enable_cache) {
            std::lock_guard<std::mutex> lock(cache_mutex);
            id_cache.erase(key);
        }
        
        // 注意：R树的删除需要更复杂的处理
        // 这里简化处理，实际需要根据key找到对应的空间条目再删除
        
        return true;
    }
    
    // 清空索引
    void clear() {
        if (btree_index) {
            btree_index->clear();
        }
        
        if (rtree_index) {
            rtree_index->clear();
        }
        
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            id_cache.clear();
        }
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            stats = IndexStats();
        }
        
        isinited_ = false;
        std::cout << "[MemoryIndex] 已清空" << std::endl;
    }
    
    // 获取统计信息
    IndexStats getStats() const {
        std::lock_guard<std::mutex> lock(stats_mutex);
        return stats;
    }
    
    // 打印统计信息
    void printStats() const {
        auto s = getStats();
        std::cout << "\n========== 内存索引统计 ==========" << std::endl;
        std::cout << "维度: " << DIM << std::endl;
        std::cout << "索引类型: " << (index_type == IndexType::BTREE_ONLY ? "B+树" :
                                      index_type == IndexType::RTREE_ONLY ? "R树" : "混合索引") << std::endl;
        std::cout << "总条目数: " << s.total_entries << std::endl;
        std::cout << "B+树节点数: " << s.btree_nodes << std::endl;
        std::cout << "R树节点数: " << s.rtree_nodes << std::endl;
        std::cout << "缓存命中: " << s.cache_hits << std::endl;
        std::cout << "缓存未命中: " << s.cache_misses << std::endl;
        
        size_t total_queries = s.cache_hits + s.cache_misses;
        if (total_queries > 0) {
            double hit_rate = (double)s.cache_hits / total_queries * 100;
            std::cout << "缓存命中率: " << std::fixed << std::setprecision(2) 
                      << hit_rate << "%" << std::endl;
        }
        std::cout << "===================================" << std::endl;
    }
    
    // 获取所有B+树键值
    std::vector<uint64_t> getAllKeys() const {
        if (!btree_index) return {};
        return btree_index->getAllKeys();
    }
};

#endif // __MEMORY_INDEX_H__
