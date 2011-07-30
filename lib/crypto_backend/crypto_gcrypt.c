/*
 * GCRYPT crypto backend implementation
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
#include <errno.h>
#include <assert.h>
#include <gcrypt.h>
#include "crypto_backend.h"

#define GCRYPT_REQ_VERSION "1.1.42"

static int crypto_backend_initialised = 0;

struct crypt_hash {
	gcry_md_hd_t hd;
	int hash_id;
	int hash_len;
};

struct crypt_hmac {
	gcry_md_hd_t hd;
	int hash_id;
	int hash_len;
};

int crypt_backend_init(struct crypt_device *ctx __attribute__((unused)))
{
	if (crypto_backend_initialised)
		return 0;

	log_dbg("Initialising gcrypt crypto backend.");
	if (!gcry_control (GCRYCTL_INITIALIZATION_FINISHED_P)) {
		if (!gcry_check_version (GCRYPT_REQ_VERSION)) {
			return -ENOSYS;
		}

/* FIXME: If gcrypt compiled to support POSIX 1003.1e capabilities,
 * it drops all privileges during secure memory initialisation.
 * For now, the only workaround is to disable secure memory in gcrypt.
 * cryptsetup always need at least cap_sys_admin privilege for dm-ioctl
 * and it locks its memory space anyway.
 */
#if 0
		log_dbg("Initializing crypto backend (secure memory disabled).");
		gcry_control (GCRYCTL_DISABLE_SECMEM);
#else

		gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
		gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
		gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
#endif
		gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
	}

	crypto_backend_initialised = 1;
	return 0;
}

uint32_t crypt_backend_flags(void)
{
	return 0;
}

/* HASH */
int crypt_hash_size(const char *name)
{
	int hash_id;

	assert(crypto_backend_initialised);

	hash_id = gcry_md_map_name(name);
	if (!hash_id)
		return -EINVAL;

	return gcry_md_get_algo_dlen(hash_id);
}

int crypt_hash_init(struct crypt_hash **ctx, const char *name)
{
	struct crypt_hash *h;

	assert(crypto_backend_initialised);

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;

	h->hash_id = gcry_md_map_name(name);
	if (!h->hash_id) {
		free(h);
		return -EINVAL;
	}

	if (gcry_md_open(&h->hd, h->hash_id, 0)) {
		free(h);
		return -EINVAL;
	}

	h->hash_len = gcry_md_get_algo_dlen(h->hash_id);
	*ctx = h;
	return 0;
}

static void crypt_hash_restart(struct crypt_hash *ctx)
{
	gcry_md_reset(ctx->hd);
}

int crypt_hash_write(struct crypt_hash *ctx, const char *buffer, size_t length)
{
	gcry_md_write(ctx->hd, buffer, length);
	return 0;
}

int crypt_hash_final(struct crypt_hash *ctx, char *buffer, size_t length)
{
	unsigned char *hash;

	if (length > (size_t)ctx->hash_len)
		return -EINVAL;

	hash = gcry_md_read(ctx->hd, ctx->hash_id);
	if (!hash)
		return -EINVAL;

	memcpy(buffer, hash, length);
	crypt_hash_restart(ctx);

	return 0;
}

int crypt_hash_destroy(struct crypt_hash *ctx)
{
	gcry_md_close(ctx->hd);
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

	assert(crypto_backend_initialised);

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;

	h->hash_id = gcry_md_map_name(name);
	if (!h->hash_id) {
		free(h);
		return -EINVAL;
	}

	if (gcry_md_open(&h->hd, h->hash_id, GCRY_MD_FLAG_HMAC)) {
		free(h);
		return -EINVAL;
	}

	if (gcry_md_setkey(h->hd, buffer, length)) {
		gcry_md_close(h->hd);
		free(h);
		return -EINVAL;
	}

	h->hash_len = gcry_md_get_algo_dlen(h->hash_id);
	*ctx = h;
	return 0;
}

static void crypt_hmac_restart(struct crypt_hmac *ctx)
{
	gcry_md_reset(ctx->hd);
}

int crypt_hmac_write(struct crypt_hmac *ctx, const char *buffer, size_t length)
{
	gcry_md_write(ctx->hd, buffer, length);
	return 0;
}

int crypt_hmac_final(struct crypt_hmac *ctx, char *buffer, size_t length)
{
	unsigned char *hash;

	if (length > (size_t)ctx->hash_len)
		return -EINVAL;

	hash = gcry_md_read(ctx->hd, ctx->hash_id);
	if (!hash)
		return -EINVAL;

	memcpy(buffer, hash, length);
	crypt_hmac_restart(ctx);

	return 0;
}

int crypt_hmac_destroy(struct crypt_hmac *ctx)
{
	gcry_md_close(ctx->hd);
	memset(ctx, 0, sizeof(*ctx));
	free(ctx);
	return 0;
}
