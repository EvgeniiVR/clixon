/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
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

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Exported functions in this file are in clixon_cli_api.h */
#include "clixon_cli_api.h"
#include "cli_autocli.h"
#include "cli_common.h" /* internal functions */

/*! Given an xpath encoded in a cbuf, append a second xpath into the first
 *
 * The method reuses prefixes from xpath1 if they exist, otherwise the module prefix
 * from y is used. Unless the element is .., .
 * XXX: Predicates not handled
 * The algorithm is not fool-proof, there are many cases it may not work
 * To make it more complete, maybe parse the xpath to a tree and put it
 * back to an xpath after modifcations, something like:
   if (xpath_parse(yang_argument_get(ypath), &xpt) < 0)
     goto done;
   if (xpath_tree2cbuf(xpt, xcb) < 0)
     goto done;
and
traverse_canonical
 */
static int
xpath_append(cbuf      *cb0,
	     char      *xpath1,
	     yang_stmt *y,
	     cvec      *nsc)
{
    int    retval = -1;
    char **vec = NULL;
    char  *v;
    int    nvec;
    int    i;
    char  *myprefix;
    char  *id = NULL;
    char  *prefix = NULL;
    int    initialups = 1; /* If starts with ../../.. */
    char  *xpath0;

    if (cb0 == NULL){
	clicon_err(OE_XML, EINVAL, "cb0 is NULL");
	goto done;
    }
    if (xpath1 == NULL || strlen(xpath1)==0)
	goto ok;
    if ((myprefix = yang_find_myprefix(y)) == NULL)
	goto done;
    if ((vec = clicon_strsep(xpath1, "/", &nvec)) == NULL)
	goto done;
    if (xpath1[0] == '/')
	cbuf_reset(cb0);
    xpath0 = cbuf_get(cb0);
    for (i=0; i<nvec; i++){
	v = vec[i];
	if (strlen(v) == 0)
	    continue;
	if (nodeid_split(v, &prefix, &id) < 0)
	    goto done;
	if (strcmp(id, ".") == 0)
	    initialups = 0;
	else if (strcmp(id, "..") == 0){
	    if (initialups){
		/* Subtract from xpath0 */
		int j;
		for (j=cbuf_len(cb0); j >= 0; j--){
		    if (xpath0[j] != '/')
			continue;
		    cbuf_trunc(cb0, j);
		    break;
		}
	    }
	    else{
		initialups = 0;
		cprintf(cb0, "/%s", id);
	    }
	}
	else{
	    initialups = 0;
	    cprintf(cb0, "/%s:%s", prefix?prefix:myprefix, id);
	}
	if (prefix){
	    free(prefix);
	    prefix = NULL;
	}
	if (id){
	    free(id);
	    id = NULL;
	}
    }
 ok:
    retval = 0;
 done:
    if (prefix)
	free(prefix);
    if (id)
	free(id);
    free(vec); 
    return retval;
}

/*! Completion callback intended for automatically generated data model
 *
 * Returns an expand-type list of commands as used by cligen 'expand' 
 * functionality.
 *
 * Assume callback given in a cligen spec: a <x:int expand_dbvar("db" "<xmlkeyfmt>")
 * @param[in]   h        clicon handle 
 * @param[in]   name     Name of this function (eg "expand_dbvar")
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback ("<db>" "<xmlkeyfmt>")
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @see cli_expand_var_generate  This is where arg is generated
 */
