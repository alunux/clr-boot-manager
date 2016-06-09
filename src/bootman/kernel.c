/*
 * This file is part of clr-boot-manager.
 *
 * Copyright © 2016 Intel Corporation
 *
 * clr-boot-manager is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bootman.h"
#include "bootman_private.h"
#include "files.h"
#include "nica/files.h"

#include "config.h"

/**
 * Determine the applicable kboot file
 */
static inline char *boot_manager_get_kboot_file(BootManager *self, Kernel *k)
{
        char *p = NULL;
        /* /var/lib/kernel/k_booted_4.4.0-120.lts - new */
        if (asprintf(&p,
                     "%s/var/lib/kernel/k_booted_%s-%d.%s",
                     self->prefix,
                     k->version,
                     k->release,
                     k->ktype) < 0) {
                return NULL;
        }
        return p;
}

Kernel *boot_manager_inspect_kernel(BootManager *self, char *path)
{
        Kernel *kern = NULL;
        autofree(char) *cmp = NULL;
        char type[15] = { 0 };
        char version[15] = { 0 };
        int release = 0;
        autofree(char) *parent = NULL;
        autofree(char) *cmdline = NULL;
        autofree(char) *module_dir = NULL;
        autofree(char) *kconfig_file = NULL;
        autofree(char) *default_file = NULL;
        /* Consider making this a namespace option */
        autofree(FILE) *f = NULL;
        size_t sn;
        int r = 0;
        char *buf = NULL;
        char *bcp = NULL;

        if (!self || !path) {
                return NULL;
        }

        cmp = strdup(path);
        if (!cmp) {
                return NULL;
        }
        bcp = basename(cmp);

        /* org.clearlinux.kvm.4.2.1-121 */
        r = sscanf(bcp, KERNEL_NAMESPACE ".%15[^.].%15[^-]-%d", type, version, &release);
        if (r != 3) {
                return NULL;
        }

        parent = cbm_get_file_parent(path);
        if (!asprintf(&cmdline, "%s/cmdline-%s-%d.%s", parent, version, release, type)) {
                DECLARE_OOM();
                abort();
        }

        if (!asprintf(&kconfig_file, "%s/config-%s-%d.%s", parent, version, release, type)) {
                DECLARE_OOM();
                abort();
        }

        /* TODO: We may actually be uninstalling a partially flopped kernel,
         * so validity of existing kernels may be questionable
         * Thus, flag it, and return kernel */
        if (!nc_file_exists(cmdline)) {
                LOG("Valid kernel found with no cmdline: %s (expected %s)\n", path, cmdline);
                return NULL;
        }

        /* Check local modules */
        if (!asprintf(&module_dir,
                      "%s/%s/%s-%d.%s",
                      self->prefix,
                      KERNEL_MODULES_DIRECTORY,
                      version,
                      release,
                      type)) {
                DECLARE_OOM();
                abort();
        }

        /* Fallback to an older namespace */
        if (!nc_file_exists(module_dir)) {
                free(module_dir);
                if (!asprintf(&module_dir,
                              "%s/%s/%s-%d",
                              self->prefix,
                              KERNEL_MODULES_DIRECTORY,
                              version,
                              release)) {
                        DECLARE_OOM();
                        abort();
                }
                if (!nc_file_exists(module_dir)) {
                        LOG("Valid kernel with no modules: %s %s\n", path, module_dir);
                        return NULL;
                }
        }

        /* Got this far, we have a valid clear kernel */
        kern = calloc(1, sizeof(struct Kernel));
        if (!kern) {
                abort();
        }

        kern->path = strdup(path);
        kern->bpath = strdup(bcp);
        kern->version = strdup(version);
        kern->module_dir = strdup(module_dir);
        kern->ktype = strdup(type);

        if (nc_file_exists(kconfig_file)) {
                kern->kconfig_file = strdup(kconfig_file);
        }

        kern->release = release;

        if (!(f = fopen(cmdline, "r"))) {
                LOG("Unable to open %s: %s\n", cmdline, strerror(errno));
                free_kernel(kern);
                return NULL;
        }

        /* Keep cmdline in a single "line" with no spaces */
        while ((r = getline(&buf, &sn, f)) > 0) {
                char *tmp = NULL;
                /* Strip newlines */
                if (r > 1 && buf[r - 1] == '\n') {
                        buf[r - 1] = '\0';
                }
                if (kern->cmdline) {
                        if (!asprintf(&tmp, "%s %s", kern->cmdline, buf)) {
                                DECLARE_OOM();
                                abort();
                        }
                        free(kern->cmdline);
                        kern->cmdline = tmp;
                        free(buf);
                } else {
                        kern->cmdline = buf;
                }
                buf = NULL;
        }

        kern->cmdline_file = strdup(cmdline);
        if (buf) {
                free(buf);
        }

        /** Determine if the kernel boots */
        kern->kboot_file = boot_manager_get_kboot_file(self, kern);
        if (kern->kboot_file && nc_file_exists(kern->kboot_file)) {
                kern->boots = true;
        }
        return kern;
}

