#include "lsmdb/cache.h"

namespace lsmdb {

BlockPtr BlockCache::Lookup(uint64_t file_number, uint64_t offset) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(Key{file_number, offset});
  if (it == map_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  // Promote to most-recently-used by splicing the node to the front.
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->block;
}

void BlockCache::Insert(uint64_t file_number, uint64_t offset, BlockPtr block) {
  if (capacity_bytes_ == 0 || !block) return;
  std::lock_guard<std::mutex> lk(mu_);
  Key key{file_number, offset};
  const size_t bytes = block->size();

  auto it = map_.find(key);
  if (it != map_.end()) {
    // Refresh in place.
    used_bytes_ -= it->second->bytes;
    it->second->block = std::move(block);
    it->second->bytes = bytes;
    used_bytes_ += bytes;
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }

  lru_.push_front(Entry{key, std::move(block), bytes});
  map_[key] = lru_.begin();
  used_bytes_ += bytes;

  // Evict least-recently-used blocks until back under budget (always keep at
  // least the block just inserted).
  while (used_bytes_ > capacity_bytes_ && lru_.size() > 1) {
    Entry& victim = lru_.back();
    used_bytes_ -= victim.bytes;
    map_.erase(victim.key);
    lru_.pop_back();
  }
}

uint64_t BlockCache::hits() const {
  std::lock_guard<std::mutex> lk(mu_);
  return hits_;
}
uint64_t BlockCache::misses() const {
  std::lock_guard<std::mutex> lk(mu_);
  return misses_;
}
double BlockCache::HitRate() const {
  std::lock_guard<std::mutex> lk(mu_);
  uint64_t total = hits_ + misses_;
  return total ? static_cast<double>(hits_) / total : 0.0;
}
size_t BlockCache::size_bytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  return used_bytes_;
}
void BlockCache::ResetStats() {
  std::lock_guard<std::mutex> lk(mu_);
  hits_ = 0;
  misses_ = 0;
}

}  // namespace lsmdb
