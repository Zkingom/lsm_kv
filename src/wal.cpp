#include "kv/wal.h"

#include "kv/coding.h"
#include "kv/crc32.h"

namespace kv {
namespace {

constexpr size_t kHeaderSize = 8;  // crc32 + len

// 解析 payload。crc 已经校验通过，这里失败只可能是程序 bug 写出了错误格式。
bool DecodePayload(std::string_view payload, ValueType* type, std::string_view* key,
                   std::string_view* value) {
  if (payload.empty()) return false;
  uint8_t raw_type = static_cast<uint8_t>(payload[0]);
  if (raw_type != static_cast<uint8_t>(ValueType::kValue) &&
      raw_type != static_cast<uint8_t>(ValueType::kDeletion)) {
    return false;
  }
  *type = static_cast<ValueType>(raw_type);
  payload.remove_prefix(1);
  return GetLengthPrefixed(&payload, key) && GetLengthPrefixed(&payload, value) &&
         payload.empty();
}

}  // namespace

WalWriter::WalWriter(const std::string& path, bool sync_each_write)
    : file_(path), sync_each_write_(sync_each_write) {}

void WalWriter::AddRecord(ValueType type, std::string_view key, std::string_view value) {
  std::string payload;
  payload.reserve(1 + 8 + key.size() + value.size());
  payload.push_back(static_cast<char>(type));
  PutLengthPrefixed(&payload, key);
  PutLengthPrefixed(&payload, value);

  std::string record;
  record.reserve(kHeaderSize + payload.size());
  PutFixed32(&record, 0);  // crc 占位，算完再填
  PutFixed32(&record, static_cast<uint32_t>(payload.size()));
  record += payload;
  EncodeFixed32(record.data(), Crc32(std::string_view(record).substr(4)));

  // [持久化] 整条记录拼好后一次性写出，减少"只写了半条"的窗口；
  // 即使真的只写了半条，重放时 crc/长度校验也会发现并截断。
  file_.Append(record);
  // [持久化][性能] 每条都 fsync 最安全，但每次要等磁盘（毫秒级），吞吐会下降几十倍。
  if (sync_each_write_) file_.Sync();
}

WalReplayResult ReplayWal(const std::string& path, const WalApplyFn& apply) {
  // [性能] 一次性把整个日志读进内存。日志大小约等于 MemTable 阈值（默认 4MB），可以接受。
  std::string data = ReadFileToString(path);
  WalReplayResult result;
  result.file_bytes = data.size();

  std::string_view rest(data);
  while (rest.size() >= kHeaderSize) {
    uint32_t expected_crc = DecodeFixed32(rest.data());
    uint32_t len = DecodeFixed32(rest.data() + 4);
    if (rest.size() - kHeaderSize < len) break;  // 记录体不完整：写到一半崩溃

    std::string_view covered = rest.substr(4, 4 + len);  // len 字段 + payload
    if (Crc32(covered) != expected_crc) break;  // 内容损坏

    ValueType type;
    std::string_view key, value;
    if (!DecodePayload(rest.substr(kHeaderSize, len), &type, &key, &value)) break;

    apply(type, key, value);
    rest.remove_prefix(kHeaderSize + len);
    result.valid_bytes += kHeaderSize + len;
    ++result.records;
  }
  return result;
}

}  // namespace kv