KernelArray *boot_manager_get_kernels(BootManager *self)
{
        KernelArray *ret = NULL;
        DIR *dir = NULL;
        struct dirent *ent = NULL;
        struct stat st = { 0 };
        if (!self || !self->kernel_dir) {
                return NULL;
        }

        ret = nc_array_new();
        OOM_CHECK_RET(ret, NULL);

        dir = opendir(self->kernel_dir);
        if (!dir) {
                LOG("Error opening %s: %s\n", self->kernel_dir, strerror(errno));
                nc_array_free(&ret, NULL);
                return NULL;
        }

        while ((ent = readdir(dir)) != NULL) {
                autofree(char) *path = NULL;
                Kernel *kern = NULL;

                if (!asprintf(&path, "%s/%s", self->kernel_dir, ent->d_name)) {
                        DECLARE_OOM();
                        abort();
                }

                /* Some kind of broken link */
                if (lstat(path, &st) != 0) {
                        continue;
                }

                /* Regular only */
                if (!S_ISREG(st.st_mode)) {
                        continue;
                }

                /* empty files are skipped too */
                if (st.st_size == 0) {
                        continue;
                }

                /* Now see if its a kernel */
                kern = boot_manager_inspect_kernel(self, path);
                if (!kern) {
                        continue;
                }
                if (!nc_array_add(ret, kern)) {
                        DECLARE_OOM();
                        abort();
                }
        }
        closedir(dir);
        return ret;
}

void free_kernel(Kernel *t)
{
        if (!t) {
                return;
        }
        free(t->path);
        free(t->bpath);
        free(t->version);
        free(t->cmdline);
        free(t->module_dir);
        free(t->cmdline_file);
        free(t->kconfig_file);
        free(t->kboot_file);
        free(t->ktype);
        free(t);
}

Kernel *boot_manager_get_default_for_type(BootManager *self, KernelArray *kernels, const char *type)
{
        autofree(char) *default_file = NULL;
        char linkbuf[PATH_MAX] = { 0 };

        if (!self || !kernels || !type) {
                return NULL;
        }

        if (asprintf(&default_file, "%s/default-%s", self->kernel_dir, type) < 0) {
                return NULL;
        }

        if (readlink(default_file, linkbuf, sizeof(linkbuf)) < 0) {
                return NULL;
        }

        for (int i = 0; i < kernels->len; i++) {
                Kernel *k = nc_array_get(kernels, i);
                if (streq(k->bpath, linkbuf)) {
                        return k;
                }
        }

        return NULL;
}

static inline void kern_dup_free(void *v)
{
        NcArray *array = v;
        nc_array_free(&array, NULL);
}

