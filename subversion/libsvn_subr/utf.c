/*
 * utf.c:  UTF-8 conversion routines
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <string.h>
#include <ctype.h>
#include <assert.h>

#include <apr_strings.h>
#include <apr_lib.h>
#include <apr_xlate.h>
#include <apr_thread_proc.h>

#include "svn_string.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_utf.h"
#include "utf_impl.h"



#define SVN_UTF_NTOU_XLATE_HANDLE "svn-utf-ntou-xlate-handle"
#define SVN_UTF_UTON_XLATE_HANDLE "svn-utf-uton-xlate-handle"

#if APR_HAS_THREADS
static apr_thread_mutex_t *xlate_handle_mutex = NULL;
#endif

/* The xlate handle cache is a global hash table with linked lists of xlate
 * handles.  In multi-threaded environments, a thread "borrows" an xlate
 * handle from the cache during a translation and puts it back afterwards.
 * This avoids holding a global lock for all translations.
 * If there is no handle for a particular key when needed, a new is
 * handle is created and put in the cache after use.
 * This means that there will be at most N handles open for a key, where N
 * is the number of simultanous handles in use for that key. */

typedef struct xlate_handle_node_t {
  apr_xlate_t *handle;
  struct xlate_handle_node_t *next;
} xlate_handle_node_t;
static apr_hash_t *xlate_handle_hash = NULL;

/* Clean up the xlate handle cache. */
static apr_status_t
xlate_cleanup (void *arg)
{
  /* We set the cache variables to NULL so that translation works in other
     cleanup functions, even if it isn't cached then. */
#if APR_HAS_THREADS
  apr_thread_mutex_destroy (xlate_handle_mutex);
  xlate_handle_mutex = NULL;
#endif
  xlate_handle_hash = NULL;

  return APR_SUCCESS;
}

void
svn_utf_initialize (void)
{
  apr_pool_t *pool;
#if APR_HAS_THREADS
  apr_thread_mutex_t *mutex;
#endif

  if (!xlate_handle_hash)
    {
      /* We create our own pool, which we protect with the mutex.
         It will be destroyed during APR termination. */
      pool = svn_pool_create (NULL);
#if APR_HAS_THREADS
      if (apr_thread_mutex_create (&mutex, APR_THREAD_MUTEX_DEFAULT, pool)
          == APR_SUCCESS)
        xlate_handle_mutex = mutex;
      else
        return;
#endif
      
      xlate_handle_hash = apr_hash_make (pool);
      apr_pool_cleanup_register (pool, NULL, xlate_cleanup,
                                 apr_pool_cleanup_null);
    }
}

/* Return an apr_xlate handle for converting from FROMPAGE to
   TOPAGE. Create one if it doesn't exist in USERDATA_KEY. If
   unable to find a handle, or unable to create one because
   apr_xlate_open returned APR_EINVAL, then set *RET to null and
   return SVN_NO_ERROR; if fail for some other reason, return
   error. */
