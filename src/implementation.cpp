/**
 * asymmetricfs - An asymmetric encryption-aware filesystem
 * (c) 2013 Chris Kennelly <chris@ckennelly.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Workaround per bug in libstdc++ 4.5:
 * http://llvm.org/bugs/show_bug.cgi?id=13364
 */
namespace std { class type_info; }

#include <sys/types.h>
#include <attr/xattr.h>
#include <boost/thread/locks.hpp>
#include <cassert>
#include <cstdlib>
#include <dirent.h>
#include "implementation.h"
#include <stdexcept>
#include "subprocess.h"
#include <sys/mman.h>
#include <sys/stat.h>

typedef boost::unique_lock<asymmetricfs::mutex_t> scoped_lock;
typedef std::vector<gpg_recipient> RecipientList;

/**
 * System utilities such as truncate open the file descriptor for writing only.
 * This makes it difficult when we must decrypt the file, truncate, and then
 * reencrypt.
 */
int asymmetricfs::make_rdwr(int flags) const {
    if (!(read_)) {
        return flags;
    } else if (flags & O_RDONLY) {
        return flags;
    } else {
        flags &= ~O_RDONLY;
        flags &= ~O_WRONLY;
        flags |= O_RDWR;
        return flags;
    }
}

class asymmetricfs::internal {
public:
    internal(const RecipientList & recipients);
    ~internal();

    int fd;
    int flags;
    unsigned references;
    std::string path;

    bool buffer_set;
    bool dirty;
    std::string buffer;

    /**
     * Returns 0 on success, otherwise the corresponding standard error code.
     * This should not be called by multiple threads on a single instance.
     */
    int load_buffer();

    int close();
protected:
    internal(const internal &) = delete;
    const internal & operator=(const internal &) = delete;

    bool open_;
    const RecipientList & recipients_;
};

asymmetricfs::internal::internal(const RecipientList & recipients) :
        references(0), dirty(false), open_(true), recipients_(recipients) { }

asymmetricfs::internal::~internal() {
    (void) close();
    assert(references == 0);
}

int asymmetricfs::internal::close() {
    if (!(open_)) {
        return 0;
    }

    int ret = 0;
    if (dirty) {
        std::vector<std::string> argv{"gpg", "-ae", "--no-tty", "--batch"};
        for (const auto& recipient : recipients_) {
            argv.push_back("-r");
            argv.push_back(static_cast<std::string>(recipient));
        }

        /* Start gpg. */
        subprocess s(-1, fd, "gpg", argv);

        size_t str_size = buffer.size();
        s.communicate(NULL, NULL, buffer.data(), &str_size);

        int wait_ret = s.wait();
        if (wait_ret != 0) {
            ret = -EIO;
        }

        dirty = false;
    }

    open_ = false;
    int close_ret = ::close(fd);
    if (ret != 0) {
        return ret;
    } else if (close_ret == 0) {
        return 0;
    } else {
        return errno;
    }
}

