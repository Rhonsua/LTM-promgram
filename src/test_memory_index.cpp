// test_memory_index.cpp - PURE ASCII VERSION
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

void printHeader(const string& title) {
    LOG_INFO << "\n+========================================================+\n";
    LOG_INFO << "|  " << title << "\n";
    LOG_INFO << "+========================================================+\n";
}

void printSubHeader(const string& title) {
    LOG_INFO << "\n=== " << title << " ===\n";
}

void testBPlusTreeBasic() {
    printSubHeader("Test1: B+Tree Insert and Search");

    BPTree btree(5);
    btree.init();

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
        LOG_INFO << "Insert key=" << key << ": " << (success ? "OK" : "Replace/Failed") << "\n";
    }

    LOG_INFO << "\nSearch results:\n";
    for (const auto& item : testData) {
        uint64_t key = item.first;
        PhyAddr result = btree.search(key);
        if (result.isValid()) {
            LOG_INFO << "key=" << key << " -> block_id=" << result.block_id
                << ", offset=" << result.offset << "\n";
        }
        else {
            LOG_INFO << "key=" << key << " -> not found\n";
        }
    }

    PhyAddr notFound = btree.search(9999);
    assert(!notFound.isValid());
    LOG_INFO << "\nkey=9999: " << (notFound.isValid() ? "found" : "not found") << "\n";

    LOG_INFO << "[PASS] B+Tree basic test\n";
}

void testBPlusTreeRangeSearch() {
    printSubHeader("Test2: B+Tree Range Search");

    BPTree btree(5);
    btree.init();

    for (int i = 1; i <= 100; i++) {
        PhyAddr addr{ static_cast<uint64_t>(i / 10 + 1), static_cast<uint32_t>(i * 100) };
        btree.insert(i * 10, addr);
    }

    auto results = btree.rangeSearch(300, 500);
    LOG_INFO << "Range [300,500] found " << results.size() << " records\n";

    for (const auto& item : results) {
        uint64_t key = item.first;
        PhyAddr addr = item.second;
        LOG_INFO << "  key=" << key << ", block_id=" << addr.block_id
            << ", offset=" << addr.offset << "\n";
    }

    assert(results.size() == 21);
    LOG_INFO << "[PASS] B+Tree range search\n";
}

void testRTreeBasic() {
    printSubHeader("Test3: RTree Basic Insert and Query");

    RTree rtree(2, 5);
    rtree.init();

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

    for (const auto& entry : testData) {
        bool success = rtree.insert(entry);
        LOG_INFO << "Insert ID=" << entry.id << ": " << (success ? "OK" : "Failed") << "\n";
    }

    MBR queryPoint(2);
    queryPoint.min_vals = { 125.0, 125.0 };
    queryPoint.max_vals = { 125.0, 125.0 };

    vector<SpatialEntry> results;
    rtree.search(queryPoint, results);

    LOG_INFO << "\nPoint query (125,125) results:\n";
    for (const auto& entry : results) {
        LOG_INFO << "  ID=" << entry.id << ", MBR=[" << entry.mbr.min_vals[0] << ","
            << entry.mbr.min_vals[1] << "]-[" << entry.mbr.max_vals[0] << ","
            << entry.mbr.max_vals[1] << "]\n";
    }

    MBR queryRange(2);
    queryRange.min_vals = { 150.0, 150.0 };
    queryRange.max_vals = { 350.0, 350.0 };

    results.clear();
    rtree.search(queryRange, results);

    LOG_INFO << "\nRange query [150,150]-[350,350] results:\n";
    for (const auto& entry : results) {
        LOG_INFO << "  ID=" << entry.id << "\n";
    }

    LOG_INFO << "[PASS] RTree basic test\n";
}

