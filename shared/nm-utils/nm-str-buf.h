/* NetworkManager -- Network link manager
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2018 Red Hat, Inc.
 */

#include "nm-shared-utils.h"
#include "nm-secret-utils.h"

/*****************************************************************************/

/* NMStrBuf is not unlike GString. The main difference is that it can use
 * nm_explicit_bzero() when growing the buffer. */
typedef struct {
	char *_str;
	union {
		gsize len;
		const gsize _len;
	};
	gsize _allocated;
	bool _do_bzero_mem;
} NMStrBuf;

static inline void
_nm_str_buf_assert (NMStrBuf *strbuf)
{
	nm_assert (strbuf);
	nm_assert (strbuf->_str);
	nm_assert (strbuf->_allocated > 0);
	nm_assert (strbuf->_len <= strbuf->_allocated);
}

static inline void
nm_str_buf_init (NMStrBuf *strbuf,
                 gsize len,
                 bool do_bzero_mem)
{
	strbuf->_do_bzero_mem = do_bzero_mem;
	strbuf->_allocated = len;
	strbuf->_str = g_malloc (len);
	strbuf->_len = 0;

	_nm_str_buf_assert (strbuf);
}

static inline void
nm_str_buf_expand (NMStrBuf *strbuf,
                   gsize new_allocated)
{
	_nm_str_buf_assert (strbuf);

	/* currently, this only supports strictly growing the buffer. */
	nm_assert (new_allocated > strbuf->_allocated);

	strbuf->_str = nm_secret_mem_realloc (strbuf->_str, strbuf->_do_bzero_mem, strbuf->_allocated, new_allocated);
	strbuf->_allocated = new_allocated;
}

static inline void
nm_str_buf_maybe_expand (NMStrBuf *strbuf,
                         gsize reserve)
{
	_nm_str_buf_assert (strbuf);
	nm_assert (reserve > 0);

	/* @reserve is the extra space that we require. */
	if (G_UNLIKELY (reserve > strbuf->_allocated - strbuf->_len)) {
		nm_str_buf_expand (strbuf,
		                   nm_utils_get_next_realloc_size (!strbuf->_do_bzero_mem,
		                                                   strbuf->_len + reserve));
	}
}

static inline void
nm_str_buf_append_c (NMStrBuf *strbuf,
                     char ch)
{
	_nm_str_buf_assert (strbuf);

	nm_str_buf_maybe_expand (strbuf, 2);
	strbuf->_str[strbuf->_len++] = ch;
}

static inline void
nm_str_buf_append_c2 (NMStrBuf *strbuf,
                      char ch0,
                      char ch1)
{
	_nm_str_buf_assert (strbuf);

	nm_str_buf_maybe_expand (strbuf, 3);
	strbuf->_str[strbuf->_len++] = ch0;
	strbuf->_str[strbuf->_len++] = ch1;
}

static inline void
nm_str_buf_append_c4 (NMStrBuf *strbuf,
                      char ch0,
                      char ch1,
                      char ch2,
                      char ch3)
{
	_nm_str_buf_assert (strbuf);

	nm_str_buf_maybe_expand (strbuf, 5);
	strbuf->_str[strbuf->_len++] = ch0;
	strbuf->_str[strbuf->_len++] = ch1;
	strbuf->_str[strbuf->_len++] = ch2;
	strbuf->_str[strbuf->_len++] = ch3;
}

static inline void
nm_str_buf_append_len (NMStrBuf *strbuf,
                       const char *str,
                       gsize len)
{
	_nm_str_buf_assert (strbuf);

	if (len > 0) {
		nm_str_buf_maybe_expand (strbuf, len + 1);
		memcpy (&strbuf->_str[strbuf->_len], str, len);
		strbuf->_len += len;
	}
}

static inline void
nm_str_buf_append (NMStrBuf *strbuf,
                   const char *str)
{
	_nm_str_buf_assert (strbuf);
	nm_assert (str);

	nm_str_buf_append_len (strbuf, str, strlen (str));
}

_nm_printf (2, 3)
static inline void
nm_str_buf_append_printf (NMStrBuf *strbuf,
                          const char *format,
                          ...)
{
	va_list args;
	gsize len;
	int l;

	_nm_str_buf_assert (strbuf);

	va_start (args, format);
	len = g_printf_string_upper_bound (format, args);
	va_end (args);

	nm_str_buf_maybe_expand (strbuf, len);

	va_start (args, format);
	l = g_vsnprintf (&strbuf->_str[strbuf->_len], len, format, args);
	va_end (args);

	nm_assert (l >= 0);
	nm_assert (strlen (&strbuf->_str[strbuf->_len]) == (gsize) l);
	nm_assert ((gsize) l < len);

	if (l > 0)
		strbuf->_len += (gsize) l;
}

/**
 * nm_str_buf_get_str:
 * @strbuf: the #NMStrBuf instance
 *
 * Returns the NUL terminated internal string.
 *
 * While constructing the string, the intermediate buffer
 * is not NUL terminated (this makes it different from GString).
 * Usually, one would build the string and retrieve it at the
 * end with nm_str_buf_finalize(). This returns the NUL terminated
 * buffer that was  so far. Afterwards, you can still
 * append more data to the buffer.
 *
 * Returns: (transfer none): the internal string. The string
 *   is of length "strbuf->len", which may be larger if the
 *   returned string contains NUL characters (binary). The terminating
 *   NUL character is always present after "strbuf->len" characters.
 */
static inline const char *
nm_str_buf_get_str (NMStrBuf *strbuf)
{
	_nm_str_buf_assert (strbuf);

	nm_str_buf_maybe_expand (strbuf, 1);
	strbuf->_str[strbuf->_len] = '\0';
	return strbuf->_str;
}

/**
 * nm_str_buf_finalize:
 * @strbuf: an initilized #NMStrBuf
 * @out_len: (out): (allow-none): optional output
 *   argument with the length of the returned string.
 *
 * Returns: (transfer full): the string of the buffer
 *   which must be freed by the caller. The @strbuf
 *   is afterwards in undefined state, though it can be
 *   reused after nm_str_buf_init(). */
static inline char *
nm_str_buf_finalize (NMStrBuf *strbuf,
                     gsize *out_len)
{
	_nm_str_buf_assert (strbuf);

	nm_str_buf_maybe_expand (strbuf, 1);
	strbuf->_str[strbuf->_len] = '\0';

	NM_SET_OUT (out_len, strbuf->_len);

	/* the buffer is in invalid state afterwards, however, we clear it
	 * so far, that nm_auto_str_buf and nm_str_buf_destroy() is happy.  */
	return g_steal_pointer (&strbuf->_str);
}

/**
 * nm_str_buf_destroy:
 * @strbuf: an initialized #NMStrBuf
 *
 * Frees the associated memory of @strbuf. The buffer
 * afterwards is in undefined state, but can be re-initialized
 * with nm_str_buf_init().
 */
static inline void
nm_str_buf_destroy (NMStrBuf *strbuf)
{
	if (!strbuf->_str)
		return;
	_nm_str_buf_assert (strbuf);
	if (strbuf->_do_bzero_mem)
		nm_explicit_bzero (strbuf->_str, strbuf->_allocated);
	g_free (strbuf->_str);

	/* the buffer is in invalid state afterwards, however, we clear it
	 * so far, that nm_auto_str_buf is happy when calling
	 * nm_str_buf_destroy() again.  */
	strbuf->_str = NULL;
}

#define nm_auto_str_buf    nm_auto (nm_str_buf_destroy)
