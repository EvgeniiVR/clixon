/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  * nghttp2 callback mechanism
  *
  * nghttp2_session_mem_recv()
  *    on_begin_headers_callback() 
  *       create sd
  *    on_header_callback() NGHTTP2_HEADERS
  *       translate all headers
  *    on_data_chunk_recv_callback
  *       get indata
  *    on_frame_recv_callback NGHTTP2_FLAG_END_STREAM
  *       get method and call handler
  *       create rr
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <pwd.h>
#include <ctype.h>
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/resource.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif

/* restconf */
#include "restconf_lib.h"       /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"       /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"
#include "restconf_native.h"    /* Restconf-openssl mode specific headers*/
#ifdef HAVE_LIBNGHTTP2          /* Ends at end-of-file */
#include "restconf_nghttp2.h"   /* Restconf-openssl mode specific headers*/
#include "clixon_http_data.h"

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

/*! Map http2 frame types in nghttp2
 *
 * Had expected it in in libnghttp2 but havent found it
 */
static const map_str2int nghttp2_frame_type_map[] = {
    {"DATA",          NGHTTP2_DATA},
    {"HEADERS",       NGHTTP2_HEADERS},
    {"PRIORITY",      NGHTTP2_PRIORITY},
    {"RST_STREAM",    NGHTTP2_RST_STREAM},
    {"SETTINGS",      NGHTTP2_SETTINGS},
    {"PUSH_PROMISE",  NGHTTP2_PUSH_PROMISE},
    {"PING",          NGHTTP2_PING},
    {"GOAWAY",        NGHTTP2_GOAWAY},
    {"WINDOW_UPDATE", NGHTTP2_WINDOW_UPDATE},
    {"CONTINUATION",  NGHTTP2_CONTINUATION},
    {"ALTSVC",        NGHTTP2_ALTSVC},
    {NULL,            -1}
};

/* Clixon error category specialized log callback for nghttp2
 * @param[in]    handle  Application-specific handle
 * @param[in]    suberr  Application-specific handle
 * @param[out]   cb      Read log/error string into this buffer
 */
int
clixon_nghttp2_log_cb(void *handle,
                      int   suberr,
                      cbuf *cb)
{
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    cprintf(cb, "Fatal error: %s", nghttp2_strerror(suberr));
    return 0;
}

#ifdef NOTUSED
static void
nghttp2_print_header(const uint8_t *name,
                     size_t         namelen,
                     const uint8_t *value,
                     size_t         valuelen)
{
    clixon_debug(CLIXON_DBG_RESTCONF, "%s %s", name, value);
}

/*! Print HTTP headers to |f|. 
 *
 * Please note that this function does not
 * take into account that header name and value are sequence of
 *  octets, therefore they may contain non-printable characters. 
 */
static void
nghttp2_print_headers(nghttp2_nv *nva,
                      size_t      nvlen)
{
  size_t i;

  for (i = 0; i < nvlen; ++i)
      nghttp2_print_header(nva[i].name, nva[i].namelen, nva[i].value, nva[i].valuelen);
}
#endif /* NOTUSED */

/*! Send data to remote peer, Send at most the |length| bytes of |data|.
 *
 * This callback is required if the application uses
 * `nghttp2_session_send()` to send data to the remote endpoint.  If
 * the application uses solely `nghttp2_session_mem_send()` instead,
 * this callback function is unnecessary.
 * It must return the number of bytes sent if it succeeds.  
 * If it cannot send any single byte without blocking,
 * it must return :enum:`NGHTTP2_ERR_WOULDBLOCK`.  
 * For other errors, it must return :enum:`NGHTTP2_ERR_CALLBACK_FAILURE`.
 */
