#include "r_tree.h"
#include <queue>
#include <limits>
#include <iomanip>
#include <algorithm>
#include <functional>

bool RTree::init() {
    if (isinited_) {
        LOG_INFO << "[RTree] 已经初始化" << std::endl;
        return true;
    }
    if (dimension == 0) {
        LOG_ERROR << "[RTree] 错误：维度不能为0" << std::endl;
        return false;
    }
    if (order < 2) {
        LOG_ERROR << "[RTree] 错误：阶数必须大于等于2" << std::endl;
        return false;
    }
    root = std::make_shared<RTreeNode>(dimension);
    root->is_leaf = true;
    isinited_ = true;
    LOG_INFO << "[RTree] 初始化成功，维度：" << dimension << "，阶数：" << order << std::endl;
    return true;
}

bool RTree::chooseLeaf(std::shared_ptr<RTreeNode> node,
    const SpatialEntry& entry,
    std::shared_ptr<RTreeNode>& result) const {
    if (!node) return false;
    if (node->is_leaf) {
        result = node;
        return true;
    }

    std::shared_ptr<RTreeNode> best_child = nullptr;
    double min_expansion = std::numeric_limits<double>::max();
    double min_volume = std::numeric_limits<double>::max();

    for (size_t i = 0; i < node->children.size(); ++i) {
        if (i < node->mbrs.size()) {
            double expansion = node->mbrs[i].getExpansionVolume(entry.mbr);
            if (expansion < min_expansion ||
                (std::abs(expansion - min_expansion) < 1e-10 &&
                    node->mbrs[i].getVolume() < min_volume)) {
                min_expansion = expansion;
                min_volume = node->mbrs[i].getVolume();
                best_child = node->children[i];
            }
        }
    }

    if (!best_child && !node->children.empty()) {
        best_child = node->children[0];
    }

    if (best_child) {
        return chooseLeaf(best_child, entry, result);
    }
    return false;
}

bool RTree::splitNode(std::shared_ptr<RTreeNode> node,
    std::shared_ptr<RTreeNode>& new_node1,
    std::shared_ptr<RTreeNode>& new_node2) {
    if (!node) return false;

    new_node1 = std::make_shared<RTreeNode>(dimension);
    new_node2 = std::make_shared<RTreeNode>(dimension);
    new_node1->is_leaf = node->is_leaf;
    new_node2->is_leaf = node->is_leaf;

    size_t total_items = node->is_leaf ? node->entries.size() : node->children.size();
    size_t split_index = total_items / 2;

    if (node->is_leaf) {
        for (size_t i = 0; i < split_index; ++i)
            new_node1->entries.push_back(node->entries[i]);
        for (size_t i = split_index; i < total_items; ++i)
            new_node2->entries.push_back(node->entries[i]);
        new_node1->updateMBR();
        new_node2->updateMBR();
    }
    else {
        for (size_t i = 0; i < split_index; ++i) {
            new_node1->children.push_back(node->children[i]);
            node->children[i]->parent = new_node1;
        }
        for (size_t i = split_index; i < total_items; ++i) {
            new_node2->children.push_back(node->children[i]);
            node->children[i]->parent = new_node2;
        }
        new_node1->updateMBR();
        new_node2->updateMBR();
    }
    return true;
}

bool RTree::adjustTree(std::shared_ptr<RTreeNode> node) {
    if (!node) return false;
    auto current = node;

    while (current) {
        if (!current->updateMBR()) return false;

        if (current->isFull(order)) {
            std::shared_ptr<RTreeNode> new_node1, new_node2;
            if (!splitNode(current, new_node1, new_node2)) return false;

            auto parent = current->parent.lock();
            if (!parent) {
                root = std::make_shared<RTreeNode>(dimension);
                root->is_leaf = false;
                root->children.push_back(new_node1);
                root->children.push_back(new_node2);
                new_node1->parent = root;
                new_node2->parent = root;
                root->updateMBR();
                break;
            }
            else {
                auto it = std::find(parent->children.begin(), parent->children.end(), current);
                if (it != parent->children.end())
                    parent->children.erase(it);
                parent->children.push_back(new_node1);
                parent->children.push_back(new_node2);
                new_node1->parent = parent;
                new_node2->parent = parent;
                current = parent;
            }
        }
        else {
            auto parent = current->parent.lock();
            if (parent)
                current = parent;
            else
                break;
        }
    }
    return true;
}

