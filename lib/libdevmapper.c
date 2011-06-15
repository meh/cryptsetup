/*
 * libdevmapper - device-mapper backend for cryptsetup
 *
 * Copyright (C) 2004, Christophe Saout <christophe@saout.de>
 * Copyright (C) 2004-2007, Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2009-2011, Red Hat, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <libdevmapper.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <uuid/uuid.h>

#include "internal.h"
#include "luks.h"

#define DM_UUID_LEN		129
#define DM_UUID_PREFIX		"CRYPT-"
#define DM_UUID_PREFIX_LEN	6
#define DM_CRYPT_TARGET		"crypt"
#define RETRY_COUNT		5

/* Set if dm-crypt version was probed */
static int _dm_crypt_checked = 0;
static uint32_t _dm_crypt_flags = 0;

static int _dm_use_count = 0;
static struct crypt_device *_context = NULL;

/* Check if we have DM flag to instruct kernel to force wipe buffers */
#if !HAVE_DECL_DM_TASK_SECURE_DATA
static int dm_task_secure_data(struct dm_task *dmt) { return 1; }
#endif

/* Compatibility for old device-mapper without udev support */
#if HAVE_DECL_DM_UDEV_DISABLE_DISK_RULES_FLAG
#define CRYPT_TEMP_UDEV_FLAGS	DM_UDEV_DISABLE_SUBSYSTEM_RULES_FLAG | \
				DM_UDEV_DISABLE_DISK_RULES_FLAG | \
				DM_UDEV_DISABLE_OTHER_RULES_FLAG
#define _dm_task_set_cookie	dm_task_set_cookie
#define _dm_udev_wait		dm_udev_wait
#else
#define CRYPT_TEMP_UDEV_FLAGS	0
static int _dm_task_set_cookie(struct dm_task *dmt, uint32_t *cookie, uint16_t flags) { return 0; }
static int _dm_udev_wait(uint32_t cookie) { return 0; };
#endif

static int _dm_use_udev(void)
{
#ifdef USE_UDEV /* cannot be enabled if devmapper is too old */
	return dm_udev_get_sync_support();
#else
	return 0;
#endif
}

__attribute__((format(printf, 4, 5)))
static void set_dm_error(int level,
			 const char *file __attribute__((unused)),
			 int line __attribute__((unused)),
			 const char *f, ...)
{
	char *msg = NULL;
	va_list va;

	va_start(va, f);
	if (vasprintf(&msg, f, va) > 0) {
		if (level < 4) {
			log_err(_context, msg);
			log_err(_context, "\n");
		} else
			log_dbg(msg);
	}
	free(msg);
	va_end(va);
}

static int _dm_simple(int task, const char *name, int udev_wait);

static void _dm_set_crypt_compat(const char *dm_version, unsigned crypt_maj,
				 unsigned crypt_min, unsigned crypt_patch)
{
	unsigned dm_maj, dm_min, dm_patch;

	if (sscanf(dm_version, "%u.%u.%u", &dm_maj, &dm_min, &dm_patch) != 3)
		dm_maj = dm_min = dm_patch = 0;

	log_dbg("Detected dm-crypt version %i.%i.%i, dm-ioctl version %u.%u.%u.",
		crypt_maj, crypt_min, crypt_patch, dm_maj, dm_min, dm_patch);

	if (crypt_maj >= 1 && crypt_min >= 2)
		_dm_crypt_flags |= DM_KEY_WIPE_SUPPORTED;
	else
		log_dbg("Suspend and resume disabled, no wipe key support.");

	if (crypt_maj >= 1 && crypt_min >= 10)
		_dm_crypt_flags |= DM_LMK_SUPPORTED;

	if (dm_maj >= 4 && dm_min >= 20)
		_dm_crypt_flags |= DM_SECURE_SUPPORTED;

	/* not perfect, 2.6.33 supports with 1.7.0 */
	if (crypt_maj >= 1 && crypt_min >= 8)
		_dm_crypt_flags |= DM_PLAIN64_SUPPORTED;

	/* Repeat test if dm-crypt is not present */
	if (crypt_maj > 0)
		_dm_crypt_checked = 1;
}

