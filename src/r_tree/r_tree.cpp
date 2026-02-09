#include "r_tree.h"
#include <queue>
#include <limits>
#include <iomanip>

// 初始化函数：返回bool
bool RTree::init() {
    if (isinited_) {
        std::cerr << "[RTree] 已经初始化" << std::endl;
        return true;  // 已经初始化也算成功
    }

    if (dimension == 0) {
        std::cerr << "[RTree] 错误：维度不能为0" << std::endl;
        return false;  // 失败
    }

    if (order < 2) {
        std::cerr << "[RTree] 错误：阶数必须大于等于2" << std::endl;
        return false;
    }

    root = std::make_shared<RTreeNode>(dimension);
    root->is_leaf = true;
    isinited_ = true;

    std::cout << "[RTree] 初始化成功，维度：" << dimension
        << "，阶数：" << order << std::endl;
    return true;  // 成功
}

// 选择叶子节点（R树插入算法核心）
bool RTree::chooseLeaf(std::shared_ptr<RTreeNode> node,
    const SpatialEntry& entry,
    std::shared_ptr<RTreeNode>& result) const {
    if (!node) {
        return false;
    }

    // 如果当前节点是叶子节点，直接返回
    if (node->is_leaf) {
        result = node;
        return true;
    }

    // 选择需要最小扩展面积的子节点
    std::shared_ptr<RTreeNode> best_child = nullptr;
    double min_expansion = std::numeric_limits<double>::max();
    double min_volume = std::numeric_limits<double>::max();

    for (size_t i = 0; i < node->children.size(); ++i) {
        if (i < node->mbrs.size()) {
            double expansion = node->mbrs[i].getExpansionVolume(entry.mbr);

            // 如果扩展体积更小，或者扩展体积相同但当前体积更小
            if (expansion < min_expansion ||
                (std::abs(expansion - min_expansion) < 1e-10 &&
                    node->mbrs[i].getVolume() < min_volume)) {
                min_expansion = expansion;
                min_volume = node->mbrs[i].getVolume();
                best_child = node->children[i];
            }
        }
    }

    // 如果没有找到合适的子节点，选择第一个
    if (!best_child && !node->children.empty()) {
        best_child = node->children[0];
    }

    // 递归向下选择
    if (best_child) {
        return chooseLeaf(best_child, entry, result);
    }

    return false;
}

// 节点分裂（简化线性分裂）
bool RTree::splitNode(std::shared_ptr<RTreeNode> node,
    std::shared_ptr<RTreeNode>& new_node1,
    std::shared_ptr<RTreeNode>& new_node2) {
    if (!node) {
        return false;
    }

    new_node1 = std::make_shared<RTreeNode>(dimension);
    new_node2 = std::make_shared<RTreeNode>(dimension);

    new_node1->is_leaf = node->is_leaf;
    new_node2->is_leaf = node->is_leaf;

    // 简单线性分裂：将节点中的项平均分配到两个新节点
    size_t total_items = node->is_leaf ? node->entries.size() : node->children.size();
    size_t split_index = total_items / 2;

    if (node->is_leaf) {
        // 分裂叶子节点
        for (size_t i = 0; i < split_index; ++i) {
            new_node1->entries.push_back(node->entries[i]);
        }
        for (size_t i = split_index; i < total_items; ++i) {
            new_node2->entries.push_back(node->entries[i]);
        }

        new_node1->updateMBR();
        new_node2->updateMBR();
    }
    else {
        // 分裂非叶子节点
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

// 调整树结构
bool RTree::adjustTree(std::shared_ptr<RTreeNode> node) {
    if (!node) {
        return false;
    }

    auto current = node;

    while (current) {
        // 更新当前节点的MBR
        if (!current->updateMBR()) {
            return false;
        }

        // 如果节点溢出，需要分裂
        if (current->isFull(order)) {
            std::shared_ptr<RTreeNode> new_node1, new_node2;
            if (!splitNode(current, new_node1, new_node2)) {
                return false;
            }

            auto parent = current->parent.lock();
            if (!parent) {
                // 创建新的根节点
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
                // 从父节点中移除当前节点，添加两个新节点
                auto it = std::find(parent->children.begin(), parent->children.end(), current);
                if (it != parent->children.end()) {
                    parent->children.erase(it);
                }

                parent->children.push_back(new_node1);
                parent->children.push_back(new_node2);
                new_node1->parent = parent;
                new_node2->parent = parent;

                current = parent;
            }
        }
        else {
            auto parent = current->parent.lock();
            if (parent) {
                current = parent;
            }
            else {
                break;
            }
        }
    }

    return true;
}

// 递归搜索
bool RTree::searchRecursive(std::shared_ptr<RTreeNode> node,
    const MBR& query_mbr,
    std::vector<SpatialEntry>& results) const {
    if (!node) {
        return false;
    }

    if (node->is_leaf) {
        // 检查叶子节点中的每个条目
        for (const auto& entry : node->entries) {
            if (query_mbr.intersects(entry.mbr)) {
                results.push_back(entry);
            }
        }
        return true;
    }
    else {
        // 检查非叶子节点中的每个子节点
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (i < node->mbrs.size() && node->mbrs[i].intersects(query_mbr)) {
                if (!searchRecursive(node->children[i], query_mbr, results)) {
                    return false;
                }
            }
        }
        return true;
    }
}

// 插入函数：返回bool
bool RTree::insert(const SpatialEntry& entry) {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }

    if (entry.getDim() != dimension) {
        std::cerr << "[RTree] 错误：数据维度(" << entry.getDim()
            << ")与R树维度(" << dimension << ")不匹配" << std::endl;
        return false;
    }

    if (!entry.isValid()) {
        std::cerr << "[RTree] 错误：无效的空间条目" << std::endl;
        return false;
    }

    // 1. 选择叶子节点进行插入
    std::shared_ptr<RTreeNode> leaf;
    if (!chooseLeaf(root, entry, leaf)) {
        std::cerr << "[RTree] 错误：无法选择叶子节点" << std::endl;
        return false;
    }

    // 2. 插入条目
    leaf->entries.push_back(entry);

    // 3. 调整树结构
    if (!adjustTree(leaf)) {
        std::cerr << "[RTree] 错误：调整树结构失败" << std::endl;
        return false;
    }

    std::cout << "[RTree] 成功插入数据项，ID：" << entry.id << std::endl;
    return true;
}

