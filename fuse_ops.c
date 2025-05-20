
/*
 * s3backer - FUSE-based single file backing store via Amazon S3
 *
 * Copyright 2008-2023 Archie L. Cobbs <archie.cobbs@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations including
 * the two.
 *
 * You must obey the GNU General Public License in all respects for all
 * of the code used other than OpenSSL. If you modify file(s) with this
 * exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do
 * so, delete this exception statement from your version. If you delete
 * this exception statement from all source files in the program, then
 * also delete it here.
 */

#include "s3backer.h"
#include "block_cache.h"
#include "block_part.h"
#include "ec_protect.h"
#include "zero_cache.h"
#include "fuse_ops.h"
#include "http_io.h"
#include "test_io.h"
#include "s3b_config.h"
#include "util.h"

/****************************************************************************
 *                              DEFINITIONS                                 *
 ****************************************************************************/

#define ROOT_INODE      1
#define FILE_INODE      2
#define STATS_INODE     3

// Represents an open 'stats' file
struct stat_file {
    char    *buf;           // note: not necessarily nul-terminated
    size_t  len;            // length of string in 'buf'
    size_t  bufsiz;         // size allocated for 'buf'
    int     memerr;         // we got a memory error
};

/****************************************************************************
 *                          FUNCTION DECLARATIONS                           *
 ****************************************************************************/

// FUSE functions
static void *fuse_op_init(struct fuse_conn_info *conn, struct fuse_config *cfg);
static void fuse_op_destroy(void *data);
static int fuse_op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi);
static int fuse_op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
static int fuse_op_open(const char *path, struct fuse_file_info *fi);
static int fuse_op_release(const char *path, struct fuse_file_info *fi);
static int fuse_op_read(const char *path, char *buf, size_t size, off_t offset,
    struct fuse_file_info *fi);
static int fuse_op_write(const char *path, const char *buf, size_t size, off_t offset,
    struct fuse_file_info *fi);
static int fuse_op_statfs(const char *path, struct statvfs *st);
static int fuse_op_truncate(const char *path, off_t size, struct fuse_file_info *fi);
static int fuse_op_flush(const char *path, struct fuse_file_info *fi);
static int fuse_op_fsync(const char *path, int isdatasync, struct fuse_file_info *fi);
static int fuse_op_unlink(const char *path);
static int fuse_op_fallocate(const char *path, int mode, off_t offset, off_t len, struct fuse_file_info *fi);

// Attribute functions
static void fuse_op_getattr_file(struct fuse_ops_private *priv, struct stat *st);
static void fuse_op_getattr_stats(struct fuse_ops_private *priv, struct stat_file *sfile, struct stat *st);

// Stats functions
static struct stat_file *fuse_op_stats_create(struct fuse_ops_private *priv);
static void fuse_op_stats_destroy(struct stat_file *sfile);
static printer_t fuse_op_stats_printer;
static printer_t stats_mirror_printer;
static void *stats_mirror_thread(void *arg);

/****************************************************************************
 *                          VARIABLE DEFINITIONS                            *
 ****************************************************************************/

// FUSE operations
const struct fuse_operations s3backer_fuse_ops = {
    .init       = fuse_op_init,
    .destroy    = fuse_op_destroy,
    .getattr    = fuse_op_getattr,
    .readdir    = fuse_op_readdir,
    .open       = fuse_op_open,
    .read       = fuse_op_read,
    .write      = fuse_op_write,
    .statfs     = fuse_op_statfs,
    .truncate   = fuse_op_truncate,
    .flush      = fuse_op_flush,
    .fsync      = fuse_op_fsync,
    .release    = fuse_op_release,
    .unlink     = fuse_op_unlink,
    .fallocate  = fuse_op_fallocate,
};

// Configuration and underlying s3backer_store
static struct fuse_ops_conf *config;
static struct fuse_ops_private *the_priv;

/****************************************************************************
 *                      PUBLIC FUNCTION DEFINITIONS                         *
 ****************************************************************************/