bool RTree::searchRecursive(std::shared_ptr<RTreeNode> node,
    const MBR& query_mbr,
    std::vector<SpatialEntry>& results) const {
    if (!node) return false;

    if (node->is_leaf) {
        for (const auto& entry : node->entries) {
            if (query_mbr.intersects(entry.mbr))
                results.push_back(entry);
        }
        return true;
    }
    else {
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (i < node->mbrs.size() && node->mbrs[i].intersects(query_mbr)) {
                if (!searchRecursive(node->children[i], query_mbr, results))
                    return false;
            }
        }
        return true;
    }
}

bool RTree::insert(const SpatialEntry& entry) {
    if (!isinited_) {
        LOG_ERROR << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }
    if (entry.getDim() != dimension) {
        LOG_ERROR << "[RTree] 错误：数据维度不匹配" << std::endl;
        return false;
    }
    if (!entry.isValid()) {
        LOG_ERROR << "[RTree] 错误：无效的空间条目" << std::endl;
        return false;
    }

    std::shared_ptr<RTreeNode> leaf;
    if (!chooseLeaf(root, entry, leaf)) {
        LOG_ERROR << "[RTree] 错误：无法选择叶子节点" << std::endl;
        return false;
    }

    leaf->entries.push_back(entry);

    if (!adjustTree(leaf)) {
        LOG_ERROR << "[RTree] 错误：调整树结构失败" << std::endl;
        return false;
    }

    LOG_INFO << "[RTree] 成功插入数据项，ID：" << entry.id << std::endl;
    return true;
}

bool RTree::search(const MBR& query_mbr, std::vector<SpatialEntry>& results) const {
    if (!isinited_) {
        LOG_ERROR << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }
    if (query_mbr.getDim() != dimension) {
        LOG_ERROR << "[RTree] 错误：查询维度不匹配" << std::endl;
        return false;
    }
    if (!query_mbr.isValid()) {
        LOG_ERROR << "[RTree] 错误：无效的查询MBR" << std::endl;
        return false;
    }

    results.clear();
    if (!searchRecursive(root, query_mbr, results)) return false;

    LOG_INFO << "[RTree] 搜索完成，找到 " << results.size() << " 个结果" << std::endl;
    return true;
}

bool RTree::rangeQuery(const MBR& query_mbr, std::vector<SpatialEntry>& results) const {
    return search(query_mbr, results);
}

bool RTree::pointQuery(const std::vector<double>& point, std::vector<SpatialEntry>& results) const {
    if (!isinited_) return false;
    if (point.size() != dimension) return false;

    MBR query_mbr(dimension);
    query_mbr.min_vals = point;
    query_mbr.max_vals = point;
    return search(query_mbr, results);
}

bool RTree::clear() {
    root.reset();
    isinited_ = false;
    LOG_INFO << "[RTree] 已清空" << std::endl;
    return true;
}

bool RTree::validate() const {
    if (!isinited_) return false;
    if (!root) return false;
    if (root->dimension != dimension) return false;
    return true;
}

bool RTree::getStatistics(uint32_t& leaf_count, uint32_t& internal_count,
    uint32_t& total_entries, uint32_t& tree_height) const {
    if (!isinited_) return false;
    leaf_count = internal_count = total_entries = tree_height = 0;
    if (!root) return true;

    std::queue<std::pair<std::shared_ptr<RTreeNode>, uint32_t>> q;
    q.push({ root, 1 });
    while (!q.empty()) {
        auto [node, level] = q.front(); q.pop();
        tree_height = std::max(tree_height, level);
        if (node->is_leaf) {
            leaf_count++;
            total_entries += node->entries.size();
        }
        else {
            internal_count++;
            for (const auto& child : node->children)
                q.push({ child, level + 1 });
        }
    }
    return true;
}