static svn_error_t *
get_xlate_handle_node (xlate_handle_node_t **ret,
                       const char *topage, const char *frompage,
                       const char *userdata_key, apr_pool_t *pool)
{
  xlate_handle_node_t *old_handle = NULL;
  apr_status_t apr_err;

  /* If we already have a handle, just return it. */
  if (userdata_key)
    {
#if APR_HAS_THREADS
      if (xlate_handle_mutex)
        {
          apr_err = apr_thread_mutex_lock (xlate_handle_mutex);
          if (apr_err != APR_SUCCESS)
            return svn_error_create (apr_err, NULL,
                                     "Can't lock charset translation "
                                     "mutex");
          old_handle = apr_hash_get (xlate_handle_hash, userdata_key,
                                     APR_HASH_KEY_STRING);
          if (old_handle)
            {
              /* Remove from the hash table. */
              apr_hash_set (xlate_handle_hash, userdata_key,
                            APR_HASH_KEY_STRING, old_handle->next);
              old_handle->next = NULL;
              apr_err = apr_thread_mutex_unlock (xlate_handle_mutex);
              if (apr_err != APR_SUCCESS)
                return svn_error_create (apr_err, NULL,
                                         "Can't unlock charset "
                                         "translation mutex");
              *ret = old_handle;
              return SVN_NO_ERROR;
            }
        }
#else /* ! APR_HAS_THREADS */
      if (xlate_handle_hash)
        {
          old_handle = apr_hash_get(xlate_handle_hash, userdata_key,
                                    APR_HASH_KEY_STRING);
          if (old_handle)
            {
              *ret = old_handle;
              return SVN_NO_ERROR;
            }
        }
#endif
    }

  /* Note that we still have the mutex locked, so we can use the global
     pool for creating the new xlate handle. */

  /* Use the correct pool for creating the handle. */
  if (userdata_key && xlate_handle_hash)
    pool = apr_hash_pool_get (xlate_handle_hash);

  /* Try to create one. */
  *ret = apr_palloc (pool, sizeof(xlate_handle_node_t));
  apr_err = apr_xlate_open (&(**ret).handle, topage, frompage, pool);
  (**ret).next = NULL;

  /* Don't need the lock anymore. */
#if APR_HAS_THREADS
  if (xlate_handle_mutex)
    {
      apr_status_t unlock_err = apr_thread_mutex_unlock (xlate_handle_mutex);
      if (unlock_err != APR_SUCCESS)
        return svn_error_create (unlock_err, NULL,
                                 "Can't unlock charset translation "
                                 "mutex");
    }
#endif

  if (APR_STATUS_IS_EINVAL (apr_err) || APR_STATUS_IS_ENOTIMPL (apr_err))
    {
      (*ret)->handle = NULL;
      return SVN_NO_ERROR;
    }
  if (apr_err != APR_SUCCESS)
    /* Can't use svn_error_wrap_apr here because it calls functions in
       this file, leading to infinite recursion. */
    return svn_error_createf
      (apr_err, NULL, "Can't create a converter from '%s' to '%s'",
       (topage == APR_LOCALE_CHARSET ? "native" : topage),
       (frompage == APR_LOCALE_CHARSET ? "native" : frompage));

  return SVN_NO_ERROR;
}

/* Put back NODE into the xlate handle cache for use by other calls.
   Ignore errors related to locking/unlocking the mutex.
   ### Mutex errors here are very weird. Should we handle them "correctly"
   ### even if that complicates error handling in the routines below? */
static void
put_xlate_handle_node (xlate_handle_node_t *node,
                       const char *userdata_key)
{
  assert (node->next == NULL);
  if (!userdata_key)
    return;
#if APR_HAS_THREADS
  if (xlate_handle_mutex)
    {
      if (apr_thread_mutex_lock (xlate_handle_mutex) != APR_SUCCESS)
        abort ();
      node->next = apr_hash_get (xlate_handle_hash, userdata_key,
                                 APR_HASH_KEY_STRING);
      apr_hash_set (xlate_handle_hash, userdata_key, APR_HASH_KEY_STRING,
                    node);
      if (apr_thread_mutex_unlock (xlate_handle_mutex) != APR_SUCCESS)
        abort ();
    }
#endif
}

/* Return the apr_xlate handle for converting native characters to UTF-8. */
static svn_error_t *
get_ntou_xlate_handle_node (xlate_handle_node_t **ret, apr_pool_t *pool)
{
  return get_xlate_handle_node (ret, "UTF-8", APR_LOCALE_CHARSET,
                                SVN_UTF_NTOU_XLATE_HANDLE, pool);
}


/* Return the apr_xlate handle for converting UTF-8 to native characters.
   Create one if it doesn't exist.  If unable to find a handle, or
   unable to create one because apr_xlate_open returned APR_EINVAL, then
   set *RET to null and return SVN_NO_ERROR; if fail for some other
   reason, return error. */
static svn_error_t *
get_uton_xlate_handle_node (xlate_handle_node_t **ret, apr_pool_t *pool)
{
  return get_xlate_handle_node (ret, APR_LOCALE_CHARSET, "UTF-8",
                                SVN_UTF_UTON_XLATE_HANDLE, pool);
}


/* Convert SRC_LENGTH bytes of SRC_DATA in CONVSET, store the result
   in *DEST, which is allocated in POOL. */
