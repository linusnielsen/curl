#ifndef __PIPELINE_H
#define __PIPELINE_H
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

struct connectbundle {
  bool server_supports_pipelining; /* TRUE if server supports pipelining,
                                      set after first response */
  int num_connections;          /* Number of connections in the bundle */
  struct curl_llist *conn_list; /* The connectdata members of the bundle */
  struct curl_llist *pend_list; /* A queue of pending handles */
};

CURLcode Curl_bundle_create(struct SessionHandle *data,
                            struct connectbundle **cb_ptr);

void Curl_bundle_destroy(struct SessionHandle *data,
                         struct connectbundle *cb_ptr);

CURLcode Curl_bundle_add_conn(struct SessionHandle *data,
                              struct connectbundle *cb_ptr,
                              struct connectdata *conn);

int Curl_bundle_remove_conn(struct SessionHandle *data,
                            struct connectbundle *cb_ptr,
                            struct connectdata *conn);

struct connectdata *
Curl_bundle_find_best(struct SessionHandle *data,
                      struct connectbundle *cb_ptr);

CURLcode Curl_bundle_add_to_queue(struct SessionHandle *handle,
                                  struct connectbundle *cb_ptr);

CURLcode Curl_add_handle_to_pipeline(struct SessionHandle *handle,
                                     struct connectdata *conn);
void Curl_move_handle_from_send_to_recv_pipe(struct SessionHandle *handle,
                                             struct connectdata *conn);
int Curl_check_pend_pipeline(struct connectdata *conn);

void print_pipeline(struct connectdata *conn);

#endif /* __PIPELINE_H */
