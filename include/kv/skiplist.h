// skiplist.h —— 有序跳表（MemTable 的底层结构）
//
// 对应组件：LevelDB db/skiplist.h。
// 面试可能问：
//   - 为什么 MemTable 用跳表而不用红黑树（std::map）？
//     答：两者复杂度相同（期望 O(log n)）。跳表实现简单、没有旋转；更关键的是
//     LevelDB 的跳表可以做到"一写多读无锁"（插入只需按从下到上的顺序原子地挂指针）。
//     本项目为了清晰，读写都用锁保护（见 memtable.h），没有用这个特性。
//   - 层高怎么定？为什么概率取 1/4？
//     答：每多一层的概率是 p。p=1/4 时平均每节点 1/(1-p)≈1.33 个指针，
//     查找比较次数约 log_{1/p}(n)/p，是空间和时间的折中（LevelDB 也用 1/4）。
//   - 最大层高 12 够吗？
//     答：p=1/4 时 12 层能高效支撑约 4^12 ≈ 1600 万个节点，MemTable 远到不了。
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace kv {

template <typename Key, typename Value, typename Compare = std::less<Key>>
class SkipList {
 private:
  struct Node {
    Key key;
    Value value;
    std::vector<Node*> next;  // next[i] 是第 i 层的后继；next.size() 就是该节点层高

    Node(Key k, Value v, int height)
        : key(std::move(k)), value(std::move(v)), next(height, nullptr) {}
  };

 public:
  static constexpr int kMaxHeight = 12;

  // 固定随机种子：同样的插入序列得到同样的结构，调试时可复现。
  SkipList() : head_(new Node(Key(), Value(), kMaxHeight)), rng_(0x5eed) {}

  ~SkipList() {
    Node* node = head_;
    while (node != nullptr) {
      Node* next = node->next[0];
      delete node;
      node = next;
    }
  }

  // 节点之间是裸指针互相引用，默认拷贝会导致重复释放，所以禁止拷贝。
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // 插入新键，或覆盖已有键的值。
  // 返回被覆盖的旧值（新插入时返回 nullopt），调用方用它来精确统计内存占用。
  std::optional<Value> InsertOrAssign(const Key& key, Value value) {
    Node* prev[kMaxHeight];
    Node* found = FindGreaterOrEqual(key, prev);
    if (found != nullptr && Equal(found->key, key)) {
      std::optional<Value> old(std::move(found->value));
      found->value = std::move(value);
      return old;
    }

    int height = RandomHeight();
    if (height > height_) {
      // 新节点比当前所有节点都高：高出来的那几层，前驱就是头节点
      for (int level = height_; level < height; ++level) prev[level] = head_;
      height_ = height;
    }
    Node* node = new Node(key, std::move(value), height);
    for (int level = 0; level < height; ++level) {
      node->next[level] = prev[level]->next[level];
      prev[level]->next[level] = node;
    }
    ++size_;
    return std::nullopt;
  }

  // 精确查找。找不到返回 nullptr。
  const Value* Find(const Key& key) const {
    Node* node = FindGreaterOrEqual(key, nullptr);
    if (node != nullptr && Equal(node->key, key)) return &node->value;
    return nullptr;
  }

  size_t Size() const { return size_; }

  // 按键升序遍历。注意：遍历期间不能修改跳表（由调用方加锁保证）。
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    const Value& value() const { return node_->value; }
    void Next() { node_ = node_->next[0]; }
    void SeekToFirst() { node_ = list_->head_->next[0]; }
    // 定位到第一个 >= target 的节点
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && rng_() % 4 == 0) ++height;  // 每层 1/4 概率继续长高
    return height;
  }

  bool Equal(const Key& a, const Key& b) const { return !cmp_(a, b) && !cmp_(b, a); }

  // 从最高层往下找：每层尽量往右走，走不动就下降一层。
  // 返回第 0 层第一个 >= key 的节点；prev 非空时记录每层"最后一个 < key 的节点"，
  // 插入时新节点就挂在这些前驱后面。
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* node = head_;
    for (int level = height_ - 1; level >= 0; --level) {
      while (node->next[level] != nullptr && cmp_(node->next[level]->key, key)) {
        node = node->next[level];
      }
      if (prev != nullptr) prev[level] = node;
    }
    return node->next[0];
  }

  // [性能] 每个节点单独 new，并且 next 用 std::vector（额外一次堆分配）。
  // LevelDB 用 Arena 批量分配 + 柔性数组存 next，分配次数少、缓存更友好。
  // 这里为了可读性选择了简单写法。
  Node* head_;
  int height_ = 1;  // 当前最高层数
  size_t size_ = 0;
  Compare cmp_;
  std::mt19937 rng_;
};

}  // namespace kv
