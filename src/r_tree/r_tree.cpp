#include "r_tree.h"
#include <iostream>
#include <queue>
#include <limits>

void RTree::init() {
    if (isinited_) {
        std::cerr << "[RTree] 已经初始化" << std::endl;
        return;
    }

    if (dimension == 0) {
        std::cerr << "[RTree] 错误：维度不能为0" << std::endl;
        return;
    }

    root = std::make_shared<RTreeNode>(dimension);
    root->is_leaf = true;
    isinited_ = true;
    std::cout << "[RTree] 初始化成功，维度: " << dimension
        << "，阶数: " << order << std::endl;
}

bool RTree::insert(const SpatialEntry& entry) {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }

    // 检查维度是否匹配
    if (entry.getDim() != dimension) {
        std::cerr << "[RTree] 错误：数据维度(" << entry.getDim()
            << ")与R树维度(" << dimension << ")不匹配" << std::endl;
        return false;
    }

    if (!entry.isValid()) {
        std::cerr << "[RTree] 错误：无效的空间条目" << std::endl;
        return false;
    }

    // 简化版本：直接插入到根节点
    // 注意：这只是一个简化实现，实际R树需要更复杂的插入逻辑
    if (root->is_leaf) {
        // 如果是叶子节点，直接插入
        root->entries.push_back(entry);
        root->updateMBR();

        // 如果节点满了，简单打印提示（简化版本）
        if (root->isFull(order)) {
            std::cout << "[RTree] 节点已满，需要分裂（简化版本）" << std::endl;
        }

        return true;
    }

    // 如果是内部节点，简化处理（实际需要递归插入）
    std::cerr << "[RTree] 简化版本暂不支持内部节点插入" << std::endl;
    return false;
}

std::vector<SpatialEntry> RTree::search(const MBR& query_mbr) const {
    std::vector<SpatialEntry> results;

    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return results;
    }

    // 检查维度是否匹配
    if (query_mbr.getDim() != dimension) {
        std::cerr << "[RTree] 错误：查询维度(" << query_mbr.getDim()
            << ")与R树维度(" << dimension << ")不匹配" << std::endl;
        return results;
    }

    if (!query_mbr.isValid()) {
        std::cerr << "[RTree] 错误：无效的查询MBR" << std::endl;
        return results;
    }

    // 简化版本：只在根节点中搜索
    if (root->is_leaf) {
        for (const auto& entry : root->entries) {
            if (query_mbr.intersects(entry.mbr)) {
                results.push_back(entry);
            }
        }
    }

    return results;
}

void RTree::clear() {
    root.reset();
    isinited_ = false;
    std::cout << "[RTree] 已清空" << std::endl;
}