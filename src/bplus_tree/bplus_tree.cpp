#include<iostream>
#include"bplus_tree.h"
BPTree::BPTree(uint32_t treeOrder)
    : order(treeOrder)  // 初始化列表给常量order赋值
{
    std::cout << "[BPTree] 对象创建完成，阶数为：" << order << std::endl;
}
void BPTree::init() {
    if (isinited_) {
        std::cout << "B+树已初始化，无需重复初始化" << std::endl;
        return;
    }
    root = std::make_shared<BPNode>();
    isinited_ = true;
    std::cout << "B+树初始化成功" << std::endl;//调试用

}
bool BPTree::isinited() const {
    return isinited_;
}
//查找叶子节点辅助函数
std::shared_ptr<BPNode> BPTree::findLeafNode(uint64_t key) const{
    if (!isinited_) {//判断是否初始化
        std::cerr << "findLeafNode无法操作:B+树未初始化" << std::endl;
        return nullptr;
    }
    auto currNode = root;
    while (!currNode->is_leaf) {
        auto it = std::upper_bound(currNode->keys.begin(), currNode->keys.end(), key);
        int idx = it - currNode->keys.begin();//计算索引位置
        currNode = currNode->children[idx];
    }
    return currNode;
}
//insert函数实现
bool BPTree::insert(uint64_t key, const PhyAddr& addr) {
    if (!addr.isValid()) {
        std::cerr << "非法地址，插入失败！" << std::endl;
        return false;
    }
    auto leafNode = findLeafNode(key);
    if (!leafNode) return false;
    //二分查找插入位置
    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    auto idx = it - leafNode->keys.begin();//计算索引位置
    //检查key是否存在
    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        leafNode->values[idx] = addr;// 已存在直接覆盖
        return false;
    }
    //插入key和对应地址
    leafNode->keys.insert(it, key);
    leafNode->values.insert(leafNode->values.begin() + idx, addr);
    //检查是否需要分裂
    if (leafNode->keys.size() > BP_ORDER - 1) {
        SplitLeafNode(leafNode);
    }
    return true;
}
// 分裂子叶节点
void BPTree::SplitLeafNode(std::shared_ptr<BPNode> leafNode) {
    //创建新节点
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = true;
    newRightNode->parent = leafNode->parent;
    //分裂key和value
    int splitidx = BP_ORDER / 2;
    //右节点取后3个key/value
    newRightNode->keys.assign(leafNode->keys.begin() + splitidx, leafNode->keys.end());
    newRightNode->values.assign(leafNode->values.begin() + splitidx, leafNode->values.end());
    //左节点保留前两个key/value
    leafNode->keys.erase(leafNode->keys.begin() + splitidx, leafNode->keys.end());
    leafNode->values.erase(leafNode->values.begin() + splitidx, leafNode->values.end());
    //更新叶子节点链表
    newRightNode->next_leaf = leafNode->next_leaf;
    leafNode->next_leaf = newRightNode;
    //提升分裂key到父节点
    uint64_t promoteKey = newRightNode->keys[0];
    InsertIntoParent(leafNode, promoteKey, newRightNode);
}

