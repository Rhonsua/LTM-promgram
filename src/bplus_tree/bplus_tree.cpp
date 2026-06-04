#include "bplus_tree.h"
#include "log.h"

BPTree::BPTree(uint32_t treeOrder)
    : order(treeOrder)
{
    LOG_INFO << "[BPTree] 对象创建完成，阶数为：" << order << std::endl;
}

void BPTree::init() {
    if (isinited_) {
        LOG_INFO << "B+树已初始化，无需重复初始化" << std::endl;
        return;
    }
    root = std::make_shared<BPNode>();
    isinited_ = true;
    LOG_INFO << "B+树初始化成功" << std::endl;
}

bool BPTree::isinited() const {
    return isinited_;
}

std::shared_ptr<BPNode> BPTree::findLeafNode(uint64_t key) const {
    if (!isinited_) {
        LOG_ERROR << "findLeafNode无法操作: B+树未初始化" << std::endl;
        return nullptr;
    }
    auto currNode = root;
    while (!currNode->is_leaf) {
        auto it = std::upper_bound(currNode->keys.begin(), currNode->keys.end(), key);
        int idx = it - currNode->keys.begin();
        currNode = currNode->children[idx];
    }
    return currNode;
}

bool BPTree::insert(uint64_t key, const PhyAddr& addr) {
    if (!addr.isValid()) {
        LOG_ERROR << "非法地址，插入失败！" << std::endl;
        return false;
    }
    auto leafNode = findLeafNode(key);
    if (!leafNode) return false;

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    auto idx = it - leafNode->keys.begin();

    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        leafNode->values[idx] = addr;
        LOG_INFO << "键 " << key << " 已存在，覆盖地址" << std::endl;
        return false;  // 返回 false 表示未插入新键（覆盖）
    }

    leafNode->keys.insert(it, key);
    leafNode->values.insert(leafNode->values.begin() + idx, addr);

    if (leafNode->keys.size() > order - 1) {
        SplitLeafNode(leafNode);
    }
    return true;
}

void BPTree::SplitLeafNode(std::shared_ptr<BPNode> leafNode) {
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = true;
    newRightNode->parent = leafNode->parent;

    int splitidx = order / 2;
    newRightNode->keys.assign(leafNode->keys.begin() + splitidx, leafNode->keys.end());
    newRightNode->values.assign(leafNode->values.begin() + splitidx, leafNode->values.end());

    leafNode->keys.erase(leafNode->keys.begin() + splitidx, leafNode->keys.end());
    leafNode->values.erase(leafNode->values.begin() + splitidx, leafNode->values.end());

    newRightNode->next_leaf = leafNode->next_leaf;
    leafNode->next_leaf = newRightNode;

    uint64_t promoteKey = newRightNode->keys[0];
    InsertIntoParent(leafNode, promoteKey, newRightNode);
}

void BPTree::InsertIntoParent(std::shared_ptr<BPNode> leftNode, uint64_t key, std::shared_ptr<BPNode> rightNode) {
    auto parentNode = leftNode->parent.lock();
    if (!parentNode) {
        auto newRoot = std::make_shared<BPNode>();
        newRoot->is_leaf = false;
        newRoot->keys.push_back(key);
        newRoot->children.push_back(leftNode);
        newRoot->children.push_back(rightNode);
        leftNode->parent = newRoot;
        rightNode->parent = newRoot;
        root = newRoot;
        return;
    }

    int idx = -1;
    for (size_t i = 0; i < parentNode->children.size(); ++i) {
        if (parentNode->children[i] == leftNode) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        LOG_ERROR << "InsertIntoParent: 父节点中未找到左节点." << std::endl;
        return;
    }

    parentNode->keys.insert(parentNode->keys.begin() + idx, key);
    parentNode->children.insert(parentNode->children.begin() + idx + 1, rightNode);
    rightNode->parent = parentNode;

    if (parentNode->keys.size() > order - 1) {
        SplitInternalNode(parentNode);
    }
}

void BPTree::SplitInternalNode(std::shared_ptr<BPNode> internalNode) {
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = false;
    newRightNode->parent = internalNode->parent;

    int splitIdx = order / 2;
    uint64_t promoteKey = internalNode->keys[splitIdx]; // 提升键同时也是右子树最小键

    // 右节点包含 keys[splitIdx..end] 和 children[splitIdx+1..end]
    newRightNode->keys.assign(internalNode->keys.begin() + splitIdx, internalNode->keys.end());
    newRightNode->children.assign(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());
    for (auto& child : newRightNode->children) {
        child->parent = newRightNode;
    }

    // 左节点保留 keys[0..splitIdx-1] 和 children[0..splitIdx]
    internalNode->keys.erase(internalNode->keys.begin() + splitIdx, internalNode->keys.end());
    internalNode->children.erase(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());

    // promoteKey == newRightNode->keys[0]，满足索引性质
    InsertIntoParent(internalNode, promoteKey, newRightNode);
}

