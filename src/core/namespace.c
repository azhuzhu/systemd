/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#include "alloc-util.h"
#include "dev-setup.h"
#include "fd-util.h"
#include "fs-util.h"
#include "loopback-setup.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "namespace.h"
#include "path-util.h"
#include "selinux-util.h"
#include "socket-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "umask-util.h"
#include "user-util.h"
#include "util.h"

#define DEV_MOUNT_OPTIONS (MS_NOSUID|MS_STRICTATIME|MS_NOEXEC)

typedef enum MountMode {
        /* This is ordered by priority! */
        INACCESSIBLE,
        READONLY,
        PRIVATE_TMP,
        PRIVATE_VAR_TMP,
        PRIVATE_DEV,
        READWRITE,
} MountMode;

typedef struct BindMount {
        const char *path_const;   /* Memory allocated on stack or static */
        MountMode mode:6;
        bool ignore:1;            /* Ignore if path does not exist? */
        bool has_prefix:1;        /* Already is prefixed by the root dir? */
        char *path_malloc;        /* Use this instead of 'path' if we had to allocate memory */
} BindMount;

/*
 * The following Protect tables are to protect paths and mark some of them
 * READONLY, in case a path is covered by an option from another table, then
 * it is marked READWRITE in the current one, and the more restrictive mode is
 * applied from that other table. This way all options can be combined in a
 * safe and comprehensible way for users.
 */

/* ProtectKernelTunables= option and the related filesystem APIs */
static const BindMount protect_kernel_tunables_table[] = {
        { "/proc/sys",           READONLY,     false },
        { "/proc/sysrq-trigger", READONLY,     true  },
        { "/proc/latency_stats", READONLY,     true  },
        { "/proc/mtrr",          READONLY,     true  },
        { "/proc/apm",           READONLY,     true  }, /* Obsolete API, there's no point in permitting access to this, ever */
        { "/proc/acpi",          READONLY,     true  },
        { "/proc/timer_stats",   READONLY,     true  },
        { "/proc/asound",        READONLY,     true  },
        { "/proc/bus",           READONLY,     true  },
        { "/proc/fs",            READONLY,     true  },
        { "/proc/irq",           READONLY,     true  },
        { "/sys",                READONLY,     false },
        { "/sys/kernel/debug",   READONLY,     true  },
        { "/sys/kernel/tracing", READONLY,     true  },
        { "/sys/fs/cgroup",      READWRITE,    false }, /* READONLY is set by ProtectControlGroups= option */
};

/* ProtectKernelModules= option */
static const BindMount protect_kernel_modules_table[] = {
#ifdef HAVE_SPLIT_USR
        { "/lib/modules",        INACCESSIBLE, true  },
#endif
        { "/usr/lib/modules",    INACCESSIBLE, true  },
};

/*
 * ProtectHome=read-only table, protect $HOME and $XDG_RUNTIME_DIR and rest of
 * system should be protected by ProtectSystem=
 */
static const BindMount protect_home_read_only_table[] = {
        { "/home",               READONLY,     true  },
        { "/run/user",           READONLY,     true  },
        { "/root",               READONLY,     true  },
};

/* ProtectHome=yes table */
static const BindMount protect_home_yes_table[] = {
        { "/home",               INACCESSIBLE, true  },
        { "/run/user",           INACCESSIBLE, true  },
        { "/root",               INACCESSIBLE, true  },
};

/* ProtectSystem=yes table */
static const BindMount protect_system_yes_table[] = {
        { "/usr",                READONLY,     false },
        { "/boot",               READONLY,     true  },
        { "/efi",                READONLY,     true  },
};

/* ProtectSystem=full includes ProtectSystem=yes */
static const BindMount protect_system_full_table[] = {
        { "/usr",                READONLY,     false },
        { "/boot",               READONLY,     true  },
        { "/efi",                READONLY,     true  },
        { "/etc",                READONLY,     false },
};