const struct fuse_operations *
fuse_ops_create(struct fuse_ops_conf *config0, struct s3backer_store *s3b)
{
    // Sanity check
    assert(config0 != NULL);
    assert(s3b != NULL);

    // Prevent duplicate invocation
    if (config != NULL || the_priv != NULL) {
        (*config0->log)(LOG_ERR, "fuse_ops_create(): duplicate invocation");
        return NULL;
    }
    config = config0;

    // Create private structure
    if ((the_priv = calloc(1, sizeof(*the_priv))) == NULL) {
        (*config->log)(LOG_ERR, "fuse_ops_create(): %s", strerror(errno));
        return NULL;
    }
    the_priv->s3b = s3b;
    if ((the_priv->block_part = block_part_create(config->block_size, config->num_blocks)) == NULL) {
        (*config->log)(LOG_ERR, "fuse_ops_create(): %s", strerror(errno));
        free(the_priv);
        return NULL;
    }

    // Now we're ready
    return &s3backer_fuse_ops;
}

void
fuse_ops_destroy(void)
{
    if (the_priv == NULL)
        return;
    block_part_destroy(&the_priv->block_part);
    fuse_op_destroy(the_priv);
    free(the_priv);
    the_priv = NULL;
}

/****************************************************************************
 *                    FUSE OPERATION FUNCTIONS                              *
 ****************************************************************************/

static void *
fuse_op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    struct s3b_config *const s3bconf = config->s3bconf;
    struct fuse_ops_private *const priv = the_priv;
    int r;

    // Sanity check
    assert(priv != NULL);
    assert(priv->s3b != NULL);

    // Initialize
    priv->block_bits = ffs(config->block_size) - 1;
    priv->start_time = time(NULL);
    priv->file_atime = priv->start_time;
    priv->file_mtime = priv->start_time;
    priv->stats_atime = priv->start_time;
    priv->file_size = (off_t)config->num_blocks * config->block_size;

    // Apply process tweaks
    apply_process_tweaks();

    // Start up stats mirror thread
    priv->stats_mirror_state = STATS_MIRROR_INITIAL;
    if (config->stats_mirror_path != NULL
      && (r = pthread_create(&priv->stats_mirror_thread, NULL, stats_mirror_thread, priv)) != 0) {
        (*config->log)(LOG_ERR, "failed to create stats mirror thread: %s", strerror(r));
        return NULL;
    }

    // Startup other background threads
    if ((r = (*priv->s3b->create_threads)(priv->s3b)) != 0) {
        (*config->log)(LOG_ERR, "fuse_op_init(): can't create threads: %s", strerror(errno));
        if (config->stats_mirror_path != NULL) {
            (void)pthread_cancel(priv->stats_mirror_thread);
            (void)pthread_join(priv->stats_mirror_thread, NULL);
        }
        return NULL;
    }

    // Allow stats mirror thread to start writing
    if (config->stats_mirror_path != NULL)
        priv->stats_mirror_state = STATS_MIRROR_RUNNING;

    // Done
    (*config->log)(LOG_INFO, "mounting %s", s3bconf->mount);
    return priv;
}

static void
fuse_op_destroy(void *data)
{
    struct fuse_ops_private *const priv = data;
    struct s3backer_store *const s3b = priv != NULL ? priv->s3b : NULL;
    struct s3b_config *const s3bconf = config->s3bconf;
    int r;

    // Sanity check
    if (priv == NULL || s3b == NULL)
        return;
    (*config->log)(LOG_INFO, "unmount %s: initiated", s3bconf->mount);

    // Shutdown stats mirror thread, if any
    if (priv->stats_mirror_state == STATS_MIRROR_RUNNING) {
        priv->stats_mirror_state = STATS_MIRROR_SHUTDOWN;
        (void)pthread_cancel(priv->stats_mirror_thread);
        (void)pthread_join(priv->stats_mirror_thread, NULL);
        priv->stats_mirror_state = STATS_MIRROR_INITIAL;
    }

    // Shutdown S3 statck (flush dirty data)
    (*config->log)(LOG_INFO, "unmount %s: shutting down filesystem", s3bconf->mount);
    if ((r = (*s3b->shutdown)(s3b)) != 0)
        (*config->log)(LOG_ERR, "unmount %s: filesystem shutdown failed: %s", s3bconf->mount, strerror(r));

    // Clear mount token (non-shared disk mode only)
    if (!config->read_only && !s3bconf->shared_disk_mode) {
        (*config->log)(LOG_INFO, "unmount %s: clearing mount token", s3bconf->mount);
        if ((r = (*s3b->set_mount_token)(s3b, NULL, 0)) != 0)
            (*config->log)(LOG_ERR, "unmount %s: clearing mount token failed: %s", s3bconf->mount, strerror(r));
    }

    // Destroy
    (*s3b->destroy)(s3b);
    priv->s3b = NULL;
    (*config->log)(LOG_INFO, "unmount %s: completed", s3bconf->mount);
}

