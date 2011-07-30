/*
 * NSS crypto backend implementation
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
#include <nss.h>
#include <pk11pub.h>
#include "crypto_backend.h"

static int crypto_backend_initialised = 0;

struct hash_alg {
	const char *name;
	SECOidTag oid;
	CK_MECHANISM_TYPE ck_type;
	int length;
};

static struct hash_alg hash_algs[] = {
	{ "sha1", SEC_OID_SHA1, CKM_SHA_1_HMAC, 20 },
	{ "sha256", SEC_OID_SHA256, CKM_SHA256_HMAC, 32 },
	{ "sha384", SEC_OID_SHA384, CKM_SHA384_HMAC, 48 },
	{ "sha512", SEC_OID_SHA512, CKM_SHA512_HMAC, 64 },
//	{ "ripemd160", SEC_OID_RIPEMD160, CKM_RIPEMD160_HMAC, 20 },
	{ NULL, 0, 0, 0 }
};

struct crypt_hash {
	PK11Context *md;
	const struct hash_alg *hash;
};

struct crypt_hmac {
	PK11Context *md;
	PK11SymKey *key;
	PK11SlotInfo *slot;
	const struct hash_alg *hash;
};

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

int crypt_backend_init(struct crypt_device *ctx)
{
	if (crypto_backend_initialised)
		return 0;

	log_dbg("Initialising NSS crypto backend.");
	if (NSS_NoDB_Init(".") != SECSuccess)
		return -EINVAL;

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
	struct hash_alg *ha = _get_alg(name);

	return ha ? ha->length : -EINVAL;
}

int crypt_hash_init(struct crypt_hash **ctx, const char *name)
{
	struct crypt_hash *h;

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;

	h->hash = _get_alg(name);
	if (!h->hash) {
		free(h);
		return -EINVAL;
	}

	h->md = PK11_CreateDigestContext(h->hash->oid);
	if (!h->md) {
		free(h);
		return -EINVAL;
	}

	if (PK11_DigestBegin(h->md) != SECSuccess) {
		PK11_DestroyContext(h->md, PR_TRUE);
		free(h);
		return -EINVAL;
	}

	*ctx = h;
	return 0;
}

static int crypt_hash_restart(struct crypt_hash *ctx)
{
	if (PK11_DigestBegin(ctx->md) != SECSuccess)
		return -EINVAL;

	return 0;
}

int crypt_hash_write(struct crypt_hash *ctx, const char *buffer, size_t length)
{
	if (PK11_DigestOp(ctx->md, (unsigned char *)buffer, length) != SECSuccess)
		return -EINVAL;

	return 0;
}

int crypt_hash_final(struct crypt_hash *ctx, char *buffer, size_t length)
{
	unsigned char tmp[64];
	unsigned int tmp_len;

	if (length > (size_t)ctx->hash->length)
		return -EINVAL;

	if (PK11_DigestFinal(ctx->md, tmp, &tmp_len, length) != SECSuccess)
		return -EINVAL;

	memcpy(buffer, tmp, length);
	memset(tmp, 0, sizeof(tmp));

	if (tmp_len < length)
		return -EINVAL;

	if (crypt_hash_restart(ctx))
		return -EINVAL;

	return 0;
}

int crypt_hash_destroy(struct crypt_hash *ctx)
{
	PK11_DestroyContext(ctx->md, PR_TRUE);
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
	SECItem keyItem;
	SECItem noParams;

	keyItem.type = siBuffer;
	keyItem.data = (unsigned char *)buffer;
	keyItem.len = (int)length;

	noParams.type = siBuffer;
	noParams.data = 0;
	noParams.len = 0;

	h = malloc(sizeof(*h));
	if (!h)
		return -ENOMEM;
	memset(ctx, 0, sizeof(*ctx));


	h->hash = _get_alg(name);
	if (!h->hash)
		goto bad;

	h->slot = PK11_GetInternalKeySlot();
	if (!h->slot)
		goto bad;

	h->key = PK11_ImportSymKey(h->slot, h->hash->ck_type, PK11_OriginUnwrap,
				   CKA_SIGN,  &keyItem, NULL);
	if (!h->key)
		goto bad;

	h->md = PK11_CreateContextBySymKey(h->hash->ck_type, CKA_SIGN, h->key,
					   &noParams);
	if (!h->md)
		goto bad;

	if (PK11_DigestBegin(h->md) != SECSuccess)
		goto bad;

	*ctx = h;
	return 0;
bad:
	crypt_hmac_destroy(h);
	return -EINVAL;
}

static int crypt_hmac_restart(struct crypt_hmac *ctx)
{
	if (PK11_DigestBegin(ctx->md) != SECSuccess)
		return -EINVAL;

	return 0;
}

int crypt_hmac_write(struct crypt_hmac *ctx, const char *buffer, size_t length)
{
	if (PK11_DigestOp(ctx->md, (unsigned char *)buffer, length) != SECSuccess)
		return -EINVAL;

	return 0;
}

int crypt_hmac_final(struct crypt_hmac *ctx, char *buffer, size_t length)
{
	unsigned char tmp[64];
	unsigned int tmp_len;

	if (length > (size_t)ctx->hash->length)
		return -EINVAL;

	if (PK11_DigestFinal(ctx->md, tmp, &tmp_len, length) != SECSuccess)
		return -EINVAL;

	memcpy(buffer, tmp, length);
	memset(tmp, 0, sizeof(tmp));

	if (tmp_len < length)
		return -EINVAL;

	if (crypt_hmac_restart(ctx))
		return -EINVAL;

	return 0;
}

int crypt_hmac_destroy(struct crypt_hmac *ctx)
{
	if (ctx->key)
		PK11_FreeSymKey(ctx->key);
	if (ctx->slot)
		PK11_FreeSlot(ctx->slot);
	if (ctx->md)
		PK11_DestroyContext(ctx->md, PR_TRUE);
	memset(ctx, 0, sizeof(*ctx));
	free(ctx);
	return 0;
}
