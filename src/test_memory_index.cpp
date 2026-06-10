// test_memory_index.cpp
#include "memory_manager.h"
#include "hybrid_index.h"
#include "bplus_tree.h"
#include "r_tree.h"
#include "index_interface.h"
#include "log.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cassert>

using namespace std;

// 打印分隔线
void printHeader(const string& title) {
    LOG_INFO << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    LOG_INFO << "║  " << title << std::endl;
    LOG_INFO << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
}

void printSubHeader(const string& title) {
    LOG_INFO << "\n=== " << title << " ===" << std::endl;
}

// 测试1: B+树基本功能
void testBPlusTreeBasic() {
    printSubHeader("测试1: B+树基本插入和查询");

    BPTree btree(5);
    btree.init();

    // 插入测试数据
    vector<pair<uint64_t, PhyAddr>> testData = {
        {1001, {1, 100}},
        {1002, {1, 200}},
        {1003, {1, 300}},
        {1005, {2, 100}},
        {1008, {2, 200}}
    };

    for (const auto& item : testData) {
        uint64_t key = item.first;
        PhyAddr addr = item.second;
        bool success = btree.insert(key, addr);
        LOG_INFO << "插入 key=" << key << ": " << (success ? "成功" : "覆盖或失败") << std::endl;
    }

    // 查询测试
    LOG_INFO << "\n查询结果:" << std::endl;
    for (const auto& item : testData) {
        uint64_t key = item.first;
        PhyAddr result = btree.search(key);
        if (result.isValid()) {
            LOG_INFO << "key=" << key << " -> block_id=" << result.block_id
                << ", offset=" << result.offset << std::endl;
        }
        else {
            LOG_INFO << "key=" << key << " -> 未找到" << std::endl;
        }
    }

    // 测试不存在的key
    PhyAddr notFound = btree.search(9999);
    assert(!notFound.isValid());
    LOG_INFO << "\n不存在的key=9999: " << (notFound.isValid() ? "找到" : "未找到") << std::endl;

    LOG_INFO << "? B+树基本功能测试通过" << std::endl;
}

// 测试2: B+树范围查询
void testBPlusTreeRangeSearch() {
    printSubHeader("测试2: B+树范围查询");

    BPTree btree(5);
    btree.init();

    // 插入1-100的键
    for (int i = 1; i <= 100; i++) {
        PhyAddr addr{ static_cast<uint64_t>(i / 10 + 1), static_cast<uint32_t>(i * 100) };
        btree.insert(i * 10, addr);
    }

    // 范围查询
    auto results = btree.rangeSearch(300, 500);
    LOG_INFO << "范围查询 [300, 500] 找到 " << results.size() << " 条记录" << std::endl;

    // 验证结果
    for (const auto& item : results) {
        uint64_t key = item.first;
        PhyAddr addr = item.second;
        LOG_INFO << "  key=" << key << ", block_id=" << addr.block_id
            << ", offset=" << addr.offset << std::endl;
    }

    assert(results.size() == 21); // 300,310,...,500 共21个
    LOG_INFO << "? B+树范围查询测试通过" << std::endl;
}