static int
fuse_op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;

    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_ino = ROOT_INODE;
        st->st_uid = config->uid;
        st->st_gid = config->gid;
        if (priv != NULL) {
            st->st_atime = priv->start_time;
            st->st_mtime = priv->start_time;
            st->st_ctime = priv->start_time;
        }
        return 0;
    }
    if (priv == NULL)
        return -ENOENT;
    if (*path == '/' && strcmp(path + 1, config->filename) == 0) {
        fuse_op_getattr_file(priv, st);
        return 0;
    }
    if (*path == '/' && config->print_stats != NULL && strcmp(path + 1, config->stats_filename) == 0) {
        struct stat_file *sfile;

        if ((sfile = fuse_op_stats_create(priv)) == NULL)
            return -ENOMEM;
        fuse_op_getattr_stats(priv, sfile, st);
        fuse_op_stats_destroy(sfile);
        return 0;
    }
    return -ENOENT;
}

static void
fuse_op_getattr_file(struct fuse_ops_private *priv, struct stat *st)
{
    st->st_mode = S_IFREG | config->file_mode;
    st->st_nlink = 1;
    st->st_ino = FILE_INODE;
    st->st_uid = config->uid;
    st->st_gid = config->gid;
    st->st_size = priv->file_size;
    st->st_blksize = config->block_size;
    st->st_blocks = (off_t)config->num_blocks;
    st->st_atime = priv->file_atime;
    st->st_mtime = priv->file_mtime;
    st->st_ctime = priv->start_time;
}

static void
fuse_op_getattr_stats(struct fuse_ops_private *priv, struct stat_file *sfile, struct stat *st)
{
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
    st->st_ino = STATS_INODE;
    st->st_uid = config->uid;
    st->st_gid = config->gid;
    st->st_size = sfile->len;
    st->st_blksize = config->block_size;
    st->st_blocks = 0;
    st->st_atime = priv->stats_atime;
    st->st_mtime = time(NULL);
    st->st_ctime = priv->start_time;
}

static int
fuse_op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;

    (void)offset;
    (void)fi;
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    if (filler(buf, ".", NULL, 0, 0) != 0)
        return -ENOMEM;
    if (filler(buf, "..", NULL, 0, 0) != 0)
        return -ENOMEM;
    if (priv != NULL) {
        if (filler(buf, config->filename, NULL, 0, 0) != 0)
            return -ENOMEM;
        if (config->print_stats != NULL && config->stats_filename != NULL) {
            if (filler(buf, config->stats_filename, NULL, 0, 0) != 0)
                return -ENOMEM;
        }
    }
    return 0;
}

static int
fuse_op_open(const char *path, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;

    // Sanity check
    if (priv == NULL)
        return -ENOENT;

    // Backed file
    if (*path == '/' && strcmp(path + 1, config->filename) == 0) {
        fi->fh = 0;
        priv->file_atime = time(NULL);
        if (config->direct_io)
            fi->direct_io = 1;
        return 0;
    }

    // Stats file
    if (*path == '/' && config->print_stats != NULL && strcmp(path + 1, config->stats_filename) == 0) {
        struct stat_file *sfile;

        if ((sfile = fuse_op_stats_create(priv)) == NULL)
            return -ENOMEM;
        fi->fh = (uint64_t)(uintptr_t)sfile;
        priv->stats_atime = time(NULL);
        fi->direct_io = 1;
        return 0;
    }

    // Unknown file
    return -ENOENT;
}

static int
fuse_op_release(const char *path, struct fuse_file_info *fi)
{
    if (fi->fh != 0) {
        struct stat_file *const sfile = (struct stat_file *)(uintptr_t)fi->fh;

        fuse_op_stats_destroy(sfile);
    }
    return 0;
}

