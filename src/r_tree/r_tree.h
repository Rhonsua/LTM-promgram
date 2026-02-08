#ifndef __R_TREE_H__
#define __R_TREE_H__

#include<iostream>
#include<algorithm>
#include<utility>
#include<cstdint>
#include<memory>
#include<vector>
#include<cmath>
#include"common_def.h"

constexpr uint32_t DEFAULT_R_TREE_ORDER = 5;  // 默认阶数


// 动态维度MBR（移除了模板）
struct MBR {
    std::vector<double> min_vals;
    std::vector<double> max_vals;
    uint32_t dimension;  // 维度信息

    // 构造函数：传入维度
    MBR(uint32_t dim = 0) : dimension(dim) {
        if (dim > 0) {
            min_vals.resize(dim, 0.0);
            max_vals.resize(dim, 0.0);
        }
    }

    // 从现有数据初始化
    MBR(const std::vector<double>& min, const std::vector<double>& max)
        : min_vals(min), max_vals(max), dimension(min.size()) {
        if (min_vals.size() != max_vals.size()) {
            dimension = 0;  // 维度不匹配，设为0表示无效
        }
    }

    // 检查MBR是否有效
    bool isValid() const {
        if (dimension == 0) return false;
        if (min_vals.size() != dimension || max_vals.size() != dimension)
            return false;
        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > max_vals[i])
                return false;
        }
        return true;
    }

    // 获取维度
    uint32_t getDim() const { return dimension; }

    // 计算体积/面积
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

    // 合并两个MBR
    MBR merge(const MBR& other) const {
        if (dimension != other.dimension) {
            return MBR(0);  // 维度不匹配，返回空MBR
        }

        MBR result(dimension);
        for (uint32_t i = 0; i < dimension; ++i) {
            result.min_vals[i] = std::min(min_vals[i], other.min_vals[i]);
            result.max_vals[i] = std::max(max_vals[i], other.max_vals[i]);
        }
        return result;
    }

    // 计算体积增量
    double getExpansionVolume(const MBR& other) const {
        if (dimension != other.dimension) return 0.0;

        MBR merged = this->merge(other);
        double current_volume = this->getVolume();
        double merged_volume = merged.getVolume();
        return merged_volume - current_volume;
    }

    // 检查是否包含另一个MBR
    bool contains(const MBR& other) const {
        if (!isValid() || !other.isValid())
            return false;
        if (dimension != other.dimension)
            return false;

        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > other.min_vals[i] || max_vals[i] < other.max_vals[i])
                return false;
        }
        return true;
    }

    // 检查是否与另一个MBR相交
    bool intersects(const MBR& other) const {
        if (!isValid() || !other.isValid())
            return false;
        if (dimension != other.dimension)
            return false;

        for (uint32_t i = 0; i < dimension; ++i) {
            if (min_vals[i] > other.max_vals[i] || max_vals[i] < other.min_vals[i])
                return false;
        }
        return true;
    }
};

struct SpatialEntry {
    uint64_t id = INVALID_ID;
    MBR mbr;  // 使用非模板的MBR
    PhyAddr phy_addr;

    bool isValid() const {
        return id != INVALID_ID && mbr.isValid() && phy_addr.isValid();
    }

    // 获取维度
    uint32_t getDim() const { return mbr.getDim(); }
};

struct RTreeNode {
    bool is_leaf = true;
    std::weak_ptr<RTreeNode> parent;
    std::vector<MBR> mbrs;
    std::vector<SpatialEntry> entries;
    std::vector<std::shared_ptr<RTreeNode>> children;
    uint32_t dimension;  // 节点维度

    RTreeNode(uint32_t dim = 0) : dimension(dim) {}

    bool isFull(uint32_t order) const {
        return is_leaf ? entries.size() >= order
            : children.size() >= order;
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

    // 更新节点的MBR
    void updateMBR() {
        if (is_leaf) {
            mbrs.clear();
            for (const auto& entry : entries) {
                mbrs.push_back(entry.mbr);
            }
        }
        else {
            mbrs.clear();
            for (const auto& child : children) {
                if (!child->mbrs.empty()) {
                    // 子节点的MBR是其所有MBR的合并
                    MBR child_mbr = child->mbrs[0];
                    for (size_t i = 1; i < child->mbrs.size(); ++i) {
                        child_mbr = child_mbr.merge(child->mbrs[i]);
                    }
                    mbrs.push_back(child_mbr);
                }
            }
        }
    }

    // 获取维度
    uint32_t getDim() const { return dimension; }
};

// R树类（非模板，支持运行时维度）
class RTree {
private:
    std::shared_ptr<RTreeNode> root;
    uint32_t order;
    uint32_t dimension;  // 运行时确定的维度
    bool isinited_ = false;

public:
    // 构造函数：用户可以传入维度和阶数
    explicit RTree(uint32_t dim = 2, uint32_t treeOrder = DEFAULT_R_TREE_ORDER)
        : dimension(dim), order(treeOrder) {
        std::cout << "创建R树，维度：" << dimension << "，阶数：" << order << std::endl;
    }

    void init();
    bool isinited() const { return isinited_; }

    // 获取维度
    uint32_t getDimension() const { return dimension; }

    // 获取阶数
    uint32_t getOrder() const { return order; }

    bool insert(const SpatialEntry& entry);
    std::vector<SpatialEntry> search(const MBR& query_mbr) const;
    void clear();
};

#endif // __R_TREE_H__#pragma once