//将分裂的key插入父节点
void BPTree::InsertIntoParent(std::shared_ptr<BPNode> leftNode, uint64_t key, std::shared_ptr<BPNode> rightNode) {
    auto parentNode = leftNode->parent.lock();
    //如果是根节点
    if (!parentNode) {
        auto newRoot = std::make_shared<BPNode>();//创建新子节点
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
    for (int i=0; i < parentNode->children.size(); ++i) {
        if (parentNode->children[i] == leftNode) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        std::cerr << "InsertIntoParent: 父节点中未找到左节点." << std::endl;
        return;
    }
    // 步骤3：插入提升的key和右节点
    parentNode->keys.insert(parentNode->keys.begin() + idx, key);
    parentNode->children.insert(parentNode->children.begin() + idx + 1, rightNode);
    rightNode->parent = parentNode;

    // 检查父节点是否满，满则分裂
    if (parentNode->keys.size() > BP_ORDER - 1) {
        SplitInternalNode(parentNode);
    }
}
void BPTree::SplitInternalNode(std::shared_ptr<BPNode> internalNode) {
    auto newRightNode = std::make_shared<BPNode>();
    newRightNode->is_leaf = false;
    newRightNode->parent = internalNode->parent;

    int splitIdx = BP_ORDER / 2;
    uint64_t promoteKey = internalNode->keys[splitIdx];
    // 拆分key和子节点 
    newRightNode->keys.assign(internalNode->keys.begin() + splitIdx + 1, internalNode->keys.end());
    newRightNode->children.assign(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());
    //更新新右节点的children的父节点
    for (auto& child : newRightNode->children) {
        child->parent = newRightNode;
    }
    internalNode->keys.erase(internalNode->keys.begin() + splitIdx, internalNode->keys.end());
    internalNode->children.erase(internalNode->children.begin() + splitIdx + 1, internalNode->children.end());

    InsertIntoParent(internalNode, promoteKey, newRightNode);
}

PhyAddr BPTree::search(uint64_t key) const {
    if (!isinited_) {
        std::cerr << "search error:树未初始化" << std::endl;
        return PhyAddr{};//返回默认非法地址
    }

    if (key == INVALID_KEY) {
        std::cerr << "search error: 查询非法key" << std::endl;
        return PhyAddr{};
    }
    //找目标子叶节点
    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        //std::cout << "叶子节点为空" << std::endl;
        return PhyAddr{};
    }
    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    int idx = it - leafNode->keys.begin();
    if (idx < leafNode->keys.size() && leafNode->keys[idx] == key) {
        return leafNode->values[idx];
    }
    else {
        //std::cout<<"未找到key" <<endl;
        return PhyAddr{};
    }
}

