#include<iostream>
#include"r_tree.h"

template<uint32_t DIM> 
void RTree<DIM>::init() {
	if (isinited_) {
		std::cerr << "已初始化" << std::endl;
		return;
	}

	root = std::make_shared<RTreeNode<DIM>>();
	root->is_leaf = true;
	isinited_ = true;
	std::cout << "初始化成功" << std::endl;
}
template class  RTree<3>;