// merging_iterator.h —— 多路归并迭代器
//
// 对应组件：LevelDB table/merger.cc。
// 把多个各自有序的迭代器合并成一个有序流。同一个键在多个子迭代器中出现时，
// 只输出"最新"的那一份——约定子迭代器按 新→旧 的顺序传入（下标越小越新）。
//
// 面试可能问：
//   - k 路归并的复杂度？
//     答：用最小堆每步 O(log k)。这里子迭代器个数很少（MemTable 2 个 + L0 几个 + 每层若干），
//     直接线性扫描找最小值 O(k)，代码更简单、更不容易错。[性能] 子迭代器多时应换成堆。
//   - 同一个键出现在多个子迭代器里怎么办？
//     答：输出下标最小（最新）的那个，然后把所有停在这个键上的子迭代器一起前进，
//     旧版本就被跳过了。Compaction 丢弃旧版本也是靠这一点。
//   - 墓碑在这里处理吗？
//     答：不处理，原样输出（type()==kDeletion）。因为 Compaction 需要看到墓碑；
//     面向用户的 Scan 再自己跳过墓碑。
#pragma once

#include <memory>
#include <vector>

#include "kv/iterator.h"

namespace kv {

class MergingIterator : public Iterator {
 public:
  // children[0] 最新，children.back() 最旧
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children);

  bool Valid() const override { return current_ != nullptr; }
  void SeekToFirst() override;
  void Seek(const std::string& target) override;
  void Next() override;

  const std::string& key() const override { return current_->key(); }
  const std::string& value() const override { return current_->value(); }
  ValueType type() const override { return current_->type(); }

 private:
  // 在所有有效的子迭代器里找键最小的；键相同时取下标小的（更新的）
  void FindSmallest();

  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
};

}  // namespace kv
