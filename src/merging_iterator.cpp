#include "kv/merging_iterator.h"

namespace kv {

MergingIterator::MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
    : children_(std::move(children)) {}

void MergingIterator::SeekToFirst() {
  for (auto& child : children_) child->SeekToFirst();
  FindSmallest();
}

void MergingIterator::Seek(const std::string& target) {
  for (auto& child : children_) child->Seek(target);
  FindSmallest();
}

void MergingIterator::Next() {
  // 先把键拷出来：下面推进 current_ 之后，key() 引用就失效了
  const std::string current_key = current_->key();
  // 所有停在当前键上的子迭代器一起前进——旧版本就此被跳过
  for (auto& child : children_) {
    if (child->Valid() && child->key() == current_key) child->Next();
  }
  FindSmallest();
}

void MergingIterator::FindSmallest() {
  current_ = nullptr;
  for (auto& child : children_) {
    if (!child->Valid()) continue;
    // 严格小于才替换：键相同时保留先遇到的（下标更小 = 更新）
    if (current_ == nullptr || child->key() < current_->key()) current_ = child.get();
  }
}

}  // namespace kv
