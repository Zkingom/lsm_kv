#include "kv/sstable_format.h"

#include "kv/coding.h"
#include "kv/crc32.h"

namespace kv {
namespace {

[[noreturn]] void ThrowCorruption(const std::string& what) {
  throw IOError("sstable corruption: " + what);
}

bool GetBlockHandle(std::string_view* input, BlockHandle* handle) {
  return GetFixed64(input, &handle->offset) && GetFixed64(input, &handle->size);
}

void PutBlockHandle(std::string* dst, const BlockHandle& handle) {
  PutFixed64(dst, handle.offset);
  PutFixed64(dst, handle.size);
}

}  // namespace

std::string EncodeFooter(const Footer& footer) {
  std::string out;
  PutBlockHandle(&out, footer.index);
  PutBlockHandle(&out, footer.filter);
  PutFixed64(&out, kTableMagic);
  return out;
}

Footer DecodeFooter(std::string_view data) {
  Footer footer;
  uint64_t magic = 0;
  if (data.size() != kFooterSize || !GetBlockHandle(&data, &footer.index) ||
      !GetBlockHandle(&data, &footer.filter) || !GetFixed64(&data, &magic)) {
    ThrowCorruption("bad footer length");
  }
  if (magic != kTableMagic) ThrowCorruption("bad magic number");
  return footer;
}

void AppendDataEntry(std::string* block, std::string_view key, ValueType type,
                     std::string_view value) {
  // [性能] 没有做前缀压缩。LevelDB 会只存和前一个键不同的后缀，并每 16 条放一个
  // "重启点"以便块内二分查找。有序键通常前缀相同，压缩能省不少空间。留作练习。
  block->push_back(static_cast<char>(type));
  PutLengthPrefixed(block, key);
  PutLengthPrefixed(block, value);
}

std::vector<Record> DecodeDataBlock(std::string_view contents) {
  std::vector<Record> records;
  while (!contents.empty()) {
    uint8_t raw_type = static_cast<uint8_t>(contents[0]);
    contents.remove_prefix(1);
    std::string_view key, value;
    if (raw_type > static_cast<uint8_t>(ValueType::kValue) ||
        !GetLengthPrefixed(&contents, &key) || !GetLengthPrefixed(&contents, &value)) {
      ThrowCorruption("bad data block entry");
    }
    records.push_back(Record{std::string(key), static_cast<ValueType>(raw_type), std::string(value)});
  }
  return records;
}

void AppendIndexEntry(std::string* block, const IndexEntry& entry) {
  PutLengthPrefixed(block, entry.last_key);
  PutBlockHandle(block, entry.handle);
}

std::vector<IndexEntry> DecodeIndexBlock(std::string_view contents) {
  std::vector<IndexEntry> entries;
  while (!contents.empty()) {
    std::string_view key;
    IndexEntry entry;
    if (!GetLengthPrefixed(&contents, &key) || !GetBlockHandle(&contents, &entry.handle)) {
      ThrowCorruption("bad index block entry");
    }
    entry.last_key = std::string(key);
    entries.push_back(std::move(entry));
  }
  return entries;
}

BlockHandle WriteBlock(WritableFile* file, std::string_view contents) {
  BlockHandle handle{file->Size(), contents.size()};
  std::string trailer;
  PutFixed32(&trailer, Crc32(contents));
  file->Append(contents);
  file->Append(trailer);
  return handle;
}

std::string ReadBlock(const RandomAccessFile& file, const BlockHandle& handle) {
  if (handle.offset + handle.size + kBlockTrailerSize > file.Size()) {
    ThrowCorruption("block handle out of range");
  }
  std::string data = file.Read(handle.offset, handle.size + kBlockTrailerSize);
  uint32_t expected = DecodeFixed32(data.data() + handle.size);
  data.resize(handle.size);
  if (Crc32(data) != expected) ThrowCorruption("block checksum mismatch");
  return data;
}

}  // namespace kv
