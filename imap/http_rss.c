/* http_rss.c -- Routines for handling RSS feeds of mailboxes in httpd
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

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <syslog.h>

#include <libxml/HTMLtree.h>
#include <libxml/tree.h>

#include "acl.h"
#include "charset.h"
#include "global.h"
#include "httpd.h"
#include "http_err.h"
#include "imap_err.h"
#include "mailbox.h"
#include "map.h"
#include "mboxlist.h"
#include "message.h"
#include "parseaddr.h"
#include "rfc822date.h"
#include "seen.h"
#include "util.h"
#include "version.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"

#define MAX_FEED	100
#define MAX_SECTION_LEN	128

static int meth_get(struct transaction_t *txn);
static int rss_to_mboxname(struct request_target_t *req_tgt,
			   char *mboxname, uint32_t *uid,
			   char *section);
static void list_feeds(struct transaction_t *txn);
static int fetch_message(struct transaction_t *txn, struct mailbox *mailbox,
			 unsigned recno, uint32_t uid,
			 struct index_record *record, struct body **body,
			 const char **msg_base, unsigned long *msg_size);
static void list_messages(struct transaction_t *txn, struct mailbox *mailbox);
static void display_message(struct transaction_t *txn,
			    const char *mboxname, uint32_t uid,
			    struct body *body, const char *msg_base);
static void fetch_part(struct transaction_t *txn, struct body *body,
		       const char *findsection, const char *cursection,
		       const char *msg_base);


/* Namespace for RSS feeds of mailboxes */
const struct namespace_t namespace_rss = {
    URL_NS_RSS, "/rss", 1 /* auth */, ALLOW_READ,
    {
	NULL,			/* ACL		*/
	NULL,			/* COPY		*/
	NULL,			/* DELETE	*/
	&meth_get,		/* GET		*/
	&meth_get,		/* HEAD		*/
	NULL,			/* LOCK		*/
	NULL,			/* MKCALENDAR	*/
	NULL,			/* MKCOL	*/
	NULL,			/* MOVE		*/
	&meth_options,		/* OPTIONS	*/
	NULL,			/* POST		*/
	NULL,			/* PROPFIND	*/
	NULL,			/* PROPPATCH	*/
	NULL,			/* PUT		*/
	NULL,			/* REPORT	*/
	NULL			/* UNLOCK	*/
    }
};

/* Perform a GET/HEAD request */
static int meth_get(struct transaction_t *txn)
{
    int ret = 0, r;
    char mailboxname[MAX_MAILBOX_BUFFER+1], section[MAX_SECTION_LEN+1];
    uint32_t uid;
    struct mailbox *mailbox = NULL;

    /* Construct mailbox name corresponding to request target URI */
    if ((r = rss_to_mboxname(&txn->req_tgt, mailboxname, &uid, section))) {
	txn->errstr = error_message(r);
	return HTTP_NOT_FOUND;
    }

    /* If no mailboxname, list all available feeds */
    if (!*mailboxname) {
	list_feeds(txn);
	return 0;
    }

    /* Open mailbox for reading */
    if ((r = mailbox_open_irl(mailboxname, &mailbox))) {
	syslog(LOG_ERR, "mailbox_open_irl(%s) failed: %s",
	       mailboxname, error_message(r));
	txn->errstr = error_message(r);

	switch (r) {
	case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
	case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
	default: return HTTP_SERVER_ERROR;
	}
    }

    /* If no UID specified, list messages as an RSS feed */
    if (!uid) list_messages(txn, mailbox);
    else if (uid > mailbox->i.last_uid) {
	txn->errstr = "Message does not exist";
	ret = HTTP_NOT_FOUND;
    }
    else {
	struct index_record record;
	const char *msg_base;
	unsigned long msg_size;
	struct body *body;

	/* Fetch the message */
	if (!(ret = fetch_message(txn, mailbox, 0, uid,
				  &record, &body, &msg_base, &msg_size))) {
	    if (!*section) {
		/* Return entire message formatted as text/html */
		display_message(txn, mailbox->name, record.uid, body, msg_base);
	    }
	    else if (!strcmp(section, "0")) {
		/* Return entire message as text/plain */
		txn->resp_body.type = "text/plain";
		txn->resp_body.len = msg_size;

		response_header(HTTP_OK, txn);

		if (txn->meth[0] != 'H')
		    prot_write(httpd_out, msg_base, msg_size);
	    }
	    else {
		/* Fetch, decode, and return the specified MIME message part */
		fetch_part(txn, body, section, "1", msg_base);
	    }

	    mailbox_unmap_message(mailbox, record.uid, &msg_base, &msg_size);

	    if (body) {
		message_free_body(body);
		free(body);
	    }
	}
    }

    if (mailbox) mailbox_close(&mailbox);

    return ret;

}