std::vector<std::pair<uint64_t, PhyAddr>>BPTree::rangeSearch(uint64_t startKey, uint64_t endKey) const{
    std::vector<std::pair<uint64_t, PhyAddr>> result;
    if (startKey > endKey) {
        std::cerr << "rangeSearch error:起始key大于末尾key" << std::endl;
        return result;
    }
    if(!isinited_){
        std::cerr << "rangeSearch error:树未初始化" << std::endl;
        return result;
    }
    if (startKey == INVALID_KEY || endKey == INVALID_KEY) {
        std::cerr << "包含非法key" << std::endl;
        return result;
    }
    auto currNode = findLeafNode(startKey);
    if (!currNode || currNode->keys.empty()) {
        std::cout << "无匹配数据" << std::endl;
        return result;
    }
    //开始范围查询
    while (currNode) {
        for (size_t i = 0; i < currNode->keys.size();++i) {
            uint64_t currKey = currNode->keys[i];
            if (currKey > endKey) goto END_TRAVERSE;
            if (currKey >= startKey) {
                result.emplace_back(currKey, currNode->values[i]);
            }
        }
        //提前终止
        if (!currNode->keys.empty() && currNode->keys.back() >= endKey) break;
        currNode = currNode->next_leaf;
    }
END_TRAVERSE:
    if (result.empty()) {
        std::cout << "[INFO] 范围查询[" << startKey << "," << endKey << "]：无匹配数据" << std::endl;
    }
    else {
        std::cout << "[INFO] 范围查询完成，共找到" << result.size() << "条数据" << std::endl;
    }
    return result;
}
//删除函数
void BPTree::remove(uint64_t key) {
    if (!isinited_) {
        std::cerr << "remove error:树未初始化" << std::endl;
        return;
    }
    if (key==INVALID_KEY) {
        std::cerr << "remove error:非法key" << std::endl;
        return;
    }
    auto leafNode = findLeafNode(key);
    if (!leafNode || leafNode->keys.empty()) {
        std::cout << "未找到key" << std::endl;
        return;
    }
    auto it = std::lower_bound(leafNode->keys.begin(), leafNode->keys.end(), key);
    int idx = it - leafNode->keys.begin();
    if (idx >= leafNode->keys.size() || leafNode->keys[idx] != key) {
        std::cout << "未找到对应的key" << std::endl;
        return;
    }
    leafNode->keys.erase(it);
    leafNode->values.erase(leafNode->values.begin() + idx);
    std::cout << "已成功删除"<<key << std::endl;

    //判断是否下溢
    if (leafNode->keys.size() < BP_ORDER / 2) {
        handleLeafUnderflow(leafNode);
    }
    if (root->is_leaf == false && root->children.size() == 1) {
        root = root->children[0];
        root->parent.reset();
        std::cout << "根节点降级完成" << std::endl;
    }

}
//叶子节点下溢
void  BPTree::handleLeafUnderflow(std::shared_ptr<BPNode> leafNode){
    auto parentNode = leafNode->parent.lock();
    if (!parentNode) return;
    int nodeIdx = -1;
    for (int i = 0; i < parentNode->children.size();++i) {
        if (parentNode->children[i] == leafNode) {
            nodeIdx = i;
            break;
        }
    }
    if (nodeIdx == -1) return;
    //尝试向左兄弟借key（优先左兄弟）
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->is_leaf && leftSibling->keys.size() > BP_ORDER / 2) {
            uint64_t borrowKey = leftSibling->keys.back();
            PhyAddr borrowVaule = leftSibling->values.back();
            //插入到当前节点
            leafNode->keys.insert(leafNode->keys.begin(), borrowKey);
            leafNode->values.insert(leafNode->values.begin(), borrowVaule);
            //更新父节点的分隔key
            parentNode->keys[nodeIdx-1] = leafNode->keys[0];
            //删除兄弟节点借出的key
            leftSibling->keys.pop_back();
            leftSibling->values.pop_back();
            std:: cout << "向左兄弟借成功" << std::endl;
            return;
        }
    }
    if (nodeIdx < parentNode->children.size()-1) {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->is_leaf && rightSibling->keys.size() > BP_ORDER / 2) {
            uint64_t borrowKey = rightSibling->keys.front();
            PhyAddr borrowVaule = rightSibling->values.front();
            //插入到当前节点
            leafNode->keys.push_back(borrowKey);
            leafNode->values.push_back(borrowVaule);
            //更新父节点分裂key
            parentNode->keys[nodeIdx] = rightSibling->keys[0];
            //删除兄弟节点借出的key
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->values.erase(rightSibling->values.begin());
            std::cout << "向右兄弟借成功" << std::endl;
            return;
        }
    }
    //借不到key与一个兄弟合并
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        mergeLeafNodes(leftSibling, leafNode);
        //从父节点中删除当前节点
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx - 1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
        std::cout << "与左兄弟合并成功" << std::endl;
    }
    else {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        mergeLeafNodes(leafNode, rightSibling);
        //从父节点中删除当前节点
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx+1);
        std::cout << "与右兄弟合并成功" << std::endl;
    }
    //合并完成后检查父节点是否下溢
    if (parentNode->keys.size() < BP_ORDER / 2) {
        handleInternalUnderflow(parentNode,nodeIdx);
    }
}

//合并两个叶子节点
void BPTree::mergeLeafNodes(std::shared_ptr<BPNode> leftNode, std::shared_ptr<BPNode> rightNode) {
    leftNode->keys.insert(leftNode->keys.end(), rightNode->keys.begin(), rightNode->keys.end());
    leftNode->values.insert(leftNode->values.end(), rightNode->values.begin(), rightNode->values.end());
    //更新叶子节点链表
    leftNode->next_leaf = rightNode->next_leaf;
    //清空右节点 
    rightNode->keys.clear();
    rightNode->values.clear();
}

