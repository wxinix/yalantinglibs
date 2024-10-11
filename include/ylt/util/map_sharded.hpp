#pragma once
#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace ylt::util {
namespace internal {
template <typename Map>
class map_lock_t {
 public:
  using key_type = typename Map::key_type;
  using value_type = typename Map::value_type;
  using mapped_type = typename Map::mapped_type;
  map_lock_t() : mtx_(std::make_unique<std::mutex>()) {}

  std::shared_ptr<typename mapped_type::element_type> find(
      const key_type& key) const {
    std::lock_guard lock(*mtx_);
    if (!map_) [[unlikely]] {
      return nullptr;
    }
    auto it = map_->find(key);
    return it->second;
  }

  template <typename... Args>
  std::pair<value_type&, bool> try_emplace(const key_type& key,
                                           Args&&... args) {
    std::lock_guard lock(*mtx_);
    auto result = visit_map().try_emplace(key, std::forward<Args>(args)...);
    return {*result.first, result.second};
  }

  template <typename... Args>
  std::pair<value_type&, bool> try_emplace(key_type&& key, Args&&... args) {
    std::lock_guard lock(*mtx_);
    auto result =
        visit_map().try_emplace(std::move(key), std::forward<Args>(args)...);
    return {*result.first, result.second};
  }

  size_t erase(const key_type& key) {
    std::lock_guard lock(*mtx_);
    if (!map_) [[unlikely]] {
      return 0;
    }
    return map_->erase(key);
  }

  template <typename Func>
  size_t erase_if(Func&& op) {
    std::lock_guard guard(*mtx_);
    if (!map_) [[unlikely]] {
      return 0;
    }
    return std::erase_if(*map_, std::forward<Func>(op));
  }

  template <typename Func>
  bool for_each(Func&& op) {
    std::lock_guard guard(*mtx_);
    if (!map_) [[unlikely]] {
      return true;
    }
    for (auto& e : *map_) {
      if constexpr (requires { op(e) == true; }) {
        if (!op(e)) {
          break;
          return false;
        }
      }
      else {
        op(e);
      }
    }
    return true;
  }

  template <typename Func>
  bool for_each(Func&& op) const {
    std::lock_guard guard(*mtx_);
    if (!map_) [[unlikely]] {
      return true;
    }
    for (const auto& e : *map_) {
      if constexpr (requires { op(e) == true; }) {
        if (!op(e)) {
          break;
          return false;
        }
      }
      else {
        op(e);
      }
    }
    return true;
  }

 private:
  Map& visit_map() {
    if (!map_) [[unlikely]] {
      map_ = std::make_unique<Map>();
    }
    return *map_;
  }

  std::unique_ptr<std::mutex> mtx_;
  std::unique_ptr<Map> map_;
};
}  // namespace internal

template <typename Map, typename Hash>
class map_sharded_t {
 public:
  using key_type = typename Map::key_type;
  using value_type = typename Map::mapped_type;
  map_sharded_t(size_t shard_num) : shards_(shard_num) {}

  template <typename... Args>
  auto try_emplace(key_type&& key, Args&&... args) {
    auto result = get_sharded(Hash{}(key))
                      .try_emplace(std::move(key), std::forward<Args>(args)...);
    if (result.second) {
      size_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  template <typename... Args>
  auto try_emplace(const key_type& key, Args&&... args) {
    auto result =
        get_sharded(Hash{}(key)).try_emplace(key, std::forward<Args>(args)...);
    if (result.second) {
      size_.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  size_t size() const { return size_.load(std::memory_order_relaxed); }

  auto find(const key_type& key) const {
    return get_sharded(Hash{}(key)).find(key);
  }

  size_t erase(const key_type& key) {
    auto result = get_sharded(Hash{}(key)).erase(key);
    if (result) {
      size_.fetch_sub(result, std::memory_order_relaxed);
    }
    return result;
  }

  template <typename Func>
  size_t erase_if(Func&& op) {
    auto total = 0;
    for (auto& map : shards_) {
      auto result = map.erase_if(std::forward<Func>(op));
      total += result;
      size_.fetch_sub(result, std::memory_order_relaxed);
    }
    return total;
  }

  template <typename Func>
  size_t erase_one(Func&& op) {
    auto total = 0;
    for (auto& map : shards_) {
      auto result = map.erase_if(std::forward<Func>(op));
      if (result) {
        total += result;
        size_.fetch_sub(result, std::memory_order_relaxed);
        break;
      }
    }
    return total;
  }

  template <typename Func>
  void for_each(Func&& op) {
    for (auto& map : shards_) {
      if (!map.for_each(op))
        break;
    }
  }

  template <typename T>
  std::vector<T> copy(auto&& op) const {
    std::vector<T> ret;
    ret.reserve(size_.load(std::memory_order_relaxed));
    for (auto& map : shards_) {
      map.for_each([&ret, &op](auto& e) {
        if (op(e.second)) {
          ret.push_back(e.second);
        }
      });
    }
    return ret;
  }
  template <typename T>
  std::vector<T> copy() const {
    return copy<T>([](auto&) {
      return true;
    });
  }

 private:
  internal::map_lock_t<Map>& get_sharded(size_t hash) {
    return shards_[hash % shards_.size()];
  }
  const internal::map_lock_t<Map>& get_sharded(size_t hash) const {
    return shards_[hash % shards_.size()];
  }

  std::vector<internal::map_lock_t<Map>> shards_;
  std::atomic<std::size_t> size_;
};
}  // namespace ylt::util