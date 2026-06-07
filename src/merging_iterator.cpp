#include "lsmdb/merging_iterator.h"

namespace lsmdb {

MergingIterator::MergingIterator(std::vector<SSTableReader::Iterator> children)
    : children_(std::move(children)) {}

void MergingIterator::SeekToFirst() {
  for (auto& c : children_) c.SeekToFirst();
  Advance();
}

void MergingIterator::Next() {
  if (!valid_) return;
  Advance();
}

void MergingIterator::Advance() {
  // Find the smallest key among all valid child heads.
  int best = -1;
  for (size_t i = 0; i < children_.size(); ++i) {
    auto& c = children_[i];
    if (!c.status().ok()) {
      status_ = c.status();
      valid_ = false;
      return;
    }
    if (!c.Valid()) continue;
    if (best < 0 || c.key() < children_[best].key()) {
      best = static_cast<int>(i);
    }
  }
  if (best < 0) {
    valid_ = false;
    return;
  }

  // The winning value is the highest-priority (lowest-index) child holding this
  // key. children_ is already in priority order, so the first match wins.
  const std::string& winner_key = children_[best].key();
  cur_key_ = winner_key;
  int winner = best;
  for (size_t i = 0; i < children_.size(); ++i) {
    if (children_[i].Valid() && children_[i].key() == cur_key_) {
      winner = static_cast<int>(i);
      break;  // first (highest-priority) match
    }
  }
  cur_value_ = children_[winner].value();
  cur_is_tombstone_ = children_[winner].IsTombstone();

  // Consume this key from every child that holds it (drop the stale copies).
  for (auto& c : children_) {
    if (c.Valid() && c.key() == cur_key_) c.Next();
  }
  valid_ = true;
}

}  // namespace lsmdb
