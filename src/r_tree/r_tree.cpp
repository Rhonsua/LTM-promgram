#include "r_tree.h"
#include <queue>
#include <iomanip>

template<uint32_t DIM>
void RTree<DIM>::init() {
    if (isinited_) {
        std::cerr << "[RTree] 已经初始化" << std::endl;
        return;
    }
    
    root = std::make_shared<RTreeNode<DIM>>();
    root->is_leaf = true;
    isinited_ = true;
    std::cout << "[RTree] 初始化成功，维度: " << DIM 
              << "，阶数: " << order << std::endl;
}

template<uint32_t DIM>
std::shared_ptr<RTreeNode<DIM>> RTree<DIM>::chooseLeaf(
    const std::shared_ptr<RTreeNode<DIM>>& node, 
    const SpatialEntry<DIM>& entry) const {
    
    // 如果当前节点是叶子节点，直接返回
    if (node->is_leaf) {
        return node;
    }
    
    // 选择需要最少扩展面积的子节点
    std::shared_ptr<RTreeNode<DIM>> best_child = nullptr;
    double min_expansion = std::numeric_limits<double>::max();
    
    for (size_t i = 0; i < node->children.size(); ++i) {
        if (i < node->mbrs.size()) {
            double expansion = node->mbrs[i].getExpansionVolume(entry.mbr);
            
            // 如果扩展体积相同，选择当前体积较小的
            if (expansion < min_expansion || 
                (std::abs(expansion - min_expansion) < 1e-10 && 
                 (!best_child || node->mbrs[i].getVolume() < 
                  node->mbrs[std::distance(node->children.begin(), 
                    std::find(node->children.begin(), node->children.end(), best_child))].getVolume()))) {
                min_expansion = expansion;
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
        return chooseLeaf(best_child, entry);
    }
    
    return node;
}

template<uint32_t DIM>
std::pair<std::shared_ptr<RTreeNode<DIM>>, std::shared_ptr<RTreeNode<DIM>>> 
RTree<DIM>::splitNode(const std::shared_ptr<RTreeNode<DIM>>& node) {
    auto new_node1 = std::make_shared<RTreeNode<DIM>>();
    auto new_node2 = std::make_shared<RTreeNode<DIM>>();
    
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
    } else {
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
    
    return {new_node1, new_node2};
}

template<uint32_t DIM>
void RTree<DIM>::adjustTree(const std::shared_ptr<RTreeNode<DIM>>& node) {
    auto current = node;
    
    while (current) {
        // 更新当前节点的MBR
        current->updateMBR();
        
        // 如果节点溢出，需要分裂
        if (current->isFull()) {
            auto [new_node1, new_node2] = splitNode(current);
            
            auto parent = current->parent.lock();
            if (!parent) {
                // 创建新的根节点
                root = std::make_shared<RTreeNode<DIM>>();
                root->is_leaf = false;
                root->children.push_back(new_node1);
                root->children.push_back(new_node2);
                new_node1->parent = root;
                new_node2->parent = root;
                root->updateMBR();
                break;
            } else {
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
        } else {
            auto parent = current->parent.lock();
            if (parent) {
                current = parent;
            } else {
                break;
            }
        }
    }
}

template<uint32_t DIM>
bool RTree<DIM>::insert(const SpatialEntry<DIM>& entry) {
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return false;
    }
    
    if (!entry.isValid()) {
        std::cerr << "[RTree] 错误：无效的空间条目" << std::endl;
        return false;
    }
    
    // 选择叶子节点进行插入
    auto leaf = chooseLeaf(root, entry);
    
    // 插入条目
    leaf->entries.push_back(entry);
    leaf->updateMBR();
    
    // 调整树结构
    adjustTree(leaf);
    
    return true;
}

template<uint32_t DIM>
void RTree<DIM>::searchRecursive(const std::shared_ptr<RTreeNode<DIM>>& node,
                                const MBR<DIM>& query_mbr,
                                std::vector<SpatialEntry<DIM>>& results) const {
    if (!node) return;
    
    if (node->is_leaf) {
        // 检查叶子节点中的每个条目
        for (const auto& entry : node->entries) {
            if (entry.mbr.intersects(query_mbr)) {
                results.push_back(entry);
            }
        }
    } else {
        // 检查非叶子节点中的每个子节点
        for (size_t i = 0; i < node->children.size(); ++i) {
            if (i < node->mbrs.size() && node->mbrs[i].intersects(query_mbr)) {
                searchRecursive(node->children[i], query_mbr, results);
            }
        }
    }
}

template<uint32_t DIM>
std::vector<SpatialEntry<DIM>> RTree<DIM>::search(const MBR<DIM>& query_mbr) const {
    std::vector<SpatialEntry<DIM>> results;
    
    if (!isinited_) {
        std::cerr << "[RTree] 错误：R树未初始化" << std::endl;
        return results;
    }
    
    if (!query_mbr.isValid()) {
        std::cerr << "[RTree] 错误：无效的查询MBR" << std::endl;
        return results;
    }
    
    searchRecursive(root, query_mbr, results);
    return results;
}

template<uint32_t DIM>
void RTree<DIM>::clear() {
    root.reset();
    isinited_ = false;
}

template<uint32_t DIM>
void RTree<DIM>::printTree() const {
    if (!root) {
        std::cout << "[RTree] 树为空" << std::endl;
        return;
    }
    
    std::queue<std::pair<std::shared_ptr<RTreeNode<DIM>>, int>> q;
    q.push({root, 0});
    
    while (!q.empty()) {
        auto [node, level] = q.front();
        q.pop();
        
        std::cout << std::string(level * 2, ' ') 
                  << "Level " << level 
                  << (node->is_leaf ? " [Leaf]" : " [Internal]")
                  << " Items: " 
                  << (node->is_leaf ? node->entries.size() : node->children.size())
                  << std::endl;
        
        if (!node->is_leaf) {
            for (const auto& child : node->children) {
                q.push({child, level + 1});
            }
        }
    }
}

// 显式实例化模板
template class RTree<2>;
template class RTree<3>;
template class RTree<4>;