int asymmetricfs::internal::load_buffer() {
    if (buffer_set) {
        return 0;
    }

    assert(open_);

    /* Clear the current buffer. */
    dirty = false;
    buffer.clear();

    /* gpg does not react well to seeing multiple encrypted blocks in the same
     * session, so the data needs to be chunked across multiple calls. */
    const std::vector<std::string> argv{"gpg", "-d", "--no-tty", "--batch"};

    struct stat fd_stat;
    int ret = fstat(fd, &fd_stat);
    if (ret != 0) {
        return errno;
    } else if (fd_stat.st_size <= 0) {
        buffer_set = true;
        return 0;
    }

    const size_t fd_size = static_cast<size_t>(fd_stat.st_size);

    const uint8_t * underlying = static_cast<const uint8_t *>(
        mmap(NULL, fd_size, PROT_READ, MAP_SHARED, fd, 0));
    if (underlying == MAP_FAILED) {
        return errno;
    }

    static const char terminator[]    = "-----END PGP MESSAGE-----\n";
    static size_t     terminator_size = sizeof(terminator) - 1;

    buffer_set = true;
    ret = 0;
    for (size_t offset = 0; offset < fd_size; ) {
        /*
         * Find terminator of gpg block.  This can be optimized, but
         * terminator_size is small.
         */
        size_t new_offset;
        for (new_offset = offset; new_offset <= fd_size - terminator_size;
                new_offset++) {
            if (memcmp(terminator, underlying + new_offset,
                    terminator_size) == 0) {
                new_offset += terminator_size;
                break;
            }
        }
        assert(offset <= new_offset);
        assert(new_offset <= fd_size);

        const uint8_t *write_buffer;
        size_t write_size;
        int gpg_stdin;
        if (offset == 0 && new_offset == fd_size) {
            /* Special case:  Single block. */
            gpg_stdin = fd;
            write_buffer = NULL;
            write_size   = 0;
        } else {
            gpg_stdin = -1;
            write_buffer = underlying + offset;
            write_size   = new_offset - offset;

            if (write_size == 0) {
                break;
            }
        }

        /* Start gpg. */
        subprocess s(gpg_stdin, -1, "gpg", argv);

        /* Communicate with gpg. */
        const size_t chunk_size = 1 << 20;
        while (true) {
            size_t buffer_size  = buffer.size();
            size_t this_chunk   = chunk_size;
            buffer.resize(buffer_size + this_chunk);

            size_t write_remaining = write_size;
            int cret = s.communicate(&buffer[buffer_size], &this_chunk,
                write_buffer, &write_remaining);
            if (cret != 0) {
                ret = -cret;
                break;
            }

            buffer.resize(buffer_size + chunk_size - this_chunk);
            if (chunk_size == this_chunk) {
                break;
            }

            if (write_buffer) {
                write_buffer += write_size - write_remaining;
                write_size   = write_remaining;
            }
        }

        int wait = s.wait();
        if (wait != 0) {
            buffer_set = false;
            ret = EIO;
            break;
        }

        offset = new_offset;
    }

    munmap(const_cast<uint8_t *>(underlying),
        static_cast<size_t>(fd_stat.st_size));

    return ret;
}

asymmetricfs::asymmetricfs() : read_(false), root_set_(false), next_(0) { }

asymmetricfs::~asymmetricfs() {
    if (root_set_) {
        ::close(root_);
    }

    for (open_fd_map_t::iterator it = open_fds_.begin(); it != open_fds_.end();
            ++it) {
        delete it->second;
    }
}

asymmetricfs::fd_t asymmetricfs::next_fd() {
    return next_++;
}

int asymmetricfs::chmod(const char *path_, mode_t mode) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::chmod(relpath.c_str(), mode);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::chown(const char *path_, uid_t u, gid_t g) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::chown(relpath.c_str(), u, g);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::create(const char *path_, mode_t mode,
        struct fuse_file_info *info) {
    const std::string path(path_);
    const std::string relpath("." + path);

    info->flags |= O_CREAT;

    assert(info);
    int ret;
    do {
        ret = ::openat(root_, relpath.c_str(), make_rdwr(info->flags), mode);
        if (ret >= 0) {
            break;
        }

        if (read_ && (info->flags & O_WRONLY) && errno == EACCES) {
            ret = ::openat(root_, relpath.c_str(), info->flags, mode);
            if (ret >= 0) {
                break;
            }
        }

        return -errno;
    } while (0);

    /* Update list of open files. */
    scoped_lock l(mx_);
    const fd_t fd = next_fd();
    open_paths_.insert(open_map_t::value_type(path, fd));

    internal * data = new internal(recipients_);
    data->fd            = ret;
    data->flags         = info->flags;
    data->path          = path;
    data->references    = 1;
    data->buffer_set    = true;
    open_fds_  .insert(open_fd_map_t::value_type(fd, data));

    info->fh = fd;

    return 0;
}

int asymmetricfs::ftruncate(const char *path, off_t offset,
        struct fuse_file_info *info) {
    (void) path;
    assert(info);

    scoped_lock l(mx_);
    return truncatefd(info->fh, offset);
}

