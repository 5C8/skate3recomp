#pragma once

// Open-addressing hash containers for the native renderer's hot lookups
// (per-draw hooks, per-item builds on the guest render thread). The std
// node-based maps they replace cost a heap node per insert and a dependent
// cache miss (bucket -> node) per lookup, thousands of times per frame.
// Linear probing over power-of-two tables, load factor <= 1/2, backward-
// shift erase (no tombstones). Insert/lookup/erase only; NOT drop-in
// std::unordered_map replacements (find returns a pointer, no iterators
// except for_each).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace skate3::native_scene {

inline size_t FlatHash(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdull;
  k ^= k >> 33;
  return size_t(k);
}

// Values stored inline next to their keys: for small values (<= ~64 bytes).
template <typename K, typename V>
class FlatMap {
 public:
  struct Entry {
    K first;
    V second;
  };

  // Empties the map (keeps the table; grows it to fit ~`expected` keys).
  void reset(size_t expected = 0) {
    clear();
    reserve(expected);
  }
  void clear() {
    if (size_ != 0) {
      std::fill(used_.begin(), used_.end(), uint8_t(0));
      size_ = 0;
    }
  }
  void reserve(size_t n) {
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    if (cap > used_.size()) Rehash(cap);
  }
  // Inserts a value-initialized V for k unless present: {slot, inserted}.
  std::pair<V*, bool> try_emplace(K k) {
    if ((size_ + 1) * 2 > used_.size()) Rehash(used_.empty() ? 16 : used_.size() * 2);
    size_t i = FlatHash(uint64_t(k)) & mask_;
    while (used_[i]) {
      if (slots_[i].first == k) return {&slots_[i].second, false};
      i = (i + 1) & mask_;
    }
    used_[i] = 1;
    slots_[i].first = k;
    slots_[i].second = V{};
    ++size_;
    return {&slots_[i].second, true};
  }
  V& operator[](K k) { return *try_emplace(k).first; }
  V* find(K k) {
    if (size_ == 0) return nullptr;
    for (size_t i = FlatHash(uint64_t(k)) & mask_; used_[i]; i = (i + 1) & mask_) {
      if (slots_[i].first == k) return &slots_[i].second;
    }
    return nullptr;
  }
  const V* find(K k) const { return const_cast<FlatMap*>(this)->find(k); }
  bool contains(K k) const { return find(k) != nullptr; }
  bool erase(K k) {
    if (size_ == 0) return false;
    size_t i = FlatHash(uint64_t(k)) & mask_;
    while (used_[i] && !(slots_[i].first == k)) i = (i + 1) & mask_;
    if (!used_[i]) return false;
    // Backward-shift: pull later members of the probe run into the hole
    // when their home slot does not lie cyclically in (hole, j].
    used_[i] = 0;
    --size_;
    for (size_t j = (i + 1) & mask_; used_[j]; j = (j + 1) & mask_) {
      const size_t h = FlatHash(uint64_t(slots_[j].first)) & mask_;
      const bool stays = i <= j ? (i < h && h <= j) : (i < h || h <= j);
      if (stays) continue;
      slots_[i] = std::move(slots_[j]);
      used_[i] = 1;
      used_[j] = 0;
      i = j;
    }
    return true;
  }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  void swap(FlatMap& o) noexcept {
    slots_.swap(o.slots_);
    used_.swap(o.used_);
    std::swap(size_, o.size_);
    std::swap(mask_, o.mask_);
  }
  template <typename F>
  void for_each(F&& f) {
    for (size_t i = 0; i < used_.size(); ++i) {
      if (used_[i]) f(slots_[i].first, slots_[i].second);
    }
  }

 private:
  void Rehash(size_t cap) {
    std::vector<Entry> slots(cap);
    std::vector<uint8_t> used(cap, 0);
    slots.swap(slots_);
    used.swap(used_);
    mask_ = cap - 1;
    size_ = 0;
    for (size_t i = 0; i < used.size(); ++i) {
      if (!used[i]) continue;
      size_t j = FlatHash(uint64_t(slots[i].first)) & mask_;
      while (used_[j]) j = (j + 1) & mask_;
      used_[j] = 1;
      slots_[j] = std::move(slots[i]);
      ++size_;
    }
  }

  std::vector<Entry> slots_;
  std::vector<uint8_t> used_;
  size_t size_ = 0;
  size_t mask_ = 0;
};

template <typename K>
class FlatSet {
 public:
  void reset(size_t expected = 0) { map_.reset(expected); }
  void clear() { map_.clear(); }
  // std::unordered_set::insert shape: .second = newly inserted.
  std::pair<bool, bool> insert(K k) { return {true, map_.try_emplace(k).second}; }
  bool contains(K k) const { return map_.contains(k); }
  size_t count(K k) const { return map_.contains(k) ? 1 : 0; }
  bool erase(K k) { return map_.erase(k); }
  size_t size() const { return map_.size(); }
  bool empty() const { return map_.empty(); }
  void reserve(size_t n) { map_.reserve(n); }
  void swap(FlatSet& o) noexcept { map_.swap(o.map_); }
  template <typename F>
  void for_each(F&& f) {
    map_.for_each([&](K k, uint8_t) { f(k); });
  }

 private:
  FlatMap<K, uint8_t> map_;
};

// Large values in a pooled deque (stable addresses across inserts) behind a
// flat key -> pool index table: for maps whose values are hundreds of bytes
// and whose element pointers are held across other inserts.
template <typename K, typename V>
class PooledFlatMap {
 public:
  V* find(K k) {
    const uint32_t* idx = index_.find(k);
    return idx != nullptr ? &pool_[*idx] : nullptr;
  }
  V& operator[](K k) {
    auto [idx, inserted] = index_.try_emplace(k);
    if (inserted) {
      if (!free_.empty()) {
        *idx = free_.back();
        free_.pop_back();
      } else {
        *idx = uint32_t(pool_.size());
        pool_.emplace_back();
      }
    }
    return pool_[*idx];
  }
  bool erase(K k) {
    const uint32_t* idx = index_.find(k);
    if (idx == nullptr) return false;
    const uint32_t i = *idx;
    index_.erase(k);
    pool_[i] = V{};  // release the value's own resources now
    free_.push_back(i);
    return true;
  }
  void clear() {
    index_.clear();
    pool_.clear();
    free_.clear();
  }
  size_t size() const { return index_.size(); }
  bool empty() const { return index_.empty(); }

 private:
  FlatMap<K, uint32_t> index_;
  std::deque<V> pool_;
  std::vector<uint32_t> free_;
};

}  // namespace skate3::native_scene
