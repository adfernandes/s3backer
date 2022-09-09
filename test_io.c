
/*
 * s3backer - FUSE-based single file backing store via Amazon S3
 *
 * Copyright 2008-2020 Archie L. Cobbs <archie.cobbs@gmail.com>
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
#include "http_io.h"
#include "test_io.h"
#include "util.h"

// Do we want random errors?
#define RANDOM_ERROR_PERCENT    0

// Internal state
struct test_io_private {
    struct test_io_conf         *config;
    pthread_mutex_t             mutex;
    bitmap_t                    *blocks_reading;         // 1 = block is currently being read
    bitmap_t                    *blocks_writing;         // 1 = block is currently being written
    volatile int                shutdown;
};

// s3backer_store functions
static int test_io_create_threads(struct s3backer_store *s3b);
static int test_io_meta_data(struct s3backer_store *s3b, off_t *file_sizep, u_int *block_sizep);
static int test_io_set_mount_token(struct s3backer_store *s3b, int32_t *old_valuep, int32_t new_value);
static int test_io_read_block(struct s3backer_store *s3b, s3b_block_t block_num, void *dest,
  u_char *actual_etag, const u_char *expect_etag, int strict);
static int test_io_write_block(struct s3backer_store *s3b, s3b_block_t block_num, const void *src, u_char *etag,
  check_cancel_t *check_cancel, void *check_cancel_arg);
static int test_io_flush_blocks(struct s3backer_store *s3b, const s3b_block_t *block_nums, u_int num_blocks, long timeout);
static int test_io_survey_non_zero(struct s3backer_store *s3b, block_list_func_t *callback, void *arg);
static int test_io_shutdown(struct s3backer_store *s3b);
static void test_io_destroy(struct s3backer_store *s3b);

/*
 * Constructor
 *
 * On error, returns NULL and sets `errno'.
 */
struct s3backer_store *
test_io_create(struct test_io_conf *config)
{
    struct s3backer_store *s3b;
    struct test_io_private *priv;
    int r;

    // Initialize structures
    if ((s3b = calloc(1, sizeof(*s3b))) == NULL)
        return NULL;
    s3b->create_threads = test_io_create_threads;
    s3b->meta_data = test_io_meta_data;
    s3b->set_mount_token = test_io_set_mount_token;
    s3b->read_block = test_io_read_block;
    s3b->write_block = test_io_write_block;
    s3b->bulk_zero = generic_bulk_zero;
    s3b->flush_blocks = test_io_flush_blocks;
    s3b->survey_non_zero = test_io_survey_non_zero;
    s3b->shutdown = test_io_shutdown;
    s3b->destroy = test_io_destroy;
    if ((priv = calloc(1, sizeof(*priv))) == NULL) {
        r = errno;
        goto fail1;
    }
    priv->config = config;
    s3b->data = priv;

    // Initialize bitmaps and mutex
    if ((priv->blocks_reading = bitmap_init(config->num_blocks, 0)) == NULL) {
        r = errno;
        goto fail2;
    }
    if ((priv->blocks_writing = bitmap_init(config->num_blocks, 0)) == NULL) {
        r = errno;
        goto fail3;
    }
    if ((r = pthread_mutex_init(&priv->mutex, NULL)) != 0)
        goto fail4;

    // Random initialization
    if (config->random_delays || config->random_errors)
        srandom((u_int)time(NULL));

    // Done
    return s3b;

fail4:
    free(priv->blocks_writing);
fail3:
    free(priv->blocks_reading);
fail2:
    free(priv);
fail1:
    free(s3b);
    errno = r;
    return NULL;
}

static int
test_io_create_threads(struct s3backer_store *s3b)
{
    return 0;
}

static int
test_io_meta_data(struct s3backer_store *s3b, off_t *file_sizep, u_int *block_sizep)
{
    return 0;
}

static int
test_io_set_mount_token(struct s3backer_store *s3b, int32_t *old_valuep, int32_t new_value)
{
    if (old_valuep != NULL)
        *old_valuep = 0;
    return 0;
}

