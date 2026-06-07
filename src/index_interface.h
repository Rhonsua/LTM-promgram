#pragma once

#include "r_tree.h"          // MBR, SpatialEntry
#include "bplus_tree.h"
#include <memory>
#include <vector>
#include <functional>
#include <optional>
#include <mutex>
#include <unordered_map>

enum class QueryType {
    EQUAL,
    RANGE,
    SPATIAL
};

struct QueryCondition {
    QueryType type = QueryType::EQUAL;   // 默认初始化为 EQUAL
    virtual ~QueryCondition() = default;
};

struct EqualCondition : QueryCondition {
    uint64_t key = 0;                    // 初始化为 0
    EqualCondition() { type = QueryType::EQUAL; }
    explicit EqualCondition(uint64_t k) : key(k) { type = QueryType::EQUAL; }
};

struct RangeCondition : QueryCondition {
    uint64_t start_key = 0;
    uint64_t end_key = 0;
    bool include_start = true;
    bool include_end = true;
    RangeCondition() { type = QueryType::RANGE; }
    RangeCondition(uint64_t start, uint64_t end, bool include_s = true, bool include_e = true)
        : start_key(start), end_key(end), include_start(include_s), include_end(include_e) {
        type = QueryType::RANGE;
    }
};

struct SpatialCondition : QueryCondition {
    MBR query_mbr;                       // MBR 有自己的默认构造函数
    SpatialCondition() { type = QueryType::SPATIAL; }
    explicit SpatialCondition(const MBR& mbr) : query_mbr(mbr) { type = QueryType::SPATIAL; }
};

struct IndexEntry {
    uint64_t id = 0;
    uint64_t key = 0;
    MBR mbr;
    PhyAddr phy_addr;
    IndexEntry() : id(0), key(0) {}
    bool isValid() const {
        return id != 0 && key != INVALID_KEY && mbr.isValid() && phy_addr.isValid();
    }
    SpatialEntry toSpatialEntry() const {
        SpatialEntry spatial_entry;
        spatial_entry.id = id;
        spatial_entry.mbr = mbr;
        spatial_entry.phy_addr = phy_addr;
        return spatial_entry;
    }
};

enum class IndexType {
    BTREE_ONLY,
    RTREE_ONLY,
    HYBRID
};

struct IndexStats {
    size_t total_entries = 0;
    size_t btree_nodes = 0;
    size_t rtree_nodes = 0;
    size_t cache_hits = 0;
    size_t cache_misses = 0;
};

class QueryIterator {
private:
    std::vector<IndexEntry> results;
    size_t current_pos = 0;
public:
    void addResult(const IndexEntry& entry) { results.push_back(entry); }
    bool hasNext() const { return current_pos < results.size(); }
    IndexEntry next() {
        if (current_pos < results.size())
            return results[current_pos++];
        return IndexEntry();
    }
    size_t size() const { return results.size(); }
    void reset() { current_pos = 0; }
    const std::vector<IndexEntry>& getResults() const { return results; }
};