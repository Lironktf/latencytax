#include "util/gzline.hpp"

#include <dirent.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace ltx {

GzLineReader::GzLineReader(const std::string& path, std::size_t chunk) : chunk_(chunk) {
  gzFile f = gzopen(path.c_str(), "rb");
  if (!f) {
    error_ = "cannot open " + path;
    return;
  }
  gzbuffer(f, 1u << 17);
  handle_ = f;
  buf_.resize(chunk_ * 2);
}

GzLineReader::~GzLineReader() {
  if (handle_) gzclose(static_cast<gzFile>(handle_));
}

bool GzLineReader::refill() {
  if (eof_) return false;
  // Move the partial line to the front, then read after it.
  if (begin_ > 0) {
    std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }
  if (end_ + chunk_ > buf_.size()) buf_.resize(end_ + chunk_);
  const int got = gzread(static_cast<gzFile>(handle_), buf_.data() + end_,
                         static_cast<unsigned>(chunk_));
  if (got <= 0) {
    if (got < 0) {
      int err = 0;
      const char* msg = gzerror(static_cast<gzFile>(handle_), &err);
      error_ = msg ? msg : "gzread failed";
    }
    eof_ = true;
    return false;
  }
  end_ += static_cast<std::size_t>(got);
  return true;
}

bool GzLineReader::next(std::string_view& line) {
  while (true) {
    const char* base = buf_.data();
    const void* nl = std::memchr(base + begin_, '\n', end_ - begin_);
    if (nl) {
      const std::size_t pos = static_cast<const char*>(nl) - base;
      line = std::string_view(base + begin_, pos - begin_);
      begin_ = pos + 1;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      return true;
    }
    if (!refill()) {
      if (end_ > begin_) {
        line = std::string_view(buf_.data() + begin_, end_ - begin_);
        begin_ = end_;
        return true;
      }
      return false;
    }
  }
}

std::vector<std::string> list_gz_files(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() >= 9 && n.compare(n.size() - 9, 9, ".jsonl.gz") == 0) {
      out.push_back(dir + "/" + n);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> list_dates(const std::string& root) {
  std::vector<std::string> out;
  DIR* d = opendir(root.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() == 15 && n.compare(0, 5, "date=") == 0) out.push_back(n.substr(5));
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace ltx
