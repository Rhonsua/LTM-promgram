#ifndef __R_TREE_H__
#define __R_TREE_H__

#include <iostream>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <memory>
#include <vector>
#include <cmath>
<<<<<<< HEAD
#include"common_def.h"

const uint32_t DEFAULT_R_TREE_ORDER = 5;

// 动态维度MBR
=======
#include "log.h"   // 新增日志头文件
#include "common_def.h"

constexpr uint32_t DEFAULT_R_TREE_ORDER = 5;

struct MBR {
    std::vector<double> min_vals;
    std::vector<double> max_vals;
    uint32_t dimension;

    MBR(uint32_t dim = 0) : dimension(dim) {
        if (dim > 0) {
            min_vals.resize(dim, 0.0);
            max_vals.resize(dim, 0.0);
        }
    }

    MBR(const std::vector<double>& min, const std::vector<double>& max)
        : min_vals(min), max_vals(max), dimension(min.size()) {
        if (min_vals.size() != max_vals.size()) {
            dimension = 0;
        }
    }

    bool isValid() const {
        if (dimension == 0) return false;
        if (min_vals.size() != dimension || max_vals.size() != dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > max_vals[i]) return false;
        }
        return true;
    }

    uint32_t getDim() const { return dimension; }

    double getVolume() const {
        if (!isValid()) return 0.0;
        double volume = 1.0;
        for (uint32_t i = 0; i < dimension; ++i) {
            double length = max_vals[i] - min_vals[i];
            if (length < 0) return 0.0;
            volume *= length;
        }
        return volume;
    }

    MBR merge(const MBR& other) const {
        if (dimension != other.dimension) return MBR(0);
        MBR result(dimension);
        for (uint32_t i = 0; i < dimension; ++i) {
            result.min_vals[i] = std::min(min_vals[i], other.min_vals[i]);
            result.max_vals[i] = std::max(max_vals[i], other.max_vals[i]);
        }
        return result;
    }

    double getExpansionVolume(const MBR& other) const {
        if (dimension != other.dimension) return 0.0;
        MBR merged = this->merge(other);
        return merged.getVolume() - this->getVolume();
    }

    bool contains(const MBR& other) const {
        if (!isValid() || !other.isValid() || dimension != other.dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > other.min_vals[i] || max_vals[i] < other.max_vals[i]) return false;
        }
        return true;
    }

    bool intersects(const MBR& other) const {
        if (!isValid() || !other.isValid() || dimension != other.dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > other.max_vals[i] || max_vals[i] < other.min_vals[i]) return false;
        }
        return true;
    }
};

struct SpatialEntry {
    uint64_t id = INVALID_ID;
    MBR mbr;
    PhyAddr phy_addr;

    SpatialEntry() = default;
    SpatialEntry(uint32_t dim) : mbr(dim) {}

    bool isValid() const {
        return id != INVALID_ID && mbr.isValid() && phy_addr.isValid();
    }

    uint32_t getDim() const { return mbr.getDim(); }
};

struct RTreeNode {
    bool is_leaf = true;
    std::weak_ptr<RTreeNode> parent;
    std::vector<MBR> mbrs;
    std::vector<SpatialEntry> entries;
    std::vector<std::shared_ptr<RTreeNode>> children;
    uint32_t dimension;

    RTreeNode(uint32_t dim = 0) : dimension(dim) {}

    bool isFull(uint32_t order) const {
        return is_leaf ? entries.size() >= order : children.size() >= order;
    }

    bool isEmpty() const {
        return is_leaf ? entries.empty() : children.empty();
    }

    bool clear() {
        mbrs.clear();
        entries.clear();
        children.clear();
        parent.reset();
        return true;
    }

    bool updateMBR() {
        if (is_leaf) {
            mbrs.clear();
            for (const auto& entry : entries) {
                if (entry.getDim() != dimension) return false;
                mbrs.push_back(entry.mbr);
            }
        }
        else {
            mbrs.clear();
            for (const auto& child : children) {
                if (!child->mbrs.empty()) {
                    MBR child_mbr = child->mbrs[0];
                    for (size_t i = 1; i < child->mbrs.size(); ++i) {
                        child_mbr = child_mbr.merge(child->mbrs[i]);
                    }
                    mbrs.push_back(child_mbr);
                }
            }
        }
        return true;
    }

    uint32_t getDim() const { return dimension; }
};

class RTree {
private:
    std::shared_ptr<RTreeNode> root;
    uint32_t order;
    uint32_t dimension;
    bool isinited_ = false;

    bool chooseLeaf(std::shared_ptr<RTreeNode> node,
        const SpatialEntry& entry,
        std::shared_ptr<RTreeNode>& result) const;
    bool adjustTree(std::shared_ptr<RTreeNode> node);
    bool splitNode(std::shared_ptr<RTreeNode> node,
        std::shared_ptr<RTreeNode>& new_node1,
        std::shared_ptr<RTreeNode>& new_node2);
    bool searchRecursive(std::shared_ptr<RTreeNode> node,
        const MBR& query_mbr,
        std::vector<SpatialEntry>& results) const;

public:
    explicit RTree(uint32_t dim = 2, uint32_t treeOrder = DEFAULT_R_TREE_ORDER)
        : dimension(dim), order(treeOrder) {
        LOG_INFO << "[RTree] 创建R树，维度：" << dimension << "，阶数：" << order << std::endl;
    }

    bool init();
    bool isinited() const { return isinited_; }
    uint32_t getDimension() const { return dimension; }
    uint32_t getOrder() const { return order; }

    bool insert(const SpatialEntry& entry);
    bool search(const MBR& query_mbr, std::vector<SpatialEntry>& results) const;
    bool rangeQuery(const MBR& query_mbr, std::vector<SpatialEntry>& results) const;
    bool pointQuery(const std::vector<double>& point, std::vector<SpatialEntry>& results) const;

    bool clear();
    bool validate() const;
    bool getStatistics(uint32_t& leaf_count, uint32_t& internal_count,
        uint32_t& total_entries, uint32_t& tree_height) const;
    bool printTree(std::ostream& os = std::cout) const;
};

#endif // __R_TREE_H__