static int
fuse_op_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;
    struct boundary_info info;
    size_t orig_size = size;
    int r;

    // Handle stats file
    if (fi->fh != 0) {
        struct stat_file *const sfile = (struct stat_file *)(uintptr_t)fi->fh;

        if (offset > sfile->len)
            return 0;
        if (offset + size > sfile->len)
            size = sfile->len - offset;
        memcpy(buf, sfile->buf + offset, size);
        priv->stats_atime = time(NULL);
        return size;
    }

    // Check for end of file
    if (offset > priv->file_size) {
        (*config->log)(LOG_ERR, "read offset=0x%jx size=0x%jx out of range", (uintmax_t)offset, (uintmax_t)size);
        return -ESPIPE;
    }
    if (offset + size > priv->file_size) {
        size = priv->file_size - offset;
        orig_size = size;
    }

    // Calculate what bits to read, then read them
    calculate_boundary_info(&info, config->block_size, buf, size, offset);
    if (info.header.length > 0 && (r = block_part_read_block_part(priv->s3b, priv->block_part, &info.header)) != 0)
        return -r;
    while (info.mid_block_count-- > 0) {
        if ((r = (*priv->s3b->read_block)(priv->s3b, info.mid_block_start++, info.mid_data, NULL, NULL, 0)) != 0)
            return -r;
        info.mid_data += config->block_size;
    }
    if (info.footer.length > 0 && (r = block_part_read_block_part(priv->s3b, priv->block_part, &info.footer)) != 0)
        return -r;

    // Done
    priv->file_atime = time(NULL);
    return orig_size;
}

static int
fuse_op_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;
    struct boundary_info info;
    size_t orig_size = size;
    int r;

    // Handle read-only flag
    if (config->read_only)
        return -EROFS;

    // Handle stats file
    if (fi->fh != 0)
        return -EINVAL;

    // Check for end of file
    if (offset > priv->file_size) {
        (*config->log)(LOG_ERR, "write offset=0x%jx size=0x%jx out of range", (uintmax_t)offset, (uintmax_t)size);
        return -ESPIPE;
    }
    if (offset + size > priv->file_size) {
        size = priv->file_size - offset;
        orig_size = size;
    }

    // Handle request to write nothing
    if (size == 0)
        return 0;

    // Calculate what bits to write, then write them
    calculate_boundary_info(&info, config->block_size, buf, size, offset);
    if (info.header.length > 0 && (r = block_part_write_block_part(priv->s3b, priv->block_part, &info.header)) != 0)
        return -r;
    while (info.mid_block_count-- > 0) {
        if ((r = (*priv->s3b->write_block)(priv->s3b, info.mid_block_start++, info.mid_data, NULL, NULL, NULL)) != 0)
            return -r;
        info.mid_data += config->block_size;
    }
    if (info.footer.length > 0 && (r = block_part_write_block_part(priv->s3b, priv->block_part, &info.footer)) != 0)
        return -r;

    // Done
    priv->file_mtime = time(NULL);
    return orig_size;
}

static int
fuse_op_statfs(const char *path, struct statvfs *st)
{
    st->f_bsize = config->block_size;
    st->f_frsize = config->block_size;
    st->f_blocks = (off_t)config->num_blocks;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = 3;
    st->f_ffree = 0;
    st->f_favail = 0;
    return 0;
}

static int
fuse_op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    return 0;
}

static int
fuse_op_flush(const char *path, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;
    int r;

    // Ignore if stats file
    if (fi->fh != 0)
        return 0;

    // Flush ALL dirty blocks
    if ((r = (*priv->s3b->flush_blocks)(priv->s3b, NULL, 0, 0)) != 0)
        return -r;

    // Done
    return 0;
}

static int
fuse_op_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    return 0;
}

static int
fuse_op_unlink(const char *path)
{
    // Handle stats file
    if (*path == '/' && strcmp(path + 1, config->stats_filename) == 0) {
        if (config->clear_stats == NULL)
            return -EOPNOTSUPP;
        (*config->clear_stats)();
        return 0;
    }

    // Not supported
    return -EOPNOTSUPP;
}


static int
fuse_op_fallocate(const char *path, int mode, off_t offset, off_t len, struct fuse_file_info *fi)
{
    struct fuse_ops_private *const priv = (struct fuse_ops_private *)fuse_get_context()->private_data;
    struct boundary_info info;
    size_t size = (size_t)len;
    int r;

    // Handle stats file
    if (fi->fh != 0)
        return -EOPNOTSUPP;

    // Sanity check
    if (offset < 0 || len <= 0)
        return -EINVAL;
    if (offset + len > priv->file_size)
        return -ENOSPC;

    // Handle request
    if ((mode & FALLOC_FL_PUNCH_HOLE) == 0)
        return 0;
/*
    if ((mode & FALLOC_FL_KEEP_SIZE) == 0)
        return -EINVAL;
*/

    // Calculate what bits to write, then write them
    calculate_boundary_info(&info, config->block_size, NULL, size, offset);
    if (info.header.length > 0 && (r = block_part_write_block_part(priv->s3b, priv->block_part, &info.header)) != 0)
        return -r;
    while (info.mid_block_count-- > 0) {
        if ((r = (*priv->s3b->write_block)(priv->s3b, info.mid_block_start++, NULL, NULL, NULL, NULL)) != 0)
            return -r;
    }
    if (info.footer.length > 0 && (r = block_part_write_block_part(priv->s3b, priv->block_part, &info.footer)) != 0)
        return -r;

    // Done
    priv->file_mtime = time(NULL);
    return 0;
}