static svn_error_t *
convert_to_stringbuf (apr_xlate_t *convset,
                      const char *src_data,
                      apr_size_t src_length,
                      svn_stringbuf_t **dest,
                      apr_pool_t *pool)
{
  apr_size_t buflen = src_length;
  apr_status_t apr_err;
  apr_size_t srclen = src_length;
  apr_size_t destlen = 0;
  char *destbuf;

  /* Initialize *DEST to an empty stringbuf. */
  *dest = svn_stringbuf_create ("", pool);
  destbuf = (*dest)->data;

  /* Not only does it not make sense to convert an empty string, but
     apr-iconv is quite unreasonable about not allowing that. */
  if (src_length == 0)
    return SVN_NO_ERROR;

  do 
    {
      /* A 1:2 ratio of input characters to output characters should
         be enough for most translations, and conveniently enough, if
         it isn't, we'll grow the buffer size by 2 again. */
      if (destlen == 0)
        buflen *= 2;

      /* Ensure that *DEST has sufficient storage for the translated
         result. */
      svn_stringbuf_ensure (*dest, buflen + 1);

      /* Update the destination buffer pointer to the first character
         after already-converted output. */
      destbuf = (*dest)->data + (*dest)->len;

      /* Set up state variables for xlate. */
      destlen = buflen - (*dest)->len;

      /* Attempt the conversion. */
      apr_err = apr_xlate_conv_buffer (convset, 
                                       src_data + (src_length - srclen), 
                                       &srclen,
                                       destbuf, 
                                       &destlen);

      /* Now, update the *DEST->len to track the amount of output data
         churned out so far from this loop. */
      (*dest)->len += ((buflen - (*dest)->len) - destlen);

    } while (! apr_err && srclen);

  /* If we exited the loop with an error, return the error. */
  if (apr_err)
    /* Can't use svn_error_wrap_apr here because it calls functions in
       this file, leading to infinite recursion. */
    return svn_error_create (apr_err, NULL, "Can't recode string");
  
  /* Else, exited due to success.  Trim the result buffer down to the
     right length. */
  (*dest)->data[(*dest)->len] = '\0';

  return SVN_NO_ERROR;
}


/* Return APR_EINVAL if the first LEN bytes of DATA contain anything
   other than seven-bit, non-control (except for whitespace) ASCII
   characters, finding the error pool from POOL.  Otherwise, return
   SVN_NO_ERROR. */