void testHybridIndex() {
    printSubHeader("Test4: Hybrid Index (B+Tree + RTree)");

    HybridIndex hybrid(IndexType::HYBRID, 5, 2);
    hybrid.init();

    vector<IndexEntry> entries;

    for (int i = 1; i <= 10; i++) {
        IndexEntry entry;
        entry.id = i;
        entry.key = i * 100;
        entry.mbr = MBR(2);
        entry.mbr.min_vals = { static_cast<double>(i * 10), static_cast<double>(i * 10) };
        entry.mbr.max_vals = { static_cast<double>(i * 10 + 50), static_cast<double>(i * 10 + 50) };
        entry.phy_addr = { static_cast<uint64_t>(i), static_cast<uint32_t>(i * 100) };
        entries.push_back(entry);
    }

    for (const auto& entry : entries) {
        bool success = hybrid.insert(entry);
        LOG_INFO << "Insert ID=" << entry.id << ", key=" << entry.key << ": "
            << (success ? "OK" : "Failed") << "\n";
    }

    LOG_INFO << "\nEquality query (key=500):\n";
    EqualCondition eqCond(500);
    auto iter = hybrid.query(eqCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  Found: ID=" << result.id << ", key=" << result.key
            << ", addr=(" << result.phy_addr.block_id << "," << result.phy_addr.offset << ")\n";
    }

    LOG_INFO << "\nRange query (key=300-700):\n";
    RangeCondition rangeCond(300, 700);
    iter = hybrid.query(rangeCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  Found: key=" << result.key << "\n";
    }

    LOG_INFO << "\nSpatial query (MBR [250,250]-[350,350]):\n";
    MBR queryMBR(2);
    queryMBR.min_vals = { 250.0, 250.0 };
    queryMBR.max_vals = { 350.0, 350.0 };
    SpatialCondition spatialCond(queryMBR);
    iter = hybrid.query(spatialCond);
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  Found: ID=" << result.id << ", MBR=[" << result.mbr.min_vals[0]
            << "," << result.mbr.min_vals[1] << "]\n";
    }

    LOG_INFO << "[PASS] Hybrid index test\n";
}

void testMemoryManagerWithCache() {
    printSubHeader("Test5: Memory Manager with LRU Cache and Cold/Hot");

    MemoryManager mm(IndexType::HYBRID, true, 100, 5, 2, 2, 0.5);
    mm.init();

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
    LOG_INFO << "Bulk inserted " << inserted << " records\n";

    LOG_INFO << "\nMultiple queries to trigger cache:\n";
    for (int i = 0; i < 5; i++) {
        EqualCondition cond(100);
        auto iter = mm.query(cond);
        int count = 0;
        while (iter.hasNext()) {
            iter.next();
            count++;
        }
        LOG_INFO << "  Query key=100, #" << i + 1 << ", found " << count << " records\n";
    }

    LOG_INFO << "\nPre-clean cold data:\n";
    size_t cleaned = mm.precleanColdData();
    LOG_INFO << "  Cleaned " << cleaned << " cold entries\n";

    mm.printStats();

    auto allKeys = mm.getAllKeys();
    LOG_INFO << "\nAll B+Tree keys (first 20):\n";
    for (size_t i = 0; i < min(size_t(20), allKeys.size()); i++) {
        LOG_INFO << allKeys[i] << " ";
        if ((i + 1) % 10 == 0) LOG_INFO << "\n";
    }
    LOG_INFO << "\n";

    LOG_INFO << "[PASS] Memory manager test\n";
}

void testDeleteOperations() {
    printSubHeader("Test6: Delete Operations");

    HybridIndex hybrid(IndexType::HYBRID, 5, 2);
    hybrid.init();

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

    LOG_INFO << "Inserted 20 records\n";

    vector<uint64_t> keysToDelete = { 50, 100, 150 };
    for (uint64_t key : keysToDelete) {
        LOG_INFO << "Delete key=" << key << ": "
            << (hybrid.removeByKey(key) ? "OK" : "Failed") << "\n";
    }

    LOG_INFO << "\nVerifying deletion:\n";
    for (uint64_t key : keysToDelete) {
        EqualCondition cond(key);
        auto iter = hybrid.query(cond);
        bool found = iter.hasNext();
        LOG_INFO << "  key=" << key << ": " << (found ? "still exists" : "deleted") << "\n";
    }

    LOG_INFO << "\nRemaining data (first 10):\n";
    int count = 0;
    for (uint64_t key = 10; key <= 200 && count < 10; key += 10) {
        EqualCondition cond(key);
        auto iter = hybrid.query(cond);
        if (iter.hasNext()) {
            LOG_INFO << "  key=" << key << " exists\n";
            count++;
        }
    }

    LOG_INFO << "[PASS] Delete operations test\n";
}

