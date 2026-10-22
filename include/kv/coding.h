// coding.h —— 定长整数的编码/解码
//
// 对应组件：LevelDB util/coding.h。
// 面试可能问：
//   - 为什么不直接 memcpy 一个 uint32_t 到文件里？
//     答：内存里的字节序取决于 CPU（x86 是小端，有些 ARM/网络协议是大端）。
//     文件格式必须固定字节序，否则换台机器就读不出来。这里用移位显式写成小端。
//   - 为什么用定长 4 字节而不是 varint？
//     答：定长更好懂、解码没有分支。varint 能省空间（小数字只占 1 字节），
//     LevelDB 用 varint。留作练习，见 docs/LEARNING_GUIDE.md。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kv {

inline void EncodeFixed32(char* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

inline void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

// 注意要先转成 unsigned char：char 可能是有符号的，直接移位会把符号位扩展进来
inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, 8);
}

// 格式：[长度 fixed32][字节]
inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutFixed32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

// 下面的 Get* 从 *input 头部解析，成功则把 *input 向后移动；数据不够返回 false。
inline bool GetFixed32(std::string_view* input, uint32_t* v) {
  if (input->size() < 4) return false;
  *v = DecodeFixed32(input->data());
  input->remove_prefix(4);
  return true;
}

inline bool GetFixed64(std::string_view* input, uint64_t* v) {
  if (input->size() < 8) return false;
  *v = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}

inline bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  if (!GetFixed32(input, &len) || input->size() < len) return false;
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace kv
