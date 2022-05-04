/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Kristofer Hallin

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
  * Extensions are grouped in some categories, the one I have seen are, example:
  * 1. leaf
  *      smiv2:max-access "read-write";
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
  *      smiv2:defval "42"; (not always)
  * 2. container, list
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1";	
  * 3. module level
  *      smiv2:alias "netSnmpExamples" {
  *        smiv2:oid "1.3.6.1.4.1.8072.2";
  *

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "snmp_mib_yang.h"

#define IETF_YANG_SMIV2_NS "urn:ietf:params:xml:ns:yang:ietf-yang-smiv2"

/*
 * Local variables
 */
/* Mapping between yang keyword string <--> clixon constants 
 * Here is also the place where doc on some types store variables (cv)
 */
/* Mapping between smiv2 yang extension access string string <--> netsnmp handler codes (agent_handler.h) 
 * Here is also the place where doc on some types store variables (cv)
 * see netsnmp_handler_registration_create()
 */
static const map_str2int acc_map[] = {
    {"read-only",             HANDLER_CAN_RONLY}, /* HANDLER_CAN_GETANDGETNEXT */
    {"read-write",            HANDLER_CAN_RWRITE}, /* HANDLER_CAN_GETANDGETNEXT | HANDLER_CAN_SET */
    {"not-accessible",        0}, // XXX
    {"accessible-for-notify", 0}, // XXX
    {NULL,               -1}
};

#if 1 /* table example */
static netsnmp_table_data_set *table_set;

/*
 * https://net-snmp.sourceforge.io/dev/agent/data_set_8c-example.html#_a0
 */
static void
init_testtable(void)
{
    netsnmp_table_row *row;

    /*
     * the OID we want to register our integer at.  This should be the
     * * OID node for the entire table.  In our case this is the
     * * netSnmpIETFWGTable oid definition
     */
    oid     my_oid[] = { 1, 3, 6, 1, 4, 1, 8072, 2, 2, 1 };

    /*
     * a debugging statement.  Run the agent with -Dexample_data_set to see
     * * the output of this debugging statement.
     */
    DEBUGMSGTL(("example_data_set",
                "Initalizing example dataset table\n"));

    /*
     * It's going to be the "working group chairs" table, since I'm
     * * sitting at an IETF convention while I'm writing this.
     * *
     * *  column 1 = index = string = WG name
     * *  column 2 = string = chair #1
     * *  column 3 = string = chair #2  (most WGs have 2 chairs now)
     */

    table_set = netsnmp_create_table_data_set("netSnmpIETFWGTable");

    /*
     * allow the creation of new rows via SNMP SETs
     */
    table_set->allow_creation = 1;

    /*
     * set up what a row "should" look like, starting with the index
     */
    netsnmp_table_dataset_add_index(table_set, ASN_OCTET_STR);

    /*
     * define what the columns should look like.  both are octet strings here
     */
    netsnmp_table_set_multi_add_default_row(table_set,
                                            /*
                                             * column 2 = OCTET STRING,
                                             * writable = 1,
                                             * default value = NULL,
                                             * default value len = 0
                                             */
                                            2, ASN_OCTET_STR, 1, NULL, 0,
                                            /*
                                             * similar
                                             */
                                            3, ASN_OCTET_STR, 1, NULL, 0,
                                            0 /* done */ );

    /*
     * register the table
     */
    /*
     * if we wanted to handle specific data in a specific way, or note
     * * when requests came in we could change the NULL below to a valid
     * * handler method in which we could over ride the default
     * * behaviour of the table_dataset helper
     */
    netsnmp_register_table_data_set(netsnmp_create_handler_registration
                                    ("netSnmpIETFWGTable", NULL,
                                     my_oid, OID_LENGTH(my_oid),
                                     HANDLER_CAN_RWRITE), table_set, NULL);


    /*
     * create the a row for the table, and add the data
     */
    row = netsnmp_create_table_data_row();
    /*
     * set the index to the IETF WG name "snmpv3"
     */
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "snmpv3",
                                strlen("snmpv3"));


    /*
     * set column 2 to be the WG chair name "Russ Mundy"
     */
    netsnmp_set_row_column(row, 2, ASN_OCTET_STR,
                           "Russ Mundy", strlen("Russ Mundy"));
    netsnmp_mark_row_column_writable(row, 2, 1);        /* make writable via SETs */

    /*
     * set column 3 to be the WG chair name "David Harrington"
     */
    netsnmp_set_row_column(row, 3, ASN_OCTET_STR, "David Harrington",
                           strlen("David Harrington"));
    netsnmp_mark_row_column_writable(row, 3, 1);        /* make writable via SETs */

    /*
     * add the row to the table
     */
    netsnmp_table_dataset_add_row(table_set, row);

    /*
     * add the data, for the second row
     */
    row = netsnmp_create_table_data_row();
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "snmpconf",
                                strlen("snmpconf"));
    netsnmp_set_row_column(row, 2, ASN_OCTET_STR, "David Partain",
                           strlen("David Partain"));
    netsnmp_mark_row_column_writable(row, 2, 1);        /* make writable */
    netsnmp_set_row_column(row, 3, ASN_OCTET_STR, "Jon Saperia",
                           strlen("Jon Saperia"));
    netsnmp_mark_row_column_writable(row, 3, 1);        /* make writable */
    netsnmp_table_dataset_add_row(table_set, row);

    /*
     * Finally, this actually allows the "add_row" token it the
     * * snmpd.conf file to add rows to this table.
     * * Example snmpd.conf line:
     * *   add_row netSnmpIETFWGTable eos "Glenn Waters" "Dale Francisco"
     */
    netsnmp_register_auto_data_table(table_set, NULL);

    DEBUGMSGTL(("example_data_set", "Done initializing.\n"));
}
#endif /* table example */