// 测试3: R树基本功能
void testRTreeBasic() {
    printSubHeader("测试3: R树基本插入和查询");

    RTree rtree(2, 5);
    rtree.init();

    // 创建空间数据
    vector<SpatialEntry> testData;

    SpatialEntry entry1;
    entry1.id = 1;
    entry1.mbr = MBR(2);
    entry1.mbr.min_vals = { 100.0, 100.0 };
    entry1.mbr.max_vals = { 150.0, 150.0 };
    entry1.phy_addr = { 1, 100 };
    testData.push_back(entry1);

    SpatialEntry entry2;
    entry2.id = 2;
    entry2.mbr = MBR(2);
    entry2.mbr.min_vals = { 200.0, 200.0 };
    entry2.mbr.max_vals = { 250.0, 250.0 };
    entry2.phy_addr = { 1, 200 };
    testData.push_back(entry2);

    SpatialEntry entry3;
    entry3.id = 3;
    entry3.mbr = MBR(2);
    entry3.mbr.min_vals = { 300.0, 300.0 };
    entry3.mbr.max_vals = { 350.0, 350.0 };
    entry3.phy_addr = { 2, 100 };
    testData.push_back(entry3);

    // 插入
    for (const auto& entry : testData) {
        bool success = rtree.insert(entry);
        LOG_INFO << "插入 ID=" << entry.id << ": " << (success ? "成功" : "失败") << std::endl;
    }

    // 查询测试 - 点查询
    MBR queryPoint(2);
    queryPoint.min_vals = { 125.0, 125.0 };
    queryPoint.max_vals = { 125.0, 125.0 };

    vector<SpatialEntry> results;
    rtree.search(queryPoint, results);

    LOG_INFO << "\n点查询 (125,125) 结果:" << std::endl;
    for (const auto& entry : results) {
        LOG_INFO << "  ID=" << entry.id << ", MBR=[" << entry.mbr.min_vals[0] << ","
            << entry.mbr.min_vals[1] << "]-[" << entry.mbr.max_vals[0] << ","
            << entry.mbr.max_vals[1] << "]" << std::endl;
    }

    // 范围查询
    MBR queryRange(2);
    queryRange.min_vals = { 150.0, 150.0 };
    queryRange.max_vals = { 350.0, 350.0 };

    results.clear();
    rtree.search(queryRange, results);

    LOG_INFO << "\n范围查询 [150,150]-[350,350] 结果:" << std::endl;
    for (const auto& entry : results) {
        LOG_INFO << "  ID=" << entry.id << std::endl;
    }

    LOG_INFO << "? R树基本功能测试通过" << std::endl;
}

