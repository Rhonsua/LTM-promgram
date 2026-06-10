#include "bplus_tree.h"
#include "log.h"
#include <limits>
#include <cassert>

BPTree::BPTree(uint32_t treeOrder)
    : order(treeOrder)
{
    LOG_INFO << "[BPTree] Object created, order: " << order << std::endl;
}

void BPTree::init() {
    if (isinited_) {
        LOG_INFO << "B+ tree already initialized" << std::endl;
        return;
    }
    root = std::make_shared<BPNode>();
    isinited_ = true;
    LOG_INFO << "B+ tree initialized successfully" << std::endl;
}

bool BPTree::isinited() const {
    return isinited_;
}

std::shared_ptr<BPNode> BPTree::findLeafNode(uint64_t key) const {
    if (!isinited_) {
        LOG_ERROR << "findLeafNode: B+ tree not initialized" << std::endl;
        return nullptr;
    }
    auto currNode = root;
    while (!currNode->is_leaf) {
        auto it = std::upper_bound(currNode->keys.begin(), currNode->keys.end(), key);
        size_t idx = static_cast<size_t>(it - currNode->keys.begin());
        // Defensive check: children size must be keys.size() + 1
        if (idx >= currNode->children.size()) {
            LOG_ERROR << "findLeafNode: index out of range, idx=" << idx
                << ", children.size()=" << currNode->children.size()
                << ", keys.size()=" << currNode->keys.size() << std::endl;
            return nullptr;
        }
        currNode = currNode->children[idx];
    }
    return currNode;
}

bool BPTree::insert(uint64_t key, const PhyAddr& addr) {
    if (!addr.isValid()) {
        LOG_ERROR << "Insert failed: invalid physical address" << std::endl;
        return false;
    }
    auto leafNode = findLeafNode(key);
    if (!leafNode) return false;

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    size_t idx = static_cast<size_t>(it - leafNode->keys.begin());

    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        leafNode->values[idx] = addr;
        LOG_INFO << "Key " << key << " already exists, overwritten" << std::endl;
        return false;  // overwrite, not a new insertion
    }

    leafNode->keys.insert(it, key);
    leafNode->values.insert(leafNode->values.begin() + idx, addr);

    if (leafNode->keys.size() > static_cast<size_t>(order - 1)) {
        SplitLeafNode(leafNode);
    }
    return true;
}

void BPTree::SplitLeafNode(std::shared_ptr<BPNode> leafNode) {
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = true;
    newRightNode->parent = leafNode->parent;

    size_t splitidx = static_cast<size_t>(order / 2);
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

    size_t idx = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < parentNode->children.size(); ++i) {
        if (parentNode->children[i] == leftNode) {
            idx = i;
            break;
        }
    }
    if (idx == std::numeric_limits<size_t>::max()) {
        LOG_ERROR << "InsertIntoParent: left node not found in parent" << std::endl;
        return;
    }

    parentNode->keys.insert(parentNode->keys.begin() + idx, key);
    parentNode->children.insert(parentNode->children.begin() + idx + 1, rightNode);
    rightNode->parent = parentNode;

    if (parentNode->keys.size() > static_cast<size_t>(order - 1)) {
        SplitInternalNode(parentNode);
    }
}

void BPTree::SplitInternalNode(std::shared_ptr<BPNode> internalNode) {
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = false;
    newRightNode->parent = internalNode->parent;

    size_t splitIdx = static_cast<size_t>(order / 2);
    // Ensure splitIdx is within bounds
    if (splitIdx >= internalNode->keys.size()) {
        LOG_ERROR << "SplitInternalNode: splitIdx out of range, keys.size()=" << internalNode->keys.size()
            << ", splitIdx=" << splitIdx << std::endl;
        return;
    }
    uint64_t promoteKey = internalNode->keys[splitIdx]; // promote key

    // 修复：右节点键的范围从 splitIdx+1 开始，跳过提升的键
    newRightNode->keys.assign(internalNode->keys.begin() + splitIdx + 1, internalNode->keys.end());
    newRightNode->children.assign(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());
    for (auto& child : newRightNode->children) {
        child->parent = newRightNode;
    }

    // Left node keeps keys[0..splitIdx-1] and children[0..splitIdx]
    internalNode->keys.erase(internalNode->keys.begin() + splitIdx, internalNode->keys.end());
    internalNode->children.erase(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());

    // Verify invariants
    if (internalNode->children.size() != internalNode->keys.size() + 1) {
        LOG_ERROR << "SplitInternalNode: left node invariant violated" << std::endl;
    }
    if (newRightNode->children.size() != newRightNode->keys.size() + 1) {
        LOG_ERROR << "SplitInternalNode: right node invariant violated" << std::endl;
    }

    InsertIntoParent(internalNode, promoteKey, newRightNode);
}

PhyAddr BPTree::search(uint64_t key) const {
    if (!isinited_) {
        LOG_ERROR << "search: tree not initialized" << std::endl;
        return PhyAddr{};
    }
    if (key == INVALID_KEY) {
        LOG_ERROR << "search: invalid key" << std::endl;
        return PhyAddr{};
    }

    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        return PhyAddr{};
    }

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    size_t idx = static_cast<size_t>(it - leafNode->keys.begin());
    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        return leafNode->values[idx];
    }
    return PhyAddr{};
}

