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

#define COB_HTTP_METHOD_GET	0
#define COB_HTTP_METHOD_POST	1
#define COB_HTTP_METHOD_PUT	2
#define COB_HTTP_METHOD_PATCH	3
#define COB_HTTP_METHOD_DELETE	4

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

static struct curl_slist *
build_headers (cob_field *header_count, cob_field *header_entries)
{
	struct curl_slist	*headers = NULL;
	int			i, entry_count;
	size_t			entry_size;
	cob_u64_t		raw = 0;
	size_t			sz;

	if (!header_count || !header_count->data || header_count->size == 0
	    || !header_entries || !header_entries->data
	    || header_entries->size == 0) {
		return NULL;
	}

	sz = header_count->size > 8 ? 8 : header_count->size;
	memcpy (&raw, header_count->data, sz);
	if (COB_FIELD_BINARY_SWAP (header_count)) {
		if (sz <= 2) {
			raw = COB_BSWAP_16 ((cob_u16_t) raw);
		} else if (sz <= 4) {
			raw = COB_BSWAP_32 ((cob_u32_t) raw);
		} else {
			raw = COB_BSWAP_64 (raw);
		}
	}
	entry_count = (int) raw;

	entry_size = header_entries->size;
	if (entry_size > (size_t) header_count->size) {
		entry_size -= (size_t) header_count->size;
		entry_size /= 20;	/* OCCURS 20 — fixed per convention */
	}

	for (i = 0; i < entry_count && entry_size > 0; i++) {
		const unsigned char *entry = header_entries->data
					    + (size_t) header_count->size
					    + (size_t) i * entry_size;
		const unsigned char *hdr_name = entry;
		const unsigned char *hdr_value = entry + 256;
		char		name_buf[257];
		char		value_buf[257];
		char		*header_str;
		size_t		name_len, value_len;

		memcpy (name_buf, hdr_name, 256);
		name_buf[256] = '\0';
		name_len = strlen (name_buf);
		while (name_len > 0 && name_buf[name_len - 1] == ' ') {
			name_buf[--name_len] = '\0';
		}

		memcpy (value_buf, hdr_value, 256);
		value_buf[256] = '\0';
		value_len = strlen (value_buf);
		while (value_len > 0 && value_buf[value_len - 1] == ' ') {
			value_buf[--value_len] = '\0';
		}

		if (name_len == 0 || value_len == 0) {
			continue;
		}

		header_str = malloc (name_len + value_len + 3);
		if (!header_str) {
			continue;
		}
		memcpy (header_str, name_buf, name_len);
		header_str[name_len] = ':';
		header_str[name_len + 1] = ' ';
		memcpy (header_str + name_len + 2, value_buf, value_len);
		header_str[name_len + value_len + 2] = '\0';

		headers = curl_slist_append (headers, header_str);
		free (header_str);
	}

	return headers;
}

static void
write_status_code (cob_field *status_code, long http_code)
{
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
}

