#ifndef __BPLUS_TREE_H__
#define __BPLUS_TREE_H__

#include <iostream>
#include <algorithm>
#include <utility>   //提供std::swap,std::
#include <cstdint>   //提供精确位数类型数 uint32_t uint64_t等
#include <memory>    //提供智能指针 std::shared_ptr,std::weak_ptr
#include <vector>
#include "common_def.h"

//全局常量（仅作默认值，实际使用成员变量 order）
constexpr uint32_t BP_ORDER = 5;

//节点结构体
struct BPNode {
    bool is_leaf = true;
    std::vector<uint64_t> keys;
    std::vector<PhyAddr> values;
    std::vector<std::shared_ptr<BPNode>> children;
    std::shared_ptr<BPNode> next_leaf;
    std::weak_ptr<BPNode> parent;

    BPNode() : is_leaf(true), next_leaf(nullptr), parent() {}
};

//类骨架
class BPTree {
private:
    std::shared_ptr<BPNode> root;
    const uint32_t order;          // 实际使用的阶数
    bool isinited_ = false;

    std::shared_ptr<BPNode> findLeafNode(uint64_t key) const;
    void SplitLeafNode(std::shared_ptr<BPNode> leafNode);
    void InsertIntoParent(std::shared_ptr<BPNode> leftNode, uint64_t promoteKey, std::shared_ptr<BPNode> rightNode);
    void SplitInternalNode(std::shared_ptr<BPNode> internalNode);
    void handleLeafUnderflow(std::shared_ptr<BPNode> leafNode);
    void handleInternalUnderflow(std::shared_ptr<BPNode> internalNode, int nodeIdx);
    void mergeLeafNodes(std::shared_ptr<BPNode> leftNode, std::shared_ptr<BPNode> rightNode);
    void mergeInternalNodes(std::shared_ptr<BPNode> parentNode, int leftIdx, uint64_t splitKey);

public:
    explicit BPTree(uint32_t treeOrder = BP_ORDER);
    void init();
    bool isinited() const;
    bool insert(uint64_t key, const PhyAddr& addr);
    void remove(uint64_t key);
    PhyAddr search(uint64_t key) const;
    std::vector<std::pair<uint64_t, PhyAddr>> rangeSearch(uint64_t startKey, uint64_t endKey) const;
    void clear();
    std::vector<uint64_t> getAllKeys() const;
};

#endif // __BPLUS_TREE_H__