/*
   Copyright (C) 2025 Free Software Foundation, Inc.

   This file is part of GnuCOBOL.

   The GnuCOBOL runtime library is free software: you can redistribute it
   and/or modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   GnuCOBOL is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GnuCOBOL.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <stdlib.h>
#include <string.h>

/* include internal and external libcob definitions, forcing exports */
#define	COB_LIB_EXPIMP

#include "common.h"

#if defined (WITH_CURL)
#include <curl/curl.h>

struct write_mem {
	char	*buf;
	size_t	capacity;
};

static size_t
write_callback (void *ptr, size_t size, size_t nmemb, void *stream)
{
	struct write_mem	*mem = (struct write_mem *) stream;
	size_t			total = size * nmemb;
	size_t			to_copy;

	to_copy = total < mem->capacity ? total : mem->capacity;
	memcpy (mem->buf, ptr, to_copy);
	if (to_copy < mem->capacity) {
		mem->buf[to_copy] = '\0';
	}
	mem->capacity = to_copy;
	return total;
}
#endif /* WITH_CURL */

void
cob_http_get (cob_field *url, cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	CURL		*curl;
	CURLcode	res;
	long		http_code = 0;
	struct write_mem chunk;
	char		*url_copy;
	size_t		url_len;

	if (!url || !url->data || url->size == 0) {
		cob_set_exception (COB_EC_HTTP_IMP);
		return;
	}

	url_len = url->size;
	url_copy = malloc (url_len + 1);
	if (!url_copy) {
		cob_set_exception (COB_EC_HTTP_IMP);
		return;
	}
	memcpy (url_copy, url->data, url_len);
	url_copy[url_len] = '\0';
	while (url_len > 0 && url_copy[url_len - 1] == ' ') {
		url_copy[--url_len] = '\0';
	}

	if (response_body && response_body->data && response_body->size > 0) {
		chunk.buf = response_body->data;
		chunk.capacity = response_body->size;
		memset (chunk.buf, ' ', chunk.capacity);
	} else {
		chunk.buf = NULL;
		chunk.capacity = 0;
	}

	curl = curl_easy_init ();
	if (!curl) {
		free (url_copy);
		cob_set_exception (COB_EC_HTTP_IMP);
		return;
	}

	curl_easy_setopt (curl, CURLOPT_URL, url_copy);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *) &chunk);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 30L);

	res = curl_easy_perform (curl);
	if (res == CURLE_OK) {
		curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
	} else {
		http_code = -1;
		cob_set_exception (COB_EC_HTTP_IMP);
	}

	curl_easy_cleanup (curl);
	free (url_copy);

	if (status_code && status_code->data && status_code->attr) {
		memset (status_code->data, 0, status_code->size);
		if (COB_FIELD_BINARY_SWAP (status_code)) {
			unsigned char *s;
			cob_s64_t val64 = (cob_s64_t) http_code;
			cob_s64_t fsiz = 8 - status_code->size;
			val64 = COB_BSWAP_64 (val64);
			s = (unsigned char *) &val64 + fsiz;
			memcpy (status_code->data, s, status_code->size);
		} else {
			cob_s32_t val32 = (cob_s32_t) http_code;
			if (status_code->size >= (long) sizeof (val32)) {
				memcpy (status_code->data, &val32, sizeof (val32));
			} else {
				memcpy (status_code->data, &val32, status_code->size);
			}
		}
	}
#else  /* !WITH_CURL */
	COB_UNUSED (url);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif /* WITH_CURL */
}