PhyAddr BPTree::search(uint64_t key) const {
    if (!isinited_) {
        LOG_ERROR << "search error: 树未初始化" << std::endl;
        return PhyAddr{};
    }
    if (key == INVALID_KEY) {
        LOG_ERROR << "search error: 查询非法key" << std::endl;
        return PhyAddr{};
    }

    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        return PhyAddr{};
    }

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    int idx = it - leafNode->keys.begin();
    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        return leafNode->values[idx];
    }
    return PhyAddr{};
}

std::vector<std::pair<uint64_t, PhyAddr>> BPTree::rangeSearch(uint64_t startKey, uint64_t endKey) const {
    std::vector<std::pair<uint64_t, PhyAddr>> result;
    if (startKey > endKey) {
        LOG_ERROR << "rangeSearch error: 起始key大于末尾key" << std::endl;
        return result;
    }
    if (!isinited_) {
        LOG_ERROR << "rangeSearch error: 树未初始化" << std::endl;
        return result;
    }
    if (startKey == INVALID_KEY || endKey == INVALID_KEY) {
        LOG_ERROR << "包含非法key" << std::endl;
        return result;
    }

    auto currNode = findLeafNode(startKey);
    if (!currNode || currNode->keys.empty()) {
        LOG_INFO << "无匹配数据" << std::endl;
        return result;
    }

    while (currNode) {
        for (size_t i = 0; i < currNode->keys.size(); ++i) {
            uint64_t currKey = currNode->keys[i];
            if (currKey > endKey) goto END_TRAVERSE;
            if (currKey >= startKey) {
                result.emplace_back(currKey, currNode->values[i]);
            }
        }
        if (!currNode->keys.empty() && currNode->keys.back() >= endKey) break;
        currNode = currNode->next_leaf;
    }
END_TRAVERSE:
    LOG_INFO << "[INFO] 范围查询[" << startKey << "," << endKey << "]：共找到" << result.size() << "条数据" << std::endl;
    return result;
}

void BPTree::remove(uint64_t key) {
    if (!isinited_) {
        LOG_ERROR << "remove error: 树未初始化" << std::endl;
        return;
    }
    if (key == INVALID_KEY) {
        LOG_ERROR << "remove error: 非法key" << std::endl;
        return;
    }

    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        LOG_INFO << "未找到key" << std::endl;
        return;
    }

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    int idx = it - leafNode->keys.begin();
    if (idx >= leafNode->keys.size() || leafNode->keys[idx] != key) {
        LOG_INFO << "未找到对应的key" << std::endl;
        return;
    }

    leafNode->keys.erase(it);
    leafNode->values.erase(leafNode->values.begin() + idx);
    LOG_INFO << "已成功删除 " << key << std::endl;

    if (leafNode->keys.size() < order / 2) {
        handleLeafUnderflow(leafNode);
    }
    if (root->is_leaf == false && root->children.size() == 1) {
        root = root->children[0];
        root->parent.reset();
        LOG_INFO << "根节点降级完成" << std::endl;
    }
}

void BPTree::handleLeafUnderflow(std::shared_ptr<BPNode> leafNode) {
    auto parentNode = leafNode->parent.lock();
    if (!parentNode) return;

    int nodeIdx = -1;
    for (size_t i = 0; i < parentNode->children.size(); ++i) {
        if (parentNode->children[i] == leafNode) {
            nodeIdx = i;
            break;
        }
    }
    if (nodeIdx == -1) return;

    // 向左兄弟借
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->is_leaf && leftSibling->keys.size() > order / 2) {
            uint64_t borrowKey = leftSibling->keys.back();
            PhyAddr borrowValue = leftSibling->values.back();
            leafNode->keys.insert(leafNode->keys.begin(), borrowKey);
            leafNode->values.insert(leafNode->values.begin(), borrowValue);
            parentNode->keys[nodeIdx - 1] = leafNode->keys[0];
            leftSibling->keys.pop_back();
            leftSibling->values.pop_back();
            LOG_INFO << "向左兄弟借成功" << std::endl;
            return;
        }
    }

    // 向右兄弟借
    if (nodeIdx < static_cast<int>(parentNode->children.size()) - 1) {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->is_leaf && rightSibling->keys.size() > order / 2) {
            uint64_t borrowKey = rightSibling->keys.front();
            PhyAddr borrowValue = rightSibling->values.front();
            leafNode->keys.push_back(borrowKey);
            leafNode->values.push_back(borrowValue);
            // 先删除再更新父键为右兄弟新最小键
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->values.erase(rightSibling->values.begin());
            parentNode->keys[nodeIdx] = rightSibling->keys[0];
            LOG_INFO << "向右兄弟借成功" << std::endl;
            return;
        }
    }

    // 合并
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        mergeLeafNodes(leftSibling, leafNode);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx - 1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
        LOG_INFO << "与左兄弟合并成功" << std::endl;
    }
    else {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        mergeLeafNodes(leafNode, rightSibling);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx + 1);
        LOG_INFO << "与右兄弟合并成功" << std::endl;
    }

    if (parentNode->keys.size() < order / 2) {
        auto grandParent = parentNode->parent.lock();
        if (grandParent) {
            int parentIdx = -1;
            for (size_t i = 0; i < grandParent->children.size(); ++i) {
                if (grandParent->children[i] == parentNode) {
                    parentIdx = i;
                    break;
                }
            }
            if (parentIdx != -1) handleInternalUnderflow(parentNode, parentIdx);
        }
    }
}