#if 1 /* scalar example */


#define TESTHANDLER_SET_NAME "my_test"
int
my_test_instance_handler(netsnmp_mib_handler          *handler,
                         netsnmp_handler_registration *reginfo,
                         netsnmp_agent_request_info   *reqinfo,
                         netsnmp_request_info         *requests)
{

    static int  accesses = 42;
    u_long     *accesses_cache = NULL;
    //    clicon_handle h = (clicon_handle)handler->myvoid;

    clicon_debug(1, "%s", __FUNCTION__);
    clicon_debug(1, "%s %p", __FUNCTION__, reginfo);

    switch (reqinfo->mode) {
    case MODE_GET:
        snmp_set_var_typed_value(requests->requestvb, ASN_INTEGER,
                                 (u_char *) & accesses, sizeof(accesses));
        break;
    case MODE_SET_RESERVE1:
        if (requests->requestvb->type != ASN_INTEGER)
            netsnmp_set_request_error(reqinfo, requests,
                                      SNMP_ERR_WRONGTYPE);
        break;

    case MODE_SET_RESERVE2:
        /*
         * store old info for undo later 
         */
        accesses_cache = netsnmp_memdup(&accesses, sizeof(accesses));
        if (accesses_cache == NULL) {
            netsnmp_set_request_error(reqinfo, requests,
                                      SNMP_ERR_RESOURCEUNAVAILABLE);
            return SNMP_ERR_NOERROR;
        }
        netsnmp_request_add_list_data(requests,
                                      netsnmp_create_data_list
                                      (TESTHANDLER_SET_NAME,
                                       accesses_cache, free));
        break;

    case MODE_SET_ACTION:
        /*
         * update current 
         */
        accesses = *(requests->requestvb->val.integer);
        DEBUGMSGTL(("testhandler", "updated accesses -> %d\n", accesses));
        break;

    case MODE_SET_UNDO:
        accesses =
            *((u_long *) netsnmp_request_get_list_data(requests,
                                                       TESTHANDLER_SET_NAME));
        break;

    case MODE_SET_COMMIT:
    case MODE_SET_FREE:
        /*
         * nothing to do 
         */
        break;
    }

    return SNMP_ERR_NOERROR;
}
#endif

/*! Parse smiv2 extensions for YANG leaf
  * Typical leaf:
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
  *      smiv2:max-access "read-write";
  *      smiv2:defval "42"; (optional)
 */