/*
 * ProtectSystem=strict table. In this strict mode, we mount everything
 * read-only, except for /proc, /dev, /sys which are the kernel API VFS,
 * which are left writable, but PrivateDevices= + ProtectKernelTunables=
 * protect those, and these options should be fully orthogonal.
 * (And of course /home and friends are also left writable, as ProtectHome=
 * shall manage those, orthogonally).
 */
static const BindMount protect_system_strict_table[] = {
        { "/",          READONLY,       false },
        { "/proc",      READWRITE,      false },      /* ProtectKernelTunables= */
        { "/sys",       READWRITE,      false },      /* ProtectKernelTunables= */
        { "/dev",       READWRITE,      false },      /* PrivateDevices= */
        { "/home",      READWRITE,      true  },      /* ProtectHome= */
        { "/run/user",  READWRITE,      true  },      /* ProtectHome= */
        { "/root",      READWRITE,      true  },      /* ProtectHome= */
};

static const char *bind_mount_path(const BindMount *p) {
        assert(p);

        /* Returns the path of this bind mount. If the malloc()-allocated ->path_buffer field is set we return that,
         * otherwise the stack/static ->path field is returned. */

        return p->path_malloc ?: p->path_const;
}

static int append_access_mounts(BindMount **p, char **strv, MountMode mode) {
        char **i;

        assert(p);

        /* Adds a list of user-supplied READWRITE/READONLY/INACCESSIBLE entries */

        STRV_FOREACH(i, strv) {
                bool ignore = false, needs_prefix = false;
                const char *e = *i;

                /* Look for any prefixes */
                if (startswith(e, "-")) {
                        e++;
                        ignore = true;
                }
                if (startswith(e, "+")) {
                        e++;
                        needs_prefix = true;
                }

                if (!path_is_absolute(e))
                        return -EINVAL;

                *((*p)++) = (BindMount) {
                        .path_const = e,
                        .mode = mode,
                        .ignore = ignore,
                        .has_prefix = !needs_prefix,
                };
        }

        return 0;
}

static int append_static_mounts(BindMount **p, const BindMount *mounts, unsigned n, bool ignore_protect) {
        unsigned i;

        assert(p);
        assert(mounts);

        /* Adds a list of static pre-defined entries */

        for (i = 0; i < n; i++)
                *((*p)++) = (BindMount) {
                        .path_const = bind_mount_path(mounts+i),
                        .mode = mounts[i].mode,
                        .ignore = mounts[i].ignore || ignore_protect,
                };

        return 0;
}

static int append_protect_home(BindMount **p, ProtectHome protect_home, bool ignore_protect) {
        assert(p);

        switch (protect_home) {

        case PROTECT_HOME_NO:
                return 0;

        case PROTECT_HOME_READ_ONLY:
                return append_static_mounts(p, protect_home_read_only_table, ELEMENTSOF(protect_home_read_only_table), ignore_protect);

        case PROTECT_HOME_YES:
                return append_static_mounts(p, protect_home_yes_table, ELEMENTSOF(protect_home_yes_table), ignore_protect);

        default:
                assert_not_reached("Unexpected ProtectHome= value");
        }
}

static int append_protect_system(BindMount **p, ProtectSystem protect_system, bool ignore_protect) {
        assert(p);

        switch (protect_system) {

        case PROTECT_SYSTEM_NO:
                return 0;

        case PROTECT_SYSTEM_STRICT:
                return append_static_mounts(p, protect_system_strict_table, ELEMENTSOF(protect_system_strict_table), ignore_protect);

        case PROTECT_SYSTEM_YES:
                return append_static_mounts(p, protect_system_yes_table, ELEMENTSOF(protect_system_yes_table), ignore_protect);

        case PROTECT_SYSTEM_FULL:
                return append_static_mounts(p, protect_system_full_table, ELEMENTSOF(protect_system_full_table), ignore_protect);

        default:
                assert_not_reached("Unexpected ProtectSystem= value");
        }
}