int asymmetricfs::truncatefd(fd_t fd, off_t offset) {
    auto it = open_fds_.find(fd);
    if (it == open_fds_.end()) {
        return -EBADF;
    }

    if (offset < 0) {
        return -EINVAL;
    } else if (offset == 0) {
        int ret = ::ftruncate(it->second->fd, 0);
        if (ret != 0) {
            return -errno;
        } else {
            it->second->buffer.resize(0);
            it->second->dirty = true;
            return 0;
        }
    } else if (read_) {
        /* Decrypt, truncate, (lazily) reencrypt. */
        int ret = it->second->load_buffer();
        if (ret != 0) {
            return -ret;
        } else {
            it->second->buffer.resize(static_cast<size_t>(offset));
            it->second->dirty = true;
            return 0;
        }
    } else {
        return -EACCES;
    }
}

void* asymmetricfs::init(struct fuse_conn_info *conn) {
    (void) conn;

    assert(root_set_);
    int ret = fchdir(root_);
    if (ret != 0) {
        throw std::runtime_error("Unable to chdir.");
    }

    return NULL;
}

bool asymmetricfs::ready() const {
    return root_set_ && !(recipients_.empty());
}

void asymmetricfs::set_read(bool r) {
    read_ = r;
}

bool asymmetricfs::set_target(const std::string & target) {
    if (target.empty()) {
        return false;
    }

    if (root_set_) {
        ::close(root_);
        root_set_ = false;
    }

    root_ = ::open(target.c_str(), O_DIRECTORY);
    return (root_set_ = (root_ >= 0));
}

void asymmetricfs::set_recipients(
        const std::vector<gpg_recipient> & recipients) {
    /*
     * We guarantee the lifetime of the recipient list to
     * asymmetricfs::internal, so reject changes if there are outstanding
     * files.
     */
    if (!(open_fds_.empty())) {
        throw std::runtime_error("Changing recipient list with open files.");
    }

    recipients_ = recipients;
}

int asymmetricfs::fgetattr(const char *path, struct stat *buf,
        struct fuse_file_info *info) {
    (void) path;

    scoped_lock l(mx_);
    return statfd(info->fh, buf);
}

int asymmetricfs::statfd(fd_t fd, struct stat *buf) {
    if (!(buf)) {
        return -EFAULT;
    }

    auto it = open_fds_.find(fd);
    if (it == open_fds_.end()) {
        return -EBADF;
    }

    struct stat s;
    const int ret = ::fstat(it->second->fd, &s);
    if (ret != 0) {
        return -errno;
    }

    if (read_) {
        int lret = it->second->load_buffer();
        if (lret != 0) {
            return -lret;
        }
    }

    assert(!(read_) || it->second->buffer_set);
    const size_t size = it->second->buffer.size();
    if (it->second->buffer_set) {
        s.st_size = static_cast<off_t>(size);
    } else if (it->second->flags & O_APPEND) {
        s.st_size += size;
    } /* else: leave st_size as-is. */

    *buf = s;
    return 0;
}

int asymmetricfs::getattr(const char *path_, struct stat *buf) {
    const std::string path(path_);

    /**
     * If !read_, clear the appropriate bits unless the file is open.
     */
    scoped_lock l(mx_);
    auto it = open_paths_.find(path);
    const bool is_open = it != open_paths_.end();

    if (is_open) {
        return statfd(it->second, buf);
    } else {
        if (!(buf)) {
            return -EFAULT;
        }

        const std::string relpath("." + path);

        struct stat s;
        const int ret = ::lstat(relpath.c_str(), &s);
        if (ret != 0) {
            return -errno;
        }

        if (!(read_) && !(S_ISDIR(s.st_mode))) {
            s.st_mode = s.st_mode &
                static_cast<mode_t>(~(S_IRUSR | S_IRGRP | S_IROTH));
        }

        *buf = s;
        return 0;
    }
}