std::vector<std::pair<uint64_t, PhyAddr>> BPTree::rangeSearch(uint64_t startKey, uint64_t endKey) const {
    std::vector<std::pair<uint64_t, PhyAddr>> result;
    if (startKey > endKey) {
        LOG_ERROR << "rangeSearch: startKey > endKey" << std::endl;
        return result;
    }
    if (!isinited_) {
        LOG_ERROR << "rangeSearch: tree not initialized" << std::endl;
        return result;
    }
    if (startKey == INVALID_KEY || endKey == INVALID_KEY) {
        LOG_ERROR << "rangeSearch: invalid key range" << std::endl;
        return result;
    }

    auto currNode = findLeafNode(startKey);
    if (!currNode || currNode->keys.empty()) {
        LOG_INFO << "No data found in range" << std::endl;
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
    LOG_INFO << "Range query [" << startKey << "," << endKey << "]: found " << result.size() << " entries" << std::endl;
    return result;
}

void BPTree::remove(uint64_t key) {
    if (!isinited_) {
        LOG_ERROR << "remove: tree not initialized" << std::endl;
        return;
    }
    if (key == INVALID_KEY) {
        LOG_ERROR << "remove: invalid key" << std::endl;
        return;
    }

    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        LOG_INFO << "Key not found" << std::endl;
        return;
    }

    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    size_t idx = static_cast<size_t>(it - leafNode->keys.begin());
    if (idx >= leafNode->keys.size() || leafNode->keys[idx] != key) {
        LOG_INFO << "Key not found" << std::endl;
        return;
    }

    leafNode->keys.erase(it);
    leafNode->values.erase(leafNode->values.begin() + idx);
    LOG_INFO << "Successfully deleted key " << key << std::endl;

    if (leafNode->keys.size() < static_cast<size_t>(order / 2)) {
        handleLeafUnderflow(leafNode);
    }
    if (root->is_leaf == false && root->children.size() == 1) {
        root = root->children[0];
        root->parent.reset();
        LOG_INFO << "Root demoted" << std::endl;
    }
}