/* Create a mailbox name from the request URL */ 
static int rss_to_mboxname(struct request_target_t *req_tgt,
			   char *mboxname, uint32_t *uid,
			   char *section)
{
    char *start, *end;
    size_t len;

    *uid = 0;
    *section = 0;

    /* Clip off RSS prefix */
    start = req_tgt->path + strlen("/rss");
    if (*start == '/') start++;
    end = start + strlen(start);

    if ((end > start) && (end[-1] == '/')) end--;

    len = end - start;
    if (len > MAX_MAILBOX_BUFFER) return IMAP_MAILBOX_BADNAME;

    strncpy(mboxname, start, len);
    mboxname[len] = '\0';

    mboxname_hiersep_tointernal(&httpd_namespace, mboxname, len);

    if (!strncasecmp(req_tgt->query, "uid=", 4)) {
	/* UID */
	*uid = strtoul(req_tgt->query+4, &end, 10);
	if (!*uid) *uid = -1;

	if (!strncasecmp(end, ";section=", 9)) {
	    /* SECTION */
	    strlcpy(section, end+9, MAX_SECTION_LEN);
	}
    }

    return 0;
}


/*
 * mboxlist_findall() callback function to list RSS feeds
 */
struct list_rock {
    struct transaction_t *txn;
    struct buf *buf;
};

int list_cb(char *name, int matchlen, int maycreate __attribute__((unused)),
	    void *rock)
{
    static char lastname[MAX_MAILBOX_BUFFER];
    char href[MAX_MAILBOX_PATH+1];
    struct mboxlist_entry entry;
    struct list_rock *lrock = (struct list_rock *) rock;

    /* We have to reset the initial state.
     * Handle it as a dirty hack.
     */
    if (!name) {
	lastname[0] = '\0';
	return 0;
    }

    /* don't repeat */
    if (matchlen == (int) strlen(lastname) &&
	!strncmp(name, lastname, matchlen)) return 0;

    strncpy(lastname, name, matchlen);
    lastname[matchlen] = '\0';

    /* Lookup the mailbox and make sure its readable */
    mboxlist_lookup(name, &entry, NULL);
    if (!entry.acl ||
	!(cyrus_acl_myrights(httpd_authstate, entry.acl) & ACL_READ))
	return 0;

    /* Add mailbox to our HTML list */
    snprintf(href, sizeof(href), ".rss.%s", name);
    mboxname_hiersep_toexternal(&httpd_namespace, href, 0);

    buf_reset(lrock->buf);
    buf_printf(lrock->buf, "<li><a href=\"%s\">%s</a></li>\n", href, name);
    body_chunk(lrock->txn, lrock->buf->s, lrock->buf->len);

    return 0;
}


/* Create a HTML document listing all RSS feeds available to the user */
static void list_feeds(struct transaction_t *txn)
{
    int r;
    struct buf buf = BUF_INITIALIZER;
    struct list_rock lrock;

    /* Setup for chunked response */
    txn->flags |= HTTP_CHUNKED;
    txn->resp_body.type = "text/html; charset=utf-8";

    response_header(HTTP_OK, txn);

    /* Start HTML */
    buf_printf(&buf, HTML_DOCTYPE "\n");
    buf_printf(&buf, "<html><head><title>Cyrus RSS Feeds</title></head>\n");
    buf_printf(&buf, "<body><h2>Cyrus RSS Feeds</h2><ul>\n");

    body_chunk(txn, buf.s, buf.len);

    lrock.txn = txn;
    lrock.buf = &buf;
    list_cb(NULL, 0, 0, NULL);
    r = mboxlist_findall(NULL, "*", httpd_userisadmin, NULL, httpd_authstate,
			 list_cb, &lrock);

    /* End of HTML */
    buf_reset(&buf);
    buf_printf(&buf, "</ul></body></html>");
    body_chunk(txn, buf.s, buf.len);

    /* End of output */
    body_chunk(txn, NULL, 0);

    buf_free(&buf);
}