static void
cob_http_perform (cob_field *url, cob_field *request_body,
		  struct curl_slist *headers,
		  cob_field *response_body, cob_field *status_code,
		  int method)
{
	CURL		*curl;
	CURLcode	res;
	long		http_code = 0;
	struct write_mem chunk;
	char		*url_copy;
	size_t		url_len;
	char		*body_copy = NULL;
	size_t		body_len = 0;

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

	if (request_body && request_body->data && request_body->size > 0) {
		body_len = request_body->size;
		body_copy = malloc (body_len + 1);
		if (body_copy) {
			memcpy (body_copy, request_body->data, body_len);
			body_copy[body_len] = '\0';
			while (body_len > 0 && body_copy[body_len - 1] == ' ') {
				body_copy[--body_len] = '\0';
			}
		}
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
		free (body_copy);
		cob_set_exception (COB_EC_HTTP_IMP);
		return;
	}

	curl_easy_setopt (curl, CURLOPT_URL, url_copy);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *) &chunk);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 30L);

	switch (method) {
	case COB_HTTP_METHOD_POST:
		curl_easy_setopt (curl, CURLOPT_POST, 1L);
		if (body_copy) {
			curl_easy_setopt (curl, CURLOPT_POSTFIELDS, body_copy);
			curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE,
					  (long) body_len);
		}
		break;
	case COB_HTTP_METHOD_PUT:
		curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, "PUT");
		if (body_copy) {
			curl_easy_setopt (curl, CURLOPT_POSTFIELDS, body_copy);
			curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE,
					  (long) body_len);
		}
		break;
	case COB_HTTP_METHOD_PATCH:
		curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, "PATCH");
		if (body_copy) {
			curl_easy_setopt (curl, CURLOPT_POSTFIELDS, body_copy);
			curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE,
					  (long) body_len);
		}
		break;
	case COB_HTTP_METHOD_DELETE:
		curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, "DELETE");
		break;
	case COB_HTTP_METHOD_GET:
	default:
		break;
	}

	if (headers) {
		curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
	}

	res = curl_easy_perform (curl);
	if (res == CURLE_OK) {
		curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
	} else {
		http_code = -1;
		cob_set_exception (COB_EC_HTTP_IMP);
	}

	curl_easy_cleanup (curl);
	free (url_copy);
	free (body_copy);

	write_status_code (status_code, http_code);
}

#endif /* WITH_CURL */

void
cob_http_get (cob_field *url, cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	cob_http_perform (url, NULL, NULL, response_body, status_code,
			  COB_HTTP_METHOD_GET);
#else
	COB_UNUSED (url);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif
}

void
cob_http_post (cob_field *url, cob_field *request_body,
		cob_field *header_count, cob_field *header_entries,
		cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	struct curl_slist	*headers = NULL;

	headers = build_headers (header_count, header_entries);
	cob_http_perform (url, request_body, headers,
			  response_body, status_code,
			  COB_HTTP_METHOD_POST);
	if (headers) {
		curl_slist_free_all (headers);
	}
#else
	COB_UNUSED (url);
	COB_UNUSED (request_body);
	COB_UNUSED (header_count);
	COB_UNUSED (header_entries);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif
}

void
cob_http_put (cob_field *url, cob_field *request_body,
	       cob_field *header_count, cob_field *header_entries,
	       cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	struct curl_slist	*headers = NULL;

	headers = build_headers (header_count, header_entries);
	cob_http_perform (url, request_body, headers,
			  response_body, status_code,
			  COB_HTTP_METHOD_PUT);
	if (headers) {
		curl_slist_free_all (headers);
	}
#else
	COB_UNUSED (url);
	COB_UNUSED (request_body);
	COB_UNUSED (header_count);
	COB_UNUSED (header_entries);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif
}

void
cob_http_patch (cob_field *url, cob_field *request_body,
		 cob_field *header_count, cob_field *header_entries,
		 cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	struct curl_slist	*headers = NULL;

	headers = build_headers (header_count, header_entries);
	cob_http_perform (url, request_body, headers,
			  response_body, status_code,
			  COB_HTTP_METHOD_PATCH);
	if (headers) {
		curl_slist_free_all (headers);
	}
#else
	COB_UNUSED (url);
	COB_UNUSED (request_body);
	COB_UNUSED (header_count);
	COB_UNUSED (header_entries);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif
}

void
cob_http_delete (cob_field *url,
		  cob_field *header_count, cob_field *header_entries,
		  cob_field *response_body, cob_field *status_code)
{
#if defined (WITH_CURL)
	struct curl_slist	*headers = NULL;

	headers = build_headers (header_count, header_entries);
	cob_http_perform (url, NULL, headers,
			  response_body, status_code,
			  COB_HTTP_METHOD_DELETE);
	if (headers) {
		curl_slist_free_all (headers);
	}
#else
	COB_UNUSED (url);
	COB_UNUSED (header_count);
	COB_UNUSED (header_entries);
	COB_UNUSED (response_body);
	COB_UNUSED (status_code);
	cob_set_exception (COB_EC_HTTP_IMP);
#endif
}
