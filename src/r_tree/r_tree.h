#ifndef __R_TREE_H__
#define __R_TREE_H__

#include<iostream>
#include<algorithm>
#include<utility>
#include<cstdint>
#include<memory>
#include<vector>
#include<cmath>

constexpr uint32_t R_TREE_ORDER = 5;//B+树阶数
constexpr uint64_t INVALID_ID = 0;
constexpr uint64_t INVALID_BLOCK = 0;

//复用B+树物理结构地址
struct PhyAddr {
	uint64_t block_id = 0;//块id
	uint32_t offset = 0;//块内偏移量
	bool isValid() const {
		return block_id != INVALID_BLOCK && offset > 0;
	}//判断地址是否有效
};


template<uint32_t DIM> struct MBR {//可指定维数的最小包围矩形
	std::vector<double> min_vals;
	std::vector<double> max_vals;

	//构造函数
	MBR() :min_vals(DIM, 0.0), max_vals(DIM, 0.0) {}

	//检查MBR是否有效
	bool isValid() const {
		if (min_vals.size() != max_vals.size()) return false;
		for (uint32_t i = 0; i < DIM; ++i) {
			if (min_vals[i] > max_vals[i]) return false;
		}
		return true;
	}

	//计算体积
	double getVolume() const {
		if (!isValid()) return 0.0;
		double volume = 1.0;
		for (uint32_t i; i < DIM; ++i) {
			volume *= (max_vals[i] - min_vals[i]);
		}
		return volume;
	}
	//合并两个MBR
	MBR merge(const MBR& other) const {
		MBR res;
		for (uint32_t i = 0; i < DIM; ++i) {
			res.min_vals[i] = min(min_vals[i], other.min_vals[i]);
			res.max_vals[i] = max(max_vals[i], other.max_vals[i]);
		}
		return res;
	}

	//计算体积增量
	double getExpansionVolume(const MBR& other) const {
		MBR merged = merge(other);
		return merged.getVolume() - this->getVolume();
	}
	//检查当前MBR是否包含另一个MBR
	bool contains(const MBR& other) {
		if (!isValid() || !other.isValid()) return false;
		for (uint32_t i = 0; i < DIM; ++i) {
			if (min_vals[i] > other.min_vals[i] || max_vals[i] < other.max_vals[i]) return false;
		}
		return true;
	}
};

// 2. 多维空间数据项结构体（关联MBR与物理地址）
template<uint32_t DIM>
struct SpatialEntry {
	uint64_t id = INVALID_ID;        // 空间对象唯一ID
	MBR<DIM> mbr;                    // 多维MBR
	PhyAddr phy_addr;                // 物理存储地址（复用B+树）

	// 检查空间项是否有效
	bool isValid() const {
		return id != INVALID_ID && mbr.isValid() && phy_addr.isValid();
	}
};

template<uint32_t DIM> struct RTreeNode {
	bool is_leaf = true;                                     // 是否为叶子节点
	std::weak_ptr<RTreeNode<DIM>> parent;                    // 父节点弱引用（避免循环引用）
	std::vector<MBR<DIM>> mbrs;                              // 子节点/数据项的MBR集合
	std::vector<SpatialEntry<DIM>> entries;                  // 叶子节点：空间数据项
	std::vector<std::shared_ptr<RTreeNode<DIM>>> children;   // 非叶子节点：子节点指针

	// 构造函数
	RTreeNode() : is_leaf(true), parent() {}

	// 检查节点是否已满（子节点/数据项数达到阶数上限）
	bool isFull() const {
		return is_leaf ? entries.size() >= R_TREE_ORDER - 1 : children.size() >= R_TREE_ORDER - 1;
	}

	// 清空节点数据
	void clear() {
		mbrs.clear();
		entries.clear();
		children.clear();
		parent.reset();
	}
};

// 4. 多维R树类（模板类，指定维度）
template<uint32_t DIM> class RTree {
private:
	std::shared_ptr<RTreeNode<DIM>> root; // 根节点
	const uint32_t order;                 // R树阶数（与B+树对齐）
	bool isinited_ = false;               // 初始化标记
public:
	// 构造函数（指定阶数，默认使用全局常量）
	explicit RTree(uint32_t treeOrder = R_TREE_ORDER) : order(treeOrder) {
		//std::cout << "[RTree] 构造完成，维度：" << DIM << "，阶数：" << order << std::endl;
	}
	// 初始化函数（与B+树init语义完全对齐）
	void init();
	// 检查是否已初始化
	bool isinited() const { return isinited_; }
	// 插入多维空间数据项
	bool insert(const SpatialEntry<DIM>& entry);
	// 搜索包含指定MBR的空间项
	std::vector<SpatialEntry<DIM>> search(const MBR<DIM>& query_mbr) const;
	// 清空R树
	void clear();
};



#endif //__R_TREE_H__
