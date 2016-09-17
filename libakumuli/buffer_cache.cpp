#include "buffer_cache.h"

namespace Akumuli {

ChunkCache::ChunkCache(size_t limit)
    : total_size_(0ul)
    , size_limit_(limit)
{
}

bool ChunkCache::contains(KeyT key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.count(key) > 0;
}

ChunkCache::ItemT ChunkCache::get(KeyT key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return ItemT();
    }
    return it->second;
}

static size_t get_size(const std::shared_ptr<UncompressedChunk>& header) {
    return header->paramids.size()   * sizeof(aku_ParamId) +
           header->timestamps.size() * sizeof(aku_Timestamp) +
           header->values.size()     * sizeof(double);
}

void ChunkCache::put(KeyT key, const std::shared_ptr<UncompressedChunk>& header) {
    auto szdelta = get_size(header);
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_size_ + szdelta > size_limit_) {
        if (!fifo_.empty()) {
            // Eviction
            KeyT ekey;
            size_t esz;
            std::tie(ekey, esz) = fifo_.back();
            fifo_.pop_back();
            auto it = cache_.find(ekey);
            if (it != cache_.end()) {
                cache_.erase(it);
            } else {
                // TODO: report error (inconsistent cache)
            }
            total_size_ -= esz;
        }
    }
    fifo_.push_front(std::make_tuple(key, szdelta));
    cache_[key] = header;
    total_size_ += szdelta;
}

}

