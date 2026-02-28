#include "memory_index.h"
#include <random>
#include <chrono>

// 生成测试数据
template<uint32_t DIM>
std::vector<IndexEntry<DIM>> generateTestData(size_t count) {
    std::vector<IndexEntry<DIM>> data;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> pos_dis(0.0, 1000.0);
    std::uniform_real_distribution<> size_dis(1.0, 50.0);
    
    for (size_t i = 1; i <= count; ++i) {
        IndexEntry<DIM> entry;
        entry.id = i;
        entry.key = i * 10;  // 使用10的倍数作为key，便于范围查询
        
        // 生成随机MBR
        std::vector<double> min_vals(DIM);
        std::vector<double> max_vals(DIM);
        
        for (uint32_t d = 0; d < DIM; ++d) {
            double center = pos_dis(gen);
            double half_size = size_dis(gen) / 2;
            min_vals[d] = center - half_size;
            max_vals[d] = center + half_size;
        }
        
        entry.mbr = MBR<DIM>(min_vals, max_vals);
        
        // 设置物理地址
        entry.phy_addr.block_id = i * 100;
        entry.phy_addr.offset = i * 10;
        
        data.push_back(entry);
    }
    
    return data;
}

int main() {
    std::cout << "=== 内存索引集成测试 ===" << std::endl;
    
    // 测试2维混合索引
    MemoryIndex<2> hybrid_index(IndexType::HYBRID, true, 5000, 5);
    hybrid_index.init();
    
    // 生成测试数据
    std::cout << "\n生成测试数据..." << std::endl;
    auto test_data = generateTestData<2>(1000);
    
    // 批量插入
    std::cout << "插入 " << test_data.size() << " 条数据..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    size_t success = hybrid_index.bulkInsert(test_data);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cout << "插入完成，成功: " << success << " 条，耗时: " << duration.count() << "ms" << std::endl;
    
    // 测试各种查询
    
    // 1. 等值查询
    std::cout << "\n1. 等值查询 (key = 5000):" << std::endl;
    EqualCondition eq_cond(5000);
    auto eq_results = hybrid_index.query(eq_cond);
    std::cout << "找到 " << eq_results.size() << " 条结果" << std::endl;
    
    // 2. 范围查询
    std::cout << "\n2. 范围查询 (key 2000-3000):" << std::endl;
    RangeCondition range_cond(2000, 3000);
    auto range_results = hybrid_index.query(range_cond);
    std::cout << "找到 " << range_results.size() << " 条结果" << std::endl;
    
    // 3. 空间查询
    std::cout << "\n3. 空间查询 (区域 [400,400]-[600,600]):" << std::endl;
    MBR<2> query_mbr({400.0, 400.0}, {600.0, 600.0});
    SpatialCondition<2> spatial_cond(query_mbr);
    auto spatial_results = hybrid_index.query(spatial_cond);
    std::cout << "找到 " << spatial_results.size() << " 条结果" << std::endl;
    
    // 4. ID查询
    std::cout << "\n4. ID查询 (id = 750):" << std::endl;
    auto id_result = hybrid_index.findById(750);
    if (id_result) {
        std::cout << "找到条目: ID=" << id_result->id 
                  << ", key=" << id_result->key 
                  << ", MBR=[" << id_result->mbr.min_vals[0] << "," << id_result->mbr.min_vals[1] 
                  << "]-[" << id_result->mbr.max_vals[0] << "," << id_result->mbr.max_vals[1] << "]"
                  << std::endl;
    } else {
        std::cout << "未找到" << std::endl;
    }
    
    // 5. 获取所有B+树键值
    std::cout << "\n5. B+树所有键值 (前10个):" << std::endl;
    auto all_keys = hybrid_index.getAllKeys();
    for (size_t i = 0; i < std::min(size_t(10), all_keys.size()); ++i) {
        std::cout << all_keys[i] << " ";
    }
    std::cout << "..." << std::endl;
    
    // 打印统计信息
    hybrid_index.printStats();
    
    // 测试仅B+树索引
    std::cout << "\n\n=== 测试仅B+树索引 ===" << std::endl;
    MemoryIndex<2> btree_only_index(IndexType::BTREE_ONLY, true, 1000);
    btree_only_index.init();
    btree_only_index.bulkInsert(test_data);
    btree_only_index.printStats();
    
    // 测试仅R树索引
    std::cout << "\n\n=== 测试仅R树索引 ===" << std::endl;
    MemoryIndex<2> rtree_only_index(IndexType::RTREE_ONLY, true, 1000);
    rtree_only_index.init();
    rtree_only_index.bulkInsert(test_data);
    rtree_only_index.printStats();
    
    return 0;
}
