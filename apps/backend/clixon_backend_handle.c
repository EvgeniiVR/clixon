/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/time.h>
#include <regex.h>
#include <syslog.h>
#include <netinet/in.h>
#include <limits.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "backend_client.h"
#include "backend_handle.h"

/* header part is copied from struct clicon_handle in lib/src/clicon_handle.c */

#define CLICON_MAGIC 0x99aafabe

#define handle(h) (assert(clicon_handle_check(h)==0),(struct backend_handle *)(h))

/* Clicon_handle for backends.
 * First part of this is header, same for clicon_handle and cli_handle.
 * Access functions for common fields are found in clicon lib: clicon_options.[ch]
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 */
/*! Backend specific handle added to header CLICON handle
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 * @note The top part must be equivalent to struct clicon_handle in clixon_handle.c
 * @see struct clicon_handle, struct cli_handle
 */
struct backend_handle {
    int                      bh_magic;     /* magic (HDR)*/
    clicon_hash_t           *bh_copt;      /* clicon option list (HDR) */
    clicon_hash_t           *bh_data;      /* internal clicon data (HDR) */
    event_stream_t          *ch_stream;   /* notification streams, see clixon_stream.[ch] */
    
    /* ------ end of common handle ------ */
    struct client_entry     *bh_ce_list;   /* The client list */
    int                      bh_ce_nr;     /* Number of clients, just increment */
    cxobj                   *bh_nacm;      /* NACM external struct */
};

/*! Creates and returns a clicon config handle for other CLICON API calls
 */
clicon_handle
backend_handle_init(void)
{
    return clicon_handle_init0(sizeof(struct backend_handle));
}

/*! Deallocates a backend handle, including all client structs
 * @Note: handle 'h' cannot be used in calls after this
 */
int
backend_handle_exit(clicon_handle h)
{
    struct backend_handle *bh = handle(h);
    struct client_entry   *ce;

    /* only delete client structs, not close sockets, etc, see backend_client_rm */
    while ((ce = backend_client_list(h)) != NULL)
	backend_client_delete(h, ce);
    if (bh->bh_nacm)
	xml_free(bh->bh_nacm);
    clicon_handle_exit(h); /* frees h and options */
    return 0;
}

/*! Add new client, typically frontend such as cli, netconf, restconf
 * @param[in]  h        Clicon handle
 * @param[in]  addr     Address of client
 * @retval     ce       Client entry
 * @retval     NULL     Error
 */
struct client_entry *
backend_client_add(clicon_handle    h, 
		   struct sockaddr *addr)
{
    struct backend_handle *bh = handle(h);
    struct client_entry   *ce;

    if ((ce = (struct client_entry *)malloc(sizeof(*ce))) == NULL){
	clicon_err(OE_PLUGIN, errno, "malloc");
	return NULL;
    }
    memset(ce, 0, sizeof(*ce));
    ce->ce_nr = bh->bh_ce_nr++;
    memcpy(&ce->ce_addr, addr, sizeof(*addr));
    ce->ce_next = bh->bh_ce_list;
    bh->bh_ce_list = ce;
    return ce;
}

/*! Return client list
 * @param[in]  h        Clicon handle
 * @retval     ce_list  Client entry list (all sessions)
 */
struct client_entry *
backend_client_list(clicon_handle h)
{
    struct backend_handle *bh = handle(h);

    return bh->bh_ce_list;
}

/*! Actually remove client from client list
 * @param[in]  h   Clicon handle
 * @param[in]  ce  Client handle
 * @see backend_client_rm which is more high-level
 */
int
backend_client_delete(clicon_handle        h,
		      struct client_entry *ce)
{
    struct client_entry   *c;
    struct client_entry  **ce_prev;
    struct backend_handle *bh = handle(h);

    ce_prev = &bh->bh_ce_list;
    for (c = *ce_prev; c; c = c->ce_next){
	if (c == ce){
	    *ce_prev = c->ce_next;
	    free(ce);
	    break;
	}
	ce_prev = &c->ce_next;
    }
    return 0;
}

int
backend_nacm_list_set(clicon_handle h,
		      cxobj        *xnacm)
{
    struct backend_handle *bh = handle(h);

    if (bh->bh_nacm)
	xml_free(bh->bh_nacm);
    bh->bh_nacm = xnacm;
    return 0;
}

cxobj *
backend_nacm_list_get(clicon_handle h)
{
    struct backend_handle *bh = handle(h);
    
    return bh->bh_nacm;
}
