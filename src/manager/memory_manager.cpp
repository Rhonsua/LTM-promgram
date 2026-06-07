#include "memory_manager.h"
#include "log.h"
#include <iomanip>

MemoryManager::MemoryManager(IndexType type, bool useCache, size_t cacheSize,
    uint32_t btreeOrder, uint32_t rtreeDim,
    uint32_t coldThreshold, double decayFactor)
    : index_(type, btreeOrder, rtreeDim),
    cache_(cacheSize),
    hotCold_(coldThreshold, decayFactor),
    enableCache_(useCache)
{
}

void MemoryManager::updateCache(const IndexEntry& entry) {
    if (!enableCache_) return;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.put(entry.id, entry);
    hotCold_.access(entry.id);
}

std::optional<IndexEntry> MemoryManager::getFromCache(uint64_t id) {
    if (!enableCache_) return std::nullopt;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    IndexEntry entry;
    if (cache_.get(id, entry)) {
        stats_.cache_hits++;
        hotCold_.access(id);
        return entry;
    }
    stats_.cache_misses++;
    return std::nullopt;
}

void MemoryManager::init() {
    if (initialized_) {
        LOG_INFO << "[MemoryManager] Already initialized" << std::endl;
        return;
    }
    index_.init();
    cache_.clear();
    hotCold_.clear();
    key_to_id_.clear();
    initialized_ = true;
    LOG_INFO << "[MemoryManager] Init success, cache "
        << (enableCache_ ? "enabled" : "disabled") << std::endl;
}

bool MemoryManager::insert(const IndexEntry& entry) {
    if (!initialized_) {
        LOG_ERROR << "[MemoryManager] Not initialized" << std::endl;
        return false;
    }
    if (!entry.isValid()) {
        LOG_ERROR << "[MemoryManager] Invalid IndexEntry" << std::endl;
        return false;
    }

    bool ok = index_.insert(entry);
    if (ok && enableCache_) {
        updateCache(entry);
        std::lock_guard<std::mutex> lock(cacheMutex_);
        key_to_id_[entry.key] = entry.id;   // 记录 key->id
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.total_entries++;
    }
    return ok;
}

size_t MemoryManager::bulkInsert(const std::vector<IndexEntry>& entries) {
    size_t cnt = 0;
    for (auto& e : entries) {
        if (insert(e)) cnt++;
    }
    LOG_INFO << "[MemoryManager] Bulk insert: " << cnt
        << "/" << entries.size() << " succeeded" << std::endl;
    return cnt;
}

QueryIterator MemoryManager::query(const QueryCondition& condition) {
    if (!initialized_) {
        LOG_ERROR << "[MemoryManager] Not initialized" << std::endl;
        return QueryIterator{};
    }

    // 等值查询：优先通过 key 找到 id 后再查缓存
    if (condition.type == QueryType::EQUAL) {
        auto& eq = static_cast<const EqualCondition&>(condition);
        if (enableCache_) {
            uint64_t id = 0;
            {
                std::lock_guard<std::mutex> lock(cacheMutex_);
                auto it = key_to_id_.find(eq.key);
                if (it != key_to_id_.end()) id = it->second;
            }
            if (id != 0) {
                auto cached = getFromCache(id);
                if (cached) {
                    QueryIterator iter;
                    iter.addResult(*cached);
                    return iter;
                }
            }
        }
    }

    // 其他情况（范围/空间/缓存未命中）走底层索引
    QueryIterator iter = index_.query(condition);

    // 空间查询后尝试用缓存补全
    if (enableCache_ && condition.type == QueryType::SPATIAL) {
        auto& results = const_cast<std::vector<IndexEntry>&>(iter.getResults());
        for (auto& entry : results) {
            auto cached = getFromCache(entry.id);
            if (cached) entry = *cached;
        }
    }
    return iter;
}

std::optional<IndexEntry> MemoryManager::findById(uint64_t id) {
    auto cached = getFromCache(id);
    if (cached) return cached;
    if (index_.getBTree()) {
        PhyAddr addr = index_.getBTree()->search(id);
        if (addr.isValid()) {
            IndexEntry entry;
            entry.id = id;
            entry.key = id;
            entry.phy_addr = addr;
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<PhyAddr> MemoryManager::findByKey(uint64_t key) const {
    if (!index_.getBTree()) return std::nullopt;
    PhyAddr addr = index_.getBTree()->search(key);
    if (addr.isValid()) return addr;
    return std::nullopt;
}

std::vector<std::pair<uint64_t, PhyAddr>> MemoryManager::rangeSearchByKey(uint64_t start, uint64_t end) const {
    if (!index_.getBTree()) return {};
    return index_.getBTree()->rangeSearch(start, end);
}

bool MemoryManager::remove(uint64_t key) {
    if (!initialized_) {
        LOG_ERROR << "[MemoryManager] Not initialized" << std::endl;
        return false;
    }
    bool ok = index_.removeByKey(key);
    if (ok && enableCache_) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = key_to_id_.find(key);
        if (it != key_to_id_.end()) {
            cache_.erase(it->second);
            hotCold_.remove(it->second);
            key_to_id_.erase(it);
        }
    }
    return ok;
}

size_t MemoryManager::precleanColdData() {
    if (!enableCache_) return 0;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    hotCold_.decay();
    auto coldKeys = hotCold_.getColdKeys();
    size_t cleaned = 0;
    for (auto& id : coldKeys) {
        cache_.erase(id);
        hotCold_.remove(id);
        cleaned++;
    }
    if (cleaned > 0)
        LOG_INFO << "[MemoryManager] Preclean: removed " << cleaned << " cold entries" << std::endl;
    return cleaned;
}

void MemoryManager::decayAccessFrequency() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    hotCold_.decay();
}

void MemoryManager::clear() {
    index_.clear();
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.clear();
        hotCold_.clear();
        key_to_id_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = IndexStats{};
    }
    initialized_ = false;
    LOG_INFO << "[MemoryManager] Cleared" << std::endl;
}

IndexStats MemoryManager::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void MemoryManager::printStats() const {
    auto s = getStats();
    LOG_INFO << "\n======== Memory Manager Stats ========\n"
        << "Total entries: " << s.total_entries << "\n"
        << "Cache hits: " << s.cache_hits << "\n"
        << "Cache misses: " << s.cache_misses << "\n";
    size_t total = s.cache_hits + s.cache_misses;
    if (total > 0) {
        double rate = (double)s.cache_hits / total * 100.0;
        LOG_INFO << "Cache hit rate: " << std::fixed << std::setprecision(2) << rate << "%\n";
    }
    LOG_INFO << "=========================================" << std::endl;
}

std::vector<uint64_t> MemoryManager::getAllKeys() const {
    return index_.getAllKeys();
}