static int mount_path_compare(const void *a, const void *b) {
        const BindMount *p = a, *q = b;
        int d;

        /* If the paths are not equal, then order prefixes first */
        d = path_compare(bind_mount_path(p), bind_mount_path(q));
        if (d != 0)
                return d;

        /* If the paths are equal, check the mode */
        if (p->mode < q->mode)
                return -1;

        if (p->mode > q->mode)
                return 1;

        return 0;
}

static int prefix_where_needed(BindMount *m, unsigned n, const char *root_directory) {
        unsigned i;

        /* Prefixes all paths in the bind mount table with the root directory if it is specified and the entry needs
         * that. */

        if (!root_directory)
                return 0;

        for (i = 0; i < n; i++) {
                char *s;

                if (m[i].has_prefix)
                        continue;

                s = prefix_root(root_directory, bind_mount_path(m+i));
                if (!s)
                        return -ENOMEM;

                free(m[i].path_malloc);
                m[i].path_malloc = s;

                m[i].has_prefix = true;
        }

        return 0;
}

static void drop_duplicates(BindMount *m, unsigned *n) {
        BindMount *f, *t, *previous;

        assert(m);
        assert(n);

        /* Drops duplicate entries. Expects that the array is properly ordered already. */

        for (f = m, t = m, previous = NULL; f < m + *n; f++) {

                /* The first one wins (which is the one with the more restrictive mode), see mount_path_compare()
                 * above. */
                if (previous && path_equal(bind_mount_path(f), bind_mount_path(previous))) {
                        log_debug("%s is duplicate.", bind_mount_path(f));
                        f->path_malloc = mfree(f->path_malloc);
                        continue;
                }

                *t = *f;
                previous = t;
                t++;
        }

        *n = t - m;
}

static void drop_inaccessible(BindMount *m, unsigned *n) {
        BindMount *f, *t;
        const char *clear = NULL;

        assert(m);
        assert(n);

        /* Drops all entries obstructed by another entry further up the tree. Expects that the array is properly
         * ordered already. */

        for (f = m, t = m; f < m + *n; f++) {

                /* If we found a path set for INACCESSIBLE earlier, and this entry has it as prefix we should drop
                 * it, as inaccessible paths really should drop the entire subtree. */
                if (clear && path_startswith(bind_mount_path(f), clear)) {
                        log_debug("%s is masked by %s.", bind_mount_path(f), clear);
                        f->path_malloc = mfree(f->path_malloc);
                        continue;
                }

                clear = f->mode == INACCESSIBLE ? bind_mount_path(f) : NULL;

                *t = *f;
                t++;
        }

        *n = t - m;
}

static void drop_nop(BindMount *m, unsigned *n) {
        BindMount *f, *t;

        assert(m);
        assert(n);

        /* Drops all entries which have an immediate parent that has the same type, as they are redundant. Assumes the
         * list is ordered by prefixes. */

        for (f = m, t = m; f < m + *n; f++) {

                /* Only suppress such subtrees for READONLY and READWRITE entries */
                if (IN_SET(f->mode, READONLY, READWRITE)) {
                        BindMount *p;
                        bool found = false;

                        /* Now let's find the first parent of the entry we are looking at. */
                        for (p = t-1; p >= m; p--) {
                                if (path_startswith(bind_mount_path(f), bind_mount_path(p))) {
                                        found = true;
                                        break;
                                }
                        }

                        /* We found it, let's see if it's the same mode, if so, we can drop this entry */
                        if (found && p->mode == f->mode) {
                                log_debug("%s is redundant by %s", bind_mount_path(f), bind_mount_path(p));
                                f->path_malloc = mfree(f->path_malloc);
                                continue;
                        }
                }

                *t = *f;
                t++;
        }

        *n = t - m;
}

static void drop_outside_root(const char *root_directory, BindMount *m, unsigned *n) {
        BindMount *f, *t;

        assert(m);
        assert(n);

        /* Nothing to do */
        if (!root_directory)
                return;

        /* Drops all mounts that are outside of the root directory. */

        for (f = m, t = m; f < m + *n; f++) {

                if (!path_startswith(bind_mount_path(f), root_directory)) {
                        log_debug("%s is outside of root directory.", bind_mount_path(f));
                        f->path_malloc = mfree(f->path_malloc);
                        continue;
                }

                *t = *f;
                t++;
        }

        *n = t - m;
}

