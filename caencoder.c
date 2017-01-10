#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caencoder.h"
#include "caformat.h"
#include "def.h"
#include "realloc-buffer.h"
#include "util.h"

typedef struct CaEncoderNode {
        int fd;
        struct stat stat;

        /* For S_ISDIR */
        struct dirent **dirents;
        size_t n_dirents;
        size_t dirent_idx;

        /* For S_ISLNK */
        char *symlink_target;

        /* For S_ISBLK */
        uint64_t device_size;
} CaEncoderNode;

typedef enum CaEncoderState {
        CA_ENCODER_INIT,
        CA_ENCODER_HELLO,
        CA_ENCODER_ENTRY,
        CA_ENCODER_POST_CHILD,
        CA_ENCODER_GOODBYE,
        CA_ENCODER_EOF,
} CaEncoderState;

struct CaEncoder {
        CaEncoderState state;

        uint64_t feature_flags;

        uint64_t time_granularity;

        CaEncoderNode nodes[NODES_MAX];
        size_t n_nodes;
        size_t node_idx;

        ReallocBuffer buffer;

        uint64_t archive_offset;
        uint64_t payload_offset;
        uint64_t step_size;
};

CaEncoder *ca_encoder_new(void) {
        CaEncoder *e;

        e = new0(CaEncoder, 1);
        if (!e)
                return NULL;

        e->feature_flags = CA_FORMAT_WITH_BEST;
        e->time_granularity = 1;

        return e;
}

static void ca_encoder_node_free(CaEncoderNode *n) {
        size_t i;

        assert(n);

        if (n->fd >= 3)
                n->fd = safe_close(n->fd);
        else
                n->fd = -1;

        for (i = 0; i < n->n_dirents; i++)
                free(n->dirents[i]);
        n->dirents = mfree(n->dirents);
        n->n_dirents = n->dirent_idx = 0;

        n->symlink_target = mfree(n->symlink_target);

        n->device_size = UINT64_MAX;
}

CaEncoder *ca_encoder_unref(CaEncoder *e) {
        size_t i;

        if (!e)
                return NULL;

        for (i = 0; i < e->n_nodes; i++)
                ca_encoder_node_free(e->nodes + i);

        realloc_buffer_free(&e->buffer);
        free(e);

        return NULL;
}

int ca_encoder_set_feature_flags(CaEncoder *e, uint64_t flags) {
        uint64_t granularity = 0;

        if (!e)
                return -EINVAL;

        if (flags & ~CA_FORMAT_FEATURE_FLAGS_MAX)
                return -EOPNOTSUPP;

        /* Normalize a number of flags */

        if (flags & CA_FORMAT_WITH_UID_GID_32BIT)
                flags &= ~CA_FORMAT_WITH_UID_GID_16BIT;

        if (flags & CA_FORMAT_WITH_TIMES_NSEC) {
                flags &= ~(CA_FORMAT_WITH_TIMES_USEC|CA_FORMAT_WITH_TIMES_SEC|CA_FORMAT_WITH_TIMES_2SEC);
                granularity = 1;
        }
        if (flags & CA_FORMAT_WITH_TIMES_USEC) {
                flags &= ~(CA_FORMAT_WITH_TIMES_SEC|CA_FORMAT_WITH_TIMES_2SEC);
                granularity = 1000;
        }
        if (flags & CA_FORMAT_WITH_TIMES_SEC) {
                granularity = 1000000000;
                flags &= ~CA_FORMAT_WITH_TIMES_2SEC;
        }
        if (flags & CA_FORMAT_WITH_TIMES_2SEC)
                granularity = 2000000000;

        if (flags & CA_FORMAT_WITH_PERMISSIONS)
                flags &= ~CA_FORMAT_WITH_READONLY;

        e->feature_flags = flags;
        e->time_granularity = granularity;

        return 0;
}

int ca_encoder_get_feature_flags(CaEncoder *e, uint64_t *ret) {
        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        *ret = e->feature_flags;
        return 0;
}

int ca_encoder_set_base_fd(CaEncoder *e, int fd) {
        struct stat st;

        if (!e)
                return -EINVAL;
        if (fd < 0)
                return -EINVAL;
        if (e->n_nodes > 0)
                return -EBUSY;

        if (fstat(fd, &st) < 0)
                return -errno;

        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISBLK(st.st_mode))
                return -ENOTTY;

        e->nodes[0] = (struct CaEncoderNode) {
                .fd = fd,
                .stat = st,
                .device_size = UINT64_MAX,
        };

        e->n_nodes = 1;

        return 0;
}

