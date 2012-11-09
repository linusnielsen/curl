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
#include "pipeline.h"
#include "sendf.h"
#include "rawstr.h"

/* The last #include file should be: */
#include "memdebug.h"

struct site_blacklist_entry {
  char *hostname;
  unsigned short port;
};

static void conn_llist_dtor(void *user, void *element)
{
  struct connectdata *data = element;
  (void)user;

  data->bundle = NULL;
}

static void blacklist_llist_dtor(void *user, void *element)
{
  struct site_blacklist_entry *entry = element;
  (void)user;

  Curl_safefree(entry->hostname);
  Curl_safefree(entry);
}

static void pend_llist_dtor(void *user, void *element)
{
  (void)element;
  (void)user;
}

CURLcode Curl_bundle_create(struct SessionHandle *data,
                            struct connectbundle **cb_ptr)
{
  (void)data;
  *cb_ptr = malloc(sizeof(struct connectbundle));
  if(!*cb_ptr)
    return CURLE_OUT_OF_MEMORY;

  (*cb_ptr)->num_connections = 0;
  (*cb_ptr)->server_supports_pipelining = FALSE;

  (*cb_ptr)->conn_list = Curl_llist_alloc((curl_llist_dtor) conn_llist_dtor);
  if(!(*cb_ptr)->conn_list)
    return CURLE_OUT_OF_MEMORY;

  (*cb_ptr)->pend_list = Curl_llist_alloc((curl_llist_dtor) pend_llist_dtor);
  if(!(*cb_ptr)->pend_list)
    return CURLE_OUT_OF_MEMORY;
  return CURLE_OK;
}

void Curl_bundle_destroy(struct SessionHandle *data,
                         struct connectbundle *cb_ptr)
{
  (void)data;
  if(cb_ptr->pend_list)
    Curl_llist_destroy(cb_ptr->pend_list, NULL);
  if(cb_ptr->conn_list)
    Curl_llist_destroy(cb_ptr->conn_list, NULL);
  infof(data, "Curl_bundle_destroy(%p)\n", cb_ptr);
  Curl_safefree(cb_ptr);
}

/* Add a connection to a bundle */
CURLcode Curl_bundle_add_conn(struct SessionHandle *data,
                              struct connectbundle *cb_ptr,
                              struct connectdata *conn)
{
  (void)data;
  if(!Curl_llist_insert_next(cb_ptr->conn_list, cb_ptr->conn_list->tail, conn))
    return CURLE_OUT_OF_MEMORY;

  cb_ptr->num_connections++;
  return CURLE_OK;
}

/* Remove a connection from a bundle */
int Curl_bundle_remove_conn(struct SessionHandle *data,
                            struct connectbundle *cb_ptr,
                            struct connectdata *conn)
{
  struct curl_llist_element *curr;
  (void)data;

  curr = cb_ptr->conn_list->head;
  while(curr) {
    infof(data, "Curl_bundle_remove() %p == %p\n", curr->ptr, conn);
    if(curr->ptr == conn) {
      Curl_llist_remove(cb_ptr->conn_list, curr, NULL);
      cb_ptr->num_connections--;
      infof(data, "Curl_bundle_remove() %d left\n", cb_ptr->num_connections);
      return 1; /* we removed a handle */
    }
    curr = curr->next;
  }
  return 0;
}

static bool pipeline_penalized(struct connectdata *conn)
{
  struct SessionHandle *data = conn->data;
  bool penalized;

  if(data) {
    curl_off_t penalty_size =
      Curl_multi_content_length_penalty_size(data->multi);

    if(penalty_size > 0 && data->req.size > penalty_size)
      penalized = TRUE;
    else
      penalized = FALSE;

    infof(data, "Conn: %x Receive pipe weight: %d, penalized: %d\n",
          conn, data->req.size, penalized);
    return penalized;
  }
  return FALSE;
}

/* Find the best connection in a bundle to use for the next request */
struct connectdata *
Curl_bundle_find_best(struct SessionHandle *data,
                      struct connectbundle *cb_ptr)
{
  struct curl_llist_element *curr;
  struct connectdata *conn;
  struct connectdata *best_conn = NULL;
  size_t pipe_len;
  size_t best_pipe_len = 99;

  (void)data;

  curr = cb_ptr->conn_list->head;
  while(curr) {
    conn = curr->ptr;
    pipe_len = conn->send_pipe->size + conn->recv_pipe->size;

    if(!pipeline_penalized(conn) && pipe_len < best_pipe_len) {
      best_conn = conn;
      best_pipe_len = pipe_len;
    }
    curr = curr->next;
  }

