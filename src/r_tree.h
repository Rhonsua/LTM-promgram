#ifndef __R_TREE_H__
#define __R_TREE_H__

#include<iostream>
#include<algorithm>
#include<utility>
#include<cstdint>
#include<memory>
#include<vector>
#include<cmath>

constexpr uint32_t R_TREE_ORDER = 5;  // B+树阶数
constexpr uint64_t INVALID_ID = 0;
constexpr uint64_t INVALID_BLOCK = 0;

// 复用B+树物理结构地址
struct PhyAddr {
    uint64_t block_id = 0;    // 块id
    uint32_t offset = 0;      // 块内偏移量
    
    bool isValid() const {
        return block_id != INVALID_BLOCK && offset > 0;
    }
};

template<uint32_t DIM> 
struct MBR {
    std::vector<double> min_vals;
    std::vector<double> max_vals;

    MBR() : min_vals(DIM, 0.0), max_vals(DIM, 0.0) {}
    
    MBR(const std::vector<double>& min, const std::vector<double>& max) 
        : min_vals(min), max_vals(max) {
        if (min_vals.size() != DIM) min_vals.resize(DIM, 0.0);
        if (max_vals.size() != DIM) max_vals.resize(DIM, 0.0);
    }

    // 检查MBR是否有效
    bool isValid() const {
        if (min_vals.size() != DIM || max_vals.size() != DIM) 
            return false;
        for (uint32_t i = 0; i < DIM; ++i) {
            if (min_vals[i] > max_vals[i]) 
                return false;
        }
        return true;
    }

    // 计算体积/面积
    double getVolume() const {
        if (!isValid()) return 0.0;
        double volume = 1.0;
        for (uint32_t i = 0; i < DIM; ++i) {
            double length = max_vals[i] - min_vals[i];
            if (length < 0) return 0.0;
            volume *= length;
        }
        return volume;
    }

    // 合并两个MBR
    MBR merge(const MBR& other) const {
        MBR result;
        result.min_vals.resize(DIM);
        result.max_vals.resize(DIM);
        
        for (uint32_t i = 0; i < DIM; ++i) {
            result.min_vals[i] = std::min(min_vals[i], other.min_vals[i]);
            result.max_vals[i] = std::max(max_vals[i], other.max_vals[i]);
        }
        return result;
    }

    // 计算体积增量
    double getExpansionVolume(const MBR& other) const {
        MBR merged = this->merge(other);
        double current_volume = this->getVolume();
        double merged_volume = merged.getVolume();
        return merged_volume - current_volume;
    }

    // 检查是否包含另一个MBR
    bool contains(const MBR& other) const {
        if (!isValid() || !other.isValid()) 
            return false;
        for (uint32_t i = 0; i < DIM; ++i) {
            if (min_vals[i] > other.min_vals[i] || max_vals[i] < other.max_vals[i])
                return false;
        }
        return true;
    }

    // 检查是否与另一个MBR相交
    bool intersects(const MBR& other) const {
        if (!isValid() || !other.isValid()) 
            return false;
        for (uint32_t i = 0; i < DIM; ++i) {
            if (min_vals[i] > other.max_vals[i] || max_vals[i] < other.min_vals[i])
                return false;
        }
        return true;
    }
};

template<uint32_t DIM>
struct SpatialEntry {
    uint64_t id = INVALID_ID;
    MBR<DIM> mbr;
    PhyAddr phy_addr;

    bool isValid() const {
        return id != INVALID_ID && mbr.isValid() && phy_addr.isValid();
    }
};

template<uint32_t DIM> 
struct RTreeNode {
    bool is_leaf = true;
    std::weak_ptr<RTreeNode<DIM>> parent;
    std::vector<MBR<DIM>> mbrs;
    std::vector<SpatialEntry<DIM>> entries;
    std::vector<std::shared_ptr<RTreeNode<DIM>>> children;

    RTreeNode() : is_leaf(true) {}

    bool isFull() const {
        return is_leaf ? entries.size() >= R_TREE_ORDER 
                       : children.size() >= R_TREE_ORDER;
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

    // 更新节点的MBR（根据子节点或条目）
    void updateMBR() {
        if (is_leaf) {
            mbrs.clear();
            for (const auto& entry : entries) {
                mbrs.push_back(entry.mbr);
            }
        } else {
            mbrs.clear();
            for (const auto& child : children) {
                if (!child->mbrs.empty()) {
                    // 子节点的MBR是其所有MBR的合并
                    MBR<DIM> child_mbr = child->mbrs[0];
                    for (size_t i = 1; i < child->mbrs.size(); ++i) {
                        child_mbr = child_mbr.merge(child->mbrs[i]);
                    }
                    mbrs.push_back(child_mbr);
                }
            }
        }
    }
};

template<uint32_t DIM> 
class RTree {
private:
    std::shared_ptr<RTreeNode<DIM>> root;
    const uint32_t order;
    bool isinited_ = false;
    
    // 私有辅助方法
    std::shared_ptr<RTreeNode<DIM>> chooseLeaf(const std::shared_ptr<RTreeNode<DIM>>& node, 
                                               const SpatialEntry<DIM>& entry) const;
    void adjustTree(const std::shared_ptr<RTreeNode<DIM>>& node);
    std::pair<std::shared_ptr<RTreeNode<DIM>>, std::shared_ptr<RTreeNode<DIM>>> 
        splitNode(const std::shared_ptr<RTreeNode<DIM>>& node);
    void searchRecursive(const std::shared_ptr<RTreeNode<DIM>>& node, 
                        const MBR<DIM>& query_mbr,
                        std::vector<SpatialEntry<DIM>>& results) const;

public:
    explicit RTree(uint32_t treeOrder = R_TREE_ORDER) : order(treeOrder) {}
    
    void init();
    bool isinited() const { return isinited_; }
    bool insert(const SpatialEntry<DIM>& entry);
    std::vector<SpatialEntry<DIM>> search(const MBR<DIM>& query_mbr) const;
    void clear();
    
    // 获取根节点（用于测试）
    std::shared_ptr<RTreeNode<DIM>> getRoot() const { return root; }
    
    // 打印树结构（用于调试）
    void printTree() const;
};

#endif // __R_TREE_H__
