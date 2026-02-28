#ifndef __INDEX_INTERFACE_H__
#define __INDEX_INTERFACE_H__

#include <memory>
#include <vector>
#include <functional>
#include <optional>
#include <mutex>
#include <unordered_map>
#include "r_tree.h"
#include "bplus_tree.h"

// 查询条件类型
enum class QueryType {
    EQUAL,      // 等值查询
    RANGE,      // 范围查询
    SPATIAL     // 空间查询
};

// 查询条件基类
struct QueryCondition {
    QueryType type;
    virtual ~QueryCondition() = default;
};

// 一维等值查询条件
struct EqualCondition : QueryCondition {
    uint64_t key;
    
    EqualCondition() { type = QueryType::EQUAL; }
    explicit EqualCondition(uint64_t k) : key(k) { type = QueryType::EQUAL; }
};

// 一维范围查询条件
struct RangeCondition : QueryCondition {
    uint64_t start_key;
    uint64_t end_key;
    bool include_start;
    bool include_end;
    
    RangeCondition() { type = QueryType::RANGE; }
    RangeCondition(uint64_t start, uint64_t end, bool include_s = true, bool include_e = true)
        : start_key(start), end_key(end), include_start(include_s), include_end(include_e) {
        type = QueryType::RANGE;
    }
};

// 多维空间查询条件
template<uint32_t DIM>
struct SpatialCondition : QueryCondition {
    MBR<DIM> query_mbr;
    
    SpatialCondition() { type = QueryType::SPATIAL; }
    explicit SpatialCondition(const MBR<DIM>& mbr) : query_mbr(mbr) { 
        type = QueryType::SPATIAL; 
    }
};

// 统一的数据条目（结合一维和多维信息）
template<uint32_t DIM>
struct IndexEntry {
    uint64_t id;                    // 数据项唯一ID
    uint64_t key;                    // 一维索引键值（如时间戳、ID等）
    MBR<DIM> mbr;                    // 多维空间MBR
    PhyAddr phy_addr;                // 物理存储地址
    
    IndexEntry() : id(0), key(0) {}
    
    bool isValid() const {
        return id != 0 && key != INVALID_KEY && mbr.isValid() && phy_addr.isValid();
    }
    
    // 转换为R树的空间条目
    SpatialEntry<DIM> toSpatialEntry() const {
        SpatialEntry<DIM> spatial_entry;
        spatial_entry.id = id;
        spatial_entry.mbr = mbr;
        spatial_entry.phy_addr = phy_addr;
        return spatial_entry;
    }
};

// 索引类型枚举
enum class IndexType {
    BTREE_ONLY,     // 仅B+树索引
    RTREE_ONLY,     // 仅R树索引
    HYBRID          // 混合索引（同时维护两种索引）
};

// 索引统计信息
struct IndexStats {
    size_t total_entries;      // 总条目数
    size_t btree_nodes;        // B+树节点数
    size_t rtree_nodes;        // R树节点数
    size_t cache_hits;         // 缓存命中次数
    size_t cache_misses;       // 缓存未命中次数
    
    IndexStats() : total_entries(0), btree_nodes(0), rtree_nodes(0),
                   cache_hits(0), cache_misses(0) {}
};

// 查询结果迭代器
template<uint32_t DIM>
class QueryIterator {
private:
    std::vector<IndexEntry<DIM>> results;
    size_t current_pos;
    
public:
    QueryIterator() : current_pos(0) {}
    
    void addResult(const IndexEntry<DIM>& entry) {
        results.push_back(entry);
    }
    
    bool hasNext() const {
        return current_pos < results.size();
    }
    
    IndexEntry<DIM> next() {
        if (current_pos < results.size()) {
            return results[current_pos++];
        }
        return IndexEntry<DIM>();
    }
    
    size_t size() const { return results.size(); }
    void reset() { current_pos = 0; }
    
    const std::vector<IndexEntry<DIM>>& getResults() const { return results; }
};

#endif // __INDEX_INTERFACE_H__
