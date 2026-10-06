// native/src/AtomicFile.h — write a file so that pulling the power at ANY instant leaves the old content or the new, never half.
//
// The layout, the pairings, the API key and the staged node images all lived in plain `ofstream(path, trunc)` writes: the file is
// emptied the moment it is opened and filled afterwards, so a power cut (a Pi on a shop circuit) in that window leaves a truncated
// or empty file, and the brain then boots with no layout. The shape that is safe on POSIX is: write a temp file in the SAME
// directory, fsync it, rename it over the target (atomic), then fsync the directory so the rename itself survives.
//
// KEEP A PREVIOUS COPY when asked (`keep`): the target is hard-linked to `<path>.bak` just before the rename, so there is always
// one older whole file next to the new one, and `readWithBackup()` falls back to it if the current one is unreadable.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace dgbrain {

inline bool writeFileAtomic(const std::string& path, const std::string& data, bool keep = false) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += (size_t)n;
    }
    // Data on the disk BEFORE the rename makes it the file; otherwise a cut can leave the new name on empty blocks.
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
    ::close(fd);
    if (keep) {
        const std::string bak = path + ".bak";
        ::unlink(bak.c_str());
        if (::link(path.c_str(), bak.c_str()) != 0) { /* no previous file (the first save): nothing to keep */ }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const int dfd = ::open(dir.empty() ? "/" : dir.c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

}  // namespace dgbrain