static ssize_t
session_send_callback(nghttp2_session *session,
                      const uint8_t   *buf,
                      size_t           buflen,
                      int              flags,
                      void            *user_data)
{
    int            retval = NGHTTP2_ERR_CALLBACK_FAILURE;
    restconf_conn *rc = (restconf_conn *)user_data;
    int            er;
    ssize_t        len;
    ssize_t        totlen = 0;
    int            s;
    int            sslerr;

    clixon_debug(CLIXON_DBG_RESTCONF, "buflen:%zu", buflen);
    s = rc->rc_s;
    while (totlen < buflen){
        if (rc->rc_ssl){
            if ((len = SSL_write(rc->rc_ssl, buf+totlen, buflen-totlen)) <= 0){
                er = errno;
                sslerr = SSL_get_error(rc->rc_ssl, len);
                clixon_debug(CLIXON_DBG_RESTCONF, "SSL_write: errno:%s(%d) sslerr:%d",
                             strerror(er),
                             er,
                             sslerr);
                switch (sslerr){
                case SSL_ERROR_WANT_WRITE:           /* 3 */
                    clixon_debug(CLIXON_DBG_RESTCONF, "write SSL_ERROR_WANT_WRITE");
                    usleep(1000);
                    continue;
                    break;
                case SSL_ERROR_SYSCALL:              /* 5 */
                    if (er == ECONNRESET || /* Connection reset by peer */
                        er == EPIPE) {      /* Reading end of socket is closed */
                        goto done; /* Cleanup in http2_recv() */
                    }
                    else if (er == EAGAIN){
                        /* same as want_write above, but different behaviour on different
                         * platforms, linux here, freebsd want_write, or possibly differnt
                         * ssl lib versions?
                         */
                        clixon_debug(CLIXON_DBG_RESTCONF, "write EAGAIN");
                        usleep(1000);
                        continue;
                    }
                    else{
                        clixon_err(OE_RESTCONF, er, "SSL_write %d", sslerr);
                        goto done;
                    }
                    break;
                default:
                    clixon_err(OE_SSL, 0, "SSL_write");
                    goto done;
                    break;
                }
                goto done;
            }
        }
        else{
            if ((len = write(s, buf+totlen, buflen-totlen)) < 0){
                if (errno == EAGAIN){
                    clixon_debug(CLIXON_DBG_RESTCONF, "write EAGAIN");
                    usleep(10000);
                    continue;
                }
#if 1
                else if (errno == ECONNRESET) {/* Connection reset by peer */
                    close(s);
                    // XXXUnclear why this is commented, maybe should call
                    // restconf_connection_close?
                    //  clixon_event_unreg_fd(s, restconf_connection);

                    goto ok; /* Close socket and ssl */
                }
#endif
                else{
                    clixon_err(OE_UNIX, errno, "write");
                    goto done;
                }
            }
            assert(len != 0);
        }
        totlen += len;
    } /* while */
 ok:
    retval = 0;
 done:
    if (retval < 0){
        clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
        return retval;
    }
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%zd", totlen);
    return retval == 0 ? totlen : retval;
}

/*! Invoked when |session| wants to receive data from the remote peer.  
 */
static ssize_t
recv_callback(nghttp2_session *session,
              uint8_t         *buf,
              size_t           length,
              int              flags,
              void            *user_data)
{
    // restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! Callback for each incoming http request for path /
 *
 * This are all messages except /.well-known, 
 *
 * @param[in] sd    session stream struct (http/1 has a single)
 * @retval    void
 * Discussion: problematic if fatal error -1 is returned from clixon routines 
 * without actually terminating. Consider:
 * 1) sending some error? and/or
 * 2) terminating the process? 
 */