static int _dm_check_versions(void)
{
	struct dm_task *dmt;
	struct dm_versions *target, *last_target;
	char dm_version[16];

	if (_dm_crypt_checked)
		return 1;

	/* FIXME: add support to DM so it forces crypt target module load here */
	if (!(dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)))
		return 0;

	if (!dm_task_run(dmt)) {
		dm_task_destroy(dmt);
		return 0;
	}

	if (!dm_task_get_driver_version(dmt, dm_version, sizeof(dm_version))) {
		dm_task_destroy(dmt);
		return 0;
	}

	target = dm_task_get_versions(dmt);
	do {
		last_target = target;
		if (!strcmp(DM_CRYPT_TARGET, target->name)) {
			_dm_set_crypt_compat(dm_version,
					     (unsigned)target->version[0],
					     (unsigned)target->version[1],
					     (unsigned)target->version[2]);
		}
		target = (struct dm_versions *)((char *) target + target->next);
	} while (last_target != target);

	dm_task_destroy(dmt);
	return 1;
}

uint32_t dm_flags(void)
{
	if (!_dm_crypt_checked)
		_dm_check_versions();

	return _dm_crypt_flags;
}

int dm_init(struct crypt_device *context, int check_kernel)
{
	if (!_dm_use_count++) {
		log_dbg("Initialising device-mapper backend%s, UDEV is %sabled.",
			check_kernel ? "" : " (NO kernel check requested)",
			_dm_use_udev() ? "en" : "dis");
		if (check_kernel && !_dm_check_versions()) {
			log_err(context, _("Cannot initialize device-mapper. Is dm_mod kernel module loaded?\n"));
			return -1;
		}
		if (getuid() || geteuid())
			log_dbg(("WARNING: Running as a non-root user. Functionality may be unavailable."));
		dm_log_init(set_dm_error);
		dm_log_init_verbose(10);
	}

	// FIXME: global context is not safe
	if (context)
		_context = context;

	return 1;	/* unsafe memory */
}

void dm_exit(void)
{
	if (_dm_use_count && (!--_dm_use_count)) {
		log_dbg("Releasing device-mapper backend.");
		dm_log_init_verbose(0);
		dm_log_init(NULL);
		dm_lib_release();
		_context = NULL;
	}
}

/* Return path to DM device */
char *dm_device_path(int major, int minor)
{
	struct dm_task *dmt;
	const char *name;
	char path[PATH_MAX];

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		return NULL;
	if (!dm_task_set_minor(dmt, minor) ||
	    !dm_task_set_major(dmt, major) ||
	    !dm_task_run(dmt) ||
	    !(name = dm_task_get_name(dmt))) {
		dm_task_destroy(dmt);
		return NULL;
	}

	if (snprintf(path, sizeof(path), "/dev/mapper/%s", name) < 0)
		path[0] = '\0';

	dm_task_destroy(dmt);

	return strdup(path);
}

static void hex_key(char *hexkey, size_t key_size, const char *key)
{
	unsigned i;

	for(i = 0; i < key_size; i++)
		sprintf(&hexkey[i * 2], "%02x", (unsigned char)key[i]);
}

static char *get_params(const char *device, uint64_t skip, uint64_t offset,
			const char *cipher, size_t key_size, const char *key)
{
	char *params;
	char *hexkey;

	hexkey = crypt_safe_alloc(key_size * 2 + 1);
	if (!hexkey)
		return NULL;

	hex_key(hexkey, key_size, key);

	params = crypt_safe_alloc(strlen(hexkey) + strlen(cipher) + strlen(device) + 64);
	if (!params)
		goto out;

	sprintf(params, "%s %s %" PRIu64 " %s %" PRIu64,
	        cipher, hexkey, skip, device, offset);

out:
	crypt_safe_free(hexkey);
	return params;
}

/* DM helpers */
static int _dm_simple(int task, const char *name, int udev_wait)
{
	int r = 0;
	struct dm_task *dmt;
	uint32_t cookie = 0;

	if (!_dm_use_udev())
		udev_wait = 0;

	if (!(dmt = dm_task_create(task)))
		return 0;

	if (name && !dm_task_set_name(dmt, name))
		goto out;

	if (udev_wait && !_dm_task_set_cookie(dmt, &cookie, 0))
		goto out;

	r = dm_task_run(dmt);

	if (udev_wait)
		(void)_dm_udev_wait(cookie);

      out:
	dm_task_destroy(dmt);
	return r;
}

