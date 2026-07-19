#include "lsmdb/skiplist.h"

namespace lsmdb {

SkipList::SkipList(unsigned seed)
    : head_(new Node(kMaxLevel)),
      level_(1),
      count_(0),
      mem_usage_(0),
      rng_(seed),
      dist_(0.0, 1.0) {}

SkipList::~SkipList() {
  // Walk the bottom level (level 0 links every node exactly once) and free.
  Node* n = head_->forward[0];
  while (n != nullptr) {
    Node* next = n->forward[0];
    delete n;
    n = next;
  }
  delete head_;
}

// Geometric distribution with p = 0.5: each additional level is half as likely.
// Expected height ~2, capped at kMaxLevel.
int SkipList::RandomLevel() {
  int lvl = 1;
  while (lvl < kMaxLevel && dist_(rng_) < 0.5) {
    ++lvl;
  }
  return lvl;
}

void SkipList::Put(const std::string& key, const std::string& value) {
  Insert(key, value, /*tombstone=*/false);
}

void SkipList::Delete(const std::string& key) {
  Insert(key, /*value=*/std::string(), /*tombstone=*/true);
}

void SkipList::Insert(const std::string& key, const std::string& value,
                      bool tombstone) {
  // update[i] = last node at level i whose key < the search key.
  std::vector<Node*> update(kMaxLevel, head_);
  Node* x = head_;
  for (int i = level_ - 1; i >= 0; --i) {
    while (x->forward[i] != nullptr && x->forward[i]->key < key) {
      x = x->forward[i];
    }
    update[i] = x;
  }

  Node* next = x->forward[0];
  if (next != nullptr && next->key == key) {
    // Overwrite in place. Adjust the tracked memory by the value delta so the
    // flush threshold stays accurate across overwrites.
    mem_usage_ -= next->value.size();
    next->value = value;
    next->is_tombstone = tombstone;
    mem_usage_ += next->value.size();
    return;
  }

  int height = RandomLevel();
  if (height > level_) {
    for (int i = level_; i < height; ++i) {
      update[i] = head_;
    }
    level_ = height;
  }

  Node* node = new Node(key, value, tombstone, height);
  for (int i = 0; i < height; ++i) {
    node->forward[i] = update[i]->forward[i];
    update[i]->forward[i] = node;
  }

  ++count_;
  // Node overhead + forward pointer array approximated so the memtable size
  // estimate tracks real allocation, not just key/value bytes.
  mem_usage_ += key.size() + value.size() +
                sizeof(Node) + height * sizeof(Node*);
}

LookupResult SkipList::Get(const std::string& key, std::string* value) const {
  Node* x = head_;
  for (int i = level_ - 1; i >= 0; --i) {
    while (x->forward[i] != nullptr && x->forward[i]->key < key) {
      x = x->forward[i];
    }
  }
  Node* next = x->forward[0];
  if (next != nullptr && next->key == key) {
    if (next->is_tombstone) return LookupResult::kDeleted;
    if (value != nullptr) *value = next->value;
    return LookupResult::kFound;
  }
  return LookupResult::kNotFound;
}

// ---- Iterator ---------------------------------------------------------------

SkipList::Iterator::Iterator(const SkipList* list)
    : list_(list), node_(nullptr) {}

void SkipList::Iterator::SeekToFirst() {
  node_ = list_->head_->forward[0];
}

void SkipList::Iterator::Next() {
  node_ = static_cast<Node*>(node_)->forward[0];
}

const std::string& SkipList::Iterator::key() const {
  return static_cast<Node*>(node_)->key;
}

const std::string& SkipList::Iterator::value() const {
  return static_cast<Node*>(node_)->value;
}

bool SkipList::Iterator::IsTombstone() const {
  return static_cast<Node*>(node_)->is_tombstone;
}

}  // namespace lsmdb
