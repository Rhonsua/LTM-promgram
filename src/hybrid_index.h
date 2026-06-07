#ifndef HYBRID_INDEX_H
#define HYBRID_INDEX_H

#include "bplus_tree.h"
#include "r_tree.h"
#include "index_interface.h"  // IndexEntry, QueryCondition, QueryIterator, IndexType
#include "log.h"
#include <memory>
#include <stdexcept>

class HybridIndex {
private:
    std::unique_ptr<BPTree> btree_;
    std::unique_ptr<RTree>  rtree_;
    IndexType               type_;
    uint32_t                dimension_;   // R树维度
    bool                    initialized_ = false;

public:
    // 构造函数
    HybridIndex(IndexType type, uint32_t btree_order, uint32_t rtree_dim)
        : type_(type), dimension_(rtree_dim)
    {
        if (type == IndexType::BTREE_ONLY || type == IndexType::HYBRID)
            btree_ = std::make_unique<BPTree>(btree_order);
        if (type == IndexType::RTREE_ONLY || type == IndexType::HYBRID)
            rtree_ = std::make_unique<RTree>(rtree_dim, DEFAULT_R_TREE_ORDER);
    }

    // 初始化
    void init() {
        if (initialized_) {
            LOG_INFO << "[HybridIndex] 已经初始化" << std::endl;
            return;
        }
        if (btree_) btree_->init();
        if (rtree_) rtree_->init();
        initialized_ = true;
        LOG_INFO << "[HybridIndex] 初始化完成" << std::endl;
    }

    bool isInitialized() const { return initialized_; }

    // 插入（仅索引层，不涉及缓存）
    bool insert(const IndexEntry& entry) {
        if (!initialized_) {
            LOG_ERROR << "[HybridIndex] 索引未初始化，插入失败" << std::endl;
            return false;
        }
        if (!entry.isValid()) {
            LOG_ERROR << "[HybridIndex] 无效的IndexEntry" << std::endl;
            return false;
        }

        bool ok = true;
        if (btree_)
            ok &= btree_->insert(entry.key, entry.phy_addr);
        if (rtree_)
            ok &= rtree_->insert(entry.toSpatialEntry());

        return ok;
    }

    // 按key删除（B+树 + 尝试R树删除，但R树需要mbr，这里仅做B+树删除）
    bool removeByKey(uint64_t key) {
        if (!initialized_ || !btree_) {
            LOG_ERROR << "[HybridIndex] 无B+树或未初始化，删除失败" << std::endl;
            return false;
        }
        btree_->remove(key);
        // R树删除需要完整的SpatialEntry，此处暂不处理，由上层通过其他方式补充
        return true;
    }

    // 根据完整条目删除（同时删B+树和R树）
    bool removeEntry(const IndexEntry& entry) {
        if (!initialized_) return false;
        bool ok = true;
        if (btree_)
            btree_->remove(entry.key);
        if (rtree_)
            ok &= rtree_->remove(entry.toSpatialEntry());
        return ok;
    }

    // 查询
    QueryIterator query(const QueryCondition& condition) const {
        QueryIterator iter;
        if (!initialized_) {
            LOG_ERROR << "[HybridIndex] 索引未初始化，查询失败" << std::endl;
            return iter;
        }

        switch (condition.type) {
        case QueryType::EQUAL: {
            auto& eq = static_cast<const EqualCondition&>(condition);
            if (btree_) {
                PhyAddr addr = btree_->search(eq.key);
                if (addr.isValid()) {
                    IndexEntry entry;
                    entry.key = eq.key;
                    entry.phy_addr = addr;
                    entry.id = eq.key;   // 假设key即id，实际项目可改进
                    iter.addResult(entry);
                }
            }
            break;
        }
        case QueryType::RANGE: {
            auto& range = static_cast<const RangeCondition&>(condition);
            if (btree_) {
                auto res = btree_->rangeSearch(range.start_key, range.end_key);
                for (auto& [key, addr] : res) {
                    IndexEntry entry;
                    entry.key = key;
                    entry.phy_addr = addr;
                    entry.id = key;
                    iter.addResult(entry);
                }
            }
            break;
        }
        case QueryType::SPATIAL: {
            auto& spatial = static_cast<const SpatialCondition&>(condition);
            if (rtree_) {
                std::vector<SpatialEntry> sp_res;
                rtree_->search(spatial.query_mbr, sp_res);
                for (auto& sp : sp_res) {
                    IndexEntry entry;
                    entry.id = sp.id;
                    entry.mbr = sp.mbr;
                    entry.phy_addr = sp.phy_addr;
                    if (btree_) {
                        // 尝试通过id反查key（简化：key = id）
                        entry.key = sp.id;
                    }
                    iter.addResult(entry);
                }
            }
            break;
        }
        }
        return iter;
    }

    // 获取全部key（来自B+树）
    std::vector<uint64_t> getAllKeys() const {
        if (btree_) return btree_->getAllKeys();
        return {};
    }

    // 清空索引
    void clear() {
        if (btree_) btree_->clear();
        if (rtree_) rtree_->clear();
        initialized_ = false;
        LOG_INFO << "[HybridIndex] 索引已清空" << std::endl;
    }

    // 暴露底层指针（供高级操作）
    BPTree* getBTree() { return btree_.get(); }
    RTree* getRTree() { return rtree_.get(); }
    const BPTree* getBTree() const { return btree_.get(); }
    const RTree* getRTree() const { return rtree_.get(); }
};

#endif // HYBRID_INDEX_H