/* Fetch the index record & bodystructure, and mmap the message */
static int fetch_message(struct transaction_t *txn, struct mailbox *mailbox,
			 unsigned recno, uint32_t uid,
			 struct index_record *record, struct body **body,
			 const char **msg_base, unsigned long *msg_size)
{
    int r;

    *body = NULL;
    *msg_base = NULL;

    /* Fetch index record for the message */
    if (uid) r = mailbox_find_index_record(mailbox, uid, record);
    else r = mailbox_read_index_record(mailbox, recno, record);
    if ((r == CYRUSDB_NOTFOUND) ||
	(record->system_flags & (FLAG_DELETED|FLAG_EXPUNGED))) {
	txn->errstr = "Message has been removed";
	return HTTP_GONE;
    }
    else if (r) {
	syslog(LOG_ERR, "find index record failed");
	txn->errstr = error_message(r);
	return HTTP_SERVER_ERROR;
    }

    /* Fetch cache record for the message */
    if ((r = mailbox_cacherecord(mailbox, record))) {
	syslog(LOG_ERR, "read cache failed");
	txn->errstr = error_message(r);
	return HTTP_SERVER_ERROR;
    }

    /* Read message bodystructure */
    message_read_bodystructure(record, body);

    /* Map the message into memory */
    mailbox_map_message(mailbox, record->uid, msg_base, msg_size);

    return 0;
}
			 


/* List messages as an RSS feed */
static void list_messages(struct transaction_t *txn, struct mailbox *mailbox)
{
    xmlDocPtr outdoc;
    xmlNodePtr root, chan, item;
    const char **host;
    unsigned recno, recentuid = 0, feed = MAX_FEED;
    struct buf buf = BUF_INITIALIZER;
#if 0
    /* Obtain recentuid */
    if (mailbox_internal_seen(mailbox, httpd_userid)) {
	recentuid = mailbox->i.recentuid;
    }
    else if (httpd_userid) {
	struct seen *seendb = NULL;
	struct seendata sd;

	r = seen_open(httpd_userid, SEEN_CREATE, &seendb);
	if (!r) r = seen_read(seendb, mailbox->uniqueid, &sd);
	seen_close(&seendb);

	/* handle no seen DB gracefully */
	if (r) {
	    recentuid = mailbox->i.last_uid;
	    syslog(LOG_ERR, "Could not open seen state for %s (%s)",
		   httpd_userid, error_message(r));
	}
	else {
	    recentuid = sd.lastuid;
	    free(sd.seenuids);
	}
    }
    else {
	recentuid = mailbox->i.last_uid; /* nothing is recent! */
    }
#endif
    /* Set up the RSS <channel> response for the mailbox */
    outdoc = xmlNewDoc(BAD_CAST "1.0");
    root = xmlNewNode(NULL, BAD_CAST "rss");
    xmlNewProp(root, BAD_CAST "version", BAD_CAST "2.0");
    xmlDocSetRootElement(outdoc, root);

    chan = xmlNewChild(root, NULL, BAD_CAST "channel", NULL);

    xmlNewChild(chan, NULL, BAD_CAST "title", BAD_CAST mailbox->name);

    /* XXX  Add <description> if we have a /comment annotation? */

    host = spool_getheader(txn->req_hdrs, "Host");

    buf_reset(&buf);
    buf_printf(&buf, "%s://%s%s", httpd_tls_done ? "https" : "http",
	       host[0], txn->req_tgt.path);
    xmlNewChild(chan, NULL, BAD_CAST "link", BAD_CAST buf_cstring(&buf));

    if (config_serverinfo == IMAP_ENUM_SERVERINFO_ON) {
	buf_reset(&buf);
	buf_printf(&buf, "Cyrus HTTP %s", cyrus_version());
	xmlNewChild(chan, NULL, BAD_CAST "generator",
		    BAD_CAST buf_cstring(&buf));
    }

    /* Add an <item> for each message */
    for (recno = mailbox->i.num_records; feed && recno >= 1; recno--) {
	struct index_record record;
	const char *msg_base;
	unsigned long msg_size;
	struct body *body;
	char datestr[80], *subj;
	const char *content_types[] = { "text", NULL };
	struct message_content content;
	struct bodypart **parts;

	/* Fetch the message */
	if (fetch_message(txn, mailbox, recno, 0,
			  &record, &body, &msg_base, &msg_size)) {
	    continue;
	}

	if (record.uid <= recentuid) {
	    syslog(LOG_DEBUG, "recno %u not recent (%u/%u)",
		   recno, record.uid, recentuid);
	    continue;
	}

	/* Feeding this message, decrement counter */
	feed--;

	item = xmlNewChild(chan, NULL, BAD_CAST "item", NULL);

	subj = charset_parse_mimeheader(body->subject);
	xmlNewTextChild(item, NULL, BAD_CAST "title", BAD_CAST subj);
	free(subj);

	buf_reset(&buf);
	buf_printf(&buf, "%s://%s%s?uid=%u",
		   httpd_tls_done ? "https" : "http",
		   host[0], txn->req_tgt.path, record.uid);
	xmlNewChild(item, NULL, BAD_CAST "link", BAD_CAST buf_cstring(&buf));

	if (body->reply_to || body->from || body->sender) {
	    struct address *addr;

	    if (body->reply_to) addr = body->reply_to;
	    else if (body->from) addr = body->from;
	    else addr = body->sender;

	    buf_reset(&buf);
	    buf_printf(&buf, "%s@%s", addr->mailbox, addr->domain);
	    if (addr->name) buf_printf(&buf, " (%s)", addr->name);
	    xmlNewChild(item, NULL, BAD_CAST "author",
			BAD_CAST buf_cstring(&buf));
	}

	rfc822date_gen(datestr, sizeof(datestr), record.gmtime);
	xmlNewChild(item, NULL, BAD_CAST "pubDate", BAD_CAST datestr);

	/* Find and use the first text/ part as the <description> */
	content.base = msg_base;
	content.len = msg_size;
	content.body = body;
	message_fetch_part(&content, content_types, &parts);

	if (parts && *parts) {
	    const char *c;

	    buf_reset(&buf);
	    /* Translate CR in body text to HTML <br> tag */
	    for (c = parts[0]->decoded_body; c && *c; c++) {
		if (*c == '\r') buf_appendcstr(&buf, "<br>");
		else buf_putc(&buf, *c);
	    }
	    /* XXX  truncate the message? */
	    xmlNewTextChild(item, NULL, BAD_CAST "description",
			    BAD_CAST buf_cstring(&buf));
	}

	/* free the results */
	if (parts) {
	    struct bodypart **p;

	    for (p = parts; *p; p++) free(*p);
	    free(parts);
	}

	mailbox_unmap_message(mailbox, record.uid, &msg_base, &msg_size);

	if (body) {
	    message_free_body(body);
	    free(body);
	}
    }

    buf_free(&buf);

    /* Output the XML response */
    xml_response(HTTP_OK, txn, outdoc);
}


