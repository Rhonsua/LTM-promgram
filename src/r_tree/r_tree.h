#ifndef R_TREE_H
#define R_TREE_H

#include <iostream>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <memory>
#include <vector>
#include <cmath>
#include <functional>
#include <queue>
#include <limits>
#include "log.h"
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
        : min_vals(min), max_vals(max), dimension(static_cast<uint32_t>(min.size())) {
        if (min_vals.size() != max_vals.size()) dimension = 0;
    }

    bool isValid() const {
        if (dimension == 0) return false;
        if (min_vals.size() != dimension || max_vals.size() != dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i)
            if (min_vals[i] > max_vals[i]) return false;
        return true;
    }

    uint32_t getDim() const { return dimension; }

    double getVolume() const {
        if (!isValid()) return 0.0;
        double volume = 1.0;
        for (uint32_t i = 0; i < dimension; ++i) {
            double len = max_vals[i] - min_vals[i];
            if (len < 0) return 0.0;
            volume *= len;
        }
        return volume;
    }

    MBR merge(const MBR& other) const {
        if (dimension != other.dimension) return MBR(0);
        MBR res(dimension);
        for (uint32_t i = 0; i < dimension; ++i) {
            res.min_vals[i] = std::min(min_vals[i], other.min_vals[i]);
            res.max_vals[i] = std::max(max_vals[i], other.max_vals[i]);
        }
        return res;
    }

    double getExpansionVolume(const MBR& other) const {
        if (dimension != other.dimension) return 0.0;
        MBR merged = this->merge(other);
        return merged.getVolume() - this->getVolume();
    }

    bool contains(const MBR& other) const {
        if (!isValid() || !other.isValid() || dimension != other.dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i)
            if (min_vals[i] > other.min_vals[i] || max_vals[i] < other.max_vals[i]) return false;
        return true;
    }

    bool intersects(const MBR& other) const {
        if (!isValid() || !other.isValid() || dimension != other.dimension) return false;
        for (uint32_t i = 0; i < dimension; ++i)
            if (min_vals[i] > other.max_vals[i] || max_vals[i] < other.min_vals[i]) return false;
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
    std::vector<MBR> mbrs;           // leaf: per?entry MBR; internal: per?child aggregate MBR
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

    void clear() {
        mbrs.clear();
        entries.clear();
        children.clear();
        parent.reset();
    }

    // Recalculate the mbrs array for this node
    bool updateMBR() {
        if (is_leaf) {
            mbrs.clear();
            mbrs.reserve(entries.size());
            for (const auto& e : entries) {
                if (e.getDim() != dimension) return false;
                mbrs.push_back(e.mbr);
            }
        }
        else {
            mbrs.clear();
            mbrs.reserve(children.size());
            for (const auto& child : children) {
                // Compute the aggregate MBR of the child node
                MBR child_mbr(dimension);
                if (child->is_leaf) {
                    if (child->entries.empty()) continue;
                    child_mbr = child->entries[0].mbr;
                    for (size_t i = 1; i < child->entries.size(); ++i)
                        child_mbr = child_mbr.merge(child->entries[i].mbr);
                }
                else {
                    if (child->children.empty()) continue;
                    // Use child's mbrs if available, otherwise fallback to compute
                    if (!child->mbrs.empty()) {
                        child_mbr = child->mbrs[0];
                        for (size_t i = 1; i < child->mbrs.size(); ++i)
                            child_mbr = child_mbr.merge(child->mbrs[i]);
                    }
                    else {
                        // recursive fallback
                        child_mbr = child->getNodeMBR();
                    }
                }
                mbrs.push_back(child_mbr);
            }
        }
        return true;
    }

    // Recursively compute the aggregate MBR of this node (used for parent updates)
    MBR getNodeMBR() const {
        MBR res(dimension);
        if (is_leaf) {
            if (entries.empty()) return res;
            res = entries[0].mbr;
            for (size_t i = 1; i < entries.size(); ++i)
                res = res.merge(entries[i].mbr);
        }
        else {
            if (children.empty()) return res;
            res = children[0]->getNodeMBR();
            for (size_t i = 1; i < children.size(); ++i)
                res = res.merge(children[i]->getNodeMBR());
        }
        return res;
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
    bool removeEntryRecursive(std::shared_ptr<RTreeNode> node, const SpatialEntry& entry);
    void condenseTree(std::shared_ptr<RTreeNode> leaf,
        std::vector<std::shared_ptr<RTreeNode>>& removed_nodes);
    void reInsertEntries(const std::vector<SpatialEntry>& entries);

public:
    explicit RTree(uint32_t dim = 2, uint32_t treeOrder = DEFAULT_R_TREE_ORDER)
        : dimension(dim), order(treeOrder) {
        LOG_INFO << "[RTree] Created RTree, dimension: " << dimension << ", order: " << order << std::endl;
    }

    bool init();
    bool isinited() const { return isinited_; }
    uint32_t getDimension() const { return dimension; }
    uint32_t getOrder() const { return order; }

    bool insert(const SpatialEntry& entry);
    bool remove(const SpatialEntry& entry);
    bool search(const MBR& query_mbr, std::vector<SpatialEntry>& results) const;
    bool rangeQuery(const MBR& query_mbr, std::vector<SpatialEntry>& results) const;
    bool pointQuery(const std::vector<double>& point, std::vector<SpatialEntry>& results) const;

    bool clear();
    bool validate() const;
    bool getStatistics(uint32_t& leaf_count, uint32_t& internal_count,
        uint32_t& total_entries, uint32_t& tree_height) const;
    bool printTree(std::ostream& os = std::cout) const;
};

#endif // R_TREE_H