static CaEncoderNode* ca_encoder_current_node(CaEncoder *e) {
        assert(e);

        if (e->node_idx >= e->n_nodes)
                return NULL;

        return e->nodes + e->node_idx;
}

static CaEncoderNode* ca_encoder_current_child_node(CaEncoder *e) {
        assert(e);

        if (e->node_idx+1 >= e->n_nodes)
                return NULL;

        return e->nodes + e->node_idx + 1;
}

static const struct dirent *ca_encoder_node_current_dirent(CaEncoderNode *n) {
        assert(n);

        if (n->n_dirents <= 0)
                return NULL;
        if (n->dirent_idx >= n->n_dirents)
                return NULL;

        return n->dirents[n->dirent_idx];
}

static int scandir_filter(const struct dirent *de) {
        assert(de);

        /* Filter out "." and ".." */

        if (de->d_name[0] != '.')
                return true;

        if (de->d_name[1] == 0)
                return false;

        if (de->d_name[1] != '.')
                return true;

        return de->d_name[2] != 0;
}

static int scandir_compare(const struct dirent **a, const struct dirent **b) {
        assert(a);
        assert(b);

        /* We don't use alphasort() here, as we want locale-independent ordering */

        return strcmp((*a)->d_name, (*b)->d_name);
}

static int ca_encoder_node_read_dirents(CaEncoderNode *n) {
        int r;

        assert(n);

        if (n->dirents)
                return 0;
        if (!S_ISDIR(n->stat.st_mode))
                return -ENOTDIR;
        if (n->fd < 0)
                return -EBADFD;

        r = scandirat(n->fd, ".", &n->dirents, scandir_filter, scandir_compare);
        if (r < 0)
                return -errno;

        n->n_dirents = (size_t) r;
        n->dirent_idx = 0;

        return 1;
}

static int ca_encoder_node_read_device_size(CaEncoderNode *n) {
        unsigned long u = 0;

        assert(n);

        if (n->device_size != (uint64_t) -1)
                return 0;
        if (!S_ISBLK(n->stat.st_mode))
                return -ENOTTY;
        if (n->fd < 0)
                return -EBADFD;

        if (ioctl(n->fd, BLKGETSIZE, &u) < 0)
                return -errno;

        n->device_size = (uint64_t) u * 512;
        return 1;
}

static int ca_encoder_node_read_symlink(
                CaEncoderNode *n,
                const struct dirent *de,
                CaEncoderNode *symlink) {

        size_t k = 16;

        assert(n);
        assert(de);
        assert(symlink);

        if (!S_ISDIR(n->stat.st_mode))
                return -ENOTDIR;
        if (n->fd < 0)
                return -EBADFD;

        if (!S_ISLNK(symlink->stat.st_mode))
                return -ENOTTY;
        if (symlink->symlink_target)
                return 0;

        for (;;) {
                ssize_t z;
                char *buf;

                buf = malloc(k+1);
                if (!buf)
                        return -ENOMEM;

                z = readlinkat(n->fd, de->d_name, buf, k);
                if (z < 0) {
                        free(buf);
                        return -errno;
                }
                if (z < k) {
                        buf[z] = 0;

                        symlink->symlink_target = buf;
                        return 1;
                }

                free(buf);
                k *= 2;
        }
}

static void ca_encoder_forget_children(CaEncoder *e) {
        assert(e);

        while (e->n_nodes-1 > e->node_idx)
                ca_encoder_node_free(e->nodes + --e->n_nodes);
}

static CaEncoderNode* ca_encoder_init_child(CaEncoder *e) {
        CaEncoderNode *n;

        assert(e);

        ca_encoder_forget_children(e);

        if (e->n_nodes >= NODES_MAX)
                return NULL;

        n = e->nodes + e->n_nodes++;

        *n = (CaEncoderNode) {
                .fd = -1,
                .device_size = UINT64_MAX,
        };

        return n;
}