bool RTree::printTree(std::ostream& os) const {
    if (!isinited_) {
        os << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }
    if (!root) {
        os << "[RTree] 树为空" << std::endl;
        return true;
    }

    std::queue<std::pair<std::shared_ptr<RTreeNode>, int>> q;
    q.push({ root, 0 });
    os << "=== R树结构（维度：" << dimension << "，阶数：" << order << "）===" << std::endl;

    while (!q.empty()) {
        auto [node, level] = q.front(); q.pop();
        os << std::string(level * 2, ' ')
            << "层级 " << level
            << (node->is_leaf ? " [叶子]" : " [内部]")
            << " 项数: " << (node->is_leaf ? node->entries.size() : node->children.size())
            << " 维度: " << node->dimension << std::endl;

        if (!node->is_leaf) {
            for (const auto& child : node->children)
                q.push({ child, level + 1 });
        }
        else {
            for (const auto& entry : node->entries) {
                os << std::string((level + 1) * 2, ' ')
                    << "条目 ID: " << entry.id << " MBR: ";
                for (uint32_t i = 0; i < entry.mbr.getDim(); ++i)
                    os << "[" << entry.mbr.min_vals[i] << "," << entry.mbr.max_vals[i] << "] ";
                os << std::endl;
            }
        }
    }
    return true;
}

// ----- 删除相关实现 -----
bool RTree::removeEntryRecursive(std::shared_ptr<RTreeNode> node, const SpatialEntry& entry) {
    if (node->is_leaf) {
        auto it = std::find_if(node->entries.begin(), node->entries.end(),
            [&entry](const SpatialEntry& e) { return e.id == entry.id; });
        if (it != node->entries.end()) {
            node->entries.erase(it);
            node->updateMBR();
            return true;
        }
        return false;
    }
    else {
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (node->mbrs[i].contains(entry.mbr)) {
                if (removeEntryRecursive(node->children[i], entry)) {
                    node->updateMBR();
                    return true;
                }
            }
        }
        return false;
    }
}

void RTree::condenseTree(std::shared_ptr<RTreeNode> leaf,
    std::vector<std::shared_ptr<RTreeNode>>& removed_nodes) {
    auto current = leaf;
    while (current != root) {
        auto parent = current->parent.lock();
        if (!parent) break;

        int idx = -1;
        for (size_t i = 0; i < parent->children.size(); ++i) {
            if (parent->children[i] == current) {
                idx = i;
                break;
            }
        }
        if (idx == -1) return;

        bool underflow = (current->is_leaf && current->entries.size() < order / 2) ||
            (!current->is_leaf && current->children.size() < order / 2);

        if (underflow) {
            parent->children.erase(parent->children.begin() + idx);
            parent->mbrs.erase(parent->mbrs.begin() + idx);
            removed_nodes.push_back(current);
        }
        else {
            parent->updateMBR();
        }
        current = parent;
    }

    if (root->children.empty() && !root->is_leaf) {
        root = std::make_shared<RTreeNode>(dimension);
        root->is_leaf = true;
    }
}

void RTree::reInsertEntries(const std::vector<SpatialEntry>& entries) {
    for (const auto& entry : entries) {
        insert(entry);
    }
}

bool RTree::remove(const SpatialEntry& entry) {
    if (!isinited_) {
        LOG_ERROR << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }
    if (entry.getDim() != dimension) {
        LOG_ERROR << "[RTree] 错误：删除条目维度不匹配" << std::endl;
        return false;
    }
    if (!entry.isValid()) {
        LOG_ERROR << "[RTree] 错误：无效的空间条目" << std::endl;
        return false;
    }

    std::shared_ptr<RTreeNode> leaf;
    if (!chooseLeaf(root, entry, leaf)) {
        LOG_ERROR << "[RTree] 错误：未找到对应的叶子节点" << std::endl;
        return false;
    }

    if (!removeEntryRecursive(leaf, entry)) {
        LOG_ERROR << "[RTree] 错误：未找到目标条目" << std::endl;
        return false;
    }

    std::vector<std::shared_ptr<RTreeNode>> removed_nodes;
    condenseTree(leaf, removed_nodes);

    for (auto& node : removed_nodes) {
        if (node->is_leaf) {
            reInsertEntries(node->entries);
        }
        else {
            std::function<void(std::shared_ptr<RTreeNode>)> collectLeafEntries;
            collectLeafEntries = [&](std::shared_ptr<RTreeNode> n) {
                if (n->is_leaf) {
                    reInsertEntries(n->entries);
                }
                else {
                    for (auto& child : n->children)
                        collectLeafEntries(child);
                }
                };
            collectLeafEntries(node);
        }
    }

    LOG_INFO << "[RTree] 成功删除条目，ID：" << entry.id << std::endl;
    return true;
}