static int _error_device(const char *name, size_t size)
{
	struct dm_task *dmt;
	int r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_RELOAD)))
		return 0;

	if (!dm_task_set_name(dmt, name))
		goto error;

	if (!dm_task_add_target(dmt, UINT64_C(0), size, "error", ""))
		goto error;

	if (!dm_task_set_ro(dmt))
		goto error;

	if (!dm_task_no_open_count(dmt))
		goto error;

	if (!dm_task_run(dmt))
		goto error;

	if (!_dm_simple(DM_DEVICE_RESUME, name, 1)) {
		_dm_simple(DM_DEVICE_CLEAR, name, 0);
		goto error;
	}

	r = 1;

error:
	dm_task_destroy(dmt);
	return r;
}

int dm_remove_device(const char *name, int force, uint64_t size)
{
	int r = -EINVAL;
	int retries = force ? RETRY_COUNT : 1;
	int error_target = 0;

	if (!name || (force && !size))
		return -EINVAL;

	do {
		r = _dm_simple(DM_DEVICE_REMOVE, name, 1) ? 0 : -EINVAL;
		if (--retries && r) {
			log_dbg("WARNING: other process locked internal device %s, %s.",
				name, retries ? "retrying remove" : "giving up");
			if (force && (crypt_get_debug_level() == CRYPT_LOG_DEBUG))
				debug_processes_using_device(name);
			sleep(1);
			if (force && !error_target) {
				/* If force flag is set, replace device with error, read-only target.
				 * it should stop processes from reading it and also removed underlying
				 * device from mapping, so it is usable again.
				 * Force flag should be used only for temporary devices, which are
				 * intended to work inside cryptsetup only!
				 * Anyway, if some process try to read temporary cryptsetup device,
				 * it is bug - no other process should try touch it (e.g. udev).
				 */
				_error_device(name, size);
				error_target = 1;
			}
		}
	} while (r == -EINVAL && retries);

	dm_task_update_nodes();

	return r;
}

#define UUID_LEN 37 /* 36 + \0, libuuid ... */
/*
 * UUID has format: CRYPT-<devicetype>-[<uuid>-]<device name>
 * CRYPT-PLAIN-name
 * CRYPT-LUKS1-00000000000000000000000000000000-name
 * CRYPT-TEMP-name
 */
static void dm_prepare_uuid(const char *name, const char *type, const char *uuid, char *buf, size_t buflen)
{
	char *ptr, uuid2[UUID_LEN] = {0};
	uuid_t uu;
	unsigned i = 0;

	/* Remove '-' chars */
	if (uuid && !uuid_parse(uuid, uu)) {
		for (ptr = uuid2, i = 0; i < UUID_LEN; i++)
			if (uuid[i] != '-') {
				*ptr = uuid[i];
				ptr++;
			}
	}

	i = snprintf(buf, buflen, DM_UUID_PREFIX "%s%s%s%s%s",
		type ?: "", type ? "-" : "",
		uuid2[0] ? uuid2 : "", uuid2[0] ? "-" : "",
		name);

	log_dbg("DM-UUID is %s", buf);
	if (i >= buflen)
		log_err(NULL, _("DM-UUID for device %s was truncated.\n"), name);
}