  /* If we haven't found a connection, i.e all pipelines are penalized
     or full, just pick one. The request will then be queued in
     Curl_add_handle_to_pipeline(). */
  if(!best_conn) {
    best_conn = cb_ptr->conn_list->head->ptr;
  }
  return best_conn;
}

/* Add a session handle to the bundle queue */
CURLcode Curl_bundle_add_to_queue(struct SessionHandle *handle,
                                  struct connectbundle *cb_ptr)
{
  if(!Curl_llist_insert_next(cb_ptr->pend_list,
                             cb_ptr->pend_list->tail, handle))
    return CURLE_OUT_OF_MEMORY;

  return CURLE_OK;
}

CURLcode Curl_add_handle_to_pipeline(struct SessionHandle *handle,
                                     struct connectdata *conn)
{
  size_t pipeLen = conn->send_pipe->size + conn->recv_pipe->size;
  struct curl_llist_element *sendhead = conn->send_pipe->head;
  struct curl_llist *pipeline;
  CURLcode rc;
  struct connectbundle *cb_ptr = conn->bundle;

  if(!Curl_isPipeliningEnabled(handle) || pipeLen == 0)
    pipeline = conn->send_pipe;
  else {
    if(cb_ptr->server_supports_pipelining &&
       pipeLen < Curl_multi_max_pipeline_length(conn->data->multi) &&
       !pipeline_penalized(conn))
      pipeline = conn->send_pipe;
    else
      pipeline = cb_ptr->pend_list;
  }

  infof(conn->data, "Adding handle: conn: %p\n", conn);
  infof(conn->data, "Adding handle: send: %d\n", conn->send_pipe->size);
  infof(conn->data, "Adding handle: recv: %d\n", conn->recv_pipe->size);
  if(cb_ptr)
    infof(conn->data, "Adding handle: pend: %d\n", cb_ptr->pend_list->size);
  rc = Curl_addHandleToPipeline(handle, pipeline);

  if(pipeline == conn->send_pipe && sendhead != conn->send_pipe->head) {
    /* this is a new one as head, expire it */
    conn->writechannel_inuse = FALSE; /* not in use yet */
#ifdef DEBUGBUILD
    infof(conn->data, "%p is at send pipe head!\n",
          conn->send_pipe->head->ptr);
#endif
    Curl_expire(conn->send_pipe->head->ptr, 1);
  }

  print_pipeline(conn);

  return rc;
}

/* Move this transfer from the sending list to the receiving list.

   Pay special attention to the new sending list "leader" as it needs to get
   checked to update what sockets it acts on.

*/
void Curl_move_handle_from_send_to_recv_pipe(struct SessionHandle *handle,
                                             struct connectdata *conn)
{
  struct curl_llist_element *curr;

  curr = conn->send_pipe->head;
  while(curr) {
    if(curr->ptr == handle) {
      Curl_llist_move(conn->send_pipe, curr,
                      conn->recv_pipe, conn->recv_pipe->tail);

      if(conn->send_pipe->head) {
        /* Since there's a new easy handle at the start of the send pipeline,
           set its timeout value to 1ms to make it trigger instantly */
        conn->writechannel_inuse = FALSE; /* not used now */
#ifdef DEBUGBUILD
        infof(conn->data, "%p is at send pipe head B!\n",
              conn->send_pipe->head->ptr);
#endif
        Curl_expire(conn->send_pipe->head->ptr, 1);
      }

      /* The receiver's list is not really interesting here since either this
         handle is now first in the list and we'll deal with it soon, or
         another handle is already first and thus is already taken care of */

      break; /* we're done! */
    }
    curr = curr->next;
  }
}