//处理非叶子节点下溢
void BPTree::handleInternalUnderflow(std::shared_ptr<BPNode> internalNode,int nodeIdx) {
    auto parentNode = internalNode->parent.lock();
    if (!parentNode) return;
    //尝试向左兄弟借key
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        if (leftSibling->keys.size() > BP_ORDER/2) {
            //父节点下移
            uint64_t parentKey = parentNode->keys[nodeIdx - 1];
            internalNode->keys.insert(internalNode->keys.begin(), parentKey);
            //左兄弟节点上移
            uint64_t siblingKey = leftSibling->keys.back();
            parentNode->keys[nodeIdx - 1] = siblingKey;
            //左兄弟最后一个子节点移到当前节点
            auto siblingChild = leftSibling->children.back();
            internalNode->children.insert(internalNode->children.begin(), siblingChild);
            siblingChild->parent = internalNode;
            //左兄弟删除借出的key和子节点
            leftSibling->keys.pop_back();
            leftSibling->children.pop_back();
            //std::cout << "非叶节点向左兄弟借key成功" << std::endl;
            return;
        }
    }
    //尝试向右兄弟借
    if (nodeIdx < parentNode->children.size() - 1) {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        if (rightSibling->keys.size() > BP_ORDER / 2) {
            //父节点下移
            uint64_t parentKey = parentNode->keys[nodeIdx];
            internalNode->keys.push_back(parentKey);
            //兄弟节点上移
            uint64_t siblingKey = rightSibling->keys.front();
            parentNode->keys[nodeIdx] = siblingKey;
            //右兄弟节点第一个子节点移动到当前节点
            auto siblingChild = rightSibling->children.front();
            internalNode->children.push_back(siblingChild);
            siblingChild->parent = internalNode;
            //删除右兄弟节点
            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->children.erase(rightSibling->children.begin());
            //std::cout << "非叶节点向右兄弟借key成功 " << std::endl;
            return;
        }
    }
    //与左兄弟合并
    if (nodeIdx > 0) {
        auto leftSibling = parentNode->children[nodeIdx - 1];
        mergeInternalNodes(parentNode, nodeIdx-1, parentNode->keys[nodeIdx-1]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx-1);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx);
    }
    //与右兄弟合并
    else {
        auto rightSibling = parentNode->children[nodeIdx + 1];
        mergeInternalNodes(parentNode,nodeIdx,parentNode->keys[nodeIdx]);
        parentNode->keys.erase(parentNode->keys.begin() + nodeIdx);
        parentNode->children.erase(parentNode->children.begin() + nodeIdx + 1);
    }
    //检查父节点是否下溢
    if (parentNode->keys.size() < BP_ORDER / 2 && parentNode != root) {
        //祖父节点为空直接返回
        auto grandParent = parentNode->parent.lock();
        if (!grandParent) return;

        int parentIdx = -1;
        for (int i = 0; i < grandParent->children.size(); ++i) {
            if (grandParent->children[i] == parentNode) {
                parentIdx = i;
                break;
            }
        }
        if(parentIdx!=-1) handleInternalUnderflow(parentNode, parentIdx);
    }
}

//合并非叶子节点
void BPTree::mergeInternalNodes(std::shared_ptr<BPNode> parentNode, int leftIdx,uint64_t splitKey) {
    auto leftNode = parentNode->children[leftIdx];
    auto rightNode = parentNode->children[leftIdx + 1];
    //父节点的分隔key下移到左节点
    leftNode->keys.push_back(splitKey);
    leftNode->keys.insert(leftNode->keys.end(), rightNode->keys.begin(), rightNode->keys.end());
    leftNode->children.insert(leftNode->children.end(), rightNode->children.begin(), rightNode->children.end());
    //更新字节点的父节点
    for (auto& child : rightNode->children) {
        child->parent = leftNode;
    }
    //清空右节点
    rightNode->keys.clear();
    rightNode->children.clear();
}

void BPTree::clear() {
    if (!isinited_) {
        std::cerr << "clear error:B+树未初始化" << std::endl;
        return;
    }
    root.reset();
    root = std::make_shared<BPNode>();

}


//辅助查询函数
std::vector<uint64_t> BPTree::getAllKeys() const {
    std::vector<uint64_t> allKeys;
    if (!isinited_) return allKeys;
    auto currNode = root;
    while (currNode && !currNode->is_leaf) {
        currNode = currNode->children[0];
    }
    while (currNode) {
        for (auto& k : currNode->keys) {
            allKeys.push_back(k);
        }
        currNode = currNode->next_leaf;
    }
    return allKeys;
}