static void display_address(struct buf *buf, struct address *addr,
			    const char *sep)
{
    buf_printf(buf, "%s", sep);
    if (addr->name) buf_printf(buf, "\"%s\" ", addr->name);
    buf_printf(buf, "<a href=\"mailto:%s@%s\">&lt;%s@%s&gt;</a>\n",
	       addr->mailbox, addr->domain, addr->mailbox, addr->domain);
}


static void display_part(struct transaction_t *txn, struct buf *buf,
			 struct body *body, uint32_t uid,
			 const char *cursection, const char *msg_base)
{
    char nextsection[MAX_SECTION_LEN+1];

    if (!strcmp(body->type, "MULTIPART")) {
	int i;

	if (!strcmp(body->subtype, "ALTERNATIVE") &&
	    !strcmp(body->subpart[0].type, "TEXT")) {
	    /* Display a text/html subpart, otherwise display first subpart */
	    for (i = 0; (i < body->numparts) &&
		     strcmp(body->subpart[i].subtype, "HTML"); i++);
	    if (i == body->numparts) i = 0;
	    snprintf(nextsection, sizeof(nextsection), "%s%s%d",
		     cursection, *cursection ? "." : "", i+1);
	    display_part(txn, buf, &body->subpart[i],
			 uid, nextsection, msg_base);
	}
	else {
	    /* Display all subparts */
	    for (i = 0; i < body->numparts; i++) {
		snprintf(nextsection, sizeof(nextsection), "%s%s%d",
			 cursection, *cursection ? "." : "", i+1);
		display_part(txn, buf, &body->subpart[i],
			     uid, nextsection, msg_base);
	    }
	}
    }
    else if (!strcmp(body->type, "MESSAGE") &&
	     !strcmp(body->subtype, "RFC822")) {
	struct body *subpart = body->subpart;
	struct address *addr;
	char *sep;

	/* Display enclosed message header as a shaded table */
	buf_reset(buf);
	buf_printf(buf, "<table width=\"100%%\" bgcolor=\"#CCCCCC\">\n");
	/* Subject header field */
	if (subpart->subject) {
	    char *subj;

	    subj = charset_parse_mimeheader(subpart->subject);
	    buf_printf(buf, "<tr><td align=right valign=top><b>Subject: </b>");
	    buf_printf(buf, "<td>%s\n", subj);
	    free(subj);
	}
	/* From header field */
	if (subpart->from) {
	    buf_printf(buf, "<tr><td align=right><b>From: </b><td>");
	    display_address(buf, subpart->from, "");
	}
	/* Sender header field (if different than From */
	if (subpart->sender &&
	    (!subpart->from ||
	     strcmp(subpart->sender->mailbox, subpart->from->mailbox) ||
	     strcmp(subpart->sender->domain, subpart->from->domain))) {
	    buf_printf(buf, "<tr><td align=right><b>Sender: </b><td>");
	    display_address(buf, subpart->sender, "");
	}
	/* Reply-To header field (if different than From */
	if (subpart->reply_to &&
	    (!subpart->from ||
	     strcmp(subpart->reply_to->mailbox, subpart->from->mailbox) ||
	     strcmp(subpart->reply_to->domain, subpart->from->domain))) {
	    buf_printf(buf, "<tr><td align=right><b>Reply-To: </b><td>");
	    display_address(buf, subpart->reply_to, "");
	}
	/* Date header field */
	buf_printf(buf, "<tr><td align=right><b>Date: </b>");
	buf_printf(buf, "<td width=\"100%%\">%s\n", subpart->date);
	/* To header field (possibly multiple addresses) */
	if (subpart->to) {
	    buf_printf(buf, "<tr><td align=right valign=top><b>To: </b><td>");
	    for (sep = "", addr = subpart->to; addr; addr = addr->next) {
		display_address(buf, addr, sep);
		sep = ", ";
	    }
	}
	/* Cc header field (possibly multiple addresses) */
	if (subpart->cc) {
	    buf_printf(buf, "<tr><td align=right valign=top><b>Cc: </b><td>");
	    for (sep = "", addr = subpart->cc; addr; addr = addr->next) {
		display_address(buf, addr, sep);
		sep = ", ";
	    }
	}
	buf_printf(buf, "</table><br>\n");
	body_chunk(txn, buf->s, buf->len);

	/* Display subpart */
	snprintf(nextsection, sizeof(nextsection), "%s%s%d",
		 cursection, *cursection ? "." : "", 1);
	display_part(txn, buf, subpart, uid, nextsection, msg_base);
    }
    else {
	/* Leaf part - display something */

	if (!strcmp(body->type, "TEXT")) {
	    /* Display text part */
	    int ishtml = !strcmp(body->subtype, "HTML");
	    int charset = body->charset_cte >> 16;
	    int encoding = body->charset_cte & 0xff;

	    if (charset < 0) charset = 0; /* unknown, try ASCII */
	    body->decoded_body =
		charset_to_utf8(msg_base + body->content_offset,
				body->content_size, charset, encoding);
	    if (!ishtml) body_chunk(txn, "<pre>", strlen("<pre>"));
	    body_chunk(txn, body->decoded_body, strlen(body->decoded_body));
	    if (!ishtml) body_chunk(txn, "</pre>", strlen("</pre>"));
	}
	else {
	    int is_image = !strcmp(body->type, "IMAGE");
	    struct param *param = body->params;
	    const char *file_attr = "NAME";

	    /* Anything else is shown as an attachment.
	     * Show images inline, using name/description as alternative text.
	     */
	    buf_reset(buf);
	    buf_printf(buf, "<div align=center>");

	    /* Create link */
	    buf_printf(buf, "<a href=\"%s?uid=%u;section=%s\" type=\"%s/%s\">",
		       txn->req_tgt.path, uid, cursection,
		       body->type, body->subtype);

	    /* Add image */
	    if (is_image) {
		buf_printf(buf, "<img src=\"%s?uid=%u;section=%s\" alt=\"",
			   txn->req_tgt.path, uid, cursection);
	    }

	    /* Look for a filename in parameters */
	    if (body->disposition) {
		param = body->disposition_params;
		file_attr = "FILENAME";
	    }
	    for (; param && strcmp(param->attribute, file_attr);
		 param = param->next);

	    /* Create text for link or alternative text for image */
	    if (param) buf_printf(buf, "%s", param->value);
	    else {
		buf_printf(buf, "[%s/%s %lu bytes]",
			   body->type, body->subtype, body->content_size);
	    }

	    if (is_image) buf_printf(buf, "\">");
	    buf_printf(buf, "</a></div>");
	    body_chunk(txn, buf->s, buf->len);
	}

	body_chunk(txn, "\n<hr>\n", strlen("\n<hr>\n"));
    }
}