void BPTree::handleLeafUnderflow(std::shared_ptr<BPNode> leafNode) {
    auto parentNode = leafNode->parent.lock();
    if (!parentNode) return;

    size_t nodeIdx = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < parentNode->children.size(); ++i) {
        if (parentNode->children[i] == leafNode) {
            nodeIdx = i;
            break;
        }
    }
    if (nodeIdx == std::numeric_limits<size_t>::max()) return;

    // Borrow from left sibling
    if (nodeIdx > 0) {
        auto& leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->is_leaf && leftSibling->keys.size() > static_cast<size_t>(order / 2)) {
            uint64_t borrowKey = leftSibling->keys.back();
            PhyAddr borrowValue = leftSibling->values.back();
            leafNode->keys.insert(leafNode->keys.begin(), borrowKey);
            leafNode->values.insert(leafNode->values.begin(), borrowValue);
            parentNode->keys[nodeIdx - 1] = leafNode->keys[0];
            leftSibling->keys.pop_back();
            leftSibling->values.pop_back();
            LOG_INFO << "Borrowed from left sibling" << std::endl;
            return;
        }
    }

    // Borrow from right sibling
    if (nodeIdx + 1 < parentNode->children.size()) {
        auto& rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->is_leaf && rightSibling->keys.size() > static_cast<size_t>(order / 2)) {
            uint64_t borrowKey = rightSibling->keys.front();
            PhyAddr borrowValue = rightSibling->values.front();
            leafNode->keys.push_back(borrowKey);
            leafNode->values.push_back(borrowValue);
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->values.erase(rightSibling->values.begin());
            // Update parent key only if right sibling still has keys
            if (!rightSibling->keys.empty()) {
                parentNode->keys[nodeIdx] = rightSibling->keys[0];
            }
            else {
                parentNode->keys[nodeIdx] = borrowKey; // fallback, should not happen normally
            }
            LOG_INFO << "Borrowed from right sibling" << std::endl;
            return;
        }
    }

    // Merge with left sibling
    if (nodeIdx > 0) {
        auto& leftSibling = parentNode->children[nodeIdx - 1];
        mergeLeafNodes(leftSibling, leafNode);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx - 1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
        LOG_INFO << "Merged with left sibling" << std::endl;
    }
    else {
        // Merge with right sibling
        auto& rightSibling = parentNode->children[nodeIdx + 1];
        mergeLeafNodes(leafNode, rightSibling);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx + 1);
        LOG_INFO << "Merged with right sibling" << std::endl;
    }

    // Check if parent underflows
    if (parentNode->keys.size() < static_cast<size_t>(order / 2)) {
        auto grandParent = parentNode->parent.lock();
        if (grandParent) {
            size_t parentIdx = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < grandParent->children.size(); ++i) {
                if (grandParent->children[i] == parentNode) {
                    parentIdx = i;
                    break;
                }
            }
            if (parentIdx != std::numeric_limits<size_t>::max()) {
                handleInternalUnderflow(parentNode, parentIdx);
            }
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

void BPTree::handleInternalUnderflow(std::shared_ptr<BPNode> internalNode, size_t nodeIdx) {
    auto parentNode = internalNode->parent.lock();
    if (!parentNode) return;

    // Validate indices
    if (nodeIdx >= parentNode->children.size()) {
        LOG_ERROR << "handleInternalUnderflow: nodeIdx out of range" << std::endl;
        return;
    }

    // Borrow from left sibling
    if (nodeIdx > 0) {
        auto& leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->keys.size() > static_cast<size_t>(order / 2)) {
            if (nodeIdx - 1 >= parentNode->keys.size()) {
                LOG_ERROR << "handleInternalUnderflow: left borrow key index out of range" << std::endl;
                return;
            }
            uint64_t parentKey = parentNode->keys[nodeIdx - 1];
            internalNode->keys.insert(internalNode->keys.begin(), parentKey);
            uint64_t siblingKey = leftSibling->keys.back();
            parentNode->keys[nodeIdx - 1] = siblingKey;
            auto& siblingChild = leftSibling->children.back();
            internalNode->children.insert(internalNode->children.begin(), siblingChild);
            siblingChild->parent = internalNode;
            leftSibling->keys.pop_back();
            leftSibling->children.pop_back();
            LOG_INFO << "Internal node borrowed from left sibling" << std::endl;
            return;
        }
    }

    // Borrow from right sibling
    if (nodeIdx + 1 < parentNode->children.size()) {
        auto& rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->keys.size() > static_cast<size_t>(order / 2)) {
            if (nodeIdx >= parentNode->keys.size()) {
                LOG_ERROR << "handleInternalUnderflow: right borrow key index out of range" << std::endl;
                return;
            }
            uint64_t parentKey = parentNode->keys[nodeIdx];
            internalNode->keys.push_back(parentKey);
            uint64_t siblingKey = rightSibling->keys.front();
            parentNode->keys[nodeIdx] = siblingKey;
            auto& siblingChild = rightSibling->children.front();
            internalNode->children.push_back(siblingChild);
            siblingChild->parent = internalNode;
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->children.erase(rightSibling->children.begin());
            LOG_INFO << "Internal node borrowed from right sibling" << std::endl;
            return;
        }
    }

    // Merge with left sibling
    if (nodeIdx > 0) {
        if (nodeIdx - 1 >= parentNode->keys.size()) {
            LOG_ERROR << "handleInternalUnderflow: left merge key index out of range" << std::endl;
            return;
        }
        mergeInternalNodes(parentNode, nodeIdx - 1, parentNode->keys[nodeIdx - 1]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx - 1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
        LOG_INFO << "Internal node merged with left sibling" << std::endl;
    }
    else {
        // Merge with right sibling
        if (nodeIdx >= parentNode->keys.size()) {
            LOG_ERROR << "handleInternalUnderflow: right merge key index out of range" << std::endl;
            return;
        }
        mergeInternalNodes(parentNode, nodeIdx, parentNode->keys[nodeIdx]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx + 1);
        LOG_INFO << "Internal node merged with right sibling" << std::endl;
    }

    // Recursively handle parent underflow
    if (parentNode->keys.size() < static_cast<size_t>(order / 2) && parentNode != root) {
        auto grandParent = parentNode->parent.lock();
        if (grandParent) {
            size_t parentIdx = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < grandParent->children.size(); ++i) {
                if (grandParent->children[i] == parentNode) {
                    parentIdx = i;
                    break;
                }
            }
            if (parentIdx != std::numeric_limits<size_t>::max()) {
                handleInternalUnderflow(parentNode, parentIdx);
            }
        }
    }
}

void BPTree::mergeInternalNodes(std::shared_ptr<BPNode> parentNode, size_t leftIdx, uint64_t splitKey) {
    if (leftIdx + 1 >= parentNode->children.size()) {
        LOG_ERROR << "mergeInternalNodes: invalid sibling indices" << std::endl;
        return;
    }
    auto& leftNode = parentNode->children[leftIdx];
    auto& rightNode = parentNode->children[leftIdx + 1];

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
        LOG_ERROR << "clear: B+ tree not initialized" << std::endl;
        return;
    }
    root.reset();
    root = std::make_shared<BPNode>();
    LOG_INFO << "B+ tree cleared" << std::endl;
}

std::vector<uint64_t> BPTree::getAllKeys() const {
    std::vector<uint64_t> allKeys;
    if (!isinited_) return allKeys;
    auto currNode = root;
    while (currNode && !currNode->is_leaf) {
        if (currNode->children.empty()) return allKeys;
        currNode = currNode->children[0];
    }
    while (currNode) {
        allKeys.insert(allKeys.end(), currNode->keys.begin(), currNode->keys.end());
        currNode = currNode->next_leaf;
    }
    return allKeys;
}