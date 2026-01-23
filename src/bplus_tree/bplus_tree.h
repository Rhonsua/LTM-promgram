#ifndef __BPLUS_TREE_H__
#define __BPLUS_TREE_H__

#include<iostream>
#include<algorithm>
#include<utility>   //提供std::swap,std::
#include<cstdint>  //提供精确位数类型数 uint32_t uint64_t等
#include<memory>  //提供智能指针 std::shared_ptr,std::weak_ptr
#include<vector>

//全局常量
constexpr uint32_t BP_ORDER = 5;//B+树阶数
constexpr uint64_t INVALID_KEY = 0;//标记非法key
constexpr uint32_t INVALID_BLOCK = 0;//标记非法块

//物理地址结构体
struct PhyAddr{
	uint64_t block_id=0;//块id
	uint32_t offset = 0;//块内偏移量
	bool isValid() const {
		return block_id != INVALID_BLOCK && offset > 0;
	}//判断地址是否有效
};
//节点结构体
struct BPNode {
	bool is_leaf = true;
	std::vector<uint64_t> keys;
	std::vector<PhyAddr> values;
	std::vector<std::shared_ptr<BPNode>> children;
	std::shared_ptr<BPNode> next_leaf;
	std::weak_ptr<BPNode> parent;
	/*结构体 依次为是否为叶节点、叶节点储存的key集合、key对应的地址集合、
	非叶节点的子结点指针、叶子节点后继指针、父节点弱指针
	*/
	//结构体构造函数：初始化为默认值
	 BPNode():is_leaf(true),next_leaf(nullptr),parent(){}
};
//类骨架
class BPTree{
private:
	std::shared_ptr<BPNode> root;//根节点
	const uint32_t order;//阶数
	bool isinited_ = false;
	std::shared_ptr<BPNode> findLeafNode(uint64_t key) const;
	void SplitLeafNode(std::shared_ptr<BPNode> leafNode);
	void InsertIntoParent(std::shared_ptr<BPNode> leftNode, uint64_t promoteKey, std::shared_ptr<BPNode> rightNode);
	void SplitInternalNode(std::shared_ptr<BPNode> internalNode);
	//处理叶子节点下溢
	void handleLeafUnderflow(std::shared_ptr<BPNode> leafNode);
	//处理非叶子节点下溢
	void handleInternalUnderflow(std::shared_ptr<BPNode> internalNode,int nodeIdx);
	//合并两个叶子节点
	void mergeLeafNodes(std::shared_ptr<BPNode> leftNode, std::shared_ptr<BPNode> rightNode);
	//合并两个非叶子节点
	void mergeInternalNodes(std::shared_ptr<BPNode> parentNode, int leftIdx,uint64_t splitKey);
public:
	explicit BPTree(uint32_t treeOrder = BP_ORDER); 
	void init();
	bool isinited() const;
	void insert(uint64_t key,const PhyAddr&addr);
	void remove(uint64_t key);
	PhyAddr search(uint64_t key) const;
	std::vector<std::pair<uint64_t, PhyAddr>> rangeSearch(uint64_t startKey, uint64_t endKey) const;
	/*// 临时开放root访问（调试用）
	std::shared_ptr<BPNode> getRoot() const {
		return root;
	}*/
	// 辅助查询函数
	std::vector<uint64_t> getAllKeys() const;
};
#endif // __BPLUS_TREE_H__