static int
restconf_nghttp2_path(restconf_stream_data *sd)
{
    int            retval = -1;
    clixon_handle  h;
    restconf_conn *rc;
    char          *oneline = NULL;
    cvec          *cvv = NULL;
    char          *cn;

    clixon_debug(CLIXON_DBG_RESTCONF, "------------");
    rc = sd->sd_conn;
    if ((h = rc->rc_h) == NULL){
        clixon_err(OE_RESTCONF, EINVAL, "arg is NULL");
        goto done;
    }
    if (rc->rc_ssl != NULL){
        /* Slightly awkward way of taking SSL cert subject and CN and add it to restconf parameters
         * instead of accessing it directly 
         * SSL subject fields, eg CN (Common Name) , can add more here? */
        if (ssl_x509_name_oneline(rc->rc_ssl, &oneline) < 0)
            goto done;
        if (oneline != NULL) {
            if (uri_str2cvec(oneline, '/', '=', 1, &cvv) < 0)
                goto done;
            if ((cn = cvec_find_str(cvv, "CN")) != NULL){
                if (restconf_param_set(h, "SSL_CN", cn) < 0)
                    goto done;
            }
        }
    }
    /* Check sanity of session, eg ssl client cert validation, may set rc_exit */
    if (restconf_connection_sanity(h, rc, sd) < 0)
        goto done;
    if (!rc->rc_exit){
        /* Matching algorithm:
         * 1. try well-known
         * 2. try /restconf
         * 3. try /data
         * 4. call restconf anyway (because it handles errors)
         * This is for the situation where data is / and /restconf is more specific
         */
        if (strcmp(sd->sd_path, RESTCONF_WELL_KNOWN) == 0){
            if (api_well_known(h, sd) < 0)
                goto done;
        }
        else if (api_path_is_restconf(h)){
            if (api_root_restconf(h, sd, sd->sd_qvec) < 0)
                goto done;
        }
        else if (api_path_is_data(h)){
            if (api_http_data(h, sd, sd->sd_qvec) < 0)
                goto done;
        }
        else if (api_root_restconf(h, sd, sd->sd_qvec) < 0) /* error handling */
            goto done;
    }
    /* Clear (fcgi) paramaters from this request */
    if (restconf_param_del_all(h) < 0)
        goto done;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    if (cvv)
        cvec_free(cvv);
    if (oneline)
        free(oneline);
    return retval; /* void */
}

/*! data callback, just pass pointer to cbuf
 *
 * XXX handle several chunks with cbuf 
 */