static int mount_dev(BindMount *m) {
        static const char devnodes[] =
                "/dev/null\0"
                "/dev/zero\0"
                "/dev/full\0"
                "/dev/random\0"
                "/dev/urandom\0"
                "/dev/tty\0";

        char temporary_mount[] = "/tmp/namespace-dev-XXXXXX";
        const char *d, *dev = NULL, *devpts = NULL, *devshm = NULL, *devhugepages = NULL, *devmqueue = NULL, *devlog = NULL, *devptmx = NULL;
        _cleanup_umask_ mode_t u;
        int r;

        assert(m);

        u = umask(0000);

        if (!mkdtemp(temporary_mount))
                return -errno;

        dev = strjoina(temporary_mount, "/dev");
        (void) mkdir(dev, 0755);
        if (mount("tmpfs", dev, "tmpfs", DEV_MOUNT_OPTIONS, "mode=755") < 0) {
                r = -errno;
                goto fail;
        }

        devpts = strjoina(temporary_mount, "/dev/pts");
        (void) mkdir(devpts, 0755);
        if (mount("/dev/pts", devpts, NULL, MS_BIND, NULL) < 0) {
                r = -errno;
                goto fail;
        }

        devptmx = strjoina(temporary_mount, "/dev/ptmx");
        if (symlink("pts/ptmx", devptmx) < 0) {
                r = -errno;
                goto fail;
        }

        devshm = strjoina(temporary_mount, "/dev/shm");
        (void) mkdir(devshm, 01777);
        r = mount("/dev/shm", devshm, NULL, MS_BIND, NULL);
        if (r < 0) {
                r = -errno;
                goto fail;
        }

        devmqueue = strjoina(temporary_mount, "/dev/mqueue");
        (void) mkdir(devmqueue, 0755);
        (void) mount("/dev/mqueue", devmqueue, NULL, MS_BIND, NULL);

        devhugepages = strjoina(temporary_mount, "/dev/hugepages");
        (void) mkdir(devhugepages, 0755);
        (void) mount("/dev/hugepages", devhugepages, NULL, MS_BIND, NULL);

        devlog = strjoina(temporary_mount, "/dev/log");
        (void) symlink("/run/systemd/journal/dev-log", devlog);

        NULSTR_FOREACH(d, devnodes) {
                _cleanup_free_ char *dn = NULL;
                struct stat st;

                r = stat(d, &st);
                if (r < 0) {

                        if (errno == ENOENT)
                                continue;

                        r = -errno;
                        goto fail;
                }

                if (!S_ISBLK(st.st_mode) &&
                    !S_ISCHR(st.st_mode)) {
                        r = -EINVAL;
                        goto fail;
                }

                if (st.st_rdev == 0)
                        continue;

                dn = strappend(temporary_mount, d);
                if (!dn) {
                        r = -ENOMEM;
                        goto fail;
                }

                mac_selinux_create_file_prepare(d, st.st_mode);
                r = mknod(dn, st.st_mode, st.st_rdev);
                mac_selinux_create_file_clear();

                if (r < 0) {
                        r = -errno;
                        goto fail;
                }
        }

        dev_setup(temporary_mount, UID_INVALID, GID_INVALID);

        /* Create the /dev directory if missing. It is more likely to be
         * missing when the service is started with RootDirectory. This is
         * consistent with mount units creating the mount points when missing.
         */
        (void) mkdir_p_label(bind_mount_path(m), 0755);

        /* Unmount everything in old /dev */
        umount_recursive(bind_mount_path(m), 0);
        if (mount(dev, bind_mount_path(m), NULL, MS_MOVE, NULL) < 0) {
                r = -errno;
                goto fail;
        }

        rmdir(dev);
        rmdir(temporary_mount);

        return 0;

fail:
        if (devpts)
                umount(devpts);

        if (devshm)
                umount(devshm);

        if (devhugepages)
                umount(devhugepages);

        if (devmqueue)
                umount(devmqueue);

        umount(dev);
        rmdir(dev);
        rmdir(temporary_mount);

        return r;
}