int dm_create_device(const char *name,
		     const char *device,
		     const char *cipher,
		     const char *type,
		     const char *uuid,
		     uint64_t size,
		     uint64_t skip,
		     uint64_t offset,
		     size_t key_size,
		     const char *key,
		     int read_only,
		     int reload)
{
	struct dm_task *dmt = NULL;
	struct dm_info dmi;
	char *params = NULL;
	char *error = NULL;
	char dev_uuid[DM_UUID_LEN] = {0};
	int r = -EINVAL;
	uint32_t read_ahead = 0;
	uint32_t cookie = 0;
	uint16_t udev_flags = 0;

	params = get_params(device, skip, offset, cipher, key_size, key);
	if (!params)
		goto out_no_removal;

	if (type && !strncmp(type, "TEMP", 4))
		udev_flags = CRYPT_TEMP_UDEV_FLAGS;

	/* All devices must have DM_UUID, only resize on old device is exception */
	if (reload) {
		if (!(dmt = dm_task_create(DM_DEVICE_RELOAD)))
			goto out_no_removal;

		if (!dm_task_set_name(dmt, name))
			goto out_no_removal;
	} else {
		dm_prepare_uuid(name, type, uuid, dev_uuid, sizeof(dev_uuid));

		if (!(dmt = dm_task_create(DM_DEVICE_CREATE)))
			goto out_no_removal;

		if (!dm_task_set_name(dmt, name))
			goto out_no_removal;

		if (!dm_task_set_uuid(dmt, dev_uuid))
			goto out_no_removal;

		if (_dm_use_udev() && !_dm_task_set_cookie(dmt, &cookie, udev_flags))
			goto out_no_removal;
	}

	if ((dm_flags() & DM_SECURE_SUPPORTED) && !dm_task_secure_data(dmt))
		goto out_no_removal;
	if (read_only && !dm_task_set_ro(dmt))
		goto out_no_removal;
	if (!dm_task_add_target(dmt, 0, size, DM_CRYPT_TARGET, params))
		goto out_no_removal;

#ifdef DM_READ_AHEAD_MINIMUM_FLAG
	if (device_read_ahead(device, &read_ahead) &&
	    !dm_task_set_read_ahead(dmt, read_ahead, DM_READ_AHEAD_MINIMUM_FLAG))
		goto out_no_removal;
#endif

	if (!dm_task_run(dmt))
		goto out_no_removal;

	if (reload) {
		dm_task_destroy(dmt);
		if (!(dmt = dm_task_create(DM_DEVICE_RESUME)))
			goto out;
		if (!dm_task_set_name(dmt, name))
			goto out;
		if (uuid && !dm_task_set_uuid(dmt, dev_uuid))
			goto out;
		if (_dm_use_udev() && !_dm_task_set_cookie(dmt, &cookie, udev_flags))
			goto out;
		if (!dm_task_run(dmt))
			goto out;
	}

	if (!dm_task_get_info(dmt, &dmi))
		goto out;

	r = 0;
out:
	if (_dm_use_udev()) {
		(void)_dm_udev_wait(cookie);
		cookie = 0;
	}

	if (r < 0 && !reload) {
		if (get_error())
			error = strdup(get_error());

		dm_remove_device(name, 0, 0);

		if (error) {
			set_error(error);
			free(error);
		}
	}

out_no_removal:
	if (cookie && _dm_use_udev())
		(void)_dm_udev_wait(cookie);

	if (params)
		crypt_safe_free(params);
	if (dmt)
		dm_task_destroy(dmt);

	dm_task_update_nodes();
	return r;
}

int dm_status_device(const char *name)
{
	struct dm_task *dmt;
	struct dm_info dmi;
	uint64_t start, length;
	char *target_type, *params;
	void *next = NULL;
	int r = -EINVAL;

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		goto out;

	if (!dm_task_set_name(dmt, name))
		goto out;

	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &dmi))
		goto out;

	if (!dmi.exists) {
		r = -ENODEV;
		goto out;
	}

	next = dm_get_next_target(dmt, next, &start, &length,
	                          &target_type, &params);
	if (!target_type || strcmp(target_type, DM_CRYPT_TARGET) != 0 ||
	    start != 0 || next)
		r = -EINVAL;
	else
		r = (dmi.open_count > 0);
out:
	if (dmt)
		dm_task_destroy(dmt);

	return r;
}