NcHashmap *boot_manager_map_kernels(BootManager *self, KernelArray *kernels)
{
        if (!self || !kernels) {
                return NULL;
        }

        NcHashmap *map = NULL;

        map = nc_hashmap_new_full(nc_string_hash, nc_string_compare, free, kern_dup_free);

        for (int i = 0; i < kernels->len; i++) {
                KernelArray *r = NULL;
                Kernel *cur = nc_array_get(kernels, i);

                r = nc_hashmap_get(map, cur->ktype);
                if (!r) {
                        r = nc_array_new();
                        if (!r) {
                                goto oom;
                        }
                        if (!nc_hashmap_put(map, strdup(cur->ktype), r)) {
                                kern_dup_free(r);
                                goto oom;
                        }
                }
                if (!nc_array_add(r, cur)) {
                        goto oom;
                }
        }

        return map;
oom:
        DECLARE_OOM();
        nc_hashmap_free(map);
        return NULL;
}

bool cbm_parse_system_kernel(const char *inp, SystemKernel *kernel)
{
        if (!kernel || !inp) {
                return false;
        }

        /* Re-entrant, we might've mangled the kernel obj */
        memset(kernel, 0, sizeof(struct SystemKernel));

        char krelease[CBM_KELEM_LEN + 1] = { 0 };
        int release = 0;
        size_t len;
        char *c, *c2, *junk = NULL;

        c = strchr(inp, '-');
        if (!c) {
                return false;
        }
        if (c - inp >= CBM_KELEM_LEN) {
                return false;
        }
        if (c + 1 == '\0') {
                return false;
        }
        c2 = strchr(c + 1, '.');
        if (!c2) {
                return false;
        }
        /* Check length */
        if (c2 - c >= CBM_KELEM_LEN) {
                return false;
        }

        /* Copy version */
        len = c - inp;
        if (len < 1) {
                return false;
        }
        strncpy(kernel->version, inp, len);
        kernel->version[len + 1] = '\0';

        /* Copy release */
        len = c2 - c - 1;
        if (len < 1) {
                return false;
        }
        strncpy(krelease, c + 1, len);
        krelease[len + 1] = '\0';

        /* Sane release? */
        release = strtol(krelease, &junk, 10);
        if (junk && *junk != '\0') {
                return false;
        }
        kernel->release = release;

        /* Wind the type size **/
        len = 0;
        for (char *j = c2 + 1; *j; ++j) {
                ++len;
        }

        if (len < 1 || len >= CBM_KELEM_LEN) {
                return false;
        }

        /* Kernel type */
        strncpy(kernel->ktype, c2 + 1, len);
        kernel->ktype[len + 1] = '\0';

        return true;
}

const SystemKernel *boot_manager_get_system_kernel(BootManager *self)
{
        if (!self || !self->have_sys_kernel) {
                return NULL;
        }
        if (boot_manager_is_image_mode(self)) {
                return NULL;
        }
        return (const SystemKernel *)&(self->sys_kernel);
}

Kernel *boot_manager_get_running_kernel(BootManager *self, KernelArray *kernels)
{
        if (!self || !kernels) {
                return NULL;
        }
        const SystemKernel *k = boot_manager_get_system_kernel(self);
        if (!k) {
                return NULL;
        }

        for (int i = 0; i < kernels->len; i++) {
                Kernel *cur = nc_array_get(kernels, i);
                if (streq(cur->ktype, k->ktype) && streq(cur->version, k->version) &&
                    cur->release == k->release) {
                        return cur;
                }
        }
        return NULL;
}

Kernel *boot_manager_get_last_booted(BootManager *self, KernelArray *kernels)
{
        if (!self || !kernels) {
                return NULL;
        }
        int high_rel = -1;
        Kernel *candidate = NULL;

        for (int i = 0; i < kernels->len; i++) {
                Kernel *k = nc_array_get(kernels, i);
                if (k->release < high_rel) {
                        continue;
                }
                if (!k->boots) {
                        continue;
                }
                candidate = k;
                high_rel = k->release;
        }
        return candidate;
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 expandtab:
 * :indentSize=8:tabSize=8:noTabs=true:
 */