static ssize_t
restconf_sd_read(nghttp2_session     *session,
                 int32_t              stream_id,
                 uint8_t             *buf,
                 size_t               length,
                 uint32_t            *data_flags,
                 nghttp2_data_source *source,
                 void                *user_data)
{
    restconf_stream_data *sd = (restconf_stream_data *)source->ptr;
    cbuf                 *cb;
    size_t                len = 0;
    size_t                remain;

    if ((cb = sd->sd_body) == NULL){ /* shouldnt happen */
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
#if 0
    if (cbuf_len(cb) <= length){
        len = remain;
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    else{
        len = length;
    }
    memcpy(buf, cbuf_get(cb) + sd->sd_body_offset, len);
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return len;
#endif
    assert(cbuf_len(cb) > sd->sd_body_offset);
    remain = cbuf_len(cb) - sd->sd_body_offset;
    clixon_debug(CLIXON_DBG_RESTCONF, "length:%zu totlen:%zu, offset:%zu remain:%zu",
                 length,
                 cbuf_len(cb),
                 sd->sd_body_offset,
                 remain);

    if (remain <= length){
        len = remain;
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    else{
        len = length;
    }
    memcpy(buf, cbuf_get(cb) + sd->sd_body_offset, len);
    sd->sd_body_offset += len;
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%zu", len);
    return len;
}

static int
restconf_submit_response(nghttp2_session      *session,
                         restconf_conn        *rc,
                         int                   stream_id,
                         restconf_stream_data *sd)
{
    int                   retval = -1;
    nghttp2_data_provider data_prd;
    nghttp2_error         ngerr;
    cg_var               *cv;
    nghttp2_nv           *hdrs = NULL;
    nghttp2_nv           *hdr;
    int                   i = 0;
    char                  valstr[16];

    data_prd.source.ptr = sd;
    data_prd.read_callback = restconf_sd_read;
    if ((hdrs = (nghttp2_nv*)calloc(1+cvec_len(sd->sd_outp_hdrs), sizeof(nghttp2_nv))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    hdr = &hdrs[i++];
    hdr->name = (uint8_t*)":status";
    snprintf(valstr, 15, "%u", sd->sd_code);
    clixon_debug(CLIXON_DBG_RESTCONF, "status %d", sd->sd_code);
    hdr->value = (uint8_t*)valstr;
    hdr->namelen = strlen(":status");
    hdr->valuelen = strlen(valstr);
    hdr->flags = 0;

    cv = NULL;
    while ((cv = cvec_each(sd->sd_outp_hdrs, cv)) != NULL){
        hdr = &hdrs[i++];
        hdr->name = (uint8_t*)cv_name_get(cv);
        clixon_debug(CLIXON_DBG_RESTCONF, "hdr: %s", hdr->name);
        hdr->value = (uint8_t*)cv_string_get(cv);
        hdr->namelen = strlen(cv_name_get(cv));
        hdr->valuelen = strlen(cv_string_get(cv));
        hdr->flags = 0;
    }
    if ((ngerr = nghttp2_submit_response(session,
                                         stream_id,
                                         hdrs, i,
                                         (data_prd.source.ptr != NULL)?&data_prd:NULL)) < 0){
        clixon_err(OE_NGHTTP2, ngerr, "nghttp2_submit_response");
        goto done;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    if (hdrs)
        free(hdrs);
    return retval;
}

/*! Simulate a received request in an upgrade scenario by talking the http/1 parameters
 */
int
http2_exec(restconf_conn        *rc,
           restconf_stream_data *sd,
           nghttp2_session     *session,
           int32_t              stream_id)
{
    int retval = -1;

    clixon_debug(CLIXON_DBG_RESTCONF, "");
    if (sd->sd_path){
        free(sd->sd_path);
        sd->sd_path = NULL;
    }
    if ((sd->sd_path = restconf_uripath(rc->rc_h)) == NULL)
        goto done;
    sd->sd_proto = HTTP_2; /* XXX is this necessary? */
    if (strcmp(sd->sd_path, RESTCONF_WELL_KNOWN) == 0
        || api_path_is_restconf(rc->rc_h)
        || api_path_is_data(rc->rc_h)){
        if (restconf_nghttp2_path(sd) < 0)
            goto done;
    }
    else{
        sd->sd_code = 404;    /* not found */
    }
    if (restconf_param_del_all(rc->rc_h) < 0) // XXX
        goto done;

    /* If body, add a content-length header 
     *    A server MUST NOT send a Content-Length header field in any response
     * with a status code of 1xx (Informational) or 204 (No Content).  A
     * server MUST NOT send a Content-Length header field in any 2xx
     * (Successful) response to a CONNECT request (Section 4.3.6 of
     * [RFC7231]).
     */
    if (sd->sd_code != 204 && sd->sd_code > 199 && sd->sd_body_len)
        if (restconf_reply_header(sd, "Content-Length", "%zu", sd->sd_body_len) < 0)
            goto done;
    if (sd->sd_code){
        if (restconf_submit_response(session, rc, stream_id, sd) < 0)
            goto done;
    }
    else {
        /* 500 Internal server error ? */
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    return retval;
}

/*! A frame is received
 */
static int
on_frame_recv_callback(nghttp2_session     *session,
                       const nghttp2_frame *frame,
                       void                *user_data)
{
    int                   retval = -1;
    restconf_conn        *rc = (restconf_conn *)user_data;
    restconf_stream_data *sd = NULL;
    char                 *query;

    clixon_debug(CLIXON_DBG_RESTCONF, "%s %d",
                 clicon_int2str(nghttp2_frame_type_map, frame->hd.type),
                 frame->hd.stream_id);
    switch (frame->hd.type) {
    case NGHTTP2_DATA:
    case NGHTTP2_HEADERS:
        /* Check that the client request has finished */
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            /* For DATA and HEADERS frame, this callback may be called after
             * on_stream_close_callback. Check that stream still alive. 
             */
            if ((sd = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)) == NULL)
                return 0;
            /* Query vector, ie the ?a=x&b=y stuff */
            query = restconf_param_get(rc->rc_h, "REQUEST_URI");
            if ((query = index(query, '?')) != NULL){
                query++;
                if (strlen(query) &&
                    uri_str2cvec(query, '&', '=', 1, &sd->sd_qvec) < 0)
                    goto done;
            }
            if (http2_exec(rc, sd, session, frame->hd.stream_id) < 0)
                goto done;
        }
        break;
    default:
        break;
    }
    retval = 0;
 done:
    return retval;
}

/*! An invalid non-DATA frame is received. 
 */
static int
on_invalid_frame_recv_callback(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               int lib_error_code,
                               void *user_data)
{
    // restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! A chunk of data in DATA frame is received
 *
 *  ``(flags & NGHTTP2_FLAG_END_STREAM) != 0`` does not
 * necessarily mean this chunk of data is the last one in the stream.
 * You should use :type:`nghttp2_on_frame_recv_callback` to know all
 * data frames are received.
 */
static int
on_data_chunk_recv_callback(nghttp2_session *session,
                            uint8_t          flags,
                            int32_t          stream_id,
                            const uint8_t   *data,
                            size_t           len,
                            void            *user_data)
{
    restconf_conn *rc = (restconf_conn *)user_data;
    restconf_stream_data *sd;

    clixon_debug(CLIXON_DBG_RESTCONF, "%d", stream_id);
    if ((sd = restconf_stream_find(rc, stream_id)) != NULL){
        cbuf_append_buf(sd->sd_indata, (void*)data, len);
    }
    return 0;
}

/*! Just before the non-DATA frame |frame| is sent
 */
static int
before_frame_send_callback(nghttp2_session     *session,
                           const nghttp2_frame *frame,
                           void                *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! After the frame |frame| is sent
 */
static int
on_frame_send_callback(nghttp2_session     *session,
                       const nghttp2_frame *frame,
                       void                *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! After the non-DATA frame |frame| is not sent because of error
 */
static int
on_frame_not_send_callback(nghttp2_session *session,
                           const nghttp2_frame *frame,
                           int lib_error_code,
                           void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! Stream |stream_id| is closed. 
 */
static int
on_stream_close_callback(nghttp2_session   *session,
                         int32_t            stream_id,
                         nghttp2_error_code error_code,
                         void              *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;

    clixon_debug(CLIXON_DBG_RESTCONF, "%d %s", error_code, nghttp2_strerror(error_code));
#if 0 // NOTNEEDED /* XXX think this is not necessary? */
    if (error_code){
        if (restconf_close_ssl_socket(rc, __FUNCTION__, 0) < 0)
            return -1;
    }
#endif
    return 0;
}

/*! Reception of header block in HEADERS or PUSH_PROMISE is started.
 */
static int
on_begin_headers_callback(nghttp2_session     *session,
                          const nghttp2_frame *frame,
                          void                *user_data)
{
    restconf_conn      *rc = (restconf_conn *)user_data;
    restconf_stream_data *sd;

    clixon_debug(CLIXON_DBG_RESTCONF, "%s", clicon_int2str(nghttp2_frame_type_map, frame->hd.type));
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        sd = restconf_stream_data_new(rc, frame->hd.stream_id);
        nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sd);
    }
    return 0;
}

/*! Map from nghttp2 headers  to "fcgi" type parameters used in clixon code
 *
 * Both |name| and |value| are guaranteed to be NULL-terminated. 
 */
static int
nghttp2_hdr2clixon(clixon_handle  h,
                   char          *name,
                   char          *value)
{
    int retval = -1;

    if (strcmp(name, ":path") == 0){
        /* Including ?args, call restconf_uripath() to get only path */
        if (restconf_param_set(h, "REQUEST_URI", value) < 0)
            goto done;
    }
    else if (strcmp(name, ":method") == 0){
        if (restconf_param_set(h, "REQUEST_METHOD", value) < 0)
            goto done;
    }
    else if (strcmp(name, ":scheme") == 0){
        if (strcmp(value, "https") == 0 &&
            restconf_param_set(h, "HTTPS", "https") < 0) /* some string or NULL */
            goto done;
    }
    else if (strcmp(name, ":authority") == 0){
        if (restconf_param_set(h, "HTTP_HOST", value) < 0)
            goto done;
    }
    else if (restconf_convert_hdr(h, name, value) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Header name/value pair is received
 *
 * Both |name| and |value| are guaranteed to be NULL-terminated. 
 * If the application uses `nghttp2_session_mem_recv()`, it can return
 * :enum:`NGHTTP2_ERR_PAUSE` to make `nghttp2_session_mem_recv()`
 * return without processing further input bytes.
 */
static int
on_header_callback(nghttp2_session     *session,
                   const nghttp2_frame *frame,
                   const uint8_t       *name,
                   size_t              namelen,
                   const uint8_t      *value,
                   size_t              valuelen,
                   uint8_t             flags,
                   void               *user_data)
{
    int                   retval = -1;
    restconf_conn      *rc = (restconf_conn *)user_data;

    switch (frame->hd.type){
    case NGHTTP2_HEADERS:
        assert (frame->headers.cat == NGHTTP2_HCAT_REQUEST);
        clixon_debug(CLIXON_DBG_RESTCONF, "HEADERS %s %s", name, value);
        if (nghttp2_hdr2clixon(rc->rc_h, (char*)name, (char*)value) < 0)
            goto done;
        break;
    default:
        clixon_debug(CLIXON_DBG_RESTCONF, "%s %s", clicon_int2str(nghttp2_frame_type_map, frame->hd.type), name);
        break;
    }
    retval = 0;
 done:
    return retval;
}

#ifdef NOTUSED
/*! How many padding bytes are required for the transmission of the |frame|?
 */
static ssize_t
select_padding_callback(nghttp2_session *session,
                        const nghttp2_frame *frame,
                        size_t max_payloadlen,
                        void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return frame->hd.length;
}

/*! Get max length of data to send data to the remote peer
 */
static ssize_t
data_source_read_length_callback(nghttp2_session *session,
                                 uint8_t frame_type,
                                 int32_t stream_id,
                                 int32_t session_remote_window_size,
                                 int32_t stream_remote_window_size,
                                 uint32_t remote_max_frame_size,
                                 void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}
#endif /* NOTUSED */

/*! Invoked when a frame header is received.
 *
 * Unlike :type:`nghttp2_on_frame_recv_callback`, this callback will
 * also be called when frame header of CONTINUATION frame is received.
 */
static int
on_begin_frame_callback(nghttp2_session *session,
                        const nghttp2_frame_hd *hd,
                        void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "%s", clicon_int2str(nghttp2_frame_type_map, hd->type));
    if (hd->type == NGHTTP2_CONTINUATION)
        assert(0);
    return 0;
}

/*! Send complete DATA frame for no-copy
 *
 * Callback function invoked when :enum:`NGHTTP2_DATA_FLAG_NO_COPY` is
 * used in :type:`nghttp2_data_source_read_callback` to send complete
 * DATA frame.
 */
static int
send_data_callback(nghttp2_session *session,
                   nghttp2_frame *frame,
                   const uint8_t *framehd, size_t length,
                   nghttp2_data_source *source,
                   void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

#ifdef NOTUSED
/*! Pack extension payload in its wire format
 */
static ssize_t
pack_extension_callback(nghttp2_session *session,
                        uint8_t *buf, size_t len,
                        const nghttp2_frame *frame,
                        void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! Unpack extension payload from its wire format. 
 */
static int
unpack_extension_callback(nghttp2_session *session,
                          void **payload,
                          const nghttp2_frame_hd *hd,
                          void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}
#endif /* NOTUSED */

/*! Chunk of extension frame payload is received
 */
static int
on_extension_chunk_recv_callback(nghttp2_session *session,
                                 const nghttp2_frame_hd *hd,
                                 const uint8_t *data,
                                 size_t len,
                                 void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

/*! Library provides the error code, and message for debugging purpose.
 */
static int
error_callback(nghttp2_session *session,
               const char *msg,
               size_t len,
               void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    return 0;
}

#if (NGHTTP2_VERSION_NUM > 0x011201) /* Unsure of version number */
/*! Library provides the error code, and message for debugging purpose.
 */
static int
error_callback2(nghttp2_session *session,
                int lib_error_code,
                const char *msg,
                size_t len,
                void *user_data)
{
    //    restconf_conn *rc = (restconf_conn *)user_data;
    clixon_debug(CLIXON_DBG_RESTCONF, "");
    clixon_err(OE_NGHTTP2, lib_error_code, "%s", msg);
    return 0;
}
#endif

/*! Process an HTTP/2 request received in buffer, process request and send reply
 *
 * @param[in] rc   Restconf connection
 * @param[in] buf  Character buffer
 * @param[in] n    Lenght of buf
 * @retval    1    OK
 * @retval    0    Invalid request
 * @retval   -1    Fatal error
 */
int
http2_recv(restconf_conn       *rc,
           const unsigned char *buf,
           size_t               n)
{
    int           retval = -1;
    nghttp2_error ngerr;

    clixon_debug(CLIXON_DBG_RESTCONF, "");
    if (rc->rc_ngsession == NULL){
        /* http2_session_init not called */
        clixon_err(OE_RESTCONF, EINVAL, "No nghttp2 session");
        goto done;
    }
    /* may make additional pending frames */
    if ((ngerr = nghttp2_session_mem_recv(rc->rc_ngsession, buf, n)) < 0){
        if (ngerr == NGHTTP2_ERR_BAD_CLIENT_MAGIC){
            /* :enum:`NGHTTP2_ERR_BAD_CLIENT_MAGIC`
             *     Invalid client magic was detected.  This error only returns
             *     when |session| was configured as server and
             *     `nghttp2_option_set_no_recv_client_magic()` is not used with
             *     nonzero value. */
            clixon_log(NULL, LOG_INFO, "%s Received bad client magic byte strin", __FUNCTION__);
            /* unsure if this does anything, byt does not seem to hurt */
            if ((ngerr = nghttp2_session_terminate_session(rc->rc_ngsession, ngerr)) < 0)
                clixon_err(OE_NGHTTP2, ngerr, "nghttp2_session_terminate_session %d", ngerr);
            goto fail;
        }
        clixon_err(OE_NGHTTP2, ngerr, "nghttp2_session_mem_recv");
        goto done;
    }
    /* sends highest prio frame from outbound queue to remote peer.  It does this as
     * many as possible until user callback :type:`nghttp2_send_callback` returns
     * :enum:`NGHTTP2_ERR_WOULDBLOCK` or the outbound queue becomes empty.
     * @see session_send_callback()
     */
    clixon_err_reset();
    if ((ngerr = nghttp2_session_send(rc->rc_ngsession)) != 0){
        if (clixon_err_category())
            goto done;
        else
            goto fail; /* Not fatal error */
    }
    retval = 1; /* OK */
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/* Send HTTP/2 client connection header, which includes 24 bytes
   magic octets and SETTINGS frame */
int
http2_send_server_connection(restconf_conn *rc)
{
    int                    retval = -1;
    nghttp2_settings_entry iv[2] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
                                    ,{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
    nghttp2_error          ngerr;

    clixon_debug(CLIXON_DBG_RESTCONF, "");
    if ((ngerr = nghttp2_submit_settings(rc->rc_ngsession,
                                         NGHTTP2_FLAG_NONE,
                                         iv,
                                         ARRLEN(iv))) != 0){
        clixon_err(OE_NGHTTP2, ngerr, "nghttp2_submit_settings");
        goto done;
    }
    if ((ngerr = nghttp2_session_send(rc->rc_ngsession)) != 0){
        clixon_err(OE_NGHTTP2, ngerr, "nghttp2_session_send");
        goto done;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_RESTCONF, "retval:%d", retval);
    return retval;
}

/*! Initialize callbacks
 */
int
http2_session_init(restconf_conn *rc)
{
    int                        retval = -1;
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session           *session = NULL;
    nghttp2_error              ngerr;

    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, session_send_callback);
    nghttp2_session_callbacks_set_recv_callback(callbacks, recv_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(callbacks, on_invalid_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_before_frame_send_callback(callbacks, before_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_frame_not_send_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
#ifdef NOTUSED
    nghttp2_session_callbacks_set_select_padding_callback(callbacks, select_padding_callback);
    nghttp2_session_callbacks_set_data_source_read_length_callback(callbacks, data_source_read_length_callback);
#endif
    nghttp2_session_callbacks_set_on_begin_frame_callback(callbacks, on_begin_frame_callback);

    nghttp2_session_callbacks_set_send_data_callback(callbacks, send_data_callback);
#ifdef NOTUSED
    nghttp2_session_callbacks_set_pack_extension_callback(callbacks, pack_extension_callback);
    nghttp2_session_callbacks_set_unpack_extension_callback(callbacks, unpack_extension_callback);
#endif
    nghttp2_session_callbacks_set_on_extension_chunk_recv_callback(callbacks, on_extension_chunk_recv_callback);
    nghttp2_session_callbacks_set_error_callback(callbacks, error_callback);
#if (NGHTTP2_VERSION_NUM > 0x011201) /* Unsure of version number */
    nghttp2_session_callbacks_set_error_callback2(callbacks, error_callback2);
#endif

    /* Create session for server use, register callbacks */
    if ((ngerr = nghttp2_session_server_new3(&session, callbacks, rc, NULL, NULL)) < 0){
        clixon_err(OE_NGHTTP2, ngerr, "nghttp2_session_server_new");
        goto done;
    }
    nghttp2_session_callbacks_del(callbacks);
    rc->rc_ngsession = session;

    retval = 0;
 done:
    return retval;
}

#endif /* HAVE_LIBNGHTTP2 */