static int
mib_yang_leaf(clicon_handle h,
	      yang_stmt    *ys)
{
    int        retval = -1;
    netsnmp_handler_registration *nh = NULL;
    netsnmp_mib_handler *handler;
    int        ret;
    char      *oidstr = NULL;
    char      *modes_str = NULL;
    oid        oid1[MAX_OID_LEN] = {0,};
    size_t     sz1 = MAX_OID_LEN;
    int        modes;
    yang_stmt *yrestype;  /* resolved type */
    //    char      *restype;  /* resolved type */
    char      *origtype=NULL;   /* original type */
    char      *name;


    clicon_debug(1, "%s %s", __FUNCTION__, oidstr);
    if (yang_extension_value(ys, "oid", IETF_YANG_SMIV2_NS, NULL, &oidstr) < 0)
	goto done;
    if (oidstr == NULL)
	goto ok;
    if (snmp_parse_oid(oidstr, oid1, &sz1) == NULL){
	clicon_err(OE_SNMP, 0, "snmp_parse_oid");
	goto done;
    }
    if (yang_extension_value(ys, "max-access", IETF_YANG_SMIV2_NS, NULL, &modes_str) < 0)
	goto done;

    /* read-only, read-write, not-accessible, oaccessible-for-notify
     */
    if (modes_str == NULL)
	goto ok;
    modes = clicon_str2int(acc_map, modes_str);

    /* Default value, XXX How is this different from yang defaults?
     */
    //     if (yang_extension_value(ys, "defval", IETF_YANG_SMIV2_NS, NULL, &modes_str) < 0)
    //	goto done;

    /* get yang type of leaf */
    if (yang_type_get(ys, &origtype, &yrestype, NULL, NULL, NULL, NULL, NULL) < 0)
	goto done;
    //    restype = yrestype?yang_argument_get(yrestype):NULL;
    name = yang_argument_get(ys);
    if ((handler = netsnmp_create_handler(name, my_test_instance_handler)) == NULL){
	clicon_err(OE_SNMP, errno, "netsnmp_create_handler");
	goto done;
    }
    //    handler->myvoid = h;
    handler->myvoid =(void*)99;
    if ((nh = netsnmp_handler_registration_create(name,
						  handler,
						  oid1,
						  sz1,
						  modes)) == NULL){
	clicon_err(OE_SNMP, errno, "netsnmp_handler_registration_create");
	netsnmp_handler_free(handler);
	goto done;
    }
    clicon_debug(1, "%s %p", __FUNCTION__, nh);
    if ((ret = netsnmp_register_instance(nh)) < 0){
	/* XXX Failures are MIB_REGISTRATION_FAILED and MIB_DUPLICATE_REGISTRATION. */
	clicon_err(OE_SNMP, ret, "netsnmp_register_instance");
	goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}
	
/*! Check smiv2 extensions for 
 *
 * Called for each node in a mib-yang
 * Algorithm is to find smiv2:oid, then its associated parent type (eg leaf, container, list)
 * and then register callbacks.
 * @param[in]  ys   Yang node, of type unknown
 * @param[in]  arg  Argument, in effect clicon_handle
 * @retval     -1   Error
 * @retval     0    OK
 * @see yang_extension_value 
 * @see ys_populate_unknown
 */
static int
mib_yang_extension(yang_stmt *ys,
		   void      *arg)
{
    int           retval = -1;
    clicon_handle h = (clicon_handle)arg;

    switch(yang_keyword_get(ys)){
    case Y_LEAF:
	if (mib_yang_leaf(h, ys) < 0)
	    goto done;
	break;
    case Y_CONTAINER: // XXX
	break;
    case Y_LIST: // XXX
	break;
    default:
	break;
    }
    retval = 0;
done:
    return retval;
}

int
clixon_snmp_mib_yangs(clicon_handle h)
{
    int        retval = -1;
    char      *modname;
    cxobj     *x;
    yang_stmt *yspec;
    yang_stmt *ymod;

    /* XXX Hardcoded, replace this with generic MIB */
    init_testtable(); // XXX

    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_SNMP_MIB") != 0)
	    continue;
	if ((modname = xml_body(x)) == NULL)
	    continue;
	clicon_debug(1, "%s %s: \"%s\"", __FUNCTION__, xml_name(x), modname);
	/* Note, here we assume the Yang is loaded by some other mechanism and
	 * error if it not found.
	 * Alternatively, that YANG could be loaded.
	 * Problem is, if clixon_snmp has not loaded it, has backend done it?
	 * What happens if backend has not loaded it?
	 */
	if ((ymod = yang_find(yspec, Y_MODULE, modname)) == NULL){
	    clicon_err(OE_YANG, 0, "Mib-translated-yang %s not loaded", modname);
	    goto done;
	}
	/* Recursively traverse the mib-yang to find extensions */
	if (yang_apply(ymod, -1, mib_yang_extension, 1, h) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}