static int
test_io_flush_blocks(struct s3backer_store *s3b, const s3b_block_t *block_nums, u_int num_blocks, long timeout)
{
    struct test_io_private *const priv = s3b->data;
    struct test_io_conf *const config = priv->config;
    char block_hash_buf[S3B_BLOCK_NUM_DIGITS + 2];
    char path[PATH_MAX];
    int r;

    // Anything to do?
    if (config->discard_data || num_blocks == 0)
        return 0;

    // We don't handle the "all dirty blocks case"
    if (block_nums == NULL)
        return 0;

    // Sync each block
    while (num_blocks-- > 0) {
        const s3b_block_t block_num = *block_nums++;

        // Generate path
        http_io_format_block_hash(config->blockHashPrefix, block_hash_buf, sizeof(block_hash_buf), block_num);
        snvprintf(path, sizeof(path), "%s/%s%s%0*jx",
          config->bucket, config->prefix, block_hash_buf, S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);

        // Sync file
        if ((r = fsync_path(path, 0)) != 0) {
            (*config->log)(LOG_ERR, "can't fsync %s: %s", path, strerror(r));
            return r;
        }
    }

    // Sync the containing directory
    *strrchr(path, '/') = '\0';
    if ((r = fsync_path(path, 1)) != 0) {
        (*config->log)(LOG_ERR, "can't fsync %s: %s", path, strerror(r));
        return r;
    }

    // Done
    return 0;
}

static int
test_io_shutdown(struct s3backer_store *const s3b)
{
    struct test_io_private *const priv = s3b->data;

    priv->shutdown = 1;
    return 0;
}

static void
test_io_destroy(struct s3backer_store *const s3b)
{
    struct test_io_private *const priv = s3b->data;

    pthread_mutex_destroy(&priv->mutex);
    free(priv->blocks_reading);
    free(priv->blocks_writing);
    free(priv);
    free(s3b);
}

static int
test_io_read_block(struct s3backer_store *const s3b, s3b_block_t block_num, void *dest,
  u_char *actual_etag, const u_char *expect_etag, int strict)
{
    struct test_io_private *const priv = s3b->data;
    struct test_io_conf *const config = priv->config;
    char block_hash_buf[S3B_BLOCK_NUM_DIGITS + 2];
    u_char md5[MD5_DIGEST_LENGTH];
    char path[PATH_MAX];
    int read_overlap;
    int write_overlap;
    int is_zero_block;
    MD5_CTX ctx;
    int fd;
    int r;

    // Logging
    if (config->debug)
        (*config->log)(LOG_DEBUG, "test_io: read %0*jx started", S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);

    // Random delay
    if (config->random_delays)
        usleep((random() % 200) * 1000);

    // Detect overlapping reads and/or writes
    pthread_mutex_lock(&priv->mutex);
    if (!(read_overlap = bitmap_test(priv->blocks_reading, block_num)))
        bitmap_set(priv->blocks_reading, block_num, 1);
    write_overlap = bitmap_test(priv->blocks_writing, block_num);
    CHECK_RETURN(pthread_mutex_unlock(&priv->mutex));
    if (read_overlap || write_overlap) {
        (*config->log)(LOG_WARNING, "test_io: detected simultaneous %s of block %0*jx",
          read_overlap && write_overlap ? "reads and write" : read_overlap ? "reads" : "read and write",
          S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);
    }

    // Random error
    if (config->random_errors && (random() % 100) < RANDOM_ERROR_PERCENT) {
        (*config->log)(LOG_ERR, "test_io: random failure reading %0*jx", S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);
        r = EAGAIN;
        goto done;
    }

    // Read block
    if (config->discard_data)
        r = ENOENT;
    else {

        // Generate path
        http_io_format_block_hash(config->blockHashPrefix, block_hash_buf, sizeof(block_hash_buf), block_num);
        snvprintf(path, sizeof(path), "%s/%s%s%0*jx",
          config->bucket, config->prefix, block_hash_buf, S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);

        // Open and read file
        if ((fd = open(path, O_RDONLY|O_CLOEXEC)) != -1) {
            int total;

            // Read file
            for (total = 0; total < config->block_size; total += r) {
                if ((r = read(fd, (char *)dest + total, config->block_size - total)) == -1) {
                    r = errno;
                    (*config->log)(LOG_ERR, "can't read %s: %s", path, strerror(r));
                    close(fd);
                    goto done;
                }
                if (r == 0)
                    break;
            }
            close(fd);

            // Check for short read
            if (total != config->block_size) {
                (*config->log)(LOG_ERR, "%s: file is truncated (only read %d out of %u bytes)", path, total, config->block_size);
                r = EIO;
                goto done;
            }

            // Done
            r = 0;
        } else
            r = errno;
    }

    // Convert ENOENT into a read of all zeros
    if ((is_zero_block = (r == ENOENT))) {
        memset(dest, 0, config->block_size);
        r = 0;
    }

    // Check for other error
    if (r != 0) {
        (*config->log)(LOG_ERR, "can't open %s: %s", path, strerror(r));
        goto done;
    }

    // Compute MD5
    if (is_zero_block)
        memset(md5, 0, MD5_DIGEST_LENGTH);
    else {
        MD5_Init(&ctx);
        MD5_Update(&ctx, dest, config->block_size);
        MD5_Final(md5, &ctx);
    }
    if (actual_etag != NULL)
        memcpy(actual_etag, md5, MD5_DIGEST_LENGTH);

    // Check expected MD5
    if (expect_etag != NULL) {
        const int match = memcmp(md5, expect_etag, MD5_DIGEST_LENGTH) == 0;

        if (strict) {
            if (!match) {
                (*config->log)(LOG_ERR,
                   "%s: wrong MD5 checksum?! %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
                   " != %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", path,
                  (u_int)md5[0], (u_int)md5[1], (u_int)md5[2], (u_int)md5[3],
                  (u_int)md5[4], (u_int)md5[5], (u_int)md5[6], (u_int)md5[7],
                  (u_int)md5[8], (u_int)md5[9], (u_int)md5[10], (u_int)md5[11],
                  (u_int)md5[12], (u_int)md5[13], (u_int)md5[14], (u_int)md5[15],
                  (u_int)expect_etag[0], (u_int)expect_etag[1], (u_int)expect_etag[2], (u_int)expect_etag[3],
                  (u_int)expect_etag[4], (u_int)expect_etag[5], (u_int)expect_etag[6], (u_int)expect_etag[7],
                  (u_int)expect_etag[8], (u_int)expect_etag[9], (u_int)expect_etag[10], (u_int)expect_etag[11],
                  (u_int)expect_etag[12], (u_int)expect_etag[13], (u_int)expect_etag[14], (u_int)expect_etag[15]);
                r = EINVAL;
                goto done;
            }
        } else if (match)
            r = EEXIST;
    }

    // Logging
    if (config->debug) {
        (*config->log)(LOG_DEBUG,
          "test_io: read %0*jx complete, MD5 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%s%s",
          S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num,
          (u_int)md5[0], (u_int)md5[1], (u_int)md5[2], (u_int)md5[3],
          (u_int)md5[4], (u_int)md5[5], (u_int)md5[6], (u_int)md5[7],
          (u_int)md5[8], (u_int)md5[9], (u_int)md5[10], (u_int)md5[11],
          (u_int)md5[12], (u_int)md5[13], (u_int)md5[14], (u_int)md5[15],
          is_zero_block ? " (zero)" : "", r == EEXIST ? " (expected md5 match)" : "");
    }

done:
    // Reset reading flag
    if (!read_overlap) {
        pthread_mutex_lock(&priv->mutex);
        assert(bitmap_test(priv->blocks_reading, block_num));
        bitmap_set(priv->blocks_reading, block_num, 0);
        CHECK_RETURN(pthread_mutex_unlock(&priv->mutex));
    }

    // Done
    return r;
}