// 搜索函数：返回bool，结果通过参数返回
bool RTree::search(const MBR& query_mbr, std::vector<SpatialEntry>& results) const {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }

    if (query_mbr.getDim() != dimension) {
        std::cerr << "[RTree] 错误：查询维度(" << query_mbr.getDim()
            << ")与R树维度(" << dimension << ")不匹配" << std::endl;
        return false;
    }

    if (!query_mbr.isValid()) {
        std::cerr << "[RTree] 错误：无效的查询MBR" << std::endl;
        return false;
    }

    // 清空结果
    results.clear();

    // 递归搜索
    if (!searchRecursive(root, query_mbr, results)) {
        return false;
    }

    std::cout << "[RTree] 搜索完成，找到 " << results.size() << " 个结果" << std::endl;
    return true;
}

// 范围查询：返回bool，结果通过参数返回
bool RTree::rangeQuery(const MBR& query_mbr, std::vector<SpatialEntry>& results) const {
    // 范围查询就是普通搜索
    return search(query_mbr, results);
}

// 点查询：返回bool，结果通过参数返回
bool RTree::pointQuery(const std::vector<double>& point, std::vector<SpatialEntry>& results) const {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }

    if (point.size() != dimension) {
        std::cerr << "[RTree] 错误：点维度(" << point.size()
            << ")与R树维度(" << dimension << ")不匹配" << std::endl;
        return false;
    }

    // 创建一个零体积的MBR（点就是一个MBR）
    MBR query_mbr(dimension);
    query_mbr.min_vals = point;
    query_mbr.max_vals = point;

    return search(query_mbr, results);
}

// 清空函数：返回bool
bool RTree::clear() {
    root.reset();
    isinited_ = false;
    std::cout << "[RTree] 已清空" << std::endl;
    return true;
}

// 验证树结构
bool RTree::validate() const {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }

    if (!root) {
        std::cerr << "[RTree] 错误：根节点为空" << std::endl;
        return false;
    }

    // 简化的验证：检查根节点是否有效
    if (root->dimension != dimension) {
        std::cerr << "[RTree] 错误：根节点维度不匹配" << std::endl;
        return false;
    }

    std::cout << "[RTree] 验证通过" << std::endl;
    return true;
}

// 获取统计信息
bool RTree::getStatistics(uint32_t& leaf_count, uint32_t& internal_count,
    uint32_t& total_entries, uint32_t& tree_height) const {
    if (!isinited_) {
        return false;
    }

    leaf_count = 0;
    internal_count = 0;
    total_entries = 0;
    tree_height = 0;

    if (!root) {
        return true;  // 空树，统计信息都是0
    }

    // 使用BFS遍历树
    std::queue<std::pair<std::shared_ptr<RTreeNode>, uint32_t>> q;
    q.push({ root, 1 });

    while (!q.empty()) {
        auto [node, level] = q.front();
        q.pop();

        // 更新树高度
        tree_height = std::max(tree_height, level);

        if (node->is_leaf) {
            leaf_count++;
            total_entries += node->entries.size();
        }
        else {
            internal_count++;
            for (const auto& child : node->children) {
                q.push({ child, level + 1 });
            }
        }
    }

    return true;
}

// 打印树结构（调试用）
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
        auto [node, level] = q.front();
        q.pop();

        os << std::string(level * 2, ' ')
            << "层级 " << level
            << (node->is_leaf ? " [叶子]" : " [内部]")
            << " 项数: "
            << (node->is_leaf ? node->entries.size() : node->children.size())
            << " 维度: " << node->dimension
            << std::endl;

        if (!node->is_leaf) {
            for (const auto& child : node->children) {
                q.push({ child, level + 1 });
            }
        }
        else {
            // 显示叶子节点中的条目
            for (const auto& entry : node->entries) {
                os << std::string((level + 1) * 2, ' ')
                    << "条目 ID: " << entry.id << " MBR: ";
                for (uint32_t i = 0; i < entry.mbr.getDim(); ++i) {
                    os << "[" << entry.mbr.min_vals[i] << "," << entry.mbr.max_vals[i] << "] ";
                }
                os << std::endl;
            }
        }
    }

    return true;
}