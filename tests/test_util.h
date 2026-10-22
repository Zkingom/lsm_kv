// test_util.h —— 测试辅助：自动清理的临时目录
#pragma once

#include <stdlib.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace kv::test {

// 构造时在系统临时目录下创建唯一子目录，析构时整个删掉，保证仓库里不留数据文件。
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "kvtest-XXXXXX").string();
    // mkdtemp 原地把 XXXXXX 替换成随机字符并创建目录，保证并行跑测试也不冲突
    if (::mkdtemp(tmpl.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace kv::test