static int apply_mount(
                BindMount *m,
                const char *tmp_dir,
                const char *var_tmp_dir) {

        const char *what;
        int r;

        assert(m);

        log_debug("Applying namespace mount on %s", bind_mount_path(m));

        switch (m->mode) {

        case INACCESSIBLE: {
                struct stat target;

                /* First, get rid of everything that is below if there
                 * is anything... Then, overmount it with an
                 * inaccessible path. */
                (void) umount_recursive(bind_mount_path(m), 0);

                if (lstat(bind_mount_path(m), &target) < 0)
                        return log_debug_errno(errno, "Failed to lstat() %s to determine what to mount over it: %m", bind_mount_path(m));

                what = mode_to_inaccessible_node(target.st_mode);
                if (!what) {
                        log_debug("File type not supported for inaccessible mounts. Note that symlinks are not allowed");
                        return -ELOOP;
                }
                break;
        }

        case READONLY:
        case READWRITE:

                r = path_is_mount_point(bind_mount_path(m), NULL, 0);
                if (r < 0)
                        return log_debug_errno(r, "Failed to determine whether %s is already a mount point: %m", bind_mount_path(m));
                if (r > 0) /* Nothing to do here, it is already a mount. We just later toggle the MS_RDONLY bit for the mount point if needed. */
                        return 0;
                /* This isn't a mount point yet, let's make it one. */
                what = bind_mount_path(m);
                break;

        case PRIVATE_TMP:
                what = tmp_dir;
                break;

        case PRIVATE_VAR_TMP:
                what = var_tmp_dir;
                break;

        case PRIVATE_DEV:
                return mount_dev(m);

        default:
                assert_not_reached("Unknown mode");
        }

        assert(what);

        if (mount(what, bind_mount_path(m), NULL, MS_BIND|MS_REC, NULL) < 0)
                return log_debug_errno(errno, "Failed to mount %s to %s: %m", what, bind_mount_path(m));

        log_debug("Successfully mounted %s to %s", what, bind_mount_path(m));
        return 0;
}

static int make_read_only(BindMount *m, char **blacklist) {
        int r = 0;

        assert(m);

        if (IN_SET(m->mode, INACCESSIBLE, READONLY))
                r = bind_remount_recursive(bind_mount_path(m), true, blacklist);
        else if (m->mode == PRIVATE_DEV) { /* Can be readonly but the submounts can't*/
                if (mount(NULL, bind_mount_path(m), NULL, MS_REMOUNT|DEV_MOUNT_OPTIONS|MS_RDONLY, NULL) < 0)
                        r = -errno;
        } else
                return 0;

        /* Not that we only turn on the MS_RDONLY flag here, we never turn it off. Something that was marked read-only
         * already stays this way. This improves compatibility with container managers, where we won't attempt to undo
         * read-only mounts already applied. */

        return r;
}

/* Chase symlinks and remove failed paths from mounts */
static int chase_all_symlinks(const char *root_directory, BindMount *m, unsigned *n) {
        BindMount *f, *t;
        int r = 0;

        assert(m);
        assert(n);

        /* Since mount() will always follow symlinks and we need to take the different root directory into account we
         * chase the symlinks on our own first. This call wil do so for all entries and remove all entries where we
         * can't resolve the path, and which have been marked for such removal. */

        for (f = m, t = m; f < m + *n; f++) {
                _cleanup_free_ char *chased = NULL;
                int k;

                k = chase_symlinks(bind_mount_path(f), root_directory, &chased);
                if (k < 0) {
                        /* Get only real errors */
                        if (r >= 0 && (k != -ENOENT || !f->ignore))
                                r = k;

                        /* Doesn't exist or failed? Then remove it and continue! */
                        log_debug_errno(k, "Failed to chase symlinks for %s: %m", bind_mount_path(f));
                        f->path_malloc = mfree(f->path_malloc);
                        continue;
                }

                if (!path_equal(bind_mount_path(f), chased)) {
                        log_debug("Chased %s → %s", bind_mount_path(f), chased);

                        free(f->path_malloc);
                        f->path_malloc = chased;
                        chased = NULL;
                }

                *t = *f;
                t++;
        }

        *n = t - m;
        return r;
}

