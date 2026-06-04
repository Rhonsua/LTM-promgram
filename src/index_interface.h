#ifndef __INDEX_INTERFACE_H__
#define __INDEX_INTERFACE_H__

#include <memory>
#include <vector>
#include <functional>
#include <optional>
#include <mutex>
#include <unordered_map>
#include "r_tree.h"          // 你的非模板 R 树
#include "bplus_tree.h"

// ---------- 查询条件 ----------
enum class QueryType {
    EQUAL,      // 等值查询
    RANGE,      // 范围查询
    SPATIAL     // 空间查询
};

struct QueryCondition {
    QueryType type;
    virtual ~QueryCondition() = default;
};

struct EqualCondition : QueryCondition {
    uint64_t key;
    EqualCondition() { type = QueryType::EQUAL; }
    explicit EqualCondition(uint64_t k) : key(k) { type = QueryType::EQUAL; }
};

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

// 注意：这里直接使用 r_tree.h 中的 MBR，非模板
struct SpatialCondition : QueryCondition {
    MBR query_mbr;
    SpatialCondition() { type = QueryType::SPATIAL; }
    explicit SpatialCondition(const MBR& mbr) : query_mbr(mbr) { type = QueryType::SPATIAL; }
};

// ---------- 统一数据条目 ----------
struct IndexEntry {
    uint64_t id;                 // 数据项唯一ID
    uint64_t key;                // 一维键值
    MBR mbr;                     // 多维空间信息（直接使用 r_tree.h 的 MBR）
    PhyAddr phy_addr;            // 物理存储地址

    IndexEntry() : id(0), key(0) {}

    bool isValid() const {
        return id != 0 && key != INVALID_KEY && mbr.isValid() && phy_addr.isValid();
    }

    // 转换为 R 树的空间条目
    SpatialEntry toSpatialEntry() const {
        SpatialEntry spatial_entry;
        spatial_entry.id = id;
        spatial_entry.mbr = mbr;
        spatial_entry.phy_addr = phy_addr;
        return spatial_entry;
    }
};

// ---------- 索引类型与统计 ----------
enum class IndexType {
    BTREE_ONLY,     // 仅B+树
    RTREE_ONLY,     // 仅R树
    HYBRID          // 混合索引
};

struct IndexStats {
    size_t total_entries = 0;
    size_t btree_nodes = 0;
    size_t rtree_nodes = 0;
    size_t cache_hits = 0;
    size_t cache_misses = 0;
};

// ---------- 查询结果迭代器 ----------
class QueryIterator {
private:
    std::vector<IndexEntry> results;
    size_t current_pos = 0;

public:
    void addResult(const IndexEntry& entry) {
        results.push_back(entry);
    }

    bool hasNext() const {
        return current_pos < results.size();
    }

    IndexEntry next() {
        if (current_pos < results.size())
            return results[current_pos++];
        return IndexEntry();
    }

    size_t size() const { return results.size(); }
    void reset() { current_pos = 0; }

    const std::vector<IndexEntry>& getResults() const { return results; }
};

#endif // __INDEX_INTERFACE_H__