static int
test_io_write_block(struct s3backer_store *const s3b, s3b_block_t block_num, const void *src, u_char *caller_etag,
  check_cancel_t *check_cancel, void *check_cancel_arg)
{
    struct test_io_private *const priv = s3b->data;
    struct test_io_conf *const config = priv->config;
    char block_hash_buf[S3B_BLOCK_NUM_DIGITS + 2];
    u_char md5[MD5_DIGEST_LENGTH];
    char temp[PATH_MAX];
    char path[PATH_MAX];
    int read_overlap;
    int write_overlap;
    MD5_CTX ctx;
    int total;
    int fd;
    int r;

    // Check for zero block
    if (src != NULL && block_is_zeros(src))
        src = NULL;

    // Compute MD5
    if (src != NULL) {
        MD5_Init(&ctx);
        MD5_Update(&ctx, src, config->block_size);
        MD5_Final(md5, &ctx);
    } else
        memset(md5, 0, MD5_DIGEST_LENGTH);

    // Return MD5 to caller
    if (caller_etag != NULL)
        memcpy(caller_etag, md5, MD5_DIGEST_LENGTH);

    // Logging
    if (config->debug) {
        (*config->log)(LOG_DEBUG,
          "test_io: write %0*jx started, MD5 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%s",
          S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num,
          (u_int)md5[0], (u_int)md5[1], (u_int)md5[2], (u_int)md5[3],
          (u_int)md5[4], (u_int)md5[5], (u_int)md5[6], (u_int)md5[7],
          (u_int)md5[8], (u_int)md5[9], (u_int)md5[10], (u_int)md5[11],
          (u_int)md5[12], (u_int)md5[13], (u_int)md5[14], (u_int)md5[15],
          src == NULL ? " (zero block)" : "");
    }

    // Random delay
    if (config->random_delays)
        usleep((random() % 200) * 1000);

    // Detect overlapping reads and/or writes
    pthread_mutex_lock(&priv->mutex);
    read_overlap = bitmap_test(priv->blocks_reading, block_num);
    if (!(write_overlap = bitmap_test(priv->blocks_writing, block_num)))
        bitmap_set(priv->blocks_writing, block_num, 1);
    CHECK_RETURN(pthread_mutex_unlock(&priv->mutex));
    if (read_overlap || write_overlap) {
        (*config->log)(LOG_WARNING, "test_io: detected simultaneous %s of block %0*jx",
          read_overlap && write_overlap ? "read and writes" : write_overlap ? "writes" : "read and write",
          S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);
    }

    // Random error
    if (config->random_errors && (random() % 100) < RANDOM_ERROR_PERCENT) {
        (*config->log)(LOG_ERR, "test_io: random failure writing %0*jx", S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);
        r = EAGAIN;
        goto done;
    }

    // Discarding data?
    if (config->discard_data) {
        if (config->debug)
            (*config->log)(LOG_DEBUG, "test_io: discard %0*jx complete", S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);
        r = 0;
        goto done;
    }

    // Generate path
    http_io_format_block_hash(config->blockHashPrefix, block_hash_buf, sizeof(block_hash_buf), block_num);
    snvprintf(path, sizeof(path), "%s/%s%s%0*jx",
      config->bucket, config->prefix, block_hash_buf, S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);

    // Delete zero blocks
    if (src == NULL) {
        if (unlink(path) == -1 && errno != ENOENT) {
            r = errno;
            (*config->log)(LOG_ERR, "can't unlink %s: %s", path, strerror(r));
            goto done;
        }
        r = 0;
        goto done;
    }

    // Write into temporary file
    snvprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    if ((fd = mkstemp(temp)) == -1) {
        r = errno;
        (*config->log)(LOG_ERR, "%s: %s", temp, strerror(r));
        goto done;
    }
    for (total = 0; total < config->block_size; total += r) {
        if ((r = write(fd, (const char *)src + total, config->block_size - total)) == -1) {
            r = errno;
            (*config->log)(LOG_ERR, "can't write %s: %s", temp, strerror(r));
            close(fd);
            (void)unlink(temp);
            goto done;
        }
    }
    close(fd);

    // Rename file
    if (rename(temp, path) == -1) {
        r = errno;
        (*config->log)(LOG_ERR, "can't rename %s: %s", temp, strerror(r));
        (void)unlink(temp);
        goto done;
    }
    r = 0;

    // Logging
    if (config->debug)
        (*config->log)(LOG_DEBUG, "test_io: write %0*jx complete", S3B_BLOCK_NUM_DIGITS, (uintmax_t)block_num);

done:
    // Reset writing flag
    if (!write_overlap) {
        pthread_mutex_lock(&priv->mutex);
        assert(bitmap_test(priv->blocks_writing, block_num));
        bitmap_set(priv->blocks_writing, block_num, 0);
        CHECK_RETURN(pthread_mutex_unlock(&priv->mutex));
    }

    // Done
    return r;
}

static int
test_io_survey_non_zero(struct s3backer_store *s3b, block_list_func_t *callback, void *arg)
{
    struct test_io_private *const priv = s3b->data;
    struct test_io_conf *const config = priv->config;
    s3b_block_t hash_value;
    s3b_block_t block_num;
    struct dirent *dent;
    int r = 0;
    DIR *dir;
    int i;

    // Discarding data?
    if (config->discard_data)
        return 0;

    // Open directory
    if ((dir = opendir(config->bucket)) == NULL)
        return errno;

    // Scan directory
    for (i = 0; (dent = readdir(dir)) != NULL; i++) {
        if (http_io_parse_block(config->prefix, config->num_blocks,
          config->blockHashPrefix, dent->d_name, &hash_value, &block_num) == 0) {
            if ((r = (*callback)(arg, &block_num, 1)) != 0)
                break;
        }
        if (priv->shutdown) {
            r = ECANCELED;
            break;
        }
    }

    // Close directory
    closedir(dir);

    // Done
    return r;
}