static unsigned namespace_calculate_mounts(
                const NameSpaceInfo *ns_info,
                char** read_write_paths,
                char** read_only_paths,
                char** inaccessible_paths,
                const char* tmp_dir,
                const char* var_tmp_dir,
                ProtectHome protect_home,
                ProtectSystem protect_system) {

        unsigned protect_home_cnt;
        unsigned protect_system_cnt =
                (protect_system == PROTECT_SYSTEM_STRICT ?
                 ELEMENTSOF(protect_system_strict_table) :
                 ((protect_system == PROTECT_SYSTEM_FULL) ?
                  ELEMENTSOF(protect_system_full_table) :
                  ((protect_system == PROTECT_SYSTEM_YES) ?
                   ELEMENTSOF(protect_system_yes_table) : 0)));

        protect_home_cnt =
                (protect_home == PROTECT_HOME_YES ?
                 ELEMENTSOF(protect_home_yes_table) :
                 ((protect_home == PROTECT_HOME_READ_ONLY) ?
                  ELEMENTSOF(protect_home_read_only_table) : 0));

        return !!tmp_dir + !!var_tmp_dir +
                strv_length(read_write_paths) +
                strv_length(read_only_paths) +
                strv_length(inaccessible_paths) +
                ns_info->private_dev +
                (ns_info->protect_kernel_tunables ? ELEMENTSOF(protect_kernel_tunables_table) : 0) +
                (ns_info->protect_control_groups ? 1 : 0) +
                (ns_info->protect_kernel_modules ? ELEMENTSOF(protect_kernel_modules_table) : 0) +
                protect_home_cnt + protect_system_cnt;
}