void BPTree::mergeLeafNodes(std::shared_ptr<BPNode> leftNode, std::shared_ptr<BPNode> rightNode) {
    leftNode->keys.insert(leftNode->keys.end(), rightNode->keys.begin(), rightNode->keys.end());
    leftNode->values.insert(leftNode->values.end(), rightNode->values.begin(), rightNode->values.end());
    leftNode->next_leaf = rightNode->next_leaf;
    rightNode->keys.clear();
    rightNode->values.clear();
}

void BPTree::handleInternalUnderflow(std::shared_ptr<BPNode> internalNode, int nodeIdx) {
    auto parentNode = internalNode->parent.lock();
    if (!parentNode) return;

    // 向左兄弟借
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->keys.size() > order / 2) {
            uint64_t parentKey = parentNode->keys[nodeIdx - 1];
            internalNode->keys.insert(internalNode->keys.begin(), parentKey);
            uint64_t siblingKey = leftSibling->keys.back();
            parentNode->keys[nodeIdx - 1] = siblingKey;
            auto siblingChild = leftSibling->children.back();
            internalNode->children.insert(internalNode->children.begin(), siblingChild);
            siblingChild->parent = internalNode;
            leftSibling->keys.pop_back();
            leftSibling->children.pop_back();
            return;
        }
    }

    // 向右兄弟借
    if (nodeIdx < static_cast<int>(parentNode->children.size()) - 1) {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->keys.size() > order / 2) {
            uint64_t parentKey = parentNode->keys[nodeIdx];
            internalNode->keys.push_back(parentKey);
            uint64_t siblingKey = rightSibling->keys.front();
            parentNode->keys[nodeIdx] = siblingKey;
            auto siblingChild = rightSibling->children.front();
            internalNode->children.push_back(siblingChild);
            siblingChild->parent = internalNode;
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->children.erase(rightSibling->children.begin());
            return;
        }
    }

    // 合并
    if (nodeIdx > 0) {
        mergeInternalNodes(parentNode, nodeIdx - 1, parentNode->keys[nodeIdx - 1]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx - 1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
    }
    else {
        mergeInternalNodes(parentNode, nodeIdx, parentNode->keys[nodeIdx]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx + 1);
    }

    if (parentNode->keys.size() < order / 2 && parentNode != root) {
        auto grandParent = parentNode->parent.lock();
        if (grandParent) {
            int parentIdx = -1;
            for (size_t i = 0; i < grandParent->children.size(); ++i) {
                if (grandParent->children[i] == parentNode) {
                    parentIdx = i;
                    break;
                }
            }
            if (parentIdx != -1) handleInternalUnderflow(parentNode, parentIdx);
        }
    }
}

void BPTree::mergeInternalNodes(std::shared_ptr<BPNode> parentNode, int leftIdx, uint64_t splitKey) {
    auto leftNode = parentNode->children[leftIdx];
    auto rightNode = parentNode->children[leftIdx + 1];

    leftNode->keys.push_back(splitKey);
    leftNode->keys.insert(leftNode->keys.end(), rightNode->keys.begin(), rightNode->keys.end());
    leftNode->children.insert(leftNode->children.end(), rightNode->children.begin(), rightNode->children.end());

    for (auto& child : rightNode->children) {
        child->parent = leftNode;
    }

    rightNode->keys.clear();
    rightNode->children.clear();
}

void BPTree::clear() {
    if (!isinited_) {
        LOG_ERROR << "clear error: B+树未初始化" << std::endl;
        return;
    }
    root.reset();
    root = std::make_shared<BPNode>();
    LOG_INFO << "B+树已清空" << std::endl;
}

std::vector<uint64_t> BPTree::getAllKeys() const {
    std::vector<uint64_t> allKeys;
    if (!isinited_) return allKeys;
    auto currNode = root;
    while (currNode && !currNode->is_leaf) {
        currNode = currNode->children[0];
    }
    while (currNode) {
        allKeys.insert(allKeys.end(), currNode->keys.begin(), currNode->keys.end());
        currNode = currNode->next_leaf;
    }
    return allKeys;
}