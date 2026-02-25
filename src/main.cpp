#include "common_def.h"
#include "indeXY_memory.h"
#include "log.h"   // 新增日志头文件

int main() {
    const uint64_t MAX_MEM = 1024 * 1024;  // 1MB
    IndeXYMemoryIndex index(MAX_MEM, 2, 5);

    LOG_INFO << "\n===== 插入一维数据（B+树） =====" << std::endl;
    IndexEntry b_entry1;
    b_entry1.key = 1001;
    b_entry1.phy_addr = PhyAddr{ 1, 100 };
    b_entry1.type = IndexType::B_PLUS_TREE;
    b_entry1.state = MemoryState::HOT;
    index.insert(b_entry1);

    IndexEntry b_entry2;
    b_entry2.key = 1002;
    b_entry2.phy_addr = PhyAddr{ 1, 200 };
    b_entry2.type = IndexType::B_PLUS_TREE;
    b_entry2.state = MemoryState::HOT;
    index.insert(b_entry2);

    LOG_INFO << "\n===== 插入多维数据（R树） =====" << std::endl;
    IndexEntry r_entry1;
    r_entry1.key = 2001;
    r_entry1.dims = { 116.40, 39.90 };
    r_entry1.phy_addr = PhyAddr{ 2, 100 };
    r_entry1.type = IndexType::R_TREE;
    r_entry1.state = MemoryState::HOT;
    index.insert(r_entry1);

    IndexEntry r_entry2;
    r_entry2.key = 2002;
    r_entry2.dims = { 121.47, 31.23 };
    r_entry2.phy_addr = PhyAddr{ 2, 200 };
    r_entry2.type = IndexType::R_TREE;
    r_entry2.state = MemoryState::HOT;
    index.insert(r_entry2);

    LOG_INFO << "\n===== 一维查询（B+树，key=1001） =====" << std::endl;
    IndexEntry b_query;
    b_query.key = 1001;
    b_query.type = IndexType::B_PLUS_TREE;
    auto b_results = index.query(b_query);
    for (const auto& res : b_results) {
        LOG_INFO << "key=" << res.key << ", 物理地址：block_id=" << res.phy_addr.block_id
            << ", offset=" << res.phy_addr.offset << ", 内存状态="
            << (res.state == MemoryState::HOT ? "热点" : "冷数据") << std::endl;
    }

    LOG_INFO << "\n===== 多维查询（R树，坐标116.40,39.90） =====" << std::endl;
    IndexEntry r_query;
    r_query.dims = { 116.40, 39.90 };
    r_query.type = IndexType::R_TREE;
    auto r_results = index.query(r_query);
    for (const auto& res : r_results) {
        LOG_INFO << "key=" << res.key << ", 坐标：(" << res.dims[0] << "," << res.dims[1]
            << "), 物理地址：block_id=" << res.phy_addr.block_id
            << ", offset=" << res.phy_addr.offset << std::endl;
    }

    LOG_INFO << "\n===== 内存使用情况 =====" << std::endl;
    LOG_INFO << "当前内存使用量：" << index.getMemoryUsage() << "字节" << std::endl;

    index.clear();

    return 0;
}