static svn_error_t *
check_non_ascii (const char *data, apr_size_t len, apr_pool_t *pool)
{
  const char *data_start = data;

  for (; len > 0; --len, data++)
    {
      if ((! apr_isascii (*data))
          || ((! apr_isspace (*data))
              && apr_iscntrl (*data)))
        {
          /* Show the printable part of the data, followed by the
             decimal code of the questionable character.  Because if a
             user ever gets this error, she's going to have to spend
             time tracking down the non-ASCII data, so we want to help
             as much as possible.  And yes, we just call the unsafe
             data "non-ASCII", even though the actual constraint is
             somewhat more complex than that. */ 

          if (data - data_start)
            {
              const char *error_data
                = apr_pstrndup (pool, data_start, (data - data_start));

              return svn_error_createf
                (APR_EINVAL, NULL,
                 "Safe data:\n"
                 "\"%s\"\n"
                 "... was followed by non-ASCII byte %d.\n"
                 "\n"
                 "Non-ASCII character detected (see above), "
                 "and unable to convert to/from UTF-8",
                 error_data, *((const unsigned char *) data));
            }
          else
            {
              return svn_error_createf
                (APR_EINVAL, NULL,
                 "Non-ASCII character (code %d) detected, "
                 "and unable to convert to/from UTF-8",
                 *((const unsigned char *) data));
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Construct an error with a suitable message to describe the invalid UTF-8
 * sequence DATA of length LEN (which may have embedded NULLs).  We can't
 * simply print the data, almost by definition we don't really know how it
 * is encoded.
 */
static svn_error_t *
invalid_utf8 (const char *data, apr_size_t len, apr_pool_t *pool)
{
  const char *last = svn_utf__last_valid (data, len);
  const char *msg = "Valid UTF-8 data\n(hex:";
  int i, valid, invalid;

  /* We will display at most 24 valid octets (this may split a leading
     multi-byte character) as that should fit on one 80 character line. */
  valid = last - data;
  if (valid > 24)
    valid = 24;
  for (i = 0; i < valid; ++i)
    msg = apr_pstrcat (pool, msg, apr_psprintf (pool, " %02x",
                                                (unsigned char)last[i-valid]),
                       NULL);
  msg = apr_pstrcat (pool, msg,
                     ")\nfollowed by invalid UTF-8 sequence\n(hex:", NULL);

  /* 4 invalid octets will guarantee that the faulty octet is displayed */
  invalid = data + len - last;
  if (invalid > 4)
    invalid = 4;
  for (i = 0; i < invalid; ++i)
    msg = apr_pstrcat (pool, msg, apr_psprintf (pool, " %02x",
                                                (unsigned char)last[i]), NULL);
  msg = apr_pstrcat (pool, msg, ")", NULL);

  return svn_error_create (APR_EINVAL, NULL, msg);
}

/* Verify that the sequence DATA of length LEN is valid UTF-8 */
static svn_error_t *
check_utf8 (const char *data, apr_size_t len, apr_pool_t *pool)
{
  if (! svn_utf__is_valid (data, len))
    return invalid_utf8 (data, len, pool);
  return SVN_NO_ERROR;
}

/* Verify that the NULL terminated sequence DATA is valid UTF-8 */
static svn_error_t *
check_cstring_utf8 (const char *data, apr_pool_t *pool)
{

  if (! svn_utf__cstring_is_valid (data))
    return invalid_utf8 (data, strlen (data), pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_stringbuf_to_utf8 (svn_stringbuf_t **dest,
                           const svn_stringbuf_t *src,
                           apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_ntou_xlate_handle_node (&node, pool));

  if (node->handle)
    {
      err = convert_to_stringbuf (node->handle, src->data, src->len, dest,
                                  pool);
      put_xlate_handle_node (node, SVN_UTF_NTOU_XLATE_HANDLE);
      SVN_ERR (err);
      return check_utf8 ((*dest)->data, (*dest)->len, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_stringbuf_dup (src, pool);
      return SVN_NO_ERROR;
    }
}


svn_error_t *
svn_utf_string_to_utf8 (const svn_string_t **dest,
                        const svn_string_t *src,
                        apr_pool_t *pool)
{
  svn_stringbuf_t *destbuf;
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_ntou_xlate_handle_node (&node, pool));

  if (node->handle)
    {
      err = convert_to_stringbuf (node->handle, src->data, src->len, 
                                  &destbuf, pool);
      put_xlate_handle_node (node, SVN_UTF_NTOU_XLATE_HANDLE);
      SVN_ERR (err);
      SVN_ERR (check_utf8 (destbuf->data, destbuf->len, pool));
      *dest = svn_string_create_from_buf (destbuf, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_string_dup (src, pool);
    }

  return SVN_NO_ERROR;
}


/* Common implementation for svn_utf_cstring_to_utf8,
   svn_utf_cstring_to_utf8_ex, svn_utf_cstring_from_utf8 and
   svn_utf_cstring_from_utf8_ex. Convert SRC to DEST using CONVSET as
   the translator and allocating from POOL. */
static svn_error_t *
convert_cstring (const char **dest,
                 const char *src,
                 apr_xlate_t *convset,
                 apr_pool_t *pool)
{
  if (convset)
    {
      svn_stringbuf_t *destbuf;
      SVN_ERR (convert_to_stringbuf (convset, src, strlen (src),
                                     &destbuf, pool));
      *dest = destbuf->data;
    }
  else
    {
      apr_size_t len = strlen (src);
      SVN_ERR (check_non_ascii (src, len, pool));
      *dest = apr_pstrmemdup (pool, src, len);
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_to_utf8 (const char **dest,
                         const char *src,
                         apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_ntou_xlate_handle_node (&node, pool));
  err = convert_cstring (dest, src, node->handle, pool);
  put_xlate_handle_node (node, SVN_UTF_NTOU_XLATE_HANDLE);
  SVN_ERR (err);
  SVN_ERR (check_cstring_utf8 (*dest, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_to_utf8_ex (const char **dest,
                            const char *src,
                            const char *frompage,
                            const char *convset_key,
                            apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_xlate_handle_node (&node, "UTF-8", frompage, convset_key, pool));
  err = convert_cstring (dest, src, node->handle, pool);
  put_xlate_handle_node (node, convset_key);
  SVN_ERR (err);
  SVN_ERR (check_cstring_utf8 (*dest, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_stringbuf_from_utf8 (svn_stringbuf_t **dest,
                             const svn_stringbuf_t *src,
                             apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_uton_xlate_handle_node (&node, pool));

  if (node->handle)
    {
      SVN_ERR (check_utf8 (src->data, src->len, pool));
      err = convert_to_stringbuf (node->handle, src->data, src->len, dest, pool);
      put_xlate_handle_node (node, SVN_UTF_UTON_XLATE_HANDLE);
      return err;
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_stringbuf_dup (src, pool);
      return SVN_NO_ERROR;
    }
}


svn_error_t *
svn_utf_string_from_utf8 (const svn_string_t **dest,
                          const svn_string_t *src,
                          apr_pool_t *pool)
{
  svn_stringbuf_t *dbuf;
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_uton_xlate_handle_node (&node, pool));

  if (node->handle)
    {
      SVN_ERR (check_utf8 (src->data, src->len, pool));
      err = convert_to_stringbuf (node->handle, src->data, src->len,
                                  &dbuf, pool);
      put_xlate_handle_node (node, SVN_UTF_UTON_XLATE_HANDLE);
      SVN_ERR (err);
      *dest = svn_string_create_from_buf (dbuf, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_string_dup (src, pool);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8 (const char **dest,
                           const char *src,
                           apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_uton_xlate_handle_node (&node, pool));
  SVN_ERR (check_utf8 (src, strlen (src), pool));
  err = convert_cstring (dest, src, node->handle, pool);
  put_xlate_handle_node (node, SVN_UTF_UTON_XLATE_HANDLE);
  SVN_ERR (err);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8_ex (const char **dest,
                              const char *src,
                              const char *topage,
                              const char *convset_key,
                              apr_pool_t *pool)
{
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_xlate_handle_node (&node, topage, "UTF-8", convset_key, pool));
  SVN_ERR (check_utf8 (src, strlen (src), pool));
  err = convert_cstring (dest, src, node->handle, pool);
  put_xlate_handle_node (node, convset_key);

  return err;
}


const char *
svn_utf__cstring_from_utf8_fuzzy (const char *src,
                                  apr_pool_t *pool,
                                  svn_error_t *(*convert_from_utf8)
                                  (const char **, const char *, apr_pool_t *))
{
  const char *src_orig = src;
  apr_size_t new_len = 0;
  char *new;
  const char *new_orig;
  svn_error_t *err;

  /* First count how big a dest string we'll need. */
  while (*src)
    {
      if (! apr_isascii (*src))
        new_len += 5;  /* 5 slots, for "?\XXX" */
      else
        new_len += 1;  /* one slot for the 7-bit char */

      src++;
    }

  /* Allocate that amount. */
  new = apr_palloc (pool, new_len + 1);

  new_orig = new;

  /* And fill it up. */
  while (*src_orig)
    {
      if (! apr_isascii (*src_orig))
        {
          sprintf (new, "?\\%03u", (unsigned char) *src_orig);
          new += 5;
        }
      else
        {
          *new = *src_orig;
          new += 1;
        }

      src_orig++;
    }

  *new = '\0';

  /* Okay, now we have a *new* UTF-8 string, one that's guaranteed to
     contain only 7-bit bytes :-).  Recode to native... */
  err = convert_from_utf8 (((const char **) &new), new_orig, pool);

  if (err)
    {
      svn_error_clear (err);
      return new_orig;
    }
  else
    return new;

  /* ### Check the client locale, maybe we can avoid that second
   * conversion!  See Ulrich Drepper's patch at
   * http://subversion.tigris.org/issues/show_bug.cgi?id=807.
   */
}


const char *
svn_utf_cstring_from_utf8_fuzzy (const char *src,
                                 apr_pool_t *pool)
{
  return svn_utf__cstring_from_utf8_fuzzy (src, pool,
                                           svn_utf_cstring_from_utf8);
}


svn_error_t *
svn_utf_cstring_from_utf8_stringbuf (const char **dest,
                                     const svn_stringbuf_t *src,
                                     apr_pool_t *pool)
{
  svn_stringbuf_t *destbuf;

  SVN_ERR (svn_utf_stringbuf_from_utf8 (&destbuf, src, pool));
  *dest = destbuf->data;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8_string (const char **dest,
                                  const svn_string_t *src,
                                  apr_pool_t *pool)
{
  svn_stringbuf_t *dbuf;
  xlate_handle_node_t *node;
  svn_error_t *err;

  SVN_ERR (get_uton_xlate_handle_node (&node, pool));

  if (node->handle)
    {
      SVN_ERR (check_utf8 (src->data, src->len, pool));
      err = convert_to_stringbuf (node->handle, src->data, src->len,
                                  &dbuf, pool);
      put_xlate_handle_node (node, SVN_UTF_UTON_XLATE_HANDLE);
      SVN_ERR (err);
      *dest = dbuf->data;
      return SVN_NO_ERROR;
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = apr_pstrmemdup (pool, src->data, src->len);
      return SVN_NO_ERROR;
    }
}