// 测试4: 混合索引 (HybridIndex)
void testHybridIndex() {
    printSubHeader("测试4: 混合索引 (B+树 + R树)");

    HybridIndex hybrid(IndexType::HYBRID, 5, 2);
    hybrid.init();

    // 创建索引条目（同时包含B+树键和R树空间数据）
    vector<IndexEntry> entries;

    for (int i = 1; i <= 10; i++) {
        IndexEntry entry;
        entry.id = i;
        entry.key = i * 100;  // B+树键
        entry.mbr = MBR(2);
        entry.mbr.min_vals = { static_cast<double>(i * 10), static_cast<double>(i * 10) };
        entry.mbr.max_vals = { static_cast<double>(i * 10 + 50), static_cast<double>(i * 10 + 50) };
        entry.phy_addr = { static_cast<uint64_t>(i), static_cast<uint32_t>(i * 100) };
        entries.push_back(entry);
    }

    // 插入
    for (const auto& entry : entries) {
        bool success = hybrid.insert(entry);
        LOG_INFO << "插入 ID=" << entry.id << ", key=" << entry.key << ": "
            << (success ? "成功" : "失败") << std::endl;
    }

    // 等值查询
    LOG_INFO << "\n等值查询 (key=500):" << std::endl;
    EqualCondition eqCond(500);
    auto iter = hybrid.query(eqCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  找到: ID=" << result.id << ", key=" << result.key
            << ", addr=(" << result.phy_addr.block_id << "," << result.phy_addr.offset << ")"
            << std::endl;
    }

    // 范围查询
    LOG_INFO << "\n范围查询 (key=300-700):" << std::endl;
    RangeCondition rangeCond(300, 700);
    iter = hybrid.query(rangeCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  找到: key=" << result.key << std::endl;
    }

    // 空间查询
    LOG_INFO << "\n空间查询 (MBR [250,250]-[350,350]):" << std::endl;
    MBR queryMBR(2);
    queryMBR.min_vals = { 250.0, 250.0 };
    queryMBR.max_vals = { 350.0, 350.0 };
    SpatialCondition spatialCond(queryMBR);
    iter = hybrid.query(spatialCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  找到: ID=" << result.id << ", MBR=[" << result.mbr.min_vals[0]
            << "," << result.mbr.min_vals[1] << "]" << std::endl;
    }

    LOG_INFO << "? 混合索引测试通过" << std::endl;
}

// 测试5: 带缓存的内存管理器
void testMemoryManagerWithCache() {
    printSubHeader("测试5: 内存管理器 (带LRU缓存和冷热数据管理)");

    MemoryManager mm(IndexType::HYBRID, true, 100, 5, 2, 2, 0.5);
    mm.init();

    // 插入数据
    vector<IndexEntry> entries;
    for (int i = 1; i <= 50; i++) {
        IndexEntry entry;
        entry.id = i;
        entry.key = i * 10;
        entry.mbr = MBR(2);
        entry.mbr.min_vals = { static_cast<double>(i), static_cast<double>(i) };
        entry.mbr.max_vals = { static_cast<double>(i + 5), static_cast<double>(i + 5) };
        entry.phy_addr = { static_cast<uint64_t>(i / 10 + 1), static_cast<uint32_t>(i * 100) };
        entries.push_back(entry);
    }

    size_t inserted = mm.bulkInsert(entries);
    LOG_INFO << "批量插入 " << inserted << " 条记录" << std::endl;

    // 多次查询以触发缓存
    LOG_INFO << "\n执行多次查询测试缓存:" << std::endl;
    for (int i = 0; i < 5; i++) {
        EqualCondition cond(100);
        auto iter = mm.query(cond);
        int count = 0;
        while (iter.hasNext()) {
            iter.next();
            count++;
        }
        LOG_INFO << "  查询 key=100, 第" << i + 1 << "次, 找到" << count << "条" << std::endl;
    }

    // 预热冷数据清理
    LOG_INFO << "\n执行冷数据预清理:" << std::endl;
    size_t cleaned = mm.precleanColdData();
    LOG_INFO << "  清理了 " << cleaned << " 条冷数据" << std::endl;

    // 打印统计
    mm.printStats();

    // 获取所有键
    auto allKeys = mm.getAllKeys();
    LOG_INFO << "\n所有B+树键 (前20个):" << std::endl;
    for (size_t i = 0; i < min(size_t(20), allKeys.size()); i++) {
        LOG_INFO << allKeys[i] << " ";
        if ((i + 1) % 10 == 0) LOG_INFO << std::endl;
    }
    LOG_INFO << std::endl;

    LOG_INFO << "? 内存管理器测试通过" << std::endl;
}

// 测试6: 删除操作
void testDeleteOperations() {
    printSubHeader("测试6: 删除操作");

    HybridIndex hybrid(IndexType::HYBRID, 5, 2);
    hybrid.init();

    // 插入数据
    for (int i = 1; i <= 20; i++) {
        IndexEntry entry;
        entry.id = i;
        entry.key = i * 10;
        entry.mbr = MBR(2);
        entry.mbr.min_vals = { static_cast<double>(i), static_cast<double>(i) };
        entry.mbr.max_vals = { static_cast<double>(i + 5), static_cast<double>(i + 5) };
        entry.phy_addr = { 1, static_cast<uint32_t>(i * 100) };
        hybrid.insert(entry);
    }

    LOG_INFO << "插入完成，共20条记录" << std::endl;

    // 删除操作
    vector<uint64_t> keysToDelete = { 50, 100, 150 };
    for (uint64_t key : keysToDelete) {
        LOG_INFO << "删除 key=" << key << ": "
            << (hybrid.removeByKey(key) ? "成功" : "失败") << std::endl;
    }

    // 验证删除
    LOG_INFO << "\n验证删除结果:" << std::endl;
    for (uint64_t key : keysToDelete) {
        EqualCondition cond(key);
        auto iter = hybrid.query(cond);
        bool found = iter.hasNext();
        LOG_INFO << "  key=" << key << ": " << (found ? "仍然存在" : "已删除") << std::endl;
    }

    // 检查剩余数据
    LOG_INFO << "\n剩余数据 (前10个):" << std::endl;
    int count = 0;
    for (uint64_t key = 10; key <= 200 && count < 10; key += 10) {
        EqualCondition cond(key);
        auto iter = hybrid.query(cond);
        if (iter.hasNext()) {
            LOG_INFO << "  key=" << key << " 存在" << std::endl;
            count++;
        }
    }

    LOG_INFO << "? 删除操作测试通过" << std::endl;
}

// 测试7: 性能测试
void testPerformance() {
    printSubHeader("测试7: 性能测试");

    const int DATA_SIZE = 5000;

    // B+树性能测试
    {
        LOG_INFO << "\n[B+树性能测试]" << std::endl;
        BPTree btree(50);
        btree.init();

        auto start = chrono::high_resolution_clock::now();
        for (int i = 1; i <= DATA_SIZE; i++) {
            PhyAddr addr{ static_cast<uint64_t>(i / 1000 + 1), static_cast<uint32_t>(i) };
            btree.insert(i, addr);
        }
        auto insertEnd = chrono::high_resolution_clock::now();

        for (int i = 1; i <= DATA_SIZE; i++) {
            btree.search(i);
        }
        auto queryEnd = chrono::high_resolution_clock::now();

        auto insertMs = chrono::duration_cast<chrono::milliseconds>(insertEnd - start).count();
        auto queryMs = chrono::duration_cast<chrono::milliseconds>(queryEnd - insertEnd).count();

        LOG_INFO << "  插入 " << DATA_SIZE << " 条: " << insertMs << " ms" << std::endl;
        LOG_INFO << "  查询 " << DATA_SIZE << " 条: " << queryMs << " ms" << std::endl;
        LOG_INFO << "  平均每条: " << (insertMs + queryMs) * 1000.0 / DATA_SIZE / 2 << " μs" << std::endl;
    }

    // R树性能测试
    {
        LOG_INFO << "\n[R树性能测试]" << std::endl;
        RTree rtree(2, 10);
        rtree.init();

        random_device rd;
        mt19937 gen(rd());
        uniform_real_distribution<> dis(0.0, 1000.0);

        auto start = chrono::high_resolution_clock::now();
        for (int i = 1; i <= DATA_SIZE; i++) {
            SpatialEntry entry;
            entry.id = i;
            entry.mbr = MBR(2);
            entry.mbr.min_vals = { dis(gen), dis(gen) };
            entry.mbr.max_vals = { entry.mbr.min_vals[0] + 10, entry.mbr.min_vals[1] + 10 };
            entry.phy_addr = { 1, static_cast<uint32_t>(i) };
            rtree.insert(entry);
        }
        auto insertEnd = chrono::high_resolution_clock::now();

        // 范围查询测试
        MBR queryMBR(2);
        queryMBR.min_vals = { 400.0, 400.0 };
        queryMBR.max_vals = { 600.0, 600.0 };
        vector<SpatialEntry> results;
        rtree.search(queryMBR, results);
        auto queryEnd = chrono::high_resolution_clock::now();

        auto insertMs = chrono::duration_cast<chrono::milliseconds>(insertEnd - start).count();
        auto queryMs = chrono::duration_cast<chrono::milliseconds>(queryEnd - insertEnd).count();

        LOG_INFO << "  插入 " << DATA_SIZE << " 条: " << insertMs << " ms" << std::endl;
        LOG_INFO << "  范围查询: " << queryMs << " ms, 找到 " << results.size() << " 条" << std::endl;
    }

    LOG_INFO << "? 性能测试完成" << std::endl;
}

// 测试8: 完整的内存索引集成测试
void testCompleteMemoryIndex() {
    printSubHeader("测试8: 完整内存索引集成测试");

    // 使用 MemoryManager 作为完整的内存索引
    MemoryManager memIndex(IndexType::HYBRID, true, 1000, 5, 2, 3, 0.5);
    memIndex.init();

    LOG_INFO << "初始化完成" << std::endl;

    // 准备测试数据（模拟各种类型的索引条目）
    vector<IndexEntry> testData;

    // B+树数据 (一维键值)
    for (int i = 1; i <= 100; i++) {
        IndexEntry entry;
        entry.id = i;
        entry.key = i * 10;
        entry.mbr = MBR(2);
        entry.mbr.min_vals = { static_cast<double>(i), static_cast<double>(i) };
        entry.mbr.max_vals = { static_cast<double>(i + 10), static_cast<double>(i + 10) };
        entry.phy_addr = { static_cast<uint64_t>(i / 10 + 1), static_cast<uint32_t>(i * 100) };
        testData.push_back(entry);
    }

    // 批量插入
    LOG_INFO << "批量插入 " << testData.size() << " 条数据..." << std::endl;
    size_t inserted = memIndex.bulkInsert(testData);
    LOG_INFO << "成功插入 " << inserted << " 条" << std::endl;

    // 执行各种查询
    LOG_INFO << "\n--- 等值查询 ---" << std::endl;
    EqualCondition eqCond(500);
    auto iter = memIndex.query(eqCond);
    if (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "key=500 找到: ID=" << result.id << ", addr=("
            << result.phy_addr.block_id << "," << result.phy_addr.offset << ")" << std::endl;
    }

    LOG_INFO << "\n--- 范围查询 ---" << std::endl;
    RangeCondition rangeCond(200, 300);
    iter = memIndex.query(rangeCond);
    int count = 0;
    while (iter.hasNext()) {
        iter.next();
        count++;
    }
    LOG_INFO << "key在[200,300]范围内找到 " << count << " 条" << std::endl;

    LOG_INFO << "\n--- 空间查询 ---" << std::endl;
    MBR spatialMBR(2);
    spatialMBR.min_vals = { 30.0, 30.0 };
    spatialMBR.max_vals = { 60.0, 60.0 };
    SpatialCondition spatialCond(spatialMBR);
    iter = memIndex.query(spatialCond);
    count = 0;
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  找到: ID=" << result.id << ", MBR=[" << result.mbr.min_vals[0]
            << "," << result.mbr.min_vals[1] << "]" << std::endl;
        count++;
    }
    LOG_INFO << "空间查询共找到 " << count << " 条" << std::endl;

    // 删除部分数据
    LOG_INFO << "\n--- 删除操作 ---" << std::endl;
    memIndex.remove(500);
    memIndex.remove(600);
    LOG_INFO << "删除 key=500 和 key=600" << std::endl;

    // 验证删除
    iter = memIndex.query(eqCond);
    LOG_INFO << "再次查询 key=500: " << (iter.hasNext() ? "存在" : "已删除") << std::endl;

    // 打印统计
    LOG_INFO << "\n--- 最终统计 ---" << std::endl;
    memIndex.printStats();

    // 获取所有键
    auto allKeys = memIndex.getAllKeys();
    LOG_INFO << "\nB+树中剩余键数量: " << allKeys.size() << std::endl;

    LOG_INFO << "? 完整内存索引集成测试通过" << std::endl;
}

// 主函数
int main() {
    printHeader("内存索引结构完整测试套件");

    try {
        testBPlusTreeBasic();
        testBPlusTreeRangeSearch();
        testRTreeBasic();
        testHybridIndex();
        testMemoryManagerWithCache();
        testDeleteOperations();
        testPerformance();
        testCompleteMemoryIndex();

        printHeader("所有测试通过!");

        LOG_INFO << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
        LOG_INFO << "║                      测试全部通过！                          ║" << std::endl;
        LOG_INFO << "╚══════════════════════════════════════════════════════════════╝" << std::endl;

    }
    catch (const exception& e) {
        LOG_ERROR << "测试失败: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