/****************************************************************************
 *                    OTHER INTERNAL FUNCTIONS                              *
 ****************************************************************************/

static struct stat_file *
fuse_op_stats_create(struct fuse_ops_private *priv)
{
    struct stat_file *sfile;

    if ((sfile = calloc(1, sizeof(*sfile))) == NULL)
        return NULL;
    (*config->print_stats)(sfile, fuse_op_stats_printer);
    if (sfile->memerr != 0) {
        fuse_op_stats_destroy(sfile);
        return NULL;
    }
    return sfile;
}

static void
fuse_op_stats_destroy(struct stat_file *sfile)
{
    free(sfile->buf);
    free(sfile);
}

static void
fuse_op_stats_printer(void *prarg, const char *fmt, ...)
{
    struct stat_file *const sfile = prarg;
    va_list args;
    char *new_buf;
    size_t new_bufsiz;
    size_t remain;
    int added;

    // Bail if no memory
    if (sfile->memerr)
        return;

again:
    // Append to string buffer
    remain = sfile->bufsiz - sfile->len;
    va_start(args, fmt);
    added = vsnprintf(sfile->buf + sfile->len, sfile->bufsiz - sfile->len, fmt, args);
    va_end(args);
    if (added + 1 <= remain) {
        sfile->len += added;
        return;
    }

    // We need a bigger buffer
    new_bufsiz = ((sfile->bufsiz + added + 1023) / 1024) * 1024;
    if ((new_buf = realloc(sfile->buf, new_bufsiz)) == NULL) {
        sfile->memerr = 1;
        return;
    }
    sfile->buf = new_buf;
    sfile->bufsiz = new_bufsiz;
    goto again;
}

// Stats mirror thread
static void *
stats_mirror_thread(void *arg)
{
    struct fuse_ops_private *const priv = the_priv;
    int file_was_there = 0;

    while (1) {

        // Time to leave?
        if (priv->stats_mirror_state == STATS_MIRROR_SHUTDOWN)
            break;

        // If system is up & running, update the stats file
        if (priv->stats_mirror_state == STATS_MIRROR_RUNNING) {
            struct stat sb;
            char *temp_path;
            FILE *fp;

            // If file existed before but was since deleted, reset stats
            if (file_was_there && stat(config->stats_mirror_path, &sb) == -1) {
                if (errno != ENOENT) {
                    (*config->log)(LOG_ERR, "error %s stats mirror file %s (mirroring stopped): %s",
                      "accessing", config->stats_mirror_path, strerror(errno));
                    break;
                }
                (*config->clear_stats)();
                file_was_there = 0;
            }

            // Format temporary name string
            if (asprintf(&temp_path, "%s.new", config->stats_mirror_path) == -1) {
                (*config->log)(LOG_ERR, "error %s stats mirror file %s (mirroring stopped): %s",
                  "updating", config->stats_mirror_path, strerror(errno));
                break;
            }

            // Write into temporary file
            if ((fp = fopen(temp_path, "w")) == NULL) {
                (*config->log)(LOG_ERR, "error %s stats mirror file %s (mirroring stopped): %s",
                  "creating", temp_path, strerror(errno));
                free(temp_path);
                break;
            }
            (*config->print_stats)(fp, stats_mirror_printer);
            if (fclose(fp) == EOF) {
                (*config->log)(LOG_ERR, "error %s stats mirror file %s (mirroring stopped): %s",
                  "writing", config->stats_mirror_path, strerror(errno));
                unlink(temp_path);
                free(temp_path);
                break;
            }
            if (rename(temp_path, config->stats_mirror_path) == -1) {
                (*config->log)(LOG_ERR, "error %s stats mirror file %s (mirroring stopped): %s",
                  "updating", config->stats_mirror_path, strerror(errno));
                unlink(temp_path);
                free(temp_path);
                break;
            }
            free(temp_path);
            file_was_there = 1;
        }

        // Sleep for a while
        usleep(config->stats_mirror_interval * 1000);
    }

    // Done
    return NULL;
}

static void
stats_mirror_printer(void *prarg, const char *fmt, ...)
{
    FILE *const fp = prarg;
    va_list args;

    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
}
