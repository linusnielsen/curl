/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 2012, Linus Nielsen Feltzing, <linus@haxx.se>
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at http://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include "setup.h"

#include <curl/curl.h>

#include "urldata.h"
#include "url.h"
#include "progress.h"
#include "multiif.h"
#include "sendf.h"
#include "rawstr.h"
#include "bundles.h"
#include "conncache.h"

/* The last #include file should be: */
#include "memdebug.h"

#define CONNECTION_HASH_SIZE 97

static void free_bundle_hash_entry(void *freethis)
{
  struct connectbundle *b = (struct connectbundle *) freethis;

  Curl_bundle_destroy(b);
}

struct conncache *Curl_conncache_init(int type)
{
  struct conncache *connc;

  connc = calloc(1, sizeof(struct conncache));
  if(!connc)
    return NULL;

  connc->hash = Curl_hash_alloc(CONNECTION_HASH_SIZE, Curl_hash_str,
                                Curl_str_key_compare, free_bundle_hash_entry);

  if(!connc->hash) {
    free(connc);
    return NULL;
  }

  connc->type = type;

  return connc;
}

void Curl_conncache_destroy(struct conncache *connc)
{
  Curl_hash_destroy(connc->hash);
  free(connc);
}

struct connectbundle *Curl_conncache_find_bundle(struct conncache *connc,
                                                 char *hostname)
{
  struct connectbundle *bundle = NULL;

  if(connc)
    bundle = Curl_hash_pick(connc->hash, hostname, strlen(hostname)+1);

  return bundle;
}

bool Curl_conncache_add_bundle(struct conncache *connc,
                               char *hostname,
                               struct connectbundle *bundle)
{
  void *p;

  p = Curl_hash_add(connc->hash, hostname, strlen(hostname)+1, bundle);

  return p?TRUE:FALSE;
}

void Curl_conncache_remove_bundle(struct conncache *connc,
                                  struct connectbundle *bundle)
{
  struct curl_hash_iterator iter;
  struct curl_hash_element *he;

  if(!connc)
    return;

  Curl_hash_start_iterate(connc->hash, &iter);

  he = Curl_hash_next_element(&iter);
  while(he) {
    if(he->ptr == bundle) {
      /* The bundle is destroyed by the hash destructor function,
         free_bundle_hash_entry() */
      Curl_hash_delete(connc->hash, he->key, he->key_len);
      return;
    }

    he = Curl_hash_next_element(&iter);
  }
}

void Curl_conncache_remove_conn(struct conncache *connc,
                                struct connectdata *conn)
{
  struct connectbundle *bundle = conn->bundle;

  /* The bundle pointer can be NULL, since this function can be called
     due to a failed connection attempt, before being added to a bundle */
  if(bundle) {
    Curl_bundle_remove_conn(bundle, conn);
    if(bundle->num_connections == 0) {
      Curl_conncache_remove_bundle(connc, bundle);
    }
  }
}

/* This function iterates the entire connection cache and calls the
   function func() with the connection pointer as the first argument
   and the supplied 'param' argument as the other */
void Curl_conncache_foreach(struct conncache *connc,
                            void *param,
                            void (*func)(void *conn, void *param))
{
  struct curl_hash_iterator iter;
  struct curl_llist_element *curr;
  struct curl_hash_element *he;

  if(!connc)
    return;

  Curl_hash_start_iterate(connc->hash, &iter);

  he = Curl_hash_next_element(&iter);
  while(he) {
    struct connectbundle *bundle;
    struct connectdata *conn;

    bundle = he->ptr;

    curr = bundle->conn_list->head;
    while(curr) {
      /* Yes, we need to update curr before calling func(), because func()
         might decide to remove the connection */
      conn = curr->ptr;
      curr = curr->next;

      func(conn, param);
    }

    he = Curl_hash_next_element(&iter);
  }
}

#if 0
/* Useful for debugging the connection cache */
void Curl_conncache_print(struct conncache *connc)
{
  struct curl_hash_iterator iter;
  struct curl_llist_element *curr;
  struct curl_hash_element *he;

  if(!connc)
    return;

  fprintf(stderr, "=Bundle cache=\n");

  Curl_hash_start_iterate(connc->hash, &iter);

  he = Curl_hash_next_element(&iter);
  while(he) {
    struct connectbundle *bundle;
    struct connectdata *conn;

    bundle = he->ptr;

    fprintf(stderr, "%s -", he->key);
    curr = bundle->conn_list->head;
    while(curr) {
      conn = curr->ptr;

      fprintf(stderr, " [%p %d]", (void *)conn, conn->inuse);
      curr = curr->next;
    }
    fprintf(stderr, "\n");

    he = Curl_hash_next_element(&iter);
  }
}
#endif