/* Return entire message formatted as text/html */
static void display_message(struct transaction_t *txn,
			    const char *mboxname, uint32_t uid,
			    struct body *body, const char *msg_base)
{
    struct body toplevel;
    struct buf buf = BUF_INITIALIZER;

    /* Setup for chunked response */
    txn->flags |= HTTP_CHUNKED;
    txn->resp_body.type = "text/html; charset=utf-8";

    response_header(HTTP_OK, txn);

    /* Start HTML */
    buf_printf(&buf, HTML_DOCTYPE "\n");
    buf_printf(&buf, "<html><head><title>%s:%u</title></head><body>\n",
	       mboxname, uid);

    /* Create link to message source */
    buf_printf(&buf, "<div align=center>");
    buf_printf(&buf, "<a href=\"%s?uid=%u;section=0\" type=\"plain/text\">",
	       txn->req_tgt.path, uid);
    buf_printf(&buf, "[View message source]</a></div><hr>\n");

    body_chunk(txn, buf.s, buf.len);

    /* Encapsulate our body in a message/rfc822 to display toplevel hdrs */
    memset(&toplevel, 0, sizeof(struct body));
    toplevel.type = "MESSAGE";
    toplevel.subtype = "RFC822";
    toplevel.subpart = body;

    display_part(txn, &buf, &toplevel, uid, "", msg_base);