static int ca_encoder_open_child(CaEncoder *e, const struct dirent *de) {
        CaEncoderNode *n, *child;
        int r, open_flags = O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW;
        bool shall_open, have_stat;

        assert(e);
        assert(de);

        n = ca_encoder_current_node(e);
        if (!n)
                return -EUNATCH;

        if (!S_ISDIR(n->stat.st_mode))
                return -ENOTDIR;
        if (n->fd < 0)
                return -EBADFD;

        child = ca_encoder_init_child(e);
        if (!child)
                return -E2BIG;

        if (de->d_type == DT_DIR || de->d_type == DT_REG) {
                shall_open = true;
                have_stat = false;

                if (de->d_type == DT_DIR)
                        open_flags |= O_DIRECTORY;
        } else {
                if (fstatat(n->fd, de->d_name, &child->stat, AT_SYMLINK_NOFOLLOW) < 0)
                        return -errno;

                have_stat = true;
                shall_open = S_ISREG(child->stat.st_mode) || S_ISDIR(child->stat.st_mode);

                if (S_ISDIR(child->stat.st_mode))
                        open_flags |= O_DIRECTORY;
        }

        if (shall_open) {
                child->fd = openat(n->fd, de->d_name, open_flags);
                if (child->fd < 0)
                        return -errno;

                if (!have_stat) {
                        if (fstat(child->fd, &child->stat) < 0)
                                return -errno;
                }
        }

        if (S_ISLNK(child->stat.st_mode)) {
                r = ca_encoder_node_read_symlink(n, de, child);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int ca_encoder_enter_child(CaEncoder *e) {
        mode_t mode;

        assert(e);

        if (e->node_idx+1 >= e->n_nodes)
                return -EINVAL;
        mode = e->nodes[e->node_idx+1].stat.st_mode;
        if (mode == 0)
                return -EINVAL;
        if (!S_ISREG(mode) && !S_ISDIR(mode))
                return -ENOTTY;
        if (e->nodes[e->node_idx+1].fd < 0)
                return -EINVAL;

        e->node_idx++;
        return 0;
}

static int ca_encoder_leave_child(CaEncoder *e) {
        assert(e);

        if (e->node_idx <= 0)
                return 0;

        e->node_idx--;
        return 1;
}

static int ca_encoder_node_get_payload_size(CaEncoderNode *n, uint64_t *ret) {
        int r;

        assert(n);
        assert(ret);

        if (S_ISREG(n->stat.st_mode))
                *ret = n->stat.st_size;
        else if (S_ISBLK(n->stat.st_mode)) {
                r = ca_encoder_node_read_device_size(n);
                if (r < 0)
                        return r;

                *ret = n->device_size;
        } else
                return -ENOTTY;

        return 0;
}

static void ca_encoder_enter_state(CaEncoder *e, CaEncoderState state) {
        assert(e);

        e->state = state;

        realloc_buffer_empty(&e->buffer);

        e->payload_offset = 0;
        e->step_size = 0;
}

static int ca_encoder_step_regular(CaEncoder *e, CaEncoderNode *n) {
        uint64_t size;
        int r;

        assert(e);
        assert(n);
        assert(S_ISREG(n->stat.st_mode) || S_ISBLK(n->stat.st_mode));
        assert(e->state == CA_ENCODER_INIT);

        realloc_buffer_empty(&e->buffer);

        r = ca_encoder_node_get_payload_size(n, &size);
        if (r < 0)
                return r;

        if (e->payload_offset >= size) {
                ca_encoder_enter_state(e, CA_ENCODER_EOF);
                return CA_ENCODER_FINISHED;
        }

        return CA_ENCODER_DATA;
}

static int ca_encoder_step_directory(CaEncoder *e, CaEncoderNode *n) {
        int r;

        assert(e);
        assert(n);
        assert(S_ISDIR(n->stat.st_mode));

        r = ca_encoder_node_read_dirents(n);
        if (r < 0)
                return r;

        switch (e->state) {

        case CA_ENCODER_INIT:
                ca_encoder_enter_state(e, CA_ENCODER_HELLO);
                return CA_ENCODER_DATA;

        case CA_ENCODER_ENTRY: {
                CaEncoderNode *child;

                child = ca_encoder_current_child_node(e);
                if (!child)
                        return -ENOTTY;

                if (S_ISDIR(child->stat.st_mode) || S_ISREG(child->stat.st_mode)) {

                        r = ca_encoder_enter_child(e);
                        if (r < 0)
                                return r;

                        ca_encoder_enter_state(e, CA_ENCODER_INIT);
                        return ca_encoder_step(e);
                }
        }

                /* Fall through */

        case CA_ENCODER_POST_CHILD:
                n->dirent_idx++;

                /* Fall through */

        case CA_ENCODER_HELLO: {
                const struct dirent *de;

                de = ca_encoder_node_current_dirent(n);
                if (!de) {
                        ca_encoder_enter_state(e, CA_ENCODER_GOODBYE);
                        return CA_ENCODER_DATA;
                }

                r = ca_encoder_open_child(e, de);
                if (r < 0)
                        return r;

                ca_encoder_enter_state(e, CA_ENCODER_ENTRY);
                return CA_ENCODER_NEXT_FILE;
        }

        case CA_ENCODER_GOODBYE:
                ca_encoder_enter_state(e, CA_ENCODER_EOF);
                return CA_ENCODER_FINISHED;

        default:
                assert(false);
        }

        assert(false);
}

int ca_encoder_step(CaEncoder *e) {
        int r;

        if (!e)
                return -EINVAL;

        if (e->state == CA_ENCODER_EOF)
                return CA_ENCODER_FINISHED;

        e->payload_offset += e->step_size;
        e->archive_offset += e->step_size;
        e->step_size = 0;

        for (;;) {
                CaEncoderNode *n;

                n = ca_encoder_current_node(e);
                if (!n)
                        return -EUNATCH;

                if (S_ISREG(n->stat.st_mode) || S_ISBLK(n->stat.st_mode))
                        r = ca_encoder_step_regular(e, n);
                else if (S_ISDIR(n->stat.st_mode))
                        r = ca_encoder_step_directory(e, n);
                else
                        return -ENOTTY;
                if (r != CA_ENCODER_FINISHED)
                        return r;

                r = ca_encoder_leave_child(e);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                ca_encoder_enter_state(e, CA_ENCODER_POST_CHILD);
        }

        ca_encoder_forget_children(e);
        return CA_ENCODER_FINISHED;
}

static int ca_encoder_get_payload_data(CaEncoder *e, CaEncoderNode *n) {
        uint64_t size;
        ssize_t m;
        size_t k;
        void *p;
        int r;

        assert(e);
        assert(n);
        assert(S_ISREG(n->stat.st_mode) || S_ISBLK(n->stat.st_mode));
        assert(e->state == CA_ENCODER_INIT);

        r = ca_encoder_node_get_payload_size(n, &size);
        if (r < 0)
                return r;

        if (e->payload_offset >= size) /* at EOF? */
                return 0;

        if (e->buffer.size > 0) /* already in buffer? */
                return 1;

        k = (size_t) MIN(BUFFER_SIZE, size - e->payload_offset);

        p = realloc_buffer_acquire(&e->buffer, k);
        if (!p)
                return -ENOMEM;

        m = read(n->fd, p, k);
        if (m < 0) {
                r = -errno;
                goto fail;
        }
        if ((size_t) m != k) {
                r = -EIO;
                goto fail;
        }

        return 1;

fail:
        realloc_buffer_empty(&e->buffer);
        return r;
}

static int ca_encoder_get_hello_data(CaEncoder *e, CaEncoderNode *n) {
        CaFormatHello *h;

        assert(e);
        assert(n);
        assert(S_ISDIR(n->stat.st_mode));
        assert(e->state == CA_ENCODER_HELLO);

        if (e->buffer.size > 0) /* Already generated */
                return 1;

        h = realloc_buffer_acquire(&e->buffer, sizeof(CaFormatHello));
        if (!h)
                return -ENOMEM;

        *h = (CaFormatHello) {
                .header.type = htole64(CA_FORMAT_HELLO),
                .header.size = htole64(sizeof(CaFormatHello)),
                .uuid_part2 = htole64(CA_FORMAT_HELLO_UUID_PART2),
                .feature_flags = htole64(e->feature_flags),
        };

        return 1;
}

static int ca_encoder_get_entry_data(CaEncoder *e, CaEncoderNode *n) {
        const struct dirent *de;
        CaFormatEntry *entry;
        CaEncoderNode *child;
        uint64_t mtime, mode, uid, gid;
        size_t size;
        void *p;

        assert(e);
        assert(n);
        assert(S_ISDIR(n->stat.st_mode));
        assert(e->state == CA_ENCODER_ENTRY);

        if (e->buffer.size > 0) /* Already generated */
                return 1;

        de = ca_encoder_node_current_dirent(n);
        if (!de)
                return -EILSEQ;

        child = ca_encoder_current_child_node(e);
        if (!child)
                return -EILSEQ;

        if (child->stat.st_uid == UINT16_MAX ||
            child->stat.st_uid == UINT32_MAX ||
            child->stat.st_gid == UINT16_MAX ||
            child->stat.st_gid == UINT32_MAX)
                return -EINVAL;

        if ((e->feature_flags & CA_FORMAT_WITH_UID_GID_16BIT) &&
            (child->stat.st_uid > UINT16_MAX ||
             child->stat.st_gid > UINT16_MAX))
                return -EPROTONOSUPPORT;

        if (e->feature_flags & (CA_FORMAT_WITH_UID_GID_16BIT|CA_FORMAT_WITH_UID_GID_32BIT)) {
                uid = child->stat.st_uid;
                gid = child->stat.st_gid;
        } else
                uid = gid = 0;

        if ((e->feature_flags & CA_FORMAT_WITH_SYMLINKS) == 0 &&
            S_ISLNK(child->stat.st_mode))
                return -EPROTONOSUPPORT;

        if ((e->feature_flags & CA_FORMAT_WITH_DEVICE_NODES) == 0 &&
            (S_ISBLK(child->stat.st_mode) || S_ISCHR(child->stat.st_mode)))
                return -EPROTONOSUPPORT;

        if ((e->feature_flags & CA_FORMAT_WITH_FIFOS) == 0 &&
            S_ISFIFO(child->stat.st_mode))
                return -EPROTONOSUPPORT;

        if ((e->feature_flags & CA_FORMAT_WITH_SOCKETS) == 0 &&
            S_ISSOCK(child->stat.st_mode))
                return -EPROTONOSUPPORT;

        mtime = timespec_to_nsec(child->stat.st_mtim);
        mtime = (mtime / e->time_granularity) * e->time_granularity;

        mode = child->stat.st_mode;
        if (S_ISLNK(mode))
                mode = S_IFLNK | 0777;
        if (e->feature_flags & CA_FORMAT_WITH_PERMISSIONS)
                mode = mode & (S_IFMT|07777);
        else if (e->feature_flags & CA_FORMAT_WITH_READONLY)
                mode = (mode & S_IFMT) | ((mode & 0222) ? (S_ISDIR(mode) ? 0777 : 0666) : (S_ISDIR(mode) ? 0555 : 0444));
        else
                mode &= S_IFMT;

        size = offsetof(CaFormatEntry, name) + strlen(de->d_name) + 1;

        if (S_ISREG(child->stat.st_mode))
                size += offsetof(CaFormatPayload, data);
        else if (S_ISLNK(child->stat.st_mode))
                size += offsetof(CaFormatSymlink, target) +
                        strlen(child->symlink_target) + 1;
        else if (S_ISBLK(child->stat.st_mode) || S_ISCHR(child->stat.st_mode))
                size += sizeof(CaFormatDevice);

        entry = realloc_buffer_acquire0(&e->buffer, size);
        if (!entry)
                return -ENOMEM;

        entry->header = (CaFormatHeader) {
                .type = htole64(CA_FORMAT_ENTRY),
                .size = htole64(offsetof(CaFormatEntry, name) + strlen(de->d_name) + 1),
        };
        entry->mode = htole64(mode);
        entry->uid = htole64(uid);
        entry->gid = htole64(gid);
        entry->mtime = htole64(mtime);

        p = stpcpy(entry->name, de->d_name) + 1;

        /* Note that any follow-up structures from here are unaligned in memory! */

        if (S_ISREG(child->stat.st_mode)) {
                memcpy(p,
                       &(CaFormatHeader) {
                               .type = htole64(CA_FORMAT_PAYLOAD),
                               .size = htole64(offsetof(CaFormatPayload, data) + child->stat.st_size),
                       },
                       sizeof(CaFormatHeader));

        } else if (S_ISLNK(child->stat.st_mode)) {
                memcpy(p,
                       &(CaFormatHeader) {
                               .type = htole64(CA_FORMAT_SYMLINK),
                               .size = htole64(offsetof(CaFormatSymlink, target) + strlen(child->symlink_target) + 1),
                       },
                       sizeof(CaFormatHeader));

                strcpy(p + offsetof(CaFormatSymlink, target), child->symlink_target);

        } else if (S_ISBLK(child->stat.st_mode) || S_ISCHR(child->stat.st_mode)) {
                memcpy(p,
                       &(CaFormatDevice) {
                               .header.type = htole64(CA_FORMAT_DEVICE),
                               .header.size = htole64(sizeof(CaFormatDevice)),
                               .major = htole64(major(child->stat.st_rdev)),
                               .minor = htole64(minor(child->stat.st_rdev)),
                       },
                       sizeof(CaFormatDevice));
        }

        /* fprintf(stderr, "entry at %" PRIu64 " (%s)\n", e->archive_offset, entry->name); */

        return 1;
}

static int ca_encoder_get_goodbye_data(CaEncoder *e, CaEncoderNode *n) {
        CaFormatGoodbye *g;

        assert(e);
        assert(n);
        assert(S_ISDIR(n->stat.st_mode));
        assert(e->state == CA_ENCODER_GOODBYE);

        if (e->buffer.size > 0) /* Already generated */
                return 1;

        g = realloc_buffer_acquire0(&e->buffer,
                                   offsetof(CaFormatGoodbye, table) +
                                   sizeof(le64_t));
        if (!g)
                return -ENOMEM;

        g->header = (CaFormatHeader) {
                .type = htole64(CA_FORMAT_GOODBYE),
                .size = htole64(offsetof(CaFormatGoodbye, table) + sizeof(le64_t)),
        };

        memcpy(g->table, &g->header.size, sizeof(le64_t));
        return 1;
}

int ca_encoder_get_data(CaEncoder *e, const void **ret, size_t *ret_size) {
        CaEncoderNode *n;
        int r;

        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;
        if (!ret_size)
                return -EINVAL;

        n = ca_encoder_current_node(e);
        if (!n)
                return -EUNATCH;

        if (S_ISREG(n->stat.st_mode) || S_ISBLK(n->stat.st_mode)) {

                if (e->state != CA_ENCODER_INIT)
                        return -ENOTTY;

                r = ca_encoder_get_payload_data(e, n);

        } else if (S_ISDIR(n->stat.st_mode)) {

                switch (e->state) {

                case CA_ENCODER_HELLO:
                        r = ca_encoder_get_hello_data(e, n);
                        break;

                case CA_ENCODER_ENTRY:
                        r = ca_encoder_get_entry_data(e, n);
                        break;

                case CA_ENCODER_GOODBYE:
                        r = ca_encoder_get_goodbye_data(e, n);
                        break;

                default:
                        return -ENOTTY;
                }
        } else
                return -ENOTTY;
        if (r < 0)
                return r;
        if (r == 0) {
                *ret = NULL;
                *ret_size = 0;

                e->step_size = 0;
                return 0;
        }

        *ret = e->buffer.data;
        *ret_size = e->buffer.size;
        e->step_size = e->buffer.size;

        return 1;
}

int ca_encoder_current_path(CaEncoder *e, char **ret) {
        char *p = NULL;
        size_t n = 0, i;

        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        if (e->n_nodes <= 0)
                return -EUNATCH;

        for (i = 0; i < e->n_nodes; i++) {
                CaEncoderNode *node;
                const struct dirent *de;
                char *np, *q;
                size_t k, nn;

                node = e->nodes + i;

                de = ca_encoder_node_current_dirent(node);
                if (!de)
                        break;

                k = strlen(de->d_name);
                nn = n + (n > 0) + k;

                np = realloc(p, nn+1);
                if (!np) {
                        free(p);
                        return -ENOMEM;
                }

                q = np + n;
                if (n > 0)
                        *(q++) = '/';

                strcpy(q, de->d_name);

                p = np;
                n = nn;
        }

        if (!p)
                return -ENOTDIR;

        *ret = p;
        return 0;
}

int ca_encoder_current_mode(CaEncoder *e, mode_t *ret) {
        CaEncoderNode *n;

        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        n = ca_encoder_current_child_node(e);
        if (!n) {
                n = ca_encoder_current_node(e);
                if (!n)
                        return -EUNATCH;
        }

        *ret = n->stat.st_mode;
        return 0;
}

int ca_encoder_current_payload_offset(CaEncoder *e, uint64_t *ret) {
        CaEncoderNode *n;

        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        n = ca_encoder_current_node(e);
        if (!n)
                return -EUNATCH;

        if (!S_ISREG(n->stat.st_mode) && !S_ISBLK(n->stat.st_mode))
                return -EISDIR;

        *ret = e->payload_offset;
        return 0;
}

int ca_encoder_current_archive_offset(CaEncoder *e, uint64_t *ret) {
        if (!e)
                return -EINVAL;
        if (!ret)
                return -EINVAL;

        *ret = e->archive_offset;
        return 0;
}