int
expand_dbvar(void   *h, 
	     char   *name, 
	     cvec   *cvv, 
	     cvec   *argv, 
	     cvec   *commands,
	     cvec   *helptexts)
{
    int              retval = -1;
    char            *api_path_fmt;
    char            *api_path = NULL;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xpath = NULL;
    cxobj          **xvec = NULL;
    cxobj           *xe; /* direct ptr */
    cxobj           *xerr = NULL; /* free */
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    char            *bodystr0 = NULL; /* previous */
    cg_var          *cv;
    yang_stmt       *yspec;
    cxobj           *xtop = NULL; /* xpath root */
    cxobj           *xbot = NULL; /* xpath, NULL if datastore */
    yang_stmt       *y = NULL; /* yang spec of xpath */
    yang_stmt       *yp;
    cvec            *nsc = NULL;
    int              ret;
    int              cvvi = 0;
    cbuf            *cbxpath = NULL;
    yang_stmt       *ypath;
    yang_stmt       *ytype;
    
    if (argv == NULL || cvec_len(argv) != 2){
	clicon_err(OE_PLUGIN, EINVAL, "requires arguments: <db> <xmlkeyfmt>");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
	clicon_err(OE_PLUGIN, 0, "Error when accessing argument <db>");
	goto done;
    }
    dbstr  = cv_string_get(cv);
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_i(argv, 1)) == NULL){
	clicon_err(OE_PLUGIN, 0, "Error when accessing argument <api_path>");
	goto done;
    }
    api_path_fmt = cv_string_get(cv);
    /* api_path_fmt = /interface/%s/address/%s
     * api_path: -->  /interface/eth0/address/.*
     * xpath:    -->  /interface/[name="eth0"]/address
     */
    if (api_path_fmt2api_path(api_path_fmt, cvv, &api_path, &cvvi) < 0)
	goto done;
    /* Create config top-of-tree */
    if ((xtop = xml_new(DATASTORE_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
	goto done;
    xbot = xtop;
    /* This is primarily to get "y", 
     * xpath2xml would have worked!!
     * XXX: but y is just the first in this list, there could be other y:s?
     */
    if (api_path){
	if ((ret = api_path2xml(api_path, yspec, xtop, YC_DATANODE, 0, &xbot, &y, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    clixon_netconf_error(xerr, "Expand datastore symbol", NULL);
	    goto done;
	}
    }
    if (y==NULL)
	goto ok;
    /* Transform api-path to xpath for netconf */
    if (api_path2xpath(api_path, yspec, &xpath, &nsc, NULL) < 0)
	goto done;
    if (nsc != NULL){
	cvec_free(nsc);
	nsc = NULL;
    }
    if (xml_nsctx_yang(y, &nsc) < 0)
	goto done;
    if ((cbxpath = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cbxpath, "%s", xpath);
    if (clicon_option_bool(h, "CLICON_CLI_EXPAND_LEAFREF") &&
	(ytype = yang_find(y, Y_TYPE, NULL)) != NULL &&
	strcmp(yang_argument_get(ytype), "leafref") == 0){
	/* Special case for leafref. Detect leafref via Yang-type, 
	 * Get Yang path element, tentatively add the new syntax to the whole
	 * tree and apply the path to that.
	 * Last, the reference point for the xpath code below is changed to 
	 * the point of the tentative new xml.
	 * Here the whole syntax tree is loaded, and it would be better to offload
	 * such operations to the datastore by a generic xpath function.
	 */

	/* 
	 * The syntax for a path argument is a subset of the XPath abbreviated
	 * syntax.  Predicates are used only for constraining the values for the
	 * key nodes for list entries.  Each predicate consists of exactly one
	 * equality test per key, and multiple adjacent predicates MAY be
	 * present if a list has multiple keys.  The syntax is formally defined
	 * by the rule "path-arg" in Section 14.
	 * The "path" XPath expression is conceptually evaluated in the
	 * following context, in addition to the definition in Section 6.4.1:
	 *
	 * - If the "path" statement is defined within a typedef, the context
	 * node is the leaf or leaf-list node in the data tree that
	 * references the typedef.
	 * - Otherwise, the context node is the node in the data tree for which
	 * the "path" statement is defined.
	 */
	if ((ypath = yang_find(ytype, Y_PATH, NULL)) == NULL){
	    clicon_err(OE_DB, 0, "Leafref %s requires path statement", yang_argument_get(ytype));
	    goto done;
	}
	/*  */
	/* Extend xpath with leafref path: Append yang_argument_get(ypath) to xpath
	 */
	if (xpath_append(cbxpath, yang_argument_get(ypath), y, nsc) < 0)
	    goto done;
    }
    /* Get configuration based on cbxpath */
    if (clicon_rpc_get_config(h, NULL, dbstr, cbuf_get(cbxpath), nsc, &xt) < 0) 
	goto done;
    if ((xe = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xe, "Get configuration", NULL);
	goto ok; 
    }
    if (xpath_vec(xt, nsc, "%s", &xvec, &xlen, cbuf_get(cbxpath)) < 0) 
	goto done;
    /* Loop for inserting into commands cvec. 
     * Detect duplicates: for ordered-by system assume list is ordered, so you need
     * just remember previous
     * but for ordered-by system, check the whole list
     */
    bodystr0 = NULL;
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL)
	    continue; /* no body, cornercase */
	if ((y = xml_spec(x)) != NULL &&
	    (yp = yang_parent_get(y)) != NULL &&
	    yang_keyword_get(yp) == Y_LIST &&
	    yang_find(yp, Y_ORDERED_BY, "user") != NULL){
	    /* Detect duplicates linearly in existing values */
	    {
		cg_var *cv = NULL;
		while ((cv = cvec_each(commands, cv)) != NULL)
		    if (strcmp(cv_string_get(cv), bodystr) == 0)
			break;
		if (cv == NULL)
		    cvec_add_string(commands, NULL, bodystr);
	    }
	}
	else{
	    if (bodystr0 && strcmp(bodystr, bodystr0) == 0)
		continue; /* duplicate, assume sorted */
	    bodystr0 = bodystr;
	    /* RFC3986 decode */
	    cvec_add_string(commands, NULL, bodystr);
	}
    }
 ok:
    retval = 0;
  done:
    if (cbxpath)
	cbuf_free(cbxpath);
    if (xerr)
	xml_free(xerr);
    if (nsc)
	xml_nsctx_free(nsc);
    if (api_path)
	free(api_path);
    if (xvec)
	free(xvec);
    if (xtop)
	xml_free(xtop);
    if (xt)
	xml_free(xt);
    if (xpath) 
	free(xpath);
    return retval;
}

/*! CLI callback show yang spec. If arg given matches yang argument string */
int
show_yang(clicon_handle h, 
	  cvec         *cvv, 
	  cvec         *argv)
{
    yang_stmt *yn;
    char      *str = NULL;
    yang_stmt *yspec;

    yspec = clicon_dbspec_yang(h);	
    if (cvec_len(argv) > 0){
	str = cv_string_get(cvec_i(argv, 0));
	yn = yang_find(yspec, 0, str);
    }
    else
	yn = yspec;
    yang_print_cb(stdout, yn, cligen_output); /* Doesnt use cligen_output */
    return 0;
}

/*! Show configuration and state internal function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  state If set, show both config and state, otherwise only config
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"|"candidate"|"startup" # note only running state=1
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name='foo']"
 *   <namespace> If xpath set, the namespace the symbols in xpath belong to (optional)
 *   <prefix>   to print before cli syntax output (optional)
 * @code
 *   show config id <n:string>, cli_show_config("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @note if state parameter is set, then db must be running
 * @see cli_show_auto1
 */
static int
cli_show_config1(clicon_handle h, 
		 int           state,
		 cvec         *cvv, 
		 cvec         *argv)
{
    int              retval = -1;
    char            *db;
    char            *formatstr;
    char            *xpath;
    enum format_enum format;
    cbuf            *cbxpath = NULL;
    char            *val = NULL;
    cxobj           *xt = NULL;
    cxobj           *xc;
    cxobj           *xerr;
    yang_stmt       *yspec;
    char            *namespace = NULL;
    cvec            *nsc = NULL;
    char            *prefix = NULL;

    if (cvec_len(argv) < 3 || cvec_len(argv) > 5){
	clicon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <dbname>,<format>,<xpath>[,<namespace>, [<prefix>]]", cvec_len(argv));

	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    /* First argv argument: Database */
    db = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Format */
    formatstr = cv_string_get(cvec_i(argv, 1));
    if ((int)(format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    /* Third argv argument: xpath */
    xpath = cv_string_get(cvec_i(argv, 2));

    /* Create XPATH variable string */
    if ((cbxpath = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    cprintf(cbxpath, "%s", xpath);	
    /* Fourth argument is namespace */
    if (cvec_len(argv) > 3){
	namespace = cv_string_get(cvec_i(argv, 3));
	if ((nsc = xml_nsctx_init(NULL, namespace)) == NULL)
	    goto done;
    }
    if (cvec_len(argv) > 4){
	prefix = cv_string_get(cvec_i(argv, 4));
    }
    if (state == 0){     /* Get configuration-only from database */
	if (clicon_rpc_get_config(h, NULL, db, cbuf_get(cbxpath), nsc, &xt) < 0)
	    goto done;
    }
    else {               /* Get configuration and state from database */
	if (strcmp(db, "running") != 0){
	    clicon_err(OE_FATAL, 0, "Show state only for running database, not %s", db);
	    goto done;
	}
	if (clicon_rpc_get(h, cbuf_get(cbxpath), nsc, CONTENT_ALL, -1, &xt) < 0)
	    goto done;
    }
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    /* Print configuration according to format */
    switch (format){
    case FORMAT_XML:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    cli_xml2file(xc, 0, 1, cligen_output);
	break;
    case FORMAT_JSON:
	xml2json_cb(stdout, xt, 1, cligen_output);
	break;
    case FORMAT_TEXT:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    cli_xml2txt(xc, cligen_output, 0); /* tree-formed text */
	break;
    case FORMAT_CLI:
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL)
	    if (xml2cli(h, stdout, xc, prefix, cligen_output) < 0)
		goto done;
	break;
    case FORMAT_NETCONF:
	cligen_output(stdout, "<rpc xmlns=\"%s\" %s><edit-config><target><candidate/></target><config>\n",
		      NETCONF_BASE_NAMESPACE, NETCONF_MESSAGE_ID_ATTR);
	xc = NULL; /* Dont print xt itself */
	while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	    cli_xml2file(xc, 2, 1, cligen_output);
	cligen_output(stdout, "</config></edit-config></rpc>]]>]]>\n");
	break;
    }
    retval = 0;
done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xt)
	xml_free(xt);
    if (val)
	free(val);
    if (cbxpath)
	cbuf_free(cbxpath);
    return retval;
}

/*! Show configuration and state CLIGEN callback function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name="%s"]"
 *   <namespace> If xpath set, the namespace the symbols in xpath belong to (optional)
 *   <prefix>   to print before cli syntax output
 * @code
 *   show config id <n:string>, cli_show_config("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @see cli_show_config_state  For config and state data (not only config)
 */
int
cli_show_config(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    return cli_show_config1(h, 0, cvv, argv);
}

/*! Show configuration and state CLIgen callback function
 *
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <dbname>  "running"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <xpath>   xpath expression, that may contain one %, eg "/sender[name="%s"]"
 *   <varname> optional name of variable in cvv. If set, xpath must have a '%s'
 * @code
 *   show state id <n:string>, cli_show_config_state("running","xml","iface[name='foo']","urn:example:example");
 * @endcode
 * @see cli_show_config  For config-only, no state
 */
int
cli_show_config_state(clicon_handle h, 
		      cvec         *cvv, 
		      cvec         *argv)
{
    return cli_show_config1(h, 1, cvv, argv);
}

/*! Show configuration as text given an xpath using canonical namespace
 *
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line must contain xpath and default namespace (if any)
 * @param[in]  argv  A string: <dbname>
 * @note  Hardcoded that variable xpath and ns cvv must exist. (kludge)
 */
int
show_conf_xpath(clicon_handle h, 
		cvec         *cvv, 
		cvec         *argv)
{
    int              retval = -1;
    char            *dbname;
    char            *xpath;
    cg_var          *cv;
    cxobj           *xt = NULL;
    cxobj           *xerr;
    cxobj          **xv = NULL;
    size_t           xlen;
    int              i;
    cvec            *nsc = NULL;
    yang_stmt       *yspec;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, EINVAL, "Requires one element to be <dbname>");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    dbname = cv_string_get(cvec_i(argv, 0));
    /* Dont get attr here, take it from arg instead */
    if (strcmp(dbname, "running") != 0 && 
	strcmp(dbname, "candidate") != 0 && 
	strcmp(dbname, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbname);	
	goto done;
    }
    /* Look for xpath in command (kludge: cv must be called "xpath") */
    if ((cv = cvec_find(cvv, "xpath")) == NULL){
	clicon_err(OE_PLUGIN, EINVAL, "Requires one variable to be <xpath>");
	goto done;
    }
    xpath = cv_string_get(cv);

    /* Create canonical namespace */
    if (xml_nsctx_yangspec(yspec, &nsc) < 0)
	goto done;
    /* Look for and add default namespace variable in command */
    if ((cv = cvec_find(cvv, "ns")) != NULL){
	if (xml_nsctx_add(nsc, NULL, cv_string_get(cv)) < 0)
	    goto done;
    }
#if 0 /* Use state get instead of config (XXX: better use this but test_cli.sh fails) */
    if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
    	goto done;
#else
    if (clicon_rpc_get_config(h, NULL, dbname, xpath, nsc, &xt) < 0)
    	goto done;
#endif
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    if (xpath_vec(xt, nsc, "%s", &xv, &xlen, xpath) < 0) 
	goto done;
    for (i=0; i<xlen; i++)
	cli_xml2file(xv[i], 0, 1, fprintf);

    retval = 0;
done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xv)
	free(xv);
    if (xt)
	xml_free(xt);
    return retval;
}

int cli_show_version(clicon_handle h,
		     cvec         *vars,
		     cvec         *argv)
{
    fprintf(stdout, "%s\n", CLIXON_VERSION_STRING);
    return 0;
}

/*! Generic show configuration CLIgen callback using generated CLI syntax
 *
 * This callback can be used only in context of an autocli generated syntax tree, such as:
 *   show @datamodel, cli_show_auto();
 *
 * @param[in]  h     CLICON handle
 * @param[in]  state If set, show both config and state, otherwise only config
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  argv  String vector: <dbname> <format> <xpath> [<varname>]
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <prefix>   to print before cli syntax output
 * @note if state parameter is set, then db must be running
 * @note that first argument is generated by code.
 * @see cli_show_config1
 */
static int 
cli_show_generated(clicon_handle h,
		   int           state,
		   cvec         *cvv,
		   cvec         *argv)
{
    int              retval = 1;
    yang_stmt       *yspec;
    char            *api_path_fmt;  /* xml key format */
    char            *db;
    char            *xpath = NULL;
    cvec            *nsc = NULL;
    char            *formatstr;
    enum format_enum format = FORMAT_XML;
    cxobj           *xt = NULL;
    cxobj           *xp;
    cxobj           *xp_helper;
    cxobj           *xerr;
    char            *api_path = NULL;
    char            *prefix = NULL;
    enum rfc_6020    ys_keyword;
    int		     i = 0;
    int              cvvi = 0;

    if (cvec_len(argv) < 3 || cvec_len(argv) > 4){
	clicon_err(OE_PLUGIN, EINVAL, "Usage: <api-path-fmt>* <database> <format> <prefix>. (*) generated.");
	goto done;
    }
    /* First argv argument: API_path format */
    api_path_fmt = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Database */
    db = cv_string_get(cvec_i(argv, 1));
    /* Third format: output format */
    formatstr = cv_string_get(cvec_i(argv, 2));
    if (cvec_len(argv) > 3){
	/* Fourth format: prefix to print before cli syntax */
	prefix = cv_string_get(cvec_i(argv, 3));
    }
    if ((int)(format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if (api_path_fmt2api_path(api_path_fmt, cvv, &api_path, &cvvi) < 0)
	goto done;
    if (api_path2xpath(api_path, yspec, &xpath, &nsc, NULL) < 0)
	goto done;
    /* XXX Kludge to overcome a trailing / in show, that I cannot add to
     * yang2api_path_fmt_1 where it should belong.
     */
    if (xpath[strlen(xpath)-1] == '/')
	xpath[strlen(xpath)-1] = '\0';

    if (state == 0){   /* Get configuration-only from database */
	if (clicon_rpc_get_config(h, NULL, db, xpath, nsc, &xt) < 0)
	    goto done;
    }
    else{              /* Get configuration and state from database */
	if (strcmp(db, "running") != 0){
	    clicon_err(OE_FATAL, 0, "Show state only for running database, not %s", db);
	    goto done;
	}
	if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
	    goto done;
    }
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    if ((xp = xpath_first(xt, nsc, "%s", xpath)) != NULL){
	/* Print configuration according to format */
	ys_keyword = yang_keyword_get(xml_spec(xp));
	if (ys_keyword == Y_LIST)
		xp_helper = xml_child_i(xml_parent(xp), i);
	else
		xp_helper = xp;

	switch (format){
	case FORMAT_CLI:
	    if (xml2cli(h, stdout, xp, prefix, cligen_output) < 0) /* cli syntax */
		goto done;
	    break;
	case FORMAT_NETCONF:
	    fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
	    cli_xml2file(xp, 2, 1, fprintf);
	    fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
	    break;
	default:
	    for (; i < xml_child_nr(xml_parent(xp)) ; ++i, xp_helper = xml_child_i(xml_parent(xp), i)) {
		switch (format){
		case FORMAT_XML:
		    cli_xml2file(xp_helper, 0, 1, fprintf);
		    break;
		case FORMAT_JSON:
		    xml2json_cb(stdout, xp_helper, 1, cligen_output);
		    break;
		case FORMAT_TEXT:	
		    cli_xml2txt(xp_helper, cligen_output, 0);  /* tree-formed text */
		    break;
		default: /* see cli_show_config() */
		    break;
		}
		if (ys_keyword != Y_LIST)
		    break;
	    }
	    break;
	}
    }
    retval = 0;
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (api_path)
	free(api_path);
    if (xpath)
	free(xpath);
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Generic show configuration CLIgen callback using generated CLI syntax
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"|"candidate"|"startup"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <prefix>   to print before cli syntax outptu
 * @see cli_show_auto_state  For config and state
 * @note SHOULD be used: ... @datamodel, cli_show_auto(<dbname>,...) to get correct #args
 * @see cli_auto_show
 */
int 
cli_show_auto(clicon_handle h,
	      cvec         *cvv,
	      cvec         *argv)
{
    return cli_show_generated(h, 0, cvv, argv);
}

/*! Generic show config and state CLIgen callback using generated CLI syntax
 * Format of argv:
 *   <api_path_fmt> Generated API PATH
 *   <dbname>  "running"
 *   <format>  "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <prefix>   to print before cli syntax output
 * @see cli_show_auto    For config only
 * @see cli_show_config_state Not auto-generated
 */
int 
cli_show_auto_state(clicon_handle h,
		    cvec         *cvv,
		    cvec         *argv)
{
    return cli_show_generated(h, 1, cvv, argv);
}

/*! Show clixon configuration options as loaded
 */
int 
cli_show_options(clicon_handle h,
		 cvec         *cvv,
		 cvec         *argv)
{
    int            retval = -1;
    clicon_hash_t *hash = clicon_options(h);
    int            i;
    char         **keys = NULL;
    void          *val;
    size_t         klen;
    size_t         vlen;
    cxobj         *x = NULL;
    
    if (clicon_hash_keys(hash, &keys, &klen) < 0)
	goto done;
    for(i = 0; i < klen; i++) {
	val = clicon_hash_value(hash, keys[i], &vlen);
	if (vlen){
	    if (((char*)val)[vlen-1]=='\0') /* assume string */
		fprintf(stdout, "%s: \"%s\"\n", keys[i], (char*)val);
	    else
		fprintf(stdout, "%s: 0x%p , length %zu\n", keys[i], val, vlen);
	}
	else
	    fprintf(stdout, "%s: NULL\n", keys[i]);
    }
    /* Next print CLICON_FEATURE, CLICON_YANG_DIR and CLICON_SNMP_MIB from config tree
     * Since they are lists they are placed in the config tree.
     */
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_YANG_DIR") != 0)
	    continue;
	fprintf(stdout, "%s: \"%s\"\n", xml_name(x), xml_body(x));
    }
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_FEATURE") != 0)
	    continue;
	fprintf(stdout, "%s: \"%s\"\n", xml_name(x), xml_body(x));
    }
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_SNMP_MIB") != 0)
	    continue;
	fprintf(stdout, "%s: \"%s\"\n", xml_name(x), xml_body(x));
    }
   retval = 0;
 done:
    if (keys)
	free(keys);
    return retval;
}

/*! Show pagination
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. Format: <xpath> <prefix> <namespace> <format> <limit>
 * Also, if there is a cligen variable called "xpath" it will override argv xpath arg
 */
int
cli_pagination(clicon_handle h,
	       cvec         *cvv,
	       cvec         *argv)
{
    int              retval = -1;
    cbuf            *cb = NULL;    
    char            *xpath = NULL;
    char            *prefix = NULL;
    char            *namespace = NULL;
    cxobj           *xret = NULL;
    cxobj           *xerr;
    cvec            *nsc = NULL;
    char            *str;
    enum format_enum format;
    cxobj           *xc;
    cg_var          *cv;
    int              i;
    int              j;
    uint32_t         limit = 0;
    cxobj          **xvec = NULL;
    size_t           xlen;
    int              locked = 0;
    
    if (cvec_len(argv) != 5){
	clicon_err(OE_PLUGIN, 0, "Expected usage: <xpath> <prefix> <namespace> <format> <limit>");
	goto done;
    }
    /* prefix:variable overrides argv */
    if ((cv = cvec_find(cvv, "xpath")) != NULL)
	xpath = cv_string_get(cv);
    else
	xpath = cvec_i_str(argv, 0);
    prefix = cvec_i_str(argv, 1);
    namespace = cvec_i_str(argv, 2);
    str = cv_string_get(cvec_i(argv, 3));     /* Fourthformat: output format */
    if ((int)(format = format_str2int(str)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", str);
	goto done;
    }
    if ((str = cv_string_get(cvec_i(argv, 4))) != NULL){
	if (parse_uint32(str, &limit, NULL) < 1){
	    clicon_err(OE_UNIX, errno, "error parsing limit:%s", str);
	    goto done;
	}
    }
    if (limit == 0){
	clicon_err(OE_UNIX, EINVAL, "limit is 0");
	goto done;
    }
    if ((nsc = xml_nsctx_init(prefix, namespace)) == NULL)
	goto done;
    if (clicon_rpc_lock(h, "running") < 0)
	goto done;
    locked++;
    for (i = 0;; i++){
	if (clicon_rpc_get_pageable_list(h, "running", xpath, nsc,
					 CONTENT_ALL,
					 -1,        /* depth */
					 limit*i,  /* offset */
					 limit,    /* limit */
					 NULL, NULL, NULL, /* nyi */
					 &xret) < 0){
	    goto done;
	}
	if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
	    clixon_netconf_error(xerr, "Get configuration", NULL);
	    goto done;
	}
	if (xpath_vec(xret, nsc, "%s", &xvec, &xlen, xpath) < 0)
	    goto done;
	for (j = 0; j<xlen; j++){
	    xc = xvec[j];
	    switch (format){
	    case FORMAT_XML:
		clicon_xml2file_cb(stdout, xc, 0, 1, cligen_output);
		break;
	    case FORMAT_JSON:
		xml2json_cb(stdout, xc, 1, cligen_output);
		break;
	    case FORMAT_TEXT:
		xml2txt_cb(stdout, xc, cligen_output); /* tree-formed text */
		break;
	    case FORMAT_CLI:
		/* hardcoded to compress and list-keyword = nokey */
		xml2cli(h, stdout, xc, NULL, cligen_output);
		break;
	    default:
		break;
	    }
	    if (cli_output_status() < 0)
		break;
	} /* for j */
	if (cli_output_status() < 0)
	    break;
	if (xlen != limit) /* Break if fewer elements than requested */
	    break;
	if (xret){
	    xml_free(xret);
	    xret = NULL;
	}
	if (xvec){
	    free(xvec);
	    xvec = NULL;
	}
    } /* for i */
    retval = 0;
 done:
    if (locked)
	clicon_rpc_unlock(h, "running");
    if (xvec)
	free(xvec);
    if (xret)
	xml_free(xret);
    if (nsc)
	cvec_free(nsc);
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Translate from XML to CLI commands
 *
 * Howto: join strings and pass them down. 
 * Identify unique/index keywords for correct set syntax.
 * @param[in] h        Clicon handle
 * @param[in] f        Output FILE (eg stdout)
 * @param[in] xn       XML Parse-tree (to translate)
 * @param[in] prepend  Print this text in front of all commands.
 * @param[in] fn       Callback to make print function
 * @see xml2cli  XXX should probably use the generic function
 */
int
xml2cli(clicon_handle    h,
	FILE            *f, 
	cxobj           *xn,
	char            *prepend,
	clicon_output_cb *fn)
{
    int        retval = -1;
    cxobj     *xe = NULL;
    cbuf      *cbpre = NULL;
    yang_stmt *ys;
    int        match;
    char      *body;
    int        compress = 0;
    autocli_listkw_t listkw;
    int           exist = 0;

    if (autocli_list_keyword(h, &listkw) < 0)
	goto done;
    if (xml_type(xn)==CX_ATTR)
	goto ok;
    if ((ys = xml_spec(xn)) == NULL)
	goto ok;
    if (yang_extension_value(ys, "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
	goto done;
    if (exist)
	goto ok;
    /* If leaf/leaf-list or presence container, then print line */
    if (yang_keyword_get(ys) == Y_LEAF ||
	yang_keyword_get(ys) == Y_LEAF_LIST){
	if (prepend)
	    (*fn)(f, "%s", prepend);
	if (listkw != AUTOCLI_LISTKW_NONE)
	    (*fn)(f, "%s ", xml_name(xn));
	if ((body = xml_body(xn)) != NULL){
	    if (index(body, ' '))
		(*fn)(f, "\"%s\"", body);
	    else
		(*fn)(f, "%s", body);
	}
	(*fn)(f, "\n");
	goto ok;
    }
    /* Create prepend variable string */
    if ((cbpre = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    if (prepend)
	cprintf(cbpre, "%s", prepend);

    /* If non-presence container && HIDE mode && only child is 
     * a list, then skip container keyword
     * See also yang2cli_container */
    if (autocli_compress(h, ys, &compress) < 0)
	goto done;
    if (!compress)
	cprintf(cbpre, "%s ", xml_name(xn));

    /* If list then first loop through keys */
    if (yang_keyword_get(ys) == Y_LIST){
	xe = NULL;
	while ((xe = xml_child_each(xn, xe, -1)) != NULL){
	    if ((match = yang_key_match(ys, xml_name(xe), NULL)) < 0)
		goto done;
	    if (!match)
		continue;
	    if (listkw == AUTOCLI_LISTKW_ALL)
		cprintf(cbpre, "%s ", xml_name(xe));
	    cprintf(cbpre, "%s ", xml_body(xe));
	}
    }
    else if ((yang_keyword_get(ys) == Y_CONTAINER) &&
	     yang_find(ys, Y_PRESENCE, NULL) != NULL){
	/* If presence container, then print as leaf (but continue to children) */
	if (prepend)
	    (*fn)(f, "%s", prepend);
	if (listkw != AUTOCLI_LISTKW_NONE)
	    (*fn)(f, "%s ", xml_name(xn));
	if ((body = xml_body(xn)) != NULL){
	    if (index(body, ' '))
		(*fn)(f, "\"%s\"", body);
	    else
		(*fn)(f, "%s", body);
	}
	(*fn)(f, "\n");
    }

    /* For lists, print cbpre before its elements */
    if (yang_keyword_get(ys) == Y_LIST)
	(*fn)(f, "%s\n", cbuf_get(cbpre));	
    /* Then loop through all other (non-keys) */
    xe = NULL;
    while ((xe = xml_child_each(xn, xe, -1)) != NULL){
	if (yang_keyword_get(ys) == Y_LIST){
	    if ((match = yang_key_match(ys, xml_name(xe), NULL)) < 0)
		goto done;
	    if (match)
		continue; /* Not key itself */
	}
	if (xml2cli(h, f, xe, cbuf_get(cbpre), fn) < 0)
	    goto done;
    }
 ok:
    retval = 0;
 done:
    if (cbpre)
	cbuf_free(cbpre);
    return retval;
}
