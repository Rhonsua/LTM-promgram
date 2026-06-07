#ifndef COLD_HOT_MANAGER_H
#define COLD_HOT_MANAGER_H

#include <unordered_map>
#include <cstdint>
#include <vector>

template<typename Key>
class ColdHotManager {
private:
    struct AccessRecord {
        uint32_t frequency;
        uint64_t last_access;
    };

    std::unordered_map<Key, AccessRecord> records;
    uint64_t cycle = 0;
    uint32_t cold_threshold;
    double decay_factor;

public:
    explicit ColdHotManager(uint32_t threshold = 2, double decay = 0.5)
        : cold_threshold(threshold), decay_factor(decay) {
    }

    void access(const Key& key) {
        auto& rec = records[key];
        rec.frequency++;
        rec.last_access = cycle;
    }

    void decay() {
        cycle++;
        for (auto it = records.begin(); it != records.end(); ) {
            if (it->second.last_access < cycle) {
                it->second.frequency = static_cast<uint32_t>(it->second.frequency * decay_factor);
                if (it->second.frequency == 0) {
                    it = records.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    bool isCold(const Key& key) const {
        auto it = records.find(key);
        if (it == records.end()) return true;
        return it->second.frequency < cold_threshold;
    }

    std::vector<Key> getColdKeys() const {
        std::vector<Key> colds;
        for (const auto& [key, rec] : records) {
            if (rec.frequency < cold_threshold)
                colds.push_back(key);
        }
        return colds;
    }

    void remove(const Key& key) {
        records.erase(key);
    }

    void clear() {
        records.clear();
        cycle = 0;
    }
};

#endif // COLD_HOT_MANAGER_H