int dm_query_device(const char *name,
		    char **device,
		    uint64_t *size,
		    uint64_t *skip,
		    uint64_t *offset,
		    char **cipher,
		    int *key_size,
		    char **key,
		    int *read_only,
		    int *suspended,
		    char **uuid)
{
	struct dm_task *dmt;
	struct dm_info dmi;
	uint64_t start, length, val64;
	char *target_type, *params, *rcipher, *key_, *rdevice, *endp, buffer[3];
	const char *tmp_uuid;
	void *next = NULL;
	int i, r = -EINVAL;

	if (!(dmt = dm_task_create(DM_DEVICE_TABLE)))
		goto out;
	if ((dm_flags() & DM_SECURE_SUPPORTED) && !dm_task_secure_data(dmt))
		goto out;
	if (!dm_task_set_name(dmt, name))
		goto out;
	r = -ENODEV;
	if (!dm_task_run(dmt))
		goto out;

	r = -EINVAL;
	if (!dm_task_get_info(dmt, &dmi))
		goto out;

	if (!dmi.exists) {
		r = -ENODEV;
		goto out;
	}

	next = dm_get_next_target(dmt, next, &start, &length,
	                          &target_type, &params);
	if (!target_type || strcmp(target_type, DM_CRYPT_TARGET) != 0 ||
	    start != 0 || next)
		goto out;

	if (size)
		*size = length;

	rcipher = strsep(&params, " ");
	/* cipher */
	if (cipher)
		*cipher = strdup(rcipher);

	/* skip */
	key_ = strsep(&params, " ");
	if (!params)
		goto out;
	val64 = strtoull(params, &params, 10);
	if (*params != ' ')
		goto out;
	params++;
	if (skip)
		*skip = val64;

	/* device */
	rdevice = strsep(&params, " ");
	if (device)
		*device = crypt_lookup_dev(rdevice);

	/*offset */
	if (!params)
		goto out;
	val64 = strtoull(params, &params, 10);
	if (*params)
		goto out;
	if (offset)
		*offset = val64;

	/* key_size */
	if (key_size)
		*key_size = strlen(key_) / 2;

	/* key */
	if (key_size && key) {
		*key = crypt_safe_alloc(*key_size);
		if (!*key) {
			r = -ENOMEM;
			goto out;
		}

		buffer[2] = '\0';
		for(i = 0; i < *key_size; i++) {
			memcpy(buffer, &key_[i * 2], 2);
			(*key)[i] = strtoul(buffer, &endp, 16);
			if (endp != &buffer[2]) {
				crypt_safe_free(key);
				*key = NULL;
				goto out;
			}
		}
	}
	memset(key_, 0, strlen(key_));

	if (read_only)
		*read_only = dmi.read_only;

	if (suspended)
		*suspended = dmi.suspended;

	if (uuid && (tmp_uuid = dm_task_get_uuid(dmt)) &&
	    !strncmp(tmp_uuid, DM_UUID_PREFIX, DM_UUID_PREFIX_LEN))
		*uuid = strdup(tmp_uuid + DM_UUID_PREFIX_LEN);

	r = (dmi.open_count > 0);
out:
	if (dmt)
		dm_task_destroy(dmt);

	return r;
}

static int _dm_message(const char *name, const char *msg)
{
	int r = 0;
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return 0;

	if ((dm_flags() & DM_SECURE_SUPPORTED) && !dm_task_secure_data(dmt))
		goto out;

	if (name && !dm_task_set_name(dmt, name))
		goto out;

	if (!dm_task_set_sector(dmt, (uint64_t) 0))
		goto out;

	if (!dm_task_set_message(dmt, msg))
		goto out;

	r = dm_task_run(dmt);

      out:
	dm_task_destroy(dmt);
	return r;
}

int dm_suspend_and_wipe_key(const char *name)
{
	if (!_dm_check_versions())
		return -ENOTSUP;

	if (!(_dm_crypt_flags & DM_KEY_WIPE_SUPPORTED))
		return -ENOTSUP;

	if (!_dm_simple(DM_DEVICE_SUSPEND, name, 0))
		return -EINVAL;

	if (!_dm_message(name, "key wipe")) {
		_dm_simple(DM_DEVICE_RESUME, name, 1);
		return -EINVAL;
	}

	return 0;
}

int dm_resume_and_reinstate_key(const char *name,
				size_t key_size,
				const char *key)
{
	int msg_size = key_size * 2 + 10; // key set <key>
	char *msg;
	int r = 0;

	if (!_dm_check_versions())
		return -ENOTSUP;

	if (!(_dm_crypt_flags & DM_KEY_WIPE_SUPPORTED))
		return -ENOTSUP;

	msg = crypt_safe_alloc(msg_size);
	if (!msg)
		return -ENOMEM;

	memset(msg, 0, msg_size);
	strcpy(msg, "key set ");
	hex_key(&msg[8], key_size, key);

	if (!_dm_message(name, msg) ||
	    !_dm_simple(DM_DEVICE_RESUME, name, 1))
		r = -EINVAL;

	crypt_safe_free(msg);
	return r;
}

const char *dm_get_dir(void)
{
	return dm_dir();
}

int dm_is_dm_device(int major, int minor)
{
	return dm_is_dm_major((uint32_t)major);
}

int dm_is_dm_kernel_name(const char *name)
{
	return strncmp(name, "dm-", 3) ? 0 : 1;
}