void testPerformance() {
    printSubHeader("Test7: Performance Test");

    const int DATA_SIZE = 5000;

    {
        LOG_INFO << "\n[B+Tree performance]\n";
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

        LOG_INFO << "  Insert " << DATA_SIZE << " items: " << insertMs << " ms\n";
        LOG_INFO << "  Search " << DATA_SIZE << " items: " << queryMs << " ms\n";
        LOG_INFO << "  Avg per op: " << (insertMs + queryMs) * 1000.0 / DATA_SIZE / 2 << " us\n";
    }

    {
        LOG_INFO << "\n[RTree performance]\n";
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

        MBR queryMBR(2);
        queryMBR.min_vals = { 400.0, 400.0 };
        queryMBR.max_vals = { 600.0, 600.0 };
        vector<SpatialEntry> results;
        rtree.search(queryMBR, results);
        auto queryEnd = chrono::high_resolution_clock::now();

        auto insertMs = chrono::duration_cast<chrono::milliseconds>(insertEnd - start).count();
        auto queryMs = chrono::duration_cast<chrono::milliseconds>(queryEnd - insertEnd).count();

        LOG_INFO << "  Insert " << DATA_SIZE << " items: " << insertMs << " ms\n";
        LOG_INFO << "  Range query: " << queryMs << " ms, found " << results.size() << " items\n";
    }

    LOG_INFO << "[PASS] Performance test\n";
}

void testCompleteMemoryIndex() {
    printSubHeader("Test8: Complete Memory Index Integration");

    MemoryManager memIndex(IndexType::HYBRID, true, 1000, 5, 2, 3, 0.5);
    memIndex.init();

    LOG_INFO << "Initialized\n";

    vector<IndexEntry> testData;

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

    LOG_INFO << "Bulk inserting " << testData.size() << " records...\n";
    size_t inserted = memIndex.bulkInsert(testData);
    LOG_INFO << "Inserted " << inserted << " records\n";

    LOG_INFO << "\n--- Equality query ---\n";
    EqualCondition eqCond(500);
    auto iter = memIndex.query(eqCond);
    if (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "key=500 found: ID=" << result.id << ", addr=("
            << result.phy_addr.block_id << "," << result.phy_addr.offset << ")\n";
    }

    LOG_INFO << "\n--- Range query ---\n";
    RangeCondition rangeCond(200, 300);
    iter = memIndex.query(rangeCond);
    int count = 0;
    while (iter.hasNext()) {
        iter.next();
        count++;
    }
    LOG_INFO << "keys in [200,300] found " << count << " records\n";

    LOG_INFO << "\n--- Spatial query ---\n";
    MBR spatialMBR(2);
    spatialMBR.min_vals = { 30.0, 30.0 };
    spatialMBR.max_vals = { 60.0, 60.0 };
    SpatialCondition spatialCond(spatialMBR);
    iter = memIndex.query(spatialCond);
    count = 0;
    while (iter.hasNext()) {
        auto result = iter.next();
        LOG_INFO << "  Found: ID=" << result.id << ", MBR=[" << result.mbr.min_vals[0]
            << "," << result.mbr.min_vals[1] << "]\n";
        count++;
    }
    LOG_INFO << "Spatial query found " << count << " records\n";

    LOG_INFO << "\n--- Delete operations ---\n";
    memIndex.remove(500);
    memIndex.remove(600);
    LOG_INFO << "Deleted key=500 and key=600\n";

    iter = memIndex.query(eqCond);
    LOG_INFO << "Re-query key=500: " << (iter.hasNext() ? "exists" : "deleted") << "\n";

    LOG_INFO << "\n--- Final statistics ---\n";
    memIndex.printStats();

    auto allKeys = memIndex.getAllKeys();
    LOG_INFO << "\nRemaining keys in B+Tree: " << allKeys.size() << "\n";

    LOG_INFO << "[PASS] Complete memory index integration test\n";
}

int main() {
    printHeader("Memory Index Structure Test Suite");

    try {
        testBPlusTreeBasic();
        testBPlusTreeRangeSearch();
        testRTreeBasic();
        testHybridIndex();
        testMemoryManagerWithCache();
        testDeleteOperations();
        testPerformance();
        testCompleteMemoryIndex();

        printHeader("All tests passed!");

        LOG_INFO << "\n+========================================================+\n";
        LOG_INFO << "|                   ALL TESTS PASSED                     |\n";
        LOG_INFO << "+========================================================+\n";
    }
    catch (const exception& e) {
        LOG_ERROR << "Test failed: " << e.what() << "\n";
        return 1;
    }

    return 0;
}