int Curl_check_pend_pipeline(struct connectdata *conn)
{
  int result = 0;
  struct SessionHandle *data = conn->data;
  struct curl_llist_element *sendhead = conn->send_pipe->head;
  size_t pipe_len = conn->send_pipe->size + conn->recv_pipe->size;
  struct connectbundle *cb_ptr = conn->bundle;
  bool is_pipelining = (cb_ptr && cb_ptr->server_supports_pipelining);
  struct curl_llist_element *curr;
  size_t max_pipe_len;

  infof(data, "Curl_check_pend_pipeline %p\n", conn);
  if(is_pipelining)
    infof(data, "Curl_check_pend_pipeline support: %d\n", is_pipelining);
  infof(data, "Curl_check_pend_pipeline pipe_len: %d\n", pipe_len);
  print_pipeline(conn);

  if(is_pipelining)
    max_pipe_len = Curl_multi_max_pipeline_length(data->multi);
  else
    max_pipe_len = 1;

  curr = cb_ptr->pend_list->head;

  while(pipe_len < max_pipe_len && curr) {
    struct SessionHandle *handle;

    Curl_llist_move(cb_ptr->pend_list, curr,
                    conn->send_pipe, conn->send_pipe->tail);
    Curl_pgrsTime(curr->ptr, TIMER_PRETRANSFER);
    handle = curr->ptr;
    Curl_multi_set_easy_connection(handle, conn);
    ++result; /* count how many handles we moved */
    curr = cb_ptr->pend_list->head;
    ++pipe_len;
    infof(conn->data, "Curl_check_pend_pipeline len: %d\n", pipe_len);
    infof(conn->data, "Curl_check_pend_pipeline pendlen: %d\n",
          cb_ptr->pend_list->size);

    if(result) {
      conn->now = Curl_tvnow();
      /* something moved, check for a new send pipeline leader */
      if(sendhead != conn->send_pipe->head) {
        /* this is a new one as head, expire it */
        conn->writechannel_inuse = FALSE; /* not in use yet */
#ifdef DEBUGBUILD
        infof(conn->data, "%p is at send pipe head!\n",
              conn->send_pipe->head->ptr);
#endif
        Curl_expire(conn->send_pipe->head->ptr, 1);
      }
    }
  }

  return result;
}

bool Curl_pipeline_site_blacklisted(struct SessionHandle *handle,
                                    struct connectdata *conn)
{
  struct curl_llist *blacklist = Curl_multi_pipelining_site_bl(handle->multi);
  struct curl_llist_element *curr;
  size_t hostnamelen;

  if(blacklist) {
    hostnamelen = strlen(conn->host.name);

    curr = blacklist->head;
    while(curr) {
      struct site_blacklist_entry *site;

      site = curr->ptr;
      if(Curl_raw_nequal(site->hostname, conn->host.name, hostnamelen) &&
         site->port == conn->remote_port) {
        infof(handle, "Site %s:%d is blacklisted\n",
              conn->host.name, conn->remote_port);
        return TRUE;
      }
      curr = curr->next;
    }
  }

  infof(handle, "Site %s:%d is not blacklisted\n",
        conn->host.name, conn->remote_port);
  return FALSE;
}

CURLMcode Curl_pipeline_set_site_blacklist(char **sites,
                                           struct curl_llist **list_ptr)
{
  struct curl_llist *old_list = *list_ptr;
  struct curl_llist *new_list = NULL;

  if(sites) {
    new_list = Curl_llist_alloc((curl_llist_dtor) blacklist_llist_dtor);
    if(!new_list)
      return CURLM_OUT_OF_MEMORY;

    /* Parse the URLs and populate the list */
    while(*sites) {
      char *hostname;
      char *port;
      struct site_blacklist_entry *entry;

      entry = malloc(sizeof(struct site_blacklist_entry));

      hostname = strdup(*sites);
      if(!hostname)
        return CURLM_OUT_OF_MEMORY;

      port = strchr(hostname, ':');
      if(port) {
        *port = '\0';
        port++;
        entry->port = (unsigned short)strtol(port, NULL, 10);
      }
      else {
        /* Default port number for HTTP */
        entry->port = 80;
      }

      entry->hostname = hostname;

      if(!Curl_llist_insert_next(new_list, new_list->tail, entry))
        return CURLM_OUT_OF_MEMORY;

      sites++;
    }
  }

  /* Free the old list */
  if(old_list) {
    Curl_llist_destroy(old_list, NULL);
  }

  /* This might be NULL if sites == NULL, i.e the blacklist is cleared */
  *list_ptr = new_list;

  return CURLM_OK;
}

void print_pipeline(struct connectdata *conn)
{
  struct curl_llist_element *curr;
  struct connectbundle *cb_ptr;
  struct SessionHandle *data = conn->data;

  cb_ptr = conn->bundle;

  if(cb_ptr) {
    infof(data, "Bundle %p pend_list: %d\n", cb_ptr, cb_ptr->pend_list->size);

    curr = cb_ptr->conn_list->head;
    while(curr) {
      conn = curr->ptr;
      infof(data, "- Conn %p send_pipe: %d, recv_pipe: %d\n",
            conn,
            conn->send_pipe->size,
            conn->recv_pipe->size);
      curr = curr->next;
    }
  }
}