int setup_namespace(
                const char* root_directory,
                const NameSpaceInfo *ns_info,
                char** read_write_paths,
                char** read_only_paths,
                char** inaccessible_paths,
                const char* tmp_dir,
                const char* var_tmp_dir,
                ProtectHome protect_home,
                ProtectSystem protect_system,
                unsigned long mount_flags) {

        BindMount *m, *mounts = NULL;
        bool make_slave = false;
        unsigned n_mounts;
        int r = 0;

        if (mount_flags == 0)
                mount_flags = MS_SHARED;

        n_mounts = namespace_calculate_mounts(ns_info,
                                              read_write_paths,
                                              read_only_paths,
                                              inaccessible_paths,
                                              tmp_dir, var_tmp_dir,
                                              protect_home, protect_system);

        /* Set mount slave mode */
        if (root_directory || n_mounts > 0)
                make_slave = true;

        if (n_mounts > 0) {
                m = mounts = (BindMount *) alloca0(n_mounts * sizeof(BindMount));
                r = append_access_mounts(&m, read_write_paths, READWRITE);
                if (r < 0)
                        goto finish;

                r = append_access_mounts(&m, read_only_paths, READONLY);
                if (r < 0)
                        goto finish;

                r = append_access_mounts(&m, inaccessible_paths, INACCESSIBLE);
                if (r < 0)
                        goto finish;

                if (tmp_dir) {
                        *(m++) = (BindMount) {
                                .path_const = "/tmp",
                                .mode = PRIVATE_TMP,
                        };
                }

                if (var_tmp_dir) {
                        *(m++) = (BindMount) {
                                .path_const = "/var/tmp",
                                .mode = PRIVATE_VAR_TMP,
                        };
                }

                if (ns_info->private_dev) {
                        *(m++) = (BindMount) {
                                .path_const = "/dev",
                                .mode = PRIVATE_DEV,
                        };
                }

                if (ns_info->protect_kernel_tunables) {
                        r = append_static_mounts(&m, protect_kernel_tunables_table, ELEMENTSOF(protect_kernel_tunables_table), ns_info->ignore_protect_paths);
                        if (r < 0)
                                goto finish;
                }

                if (ns_info->protect_kernel_modules) {
                        r = append_static_mounts(&m, protect_kernel_modules_table, ELEMENTSOF(protect_kernel_modules_table), ns_info->ignore_protect_paths);
                        if (r < 0)
                                goto finish;
                }

                if (ns_info->protect_control_groups) {
                        *(m++) = (BindMount) {
                                .path_const = "/sys/fs/cgroup",
                                .mode = READONLY,
                        };
                }

                r = append_protect_home(&m, protect_home, ns_info->ignore_protect_paths);
                if (r < 0)
                        goto finish;

                r = append_protect_system(&m, protect_system, false);
                if (r < 0)
                        goto finish;

                assert(mounts + n_mounts == m);

                /* Prepend the root directory where that's necessary */
                r = prefix_where_needed(mounts, n_mounts, root_directory);
                if (r < 0)
                        goto finish;

                /* Resolve symlinks manually first, as mount() will always follow them relative to the host's
                 * root. Moreover we want to suppress duplicates based on the resolved paths. This of course is a bit
                 * racy. */
                r = chase_all_symlinks(root_directory, mounts, &n_mounts);
                if (r < 0)
                        goto finish;

                qsort(mounts, n_mounts, sizeof(BindMount), mount_path_compare);

                drop_duplicates(mounts, &n_mounts);
                drop_outside_root(root_directory, mounts, &n_mounts);
                drop_inaccessible(mounts, &n_mounts);
                drop_nop(mounts, &n_mounts);
        }

        if (unshare(CLONE_NEWNS) < 0) {
                r = -errno;
                goto finish;
        }

        if (make_slave) {
                /* Remount / as SLAVE so that nothing now mounted in the namespace
                   shows up in the parent */
                if (mount(NULL, "/", NULL, MS_SLAVE|MS_REC, NULL) < 0) {
                        r = -errno;
                        goto finish;
                }
        }

        if (root_directory) {
                /* Turn directory into bind mount, if it isn't one yet */
                r = path_is_mount_point(root_directory, NULL, AT_SYMLINK_FOLLOW);
                if (r < 0)
                        goto finish;
                if (r == 0) {
                        if (mount(root_directory, root_directory, NULL, MS_BIND|MS_REC, NULL) < 0) {
                                r = -errno;
                                goto finish;
                        }
                }
        }

        if (n_mounts > 0) {
                char **blacklist;
                unsigned j;

                /* First round, add in all special mounts we need */
                for (m = mounts; m < mounts + n_mounts; ++m) {
                        r = apply_mount(m, tmp_dir, var_tmp_dir);
                        if (r < 0)
                                goto finish;
                }

                /* Create a blacklist we can pass to bind_mount_recursive() */
                blacklist = newa(char*, n_mounts+1);
                for (j = 0; j < n_mounts; j++)
                        blacklist[j] = (char*) bind_mount_path(mounts+j);
                blacklist[j] = NULL;

                /* Second round, flip the ro bits if necessary. */
                for (m = mounts; m < mounts + n_mounts; ++m) {
                        r = make_read_only(m, blacklist);
                        if (r < 0)
                                goto finish;
                }
        }

        if (root_directory) {
                /* MS_MOVE does not work on MS_SHARED so the remount MS_SHARED will be done later */
                r = mount_move_root(root_directory);
                if (r < 0)
                        goto finish;
        }

        /* Remount / as the desired mode. Not that this will not
         * reestablish propagation from our side to the host, since
         * what's disconnected is disconnected. */
        if (mount(NULL, "/", NULL, mount_flags | MS_REC, NULL) < 0) {
                r = -errno;
                goto finish;
        }

        r = 0;

finish:
        for (m = mounts; m < mounts + n_mounts; m++)
                free(m->path_malloc);

        return r;
}

