/*
 * AFsplitter - Anti forensic information splitter
 *
 * Copyright (C) 2004, Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2009-2011, Red Hat, Inc. All rights reserved.
 *
 * AFsplitter diffuses information over a large stripe of data,
 * therefor supporting secure data destruction.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <errno.h>
#include "crypto_backend.h"
#include "internal.h"
#include "af.h"

static void XORblock(const char *src1, const char *src2, char *dst, size_t n)
{
	size_t j;

	for(j = 0; j < n; ++j)
		dst[j] = src1[j] ^ src2[j];
}

static int hash_buf(const char *src, char *dst, uint32_t iv,
		    size_t len, const char *hash_name)
{
	struct crypt_hash *hd = NULL;
	char *iv_char = (char *)&iv;
	int r;

	iv = htonl(iv);
	if (crypt_hash_init(&hd, hash_name))
		return -EINVAL;

	if ((r = crypt_hash_write(hd, iv_char, sizeof(uint32_t))))
		goto out;

	if ((r = crypt_hash_write(hd, src, len)))
		goto out;

	r = crypt_hash_final(hd, dst, len);
out:
	crypt_hash_destroy(hd);
	return r;
}

/* diffuse: Information spreading over the whole dataset with
 * the help of hash function.
 */

static int diffuse(char *src, char *dst, size_t size, const char *hash_name)
{
	unsigned int digest_size = crypt_hash_size(hash_name);
	unsigned int i, blocks, padding;

	blocks = size / digest_size;
	padding = size % digest_size;

	for (i = 0; i < blocks; i++)
		if(hash_buf(src + digest_size * i,
			    dst + digest_size * i,
			    i, (size_t)digest_size, hash_name))
			return 1;

	if(padding)
		if(hash_buf(src + digest_size * i,
			    dst + digest_size * i,
			    i, (size_t)padding, hash_name))
			return 1;

	return 0;
}

/*
 * Information splitting. The amount of data is multiplied by
 * blocknumbers. The same blocksize and blocknumbers values
 * must be supplied to AF_merge to recover information.
 */

int AF_split(char *src, char *dst, size_t blocksize,
	     unsigned int blocknumbers, const char *hash)
{
	unsigned int i;
	char *bufblock;
	int r = -EINVAL;

	if((bufblock = calloc(blocksize, 1)) == NULL) return -ENOMEM;

	/* process everything except the last block */
	for(i=0; i<blocknumbers-1; i++) {
		r = crypt_random_get(NULL, dst+(blocksize*i), blocksize, CRYPT_RND_NORMAL);
		if(r < 0) goto out;

		XORblock(dst+(blocksize*i),bufblock,bufblock,blocksize);
		if(diffuse(bufblock, bufblock, blocksize, hash))
			goto out;
	}
	/* the last block is computed */
	XORblock(src,bufblock,dst+(i*blocksize),blocksize);
	r = 0;
out:
	free(bufblock);
	return r;
}

int AF_merge(char *src, char *dst, size_t blocksize,
	     unsigned int blocknumbers, const char *hash)
{
	unsigned int i;
	char *bufblock;
	int r = -EINVAL;

	if((bufblock = calloc(blocksize, 1)) == NULL)
		return -ENOMEM;

	memset(bufblock,0,blocksize);
	for(i=0; i<blocknumbers-1; i++) {
		XORblock(src+(blocksize*i),bufblock,bufblock,blocksize);
		if(diffuse(bufblock, bufblock, blocksize, hash))
			goto out;
	}
	XORblock(src + blocksize * i, bufblock, dst, blocksize);
	r = 0;
out:
	free(bufblock);
	return r;
}
