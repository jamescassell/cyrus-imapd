/* httpd.h -- Common state for HTTP/WebDAV/CalDAV daemon
 *
 * Copyright (c) 1994-2011 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef HTTPD_H
#define HTTPD_H

#include <sasl/sasl.h>
#include <libxml/tree.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif /* HAVE_ZLIB */

#include "dav_prop.h"
#include "mailbox.h"
#include "spool.h"

/* Supported HTTP version */
#define HTTP_VERSION	"HTTP/1.1"
#define HTTPS_VERSION	"HTTPS/1.1"

/* Supported HTML DOCTYPE */
#define HTML_DOCTYPE \
    "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" " \
    "\"http://www.w3.org/TR/html4/loose.dtd\">\n"

/* SASL usage based on availability */
#ifdef SASL_NEED_HTTP
  #define HTTP_DIGEST_MECH "DIGEST-MD5"
  #define SASL_USAGE_FLAGS (SASL_NEED_HTTP | SASL_SUCCESS_DATA)
#else
  #define HTTP_DIGEST_MECH NULL  /* not supported by our SASL version */
  #define SASL_USAGE_FLAGS SASL_SUCCESS_DATA
#endif /* SASL_NEED_HTTP */


/* Path namespaces */
enum {
    URL_NS_DEFAULT = 0,
    URL_NS_PRINCIPAL,
    URL_NS_CALENDAR,
    URL_NS_ADDRESSBOOK,
    URL_NS_RSS
};

/* Bitmask of features/methods to allow, based on URL */
enum {
    ALLOW_READ =	(1<<0),
    ALLOW_WRITE =	(1<<1),
    ALLOW_DAV =		(1<<2),
    ALLOW_CAL =		(1<<3),
    ALLOW_CARD =	(1<<4),
    ALLOW_ALL =		0xff
};

#define MAX_QUERY_LEN	100

struct auth_scheme_t {
    unsigned idx;		/* Index value of the scheme */
    const char *name;		/* HTTP auth scheme name */
    const char *saslmech;	/* Corresponding SASL mech name */
    unsigned need_persist;	/* Need persistent connection? */
    unsigned is_server_first;	/* Is SASL mech server-first? */
    unsigned do_base64;		/* Base64 encode/decode auth data? */
    	     			/* Optional function to send success data */
    void (*send_success)(const char *name, const char *data);
    	     			/* Optional function to recv success data */
    const char *(*recv_success)(hdrcache_t hdrs);
};

/* Index into available schemes */
enum {
    AUTH_BASIC = 0,
    AUTH_DIGEST,
    AUTH_SPNEGO,
    AUTH_NTLM
};

/* List of HTTP auth schemes that we support */
extern struct auth_scheme_t auth_schemes[];

extern const char *digest_recv_success(hdrcache_t hdrs);


/* Request target context */
struct request_target_t {
    char path[MAX_MAILBOX_PATH+1]; /* working copy of URL path */
    char query[MAX_QUERY_LEN+1]; /* working copy of URL query */
    unsigned namespace;		/* namespace of path */
    char *user;			/* ptr to owner of collection (NULL = shared) */
    size_t userlen;
    char *collection;		/* ptr to collection name */
    size_t collen;
    char *resource;		/* ptr to resource name */
    size_t reslen;
    unsigned long allow;	/* bitmask of allowed features/methods */
};

/* Auth challenge context */
struct auth_challenge_t {
    struct auth_scheme_t *scheme;	/* Selected AUTH scheme */
    const char *param;	 		/* Server challenge */
};

/* Meta-data for error response */
struct error_t {
    const char *desc;			/* Error description */
    const struct precond *precond;	/* [Cal]DAV precondition */
    const char *resource;		/* Resource which lacks privileges */
    int rights;  			/* Privileges needed by resource */
};  

/* Meta-data for response body (payload & representation headers) */
struct resp_body_t {
    ulong len; 		/* Content-Length   */
    const char *enc;	/* Content-Encoding */
    const char *lang;	/* Content-Language */
    const char *loc;	/* Content-Location */
    const char *type;	/* Content-Type     */
    time_t lastmod;	/* Last-Modified    */
};

/* Transaction context */
struct transaction_t {
    const char *meth;			/* Method to be performed */
    unsigned flags;			/* Flags for this txn */
    struct request_target_t req_tgt;	/* Parsed target URL */
    hdrcache_t req_hdrs;    		/* Cached HTTP headers */
    struct buf req_body;		/* Buffered request body */
    struct auth_challenge_t auth_chal;	/* Authentication challenge */
    const char *loc;	    		/* Location: of resp representation */
    const char *etag;			/* ETag: of response representation */
    struct error_t error;		/* Error response meta-data */
    struct resp_body_t resp_body;	/* Response body meta-data */
#ifdef HAVE_ZLIB
    z_stream zstrm;			/* Compression context */
#endif
};

/* Transaction flags */
enum {
    HTTP_CLOSE =	(1<<0),
    HTTP_GZIP =	  	(1<<1),
    HTTP_CHUNKED =	(1<<2),
    HTTP_NOCACHE =	(1<<3),
    HTTP_READBODY =	(1<<4)
};

typedef int (*method_proc_t)(struct transaction_t *txn);
typedef int (*filter_proc_t)(struct transaction_t *txn,
			     const char *base, unsigned long len);

struct namespace_t {
    unsigned id;		/* Namespace identifier */
    const char *prefix;		/* Prefix of URL path denoting namespace */
    unsigned need_auth;		/* Do we need to auth for this namespace? */
    unsigned long allow;	/* Bitmask of allowed features/methods */
    method_proc_t proc[];	/* Functions to perform HTTP methods.
				 * MUST be a function pointer for EACH method
				 * (or NULL if method not supported)
				 * listed in, and in the SAME ORDER in which
				 * they appear in, the http_methods[] array.
				 */
};

extern const struct namespace_t namespace_calendar;
extern const struct namespace_t namespace_principal;
extern const struct namespace_t namespace_rss;
extern const struct namespace_t namespace_default;


/* XXX  These should be included in struct transaction_t */
extern struct buf serverinfo;
extern struct backend **backend_cached;
extern struct protstream *httpd_in;
extern struct protstream *httpd_out;
extern int https;
extern int httpd_tls_done;
extern int httpd_timeout;
extern int httpd_userisadmin;
extern int httpd_userisproxyadmin;
extern char *httpd_userid;
extern struct auth_state *httpd_authstate;
extern struct namespace httpd_namespace;

extern int parse_uri(const char *meth, const char *uri,
		     struct request_target_t *tgt, const char **errstr);
extern int target_to_mboxname(struct request_target_t *req_tgt, char *mboxname);
extern int http_mailbox_open(const char *name, struct mailbox **mailbox,
			     int locktype);
extern const char *http_statusline(long code);
extern void response_header(long code, struct transaction_t *txn);
extern void error_response(long code, struct transaction_t *txn);
extern void html_response(long code, struct transaction_t *txn, xmlDocPtr html);
extern void xml_response(long code, struct transaction_t *txn, xmlDocPtr xml);
extern void write_body(long code, struct transaction_t *txn,
		       const char *buf, unsigned len);
extern int meth_options(struct transaction_t *txn);
extern int get_doc(struct transaction_t *txn, filter_proc_t filter);
extern int meth_propfind(struct transaction_t *txn);
extern int check_precond(const char *meth, const char *etag, time_t lastmod,
			 hdrcache_t hdrcache);
extern int read_body(struct protstream *pin,
		     hdrcache_t hdrs, struct buf *body, const char **errstr);

#endif /* HTTPD_H */