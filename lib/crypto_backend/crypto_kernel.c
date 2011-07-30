/*
 * Linux kernel userspace API crypto backend implementation
 *
 * Copyright (C) 2010-2011, Red Hat, Inc. All rights reserved.
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <linux/if_alg.h>
#include "crypto_backend.h"

/* FIXME: remove later */
#ifndef AF_ALG
#define AF_ALG 38
#endif
#ifndef SOL_ALG
#define SOL_ALG 279
#endif

static int crypto_backend_initialised = 0;

struct hash_alg {
	const char *name;
	const char *kernel_name;
	int length;
};

static struct hash_alg hash_algs[] = {
	{ "sha1", "sha1", 20 },
	{ "sha256", "sha256", 32 },
	{ "sha512", "sha512", 64 },
	{ "ripemd160", "rmd160", 20 },
	{ "whirlpool", "wp512", 64 },
	{ NULL, 0 }
};

struct crypt_hash {
	int tfmfd;
	int opfd;
	int hash_len;
};

struct crypt_hmac {
	int tfmfd;
	int opfd;
	int hash_len;
};

static int _socket_init(struct sockaddr_alg *sa, int *tfmfd, int *opfd)
{
	*tfmfd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (*tfmfd == -1)
		goto bad;

	if (bind(*tfmfd, (struct sockaddr *)sa, sizeof(*sa)) == -1)
		goto bad;

	*opfd = accept(*tfmfd, NULL, 0);
	if (*opfd == -1)
		goto bad;

	return 0;
bad:
	if (*tfmfd != -1) {
		close(*tfmfd);
		*tfmfd = -1;
	}
	if (*opfd != -1) {
		close(*opfd);
		*opfd = -1;
	}
	return -EINVAL;
}

int crypt_backend_init(struct crypt_device *ctx)
{
	struct utsname uts;

	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
		.salg_type = "hash",
		.salg_name = "sha1",
	};
	int tfmfd = -1, opfd = -1;

	if (crypto_backend_initialised)
		return 0;

	log_dbg("Initialising kernel crypto API backend.");

	if (uname(&uts) == -1 || strcmp(uts.sysname, "Linux"))
		return -EINVAL;
	log_dbg("Kernel version %s %s.", uts.sysname, uts.release);

	if (_socket_init(&sa, &tfmfd, &opfd) < 0)
		return -EINVAL;

	close(tfmfd);
	close(opfd);

	crypto_backend_initialised = 1;
	return 0;
}

uint32_t crypt_backend_flags(void)
{
	return CRYPT_BACKEND_KERNEL;
}

static struct hash_alg *_get_alg(const char *name)
{
	int i = 0;

	while (name && hash_algs[i].name) {
		if (!strcmp(name, hash_algs[i].name))
			return &hash_algs[i];
		i++;
	}
	return NULL;
}

/* HASH */
int crypt_hash_size(const char *name)
{
	struct hash_alg *ha = _get_alg(name);

	return ha ? ha->length : -EINVAL;
}

int crypt_hash_init(struct crypt_hash **ctx, const char *name)
{
	struct crypt_hash *h;
	struct hash_alg *ha;
	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
		.salg_type = "hash",
	};

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;

	ha = _get_alg(name);
	if (!ha) {
		free(h);
		return -EINVAL;
	}
	h->hash_len = ha->length;

	strncpy((char *)sa.salg_name, ha->kernel_name, sizeof(sa.salg_name));

	if (_socket_init(&sa, &h->tfmfd, &h->opfd) < 0) {
		free(h);
		return -EINVAL;
	}

	*ctx = h;
	return 0;
}

int crypt_hash_write(struct crypt_hash *ctx, const char *buffer, size_t length)
{
	ssize_t r;

	r = send(ctx->opfd, buffer, length, MSG_MORE);
	if (r < 0 || (size_t)r < length)
		return -EIO;

	return 0;
}

int crypt_hash_final(struct crypt_hash *ctx, char *buffer, size_t length)
{
	ssize_t r;

	if (length > (size_t)ctx->hash_len)
		return -EINVAL;

	r = read(ctx->opfd, buffer, length);
	if (r < 0)
		return -EIO;

	return 0;
}

int crypt_hash_destroy(struct crypt_hash *ctx)
{
	if (ctx->tfmfd != -1)
		close(ctx->tfmfd);
	if (ctx->opfd != -1)
		close(ctx->opfd);
	memset(ctx, 0, sizeof(*ctx));
	free(ctx);
	return 0;
}

/* HMAC */
int crypt_hmac_size(const char *name)
{
	return crypt_hash_size(name);
}

int crypt_hmac_init(struct crypt_hmac **ctx, const char *name,
		    const void *buffer, size_t length)
{
	struct crypt_hmac *h;
	struct hash_alg *ha;
	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
		.salg_type = "hash",
	};

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;

	ha = _get_alg(name);
	if (!ha) {
		free(h);
		return -EINVAL;
	}
	h->hash_len = ha->length;

	snprintf((char *)sa.salg_name, sizeof(sa.salg_name),
		 "hmac(%s)", ha->kernel_name);

	if (_socket_init(&sa, &h->tfmfd, &h->opfd) < 0) {
		free(h);
		return -EINVAL;
	}

	if (setsockopt(h->tfmfd, SOL_ALG, ALG_SET_KEY, buffer, length) == -1) {
		crypt_hmac_destroy(h);
		return -EINVAL;
	}

	*ctx = h;
	return 0;
}

int crypt_hmac_write(struct crypt_hmac *ctx, const char *buffer, size_t length)
{
	ssize_t r;

	r = send(ctx->opfd, buffer, length, MSG_MORE);
	if (r < 0 || (size_t)r < length)
		return -EIO;

	return 0;
}

int crypt_hmac_final(struct crypt_hmac *ctx, char *buffer, size_t length)
{
	ssize_t r;

	if (length > (size_t)ctx->hash_len)
		return -EINVAL;

	r = read(ctx->opfd, buffer, length);
	if (r < 0)
		return -EIO;

	return 0;
}

int crypt_hmac_destroy(struct crypt_hmac *ctx)
{
	if (ctx->tfmfd != -1)
		close(ctx->tfmfd);
	if (ctx->opfd != -1)
		close(ctx->opfd);
	memset(ctx, 0, sizeof(*ctx));
	free(ctx);
	return 0;
}