static int setup_one_tmp_dir(const char *id, const char *prefix, char **path) {
        _cleanup_free_ char *x = NULL;
        char bid[SD_ID128_STRING_MAX];
        sd_id128_t boot_id;
        int r;

        assert(id);
        assert(prefix);
        assert(path);

        /* We include the boot id in the directory so that after a
         * reboot we can easily identify obsolete directories. */

        r = sd_id128_get_boot(&boot_id);
        if (r < 0)
                return r;

        x = strjoin(prefix, "/systemd-private-", sd_id128_to_string(boot_id, bid), "-", id, "-XXXXXX");
        if (!x)
                return -ENOMEM;

        RUN_WITH_UMASK(0077)
                if (!mkdtemp(x))
                        return -errno;

        RUN_WITH_UMASK(0000) {
                char *y;

                y = strjoina(x, "/tmp");

                if (mkdir(y, 0777 | S_ISVTX) < 0)
                        return -errno;
        }

        *path = x;
        x = NULL;

        return 0;
}

int setup_tmp_dirs(const char *id, char **tmp_dir, char **var_tmp_dir) {
        char *a, *b;
        int r;

        assert(id);
        assert(tmp_dir);
        assert(var_tmp_dir);

        r = setup_one_tmp_dir(id, "/tmp", &a);
        if (r < 0)
                return r;

        r = setup_one_tmp_dir(id, "/var/tmp", &b);
        if (r < 0) {
                char *t;

                t = strjoina(a, "/tmp");
                rmdir(t);
                rmdir(a);

                free(a);
                return r;
        }

        *tmp_dir = a;
        *var_tmp_dir = b;

        return 0;
}

int setup_netns(int netns_storage_socket[2]) {
        _cleanup_close_ int netns = -1;
        int r, q;

        assert(netns_storage_socket);
        assert(netns_storage_socket[0] >= 0);
        assert(netns_storage_socket[1] >= 0);

        /* We use the passed socketpair as a storage buffer for our
         * namespace reference fd. Whatever process runs this first
         * shall create a new namespace, all others should just join
         * it. To serialize that we use a file lock on the socket
         * pair.
         *
         * It's a bit crazy, but hey, works great! */

        if (lockf(netns_storage_socket[0], F_LOCK, 0) < 0)
                return -errno;

        netns = receive_one_fd(netns_storage_socket[0], MSG_DONTWAIT);
        if (netns == -EAGAIN) {
                /* Nothing stored yet, so let's create a new namespace */

                if (unshare(CLONE_NEWNET) < 0) {
                        r = -errno;
                        goto fail;
                }

                loopback_setup();

                netns = open("/proc/self/ns/net", O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (netns < 0) {
                        r = -errno;
                        goto fail;
                }

                r = 1;

        } else if (netns < 0) {
                r = netns;
                goto fail;

        } else {
                /* Yay, found something, so let's join the namespace */
                if (setns(netns, CLONE_NEWNET) < 0) {
                        r = -errno;
                        goto fail;
                }

                r = 0;
        }

        q = send_one_fd(netns_storage_socket[1], netns, MSG_DONTWAIT);
        if (q < 0) {
                r = q;
                goto fail;
        }

fail:
        (void) lockf(netns_storage_socket[0], F_ULOCK, 0);
        return r;
}

static const char *const protect_home_table[_PROTECT_HOME_MAX] = {
        [PROTECT_HOME_NO] = "no",
        [PROTECT_HOME_YES] = "yes",
        [PROTECT_HOME_READ_ONLY] = "read-only",
};

DEFINE_STRING_TABLE_LOOKUP(protect_home, ProtectHome);

static const char *const protect_system_table[_PROTECT_SYSTEM_MAX] = {
        [PROTECT_SYSTEM_NO] = "no",
        [PROTECT_SYSTEM_YES] = "yes",
        [PROTECT_SYSTEM_FULL] = "full",
        [PROTECT_SYSTEM_STRICT] = "strict",
};

DEFINE_STRING_TABLE_LOOKUP(protect_system, ProtectSystem);
