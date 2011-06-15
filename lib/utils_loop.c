/*
 * loopback block device utilities
 *
 * Copyright (C) 2011, Red Hat, Inc. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/loop.h>

#include "utils_loop.h"

char *crypt_loop_get_device(void)
{
	char dev[20];
	int i, loop_fd;
	struct stat st;
	struct loop_info64 lo64 = {0};

	for (i = 0; i < 256; i++) {
		sprintf(dev, "/dev/loop%d", i);
		if (stat(dev, &st) || !S_ISBLK(st.st_mode))
			return NULL;

		loop_fd = open(dev, O_RDONLY);
		if (loop_fd < 0)
			return NULL;

		if (ioctl(loop_fd, LOOP_GET_STATUS64, &lo64) &&
		    errno == ENXIO) {
			close(loop_fd);
			return strdup(dev);
		}
		close(loop_fd);
	}

	return NULL;
}

int crypt_loop_attach(const char *loop, const char *file, int offset,
		      int autoclear, int *readonly)
{
	struct loop_info64 lo64 = {0};
	int loop_fd = -1, file_fd = -1, r = 1;

	file_fd = open(file, (*readonly ? O_RDONLY : O_RDWR) | O_EXCL);
	if (file_fd < 0 && errno == EROFS && !*readonly) {
		*readonly = 1;
		file_fd = open(file, O_RDONLY | O_EXCL);
	}
	if (file_fd < 0)
		goto out;

	loop_fd = open(loop, *readonly ? O_RDONLY : O_RDWR);
	if (loop_fd < 0)
		goto out;

	strncpy((char*)lo64.lo_file_name, file, LO_NAME_SIZE);
	lo64.lo_offset = offset;
	if (autoclear)
		lo64.lo_flags |= LO_FLAGS_AUTOCLEAR;

	if (ioctl(loop_fd, LOOP_SET_FD, file_fd) < 0)
		goto out;

	if (ioctl(loop_fd, LOOP_SET_STATUS64, &lo64) < 0) {
		(void)ioctl(loop_fd, LOOP_CLR_FD, 0);
		goto out;
	}

	/* Verify that autoclear is really set */
	if (autoclear) {
		memset(&lo64, 0, sizeof(lo64));
		if (ioctl(loop_fd, LOOP_GET_STATUS64, &lo64) < 0 ||
		   !(lo64.lo_flags & LO_FLAGS_AUTOCLEAR)) {
		(void)ioctl(loop_fd, LOOP_CLR_FD, 0);
			goto out;
		}
	}

	r = 0;
out:
	if (r && loop_fd >= 0)
		close(loop_fd);
	if (file_fd >= 0)
		close(file_fd);
	return r ? -1 : loop_fd;
}

int crypt_loop_detach(const char *loop)
{
	int loop_fd = -1, r = 1;

	loop_fd = open(loop, O_RDONLY);
	if (loop_fd < 0)
                return 1;

	if (!ioctl(loop_fd, LOOP_CLR_FD, 0))
		r = 0;

	close(loop_fd);
	return r;
}

static char *_ioctl_backing_file(const char *loop)
{
	struct loop_info64 lo64 = {0};
	int loop_fd;

	loop_fd = open(loop, O_RDONLY);
	if (loop_fd < 0)
		return NULL;

	if (ioctl(loop_fd, LOOP_GET_STATUS64, &lo64) < 0) {
		close(loop_fd);
		return NULL;
	}

	lo64.lo_file_name[LO_NAME_SIZE-2] = '*';
	lo64.lo_file_name[LO_NAME_SIZE-1] = 0;

	close(loop_fd);

	return strdup((char*)lo64.lo_file_name);
}

static char *_sysfs_backing_file(const char *loop)
{
	struct stat st;
	char buf[PATH_MAX];
	size_t len;
	int fd;

	if (stat(loop, &st) || !S_ISBLK(st.st_mode))
		return NULL;

	snprintf(buf, sizeof(buf), "/sys/dev/block/%d:%d/loop/backing_file",
		 major(st.st_rdev), minor(st.st_rdev));

	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return NULL;

	len = read(fd, buf, PATH_MAX);
	close(fd);
	if (len < 2)
		return NULL;

	buf[len - 1] = '\0';
	return strdup(buf);
}

char *crypt_loop_backing_file(const char *loop)
{
	char *bf = _sysfs_backing_file(loop);
	return bf ?: _ioctl_backing_file(loop);
}

int crypt_loop_device(const char *loop)
{
	struct stat st;

	if (!loop)
		return 0;

	if (stat(loop, &st) || !S_ISBLK(st.st_mode) ||
	    major(st.st_rdev) != LOOP_DEV_MAJOR)
		return 0;

	return 1;
}