    /* End of HTML */
    buf_reset(&buf);
    buf_printf(&buf, "</body></html>");
    body_chunk(txn, buf.s, buf.len);

    /* End of output */
    body_chunk(txn, NULL, 0);

    buf_free(&buf);
}


/* Fetch, decode, and return the specified MIME message part */
static void fetch_part(struct transaction_t *txn, struct body *body,
		       const char *findsection, const char *cursection,
		       const char *msg_base)
{
    char nextsection[MAX_SECTION_LEN+1];

    if (!strcmp(body->type, "MULTIPART")) {
	int i;

	/* Recurse through all subparts */
	for (i = 0; i < body->numparts; i++) {
	    snprintf(nextsection, sizeof(nextsection), "%s%s%d",
		     cursection, *cursection ? "." : "", i+1);
	    fetch_part(txn, &body->subpart[i],
		       findsection, nextsection, msg_base);
	}
    }
    else if (!strcmp(body->type, "MESSAGE") &&
	     !strcmp(body->subtype, "RFC822")) {
	/* Recurse into supbart */
	snprintf(nextsection, sizeof(nextsection), "%s%s%d",
		 cursection, *cursection ? "." : "", 1);
	fetch_part(txn, body->subpart, findsection, nextsection, msg_base);
    }
    else if (!strcmp(findsection, cursection)) {
	int encoding = body->charset_cte & 0xff;
	const char *outbuf;
	size_t outsize;
	struct buf buf = BUF_INITIALIZER;

	outbuf = charset_decode_mimebody(msg_base + body->content_offset,
					 body->content_size, encoding,
					 &body->decoded_body, 0, &outsize);

	if (!outbuf) {
	    txn->errstr = "Unknown MIME encoding";
	    response_header(HTTP_SERVER_ERROR, txn);
	    return;

	}
	txn->resp_body.len = outsize;

	buf_printf(&buf, "%s/%s", body->type, body->subtype);
	txn->resp_body.type = buf.s;

	response_header(HTTP_OK, txn);

	if (txn->meth[0] != 'H')
	    prot_write(httpd_out, outbuf, outsize);

	buf_free(&buf);
    }
}