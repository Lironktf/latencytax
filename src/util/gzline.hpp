// Buffered line reader over a gzip file.
//
// zlib's gzgets copies a line at a time through a small internal buffer. This
// reads 256 KB at a go and hands out string_views into that buffer, so parsing
// a day of snapshots does not spend its time in libc.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ltx {

class GzLineReader {
 public:
  explicit GzLineReader(const std::string& path, std::size_t chunk = 1u << 18);
  ~GzLineReader();
  GzLineReader(const GzLineReader&) = delete;
  GzLineReader& operator=(const GzLineReader&) = delete;

  bool ok() const noexcept { return handle_ != nullptr; }
  const std::string& error() const noexcept { return error_; }

  // Returns false at end of file. The view is valid until the next call.
  bool next(std::string_view& line);

 private:
  bool refill();

  void* handle_ = nullptr;
  std::string error_;
  std::vector<char> buf_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  std::size_t chunk_;
  bool eof_ = false;
};

// Lists *.jsonl.gz under dir, sorted. Empty if the directory is missing.
std::vector<std::string> list_gz_files(const std::string& dir);
// Lists date=YYYY-MM-DD subdirectories of root, sorted, returning the dates.
std::vector<std::string> list_dates(const std::string& root);

}  // namespace ltx
