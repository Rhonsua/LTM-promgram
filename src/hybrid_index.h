#pragma once
#include "bplus_tree.h"
#include "r_tree.h"
#include "index_interface.h"
#include "log.h"
#include <memory>
#include <unordered_map>

class HybridIndex {
private:
    std::unique_ptr<BPTree> btree_;
    std::unique_ptr<RTree>  rtree_;
    IndexType               type_;
    uint32_t                dimension_;
    bool                    initialized_ = false;
    std::unordered_map<uint64_t, SpatialEntry> key_to_spatial_;

public:
    HybridIndex(IndexType type, uint32_t btree_order, uint32_t rtree_dim)
        : type_(type), dimension_(rtree_dim) {
        if (type == IndexType::BTREE_ONLY || type == IndexType::HYBRID)
            btree_ = std::make_unique<BPTree>(btree_order);
        if (type == IndexType::RTREE_ONLY || type == IndexType::HYBRID)
            rtree_ = std::make_unique<RTree>(rtree_dim, DEFAULT_R_TREE_ORDER);
    }

    void init() {
        if (initialized_) {
            LOG_INFO << "[HybridIndex] Already initialized" << std::endl;
            return;
        }
        if (btree_) btree_->init();
        if (rtree_) rtree_->init();
        key_to_spatial_.clear();
        initialized_ = true;
        LOG_INFO << "[HybridIndex] Init done" << std::endl;
    }

    bool isInitialized() const { return initialized_; }

    bool insert(const IndexEntry& entry) {
        if (!initialized_) return false;
        if (!entry.isValid()) return false;
        bool ok = true;
        if (btree_) ok &= btree_->insert(entry.key, entry.phy_addr);
        if (rtree_) {
            SpatialEntry sp = entry.toSpatialEntry();
            ok &= rtree_->insert(sp);
            if (ok) key_to_spatial_[entry.key] = sp;
        }
        return ok;
    }

    bool removeByKey(uint64_t key) {
        if (!initialized_) return false;
        if (btree_) btree_->remove(key);
        auto it = key_to_spatial_.find(key);
        if (it != key_to_spatial_.end()) {
            if (rtree_) rtree_->remove(it->second);
            key_to_spatial_.erase(it);
        }
        return true;
    }

    QueryIterator query(const QueryCondition& condition) const {
        QueryIterator iter;
        if (!initialized_) return iter;

        switch (condition.type) {
        case QueryType::EQUAL: {
            auto& eq = static_cast<const EqualCondition&>(condition);
            if (btree_) {
                PhyAddr addr = btree_->search(eq.key);
                if (addr.isValid()) {
                    IndexEntry entry;
                    entry.key = eq.key;
                    entry.phy_addr = addr;
                    // 尝试从映射中获取完整 id 和 mbr
                    auto it = key_to_spatial_.find(eq.key);
                    if (it != key_to_spatial_.end()) {
                        entry.id = it->second.id;
                        entry.mbr = it->second.mbr;
                    }
                    else {
                        entry.id = eq.key;   // 回退：无映射时假设 id == key
                    }
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
                    auto it = key_to_spatial_.find(key);
                    if (it != key_to_spatial_.end()) {
                        entry.id = it->second.id;
                        entry.mbr = it->second.mbr;
                    }
                    else {
                        entry.id = key;
                    }
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
                    // 通过 id 查找 key（若有）
                    if (btree_) entry.key = sp.id;   // 简化：假设 key == id 或从其他映射获取
                    iter.addResult(entry);
                }
            }
            break;
        }
        }
        return iter;
    }

    std::vector<uint64_t> getAllKeys() const {
        if (btree_) return btree_->getAllKeys();
        return {};
    }

    void clear() {
        if (btree_) btree_->clear();
        if (rtree_) rtree_->clear();
        key_to_spatial_.clear();
        initialized_ = false;
        LOG_INFO << "[HybridIndex] Cleared" << std::endl;
    }

    // 非 const 版本（返回非 const 指针）
    BPTree* getBTree() { return btree_.get(); }
    RTree* getRTree() { return rtree_.get(); }

    // const 版本（返回 const 指针）
    const BPTree* getBTree() const { return btree_.get(); }
    const RTree* getRTree() const { return rtree_.get(); }
};