int asymmetricfs::link(const char *oldpath, const char *newpath) {
    (void) oldpath;
    (void) newpath;

    /* asymmetricfs does not support hard links. */
    return -EPERM;
}

int asymmetricfs::listxattr(const char *path_, char *buffer, size_t size) {
    const std::string path(path_);
    const std::string relpath("." + path);

    ssize_t ret = ::listxattr(relpath.c_str(), buffer, size);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::mkdir(const char *path_, mode_t mode) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::mkdir(relpath.c_str(), mode);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::open(const char *path_, struct fuse_file_info *info) {
    const std::string path(path_);
    const std::string relpath("." + path);
    assert(info);
    int flags = info->flags;

    /* Determine if the file is already open. */
    scoped_lock l(mx_);

    open_map_t::const_iterator it = open_paths_.find(path);
    if (it != open_paths_.end()) {
        info->fh = it->second;

        auto jit = open_fds_.find(it->second);
        assert(jit != open_fds_.end());
        jit->second->references++;
        return 0;
    }

    const int  access_mode = flags & O_ACCMODE;
    const bool for_reading = (access_mode == O_RDWR) ||
                             (access_mode == O_RDONLY);
    if (!(read_) && for_reading) {
        if (flags & O_CREAT) {
            /* Require that the file be created (i.e., it does not already
             * exist. */
            flags |= O_EXCL;
        }
    }

    int ret;
    do {
        ret = ::openat(root_, relpath.c_str(), make_rdwr(flags));
        if (ret >= 0) {
            break;
        }

        if (read_ && !(for_reading) && errno == EACCES) {
            ret = ::openat(root_, relpath.c_str(), flags);
            if (ret >= 0) {
                break;
            }
        }

        return -errno;
    } while (0);

    /* Update list of open files. */
    const fd_t fd = next_fd();
    open_paths_.insert(open_map_t::value_type(path, fd));

    internal * data = new internal(recipients_);
    data->fd            = ret;
    data->flags         = flags;
    data->path          = path;
    data->references    = 1;

    /**
     * If we just created the file, it will be empty.  If so, treat the empty
     * buffer as initialized.  Otherwise, defer decryption until we read the
     * file.
     *
     * This is necessary so we can truncate empty files to non-zero size even
     * in write-only mode.
     */
    struct stat buf;
    int fstat_ret = fstat(ret, &buf);
    if (fstat_ret == 0) {
        data->buffer_set = buf.st_size == 0;
    } else {
        /* An error occured, but treat it as nonfatal. */
        data->buffer_set = false;
    }

    open_fds_  .insert(open_fd_map_t::value_type(fd, data));

    info->fh = fd;

    return 0;
}

int asymmetricfs::opendir(const char *path_, struct fuse_file_info *info) {
    const std::string path(path_);
    const std::string relpath("." + path);

    DIR *dir = ::opendir(relpath.c_str());
    if (!(dir)) {
        return -errno;
    }

    info->fh = reinterpret_cast<uint64_t>(dir);
    return 0;
}

int asymmetricfs::read(const char *path, void *buffer, size_t size,
        off_t offset_, struct fuse_file_info *info) {
    (void) path;

    scoped_lock l(mx_);
    open_fd_map_t::const_iterator it = open_fds_.find(info->fh);
    if (it == open_fds_.end()) {
        return -EBADF;
    }

    if (offset_ < 0) {
        return 0;
    }
    const size_t offset = static_cast<size_t>(offset_);

    if (!(read_)) {
        if (!(it->second->buffer_set)) {
            if (it->second->flags & O_APPEND) {
                return -EACCES;
            } else if (!(it->second->flags & O_CREAT)) {
                /*
                 * O_CREAT implies O_EXCL, so if it was not set, the file
                 * already existed and cannot be read.
                 */
                return -EACCES;
            }
        }

        const std::string & str = it->second->buffer;
        const size_t str_size = str.size();
        if (str_size < offset) {
            /* No bytes available for reading. */
            return 0;
        }

        const size_t remaining = std::min(std::min(str_size - offset, size),
            static_cast<size_t>(std::numeric_limits<int>::max()));
        memcpy(buffer, str.data() + offset, remaining);
        return static_cast<int>(remaining);
    }

    /* Read the buffer, as needed. */
    int ret = it->second->load_buffer();
    if (ret != 0) {
        return -ret;
    }

    assert(it->second->buffer_set);

    /* Grab a reference to the buffer. */
    const std::string & str = it->second->buffer;
    const size_t str_size = str.size();
    if (str_size <= offset) {
        /* Nothing left to read. */
        return 0;
    }

    const size_t remaining = std::min(str_size - offset, size);
    memcpy(buffer, str.data(), remaining);
    assert(remaining < INT_MAX);
    return (int) remaining;
}

int asymmetricfs::readdir(const char *path, void *buffer,
        fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *info) {
    (void) path;
    (void) offset;

    DIR *dir = reinterpret_cast<DIR *>(info->fh);

    /**
     * readdir is used preferentially over readdir_r here as the API for
     * readdir_r exposes us to the potential problem of failing to allocate
     * enough buffer space for the entry name.
     */
    struct dirent *result;

    /**
     * From the readdir man-page (release 3.44):
     *
     * "On success, readdir() returns a pointer to a dirent structure.  (This
     *  structure may be statically allocated; do not attempt to free(3) it.)
     *  If the end of the directory stream is reached, NULL is returned and
     *  errno is not changed.  If an error occurs, NULL is returned and errno
     *  is set appropriately."
     *
     * errno may be nonzero upon entry into the loop, so it must be cleared as
     * to detect any errors that arise.
     */
    errno = 0;

    while ((result = ::readdir(dir)) != NULL) {
        struct stat s;
        memset(&s, 0, sizeof(s));
        s.st_ino = result->d_ino;

        bool skip = false;
        switch (result->d_type) {
            case DT_LNK:
            case DT_REG:
            case DT_DIR:
            case DT_UNKNOWN:
                s.st_mode = DTTOIF((unsigned) result->d_type);
                break;
            case DT_BLK:
            case DT_CHR:
            case DT_FIFO:
            case DT_SOCK:
            default:
                skip = true;
                break;
        }

        if (skip) {
            continue;
        }

        int ret = filler(buffer, result->d_name, &s, 0);
        if (ret) {
            return 0;
        }
    }

    return -errno;
}

int asymmetricfs::readlink(const char *path_, char *buffer, size_t size) {
    const std::string path(path_);
    const std::string relpath("." + path);

    size_t len = size > 0 ? size - 1 : 0;

    ssize_t ret = ::readlink(relpath.c_str(), buffer, len);
    if (ret == -1) {
        return -errno;
    } else {
        assert(ret >= 0);
        assert(ret <= INT_MAX);
        buffer[ret] = '\0';
        return 0;
    }
}

int asymmetricfs::release(const char *path, struct fuse_file_info *info) {
    (void) path;

    scoped_lock l(mx_);

    auto it = open_fds_.find(info->fh);
    if (it == open_fds_.end()) {
        return 0 /* ignored */;
    }

    const unsigned new_count = --it->second->references;
    if (new_count == 0) {
        /* Close the file. */
        open_paths_.erase(it->second->path);

        delete it->second;
        open_fds_.erase(it);
    }

    return 0 /* ignored */;
}

int asymmetricfs::releasedir(const char *path, struct fuse_file_info *info) {
    (void) path;

    DIR *dir = reinterpret_cast<DIR *>(info->fh);
    int ret = ::closedir(dir);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::removexattr(const char *path_, const char *name) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::removexattr(relpath.c_str(), name);
    if (ret != 0) {
        return -errno;
    }

    return ret;
}

int asymmetricfs::rename(const char *oldpath_, const char *newpath_) {
    const std::string oldpath(oldpath_);
    const std::string newpath(newpath_);

    const std::string reloldpath("." + oldpath);
    const std::string relnewpath("." + newpath);

    /*
     * Avoid races to rename as our metadata for open files will be manipulated
     * if and only if the underlying rename is successful.
     */
    scoped_lock l(mx_);

    int ret = ::rename(reloldpath.c_str(), relnewpath.c_str());
    if (ret != 0) {
        return -errno;
    }

    open_map_t::iterator it = open_paths_.find(oldpath);
    if (it != open_paths_.end()) {
        /* Rename existing, open files. */
        const fd_t fd = it->second;

        open_paths_.insert(open_map_t::value_type(newpath, fd));
        open_paths_.erase(it);

        open_fd_map_t::iterator jit = open_fds_.find(fd);
        assert(jit != open_fds_.end());
        if (jit != open_fds_.end()) {
            jit->second->path = newpath;
        }
    }

    return 0;
}

int asymmetricfs::rmdir(const char *path_) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::rmdir(relpath.c_str());
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::setxattr(const char *path_, const char *name,
        const void *value, size_t size, int flags) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::setxattr(relpath.c_str(), name, value, size, flags);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::statfs(const char *path, struct statvfs *buf) {
    (void) path;

    int ret = ::fstatvfs(root_, buf);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::symlink(const char *oldpath, const char *newpath_) {
    const std::string newpath(newpath_);
    const std::string relpath("." + newpath);

    int ret = ::symlink(oldpath, relpath.c_str());
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::truncate(const char *path_, off_t offset) {
    const std::string path(path_);
    const std::string relpath("." + path);

    if (offset < 0) {
        return -EINVAL;
    }

    /* Determine if the file is already open. */
    scoped_lock l(mx_);

    open_map_t::const_iterator it = open_paths_.find(path);
    const bool is_open = it != open_paths_.end();
    if (is_open) {
        return truncatefd(it->second, offset);
    } else if (offset == 0) {
        int ret = ::truncate(relpath.c_str(), offset);
        if (ret == 0) {
            return 0;
        } else {
            return -errno;
        }
    } else if (read_) {
        /* Decrypt, truncate, encrypt. */
        const int flags = O_RDWR;
        int ret = ::open(relpath.c_str(), flags);
        if (ret < 0) {
            return -errno;
        }

        internal data(recipients_);
        data.fd         = ret;
        data.flags      = flags;
        data.path       = path;
        /* data is transient and does not escape our scope. */
        data.references = 0;

        int load_ret = data.load_buffer();
        if (load_ret != 0) {
            return -load_ret;
        }

        assert(data.buffer_set);
        data.buffer.resize(static_cast<size_t>(offset));
        data.dirty = true;

        ret = data.close();
        if (ret == 0) {
            return 0;
        } else {
            return -ret;
        }
    } else {
        return -EACCES;
    }
}

int asymmetricfs::write(const char *path_, const char * buffer, size_t size,
        off_t offset_, struct fuse_file_info *info) {
    (void) path_;

    scoped_lock l(mx_);

    assert(info);
    auto it = open_fds_.find(info->fh);
    if (it == open_fds_.end()) {
        return -EBADF;
    }

    if (size == 0) {
        return 0;
    }

    if (offset_ < 0) {
        return -EINVAL;
    }

    const size_t offset = static_cast<size_t>(offset_);
    internal & data = *it->second;

    const size_t new_size = std::max(data.buffer.size(), offset + size);
    data.buffer.resize(new_size);
    memcpy(&data.buffer[offset], buffer, size);
    data.dirty = true;

    return static_cast<int>(size);
}

int asymmetricfs::unlink(const char *path_) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = ::unlink(relpath.c_str());
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::utimens(const char *path_, const struct timespec tv[2]) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = utimensat(root_, relpath.c_str(), tv, 0);
    if (ret != 0) {
        return -errno;
    }

    return 0;
}

int asymmetricfs::access(const char *path_, int mode) {
    const std::string path(path_);
    const std::string relpath("." + path);

    int ret = 0;
    if ((mode & R_OK) && !(read_)) {
        ret = -EACCES;
    }

    int aret = ::access(relpath.c_str(), mode);
    if (aret == 0) {
        return ret;
    } else {
        return -errno;
    }
}
