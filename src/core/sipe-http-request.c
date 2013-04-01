/**
 * @file sipe-http-request.c
*
 * pidgin-sipe
 *
 * Copyright (C) 2013 SIPE Project <http://sipe.sourceforge.net/>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * SIPE HTTP request layer implementation
 *
 *  - request handling: creation, parameters, deletion, cancelling
 *  - session handling: creation, closing
 *  - client authorization handling
 *  - connection request queue handling
 *  - compile HTTP header contents and hand-off to transport layer
 *  - process HTTP response and hand-off to user callback
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <glib.h>

#include "sipmsg.h"
#include "sip-sec.h"
#include "sipe-backend.h"
#include "sipe-core.h"
#include "sipe-core-private.h"
#include "sipe-http.h"

#define _SIPE_HTTP_PRIVATE_IF_REQUEST
#include "sipe-http-request.h"
#define _SIPE_HTTP_PRIVATE_IF_TRANSPORT
#include "sipe-http-transport.h"

struct sipe_http_session {
	gchar *cookie; /* extremely simplistic cookie jar :-) */
};

struct sipe_http_request {
	struct sipe_http_connection_public *connection;

	struct sipe_http_session *session;

	gchar *path;
	gchar *headers;
	gchar *body;           /* NULL for GET */
	gchar *content_type;   /* NULL if body == NULL */
	gchar *authorization;

	const gchar *domain;   /* not copied */
	const gchar *user;     /* not copied */
	const gchar *password; /* not copied */

	sipe_http_response_callback *cb;
	gpointer cb_data;

	guint32 flags;
};

#define SIPE_HTTP_REQUEST_FLAG_FIRST    0x00000001
#define SIPE_HTTP_REQUEST_FLAG_REDIRECT 0x00000002
#define SIPE_HTTP_REQUEST_FLAG_AUTHDATA 0x00000004

static void sipe_http_request_free(struct sipe_core_private *sipe_private,
				   struct sipe_http_request *req)
{
	if (req->cb)
		/* Callback: aborted */
		(*req->cb)(sipe_private, 0, NULL, NULL, req->cb_data);
	g_free(req->path);
	g_free(req->headers);
	g_free(req->body);
	g_free(req->content_type);
	g_free(req->authorization);
	g_free(req);
}

static void sipe_http_request_send(struct sipe_http_connection_public *conn_public)
{
	struct sipe_http_request *req = conn_public->pending_requests->data;
	gchar *header;
	gchar *content = NULL;
	gchar *cookie  = NULL;

	if (req->body)
		content = g_strdup_printf("Content-Length: %" G_GSIZE_FORMAT "\r\n"
					  "Content-Type: %s\r\n",
					  strlen(req->body),
					  req->content_type);

	if (req->session && req->session->cookie)
		cookie = g_strdup_printf("Cookie: %s\r\n", req->session->cookie);

	header = g_strdup_printf("%s /%s HTTP/1.1\r\n"
				 "Host: %s\r\n"
				 "User-Agent: Sipe/" PACKAGE_VERSION "\r\n"
				 "%s%s%s%s",
				 content ? "POST" : "GET",
				 req->path,
				 conn_public->host,
				 req->authorization ? req->authorization : "",
				 req->headers ? req->headers : "",
				 cookie ? cookie : "",
				 content ? content : "");
	g_free(cookie);
	g_free(content);

	/* only use authorization once */
	g_free(req->authorization);
	req->authorization = NULL;

	sipe_http_transport_send(conn_public,
				 header,
				 req->body);
	g_free(header);
}

gboolean sipe_http_request_pending(struct sipe_http_connection_public *conn_public)
{
	return(conn_public->pending_requests != NULL);
}

void sipe_http_request_next(struct sipe_http_connection_public *conn_public)
{
	sipe_http_request_send(conn_public);
}

static void sipe_http_request_response_unauthorized(struct sipe_core_private *sipe_private,
						    struct sipe_http_request *req,
						    struct sipmsg *msg)
{
	const gchar *header = NULL;
	const gchar *name;
	guint type;
	gboolean failed = TRUE;

#if defined(HAVE_LIBKRB5) || defined(HAVE_SSPI)
#define DEBUG_STRING "NTLM and Negotiate authentications are"
	/* Use "Negotiate" unless the user requested "NTLM" */
	if (sipe_private->authentication_type != SIPE_AUTHENTICATION_TYPE_NTLM)
		header = sipmsg_find_auth_header(msg, "Negotiate");
	if (header) {
		type   = SIPE_AUTHENTICATION_TYPE_NEGOTIATE;
		name   = "Negotiate";
	} else
#else
#define DEBUG_STRING "NTLM authentication is"
#endif
	{
		header = sipmsg_find_auth_header(msg, "NTLM");
		type   = SIPE_AUTHENTICATION_TYPE_NTLM;
		name   = "NTLM";
	}

	if (header) {
		struct sipe_http_connection_public *conn_public = req->connection;

		if (!conn_public->context) {
			gboolean valid = req->flags & SIPE_HTTP_REQUEST_FLAG_AUTHDATA;
			conn_public->context = sip_sec_create_context(type,
								      !valid, /* Single Sign-On flag */
								      TRUE,   /* connection-based for HTTP */
								      valid ? req->domain   : NULL,
								      valid ? req->user     : NULL,
								      valid ? req->password : NULL);
		}


		if (conn_public->context) {
			gchar **parts = g_strsplit(header, " ", 0);
			gchar *spn    = g_strdup_printf("HTTP/%s", conn_public->host);
			gchar *token;

			SIPE_DEBUG_INFO("sipe_http_request_response_unauthorized: init context target '%s' token '%s'",
					spn, parts[1] ? parts[1] : "<NULL>");

			if (sip_sec_init_context_step(conn_public->context,
						      spn,
						      parts[1],
						      &token,
						      NULL)) {

				/* generate authorization header */
				req->authorization = g_strdup_printf("Authorization: %s %s\r\n",
								     name,
								     token ? token : "");
				g_free(token);

				/* resend request with authorization */
				sipe_http_request_send(conn_public);
				failed = FALSE;

			} else
				SIPE_DEBUG_INFO_NOFORMAT("sipe_http_request_response_unauthorized: security context init step failed");

			g_free(spn);
			g_strfreev(parts);
		} else
			SIPE_DEBUG_INFO_NOFORMAT("sipe_http_request_response_unauthorized: security context creation failed");
	} else
		SIPE_DEBUG_INFO_NOFORMAT("sipe_http_request_response_unauthorized: only " DEBUG_STRING " supported");

	if (failed) {
		/* Callback: authentication failed */
		(*req->cb)(sipe_private, 0, NULL, NULL, req->cb_data);

		/* remove completed request */
		sipe_http_request_cancel(req);
	}
}

static void sipe_http_request_response_callback(struct sipe_core_private *sipe_private,
						struct sipe_http_request *req,
						struct sipmsg *msg)
{
	const gchar *hdr;

	/* Set-Cookie: RMID=732423sdfs73242; expires=Fri, 31-Dec-2010 23:59:59 GMT; path=/; domain=.example.net */
	if (req->session &&
	    ((hdr = sipmsg_find_header(msg, "Set-Cookie")) != NULL)) {
		gchar **parts, **current;
		const gchar *part;
		gchar *new = NULL;

		g_free(req->session->cookie);
		req->session->cookie = NULL;

		current = parts = g_strsplit(hdr, ";", 0);
		while ((part = *current++) != NULL) {
			/* strip these parts from cookie */
			if (!(strstr(part, "path=")    ||
			      strstr(part, "domain=")  ||
			      strstr(part, "expires=") ||
			      strstr(part, "secure"))) {
				gchar *tmp = new;
				new = new ?
					g_strconcat(new, ";", part, NULL) :
					g_strdup(part);
				g_free(tmp);
			}
		}
		g_strfreev(parts);

		if (new) {
			req->session->cookie = new;
			SIPE_DEBUG_INFO("sipe_http_request_response_callback: cookie: %s", new);
		}
	}

	/* Callback: success */
	(*req->cb)(sipe_private,
		   msg->response,
		   msg->headers,
		   msg->body,
		   req->cb_data);

	/* remove completed request */
	sipe_http_request_cancel(req);
}

void sipe_http_request_response(struct sipe_http_connection_public *conn_public,
				struct sipmsg *msg)
{
	struct sipe_core_private *sipe_private = conn_public->sipe_private;
	struct sipe_http_request *req = conn_public->pending_requests->data;

	if (msg->response == SIPE_HTTP_STATUS_CLIENT_UNAUTHORIZED) {
		sipe_http_request_response_unauthorized(sipe_private, req, msg);
		/* req may no longer be valid */
	} else {
		/* All other cases are passed on to the user */
		sipe_http_request_response_callback(sipe_private, req, msg);
		/* req is no longer valid */
	}
}

void sipe_http_request_shutdown(struct sipe_http_connection_public *conn_public)
{
	if (conn_public->pending_requests) {
		GSList *entry = conn_public->pending_requests;
		while (entry) {
			sipe_http_request_free(conn_public->sipe_private,
					       entry->data);
			entry = entry->next;
		}
		g_slist_free(conn_public->pending_requests);
		conn_public->pending_requests = NULL;
	}

	if (conn_public->context) {
		sip_sec_destroy_context(conn_public->context);
		conn_public->context = NULL;
	}
}

struct sipe_http_request *sipe_http_request_new(struct sipe_core_private *sipe_private,
						const gchar *host,
						guint32 port,
						const gchar *path,
						const gchar *headers,
						const gchar *body,
						const gchar *content_type,
						sipe_http_response_callback *callback,
						gpointer callback_data)
{
	struct sipe_http_request *req = g_new0(struct sipe_http_request, 1);
	struct sipe_http_connection_public *conn_public;

	req->path                 = g_strdup(path);
	if (headers)
		req->headers      = g_strdup(headers);
	if (body) {
		req->body         = g_strdup(body);
		req->content_type = g_strdup(content_type);
	}

	req->flags = 0;

	/* default authentication */
	if (!SIPE_CORE_PRIVATE_FLAG_IS(SSO))
		sipe_http_request_authentication(req,
						 sipe_private->authdomain,
						 sipe_private->authuser,
						 sipe_private->password);

	req->cb      = callback;
	req->cb_data = callback_data;

	req->connection = conn_public = sipe_http_transport_new(sipe_private,
								host,
								port);
	if (!sipe_http_request_pending(conn_public))
		req->flags |= SIPE_HTTP_REQUEST_FLAG_FIRST;

	conn_public->pending_requests = g_slist_append(conn_public->pending_requests,
						       req);

	return(req);
}

void sipe_http_request_ready(struct sipe_http_request *request)
{
	struct sipe_http_connection_public *conn_public = request->connection;

	/* pass first request on already opened connection through directly */
	if ((request->flags & SIPE_HTTP_REQUEST_FLAG_FIRST) &&
	    conn_public->connected)
		sipe_http_request_send(conn_public);
}

struct sipe_http_session *sipe_http_session_start(void)
{
	return(g_new0(struct sipe_http_session, 1));
}

void sipe_http_session_close(struct sipe_http_session *session)
{
	if (session) {
		g_free(session->cookie);
		g_free(session);
	}
}

void sipe_http_request_cancel(struct sipe_http_request *request)
{
	struct sipe_http_connection_public *conn_public = request->connection;
	conn_public->pending_requests = g_slist_remove(conn_public->pending_requests,
						       request);

	/* cancelled by requester, don't use callback */
	request->cb = NULL;

	sipe_http_request_free(conn_public->sipe_private, request);
}

void sipe_http_request_session(struct sipe_http_request *request,
			       struct sipe_http_session *session)
{
	request->session = session;
}

void sipe_http_request_allow_redirect(struct sipe_http_request *request)
{
	request->flags |= SIPE_HTTP_REQUEST_FLAG_REDIRECT;
}

void sipe_http_request_authentication(struct sipe_http_request *request,
				      const gchar *domain,
				      const gchar *user,
				      const gchar *password)
{
	request->flags   |= SIPE_HTTP_REQUEST_FLAG_AUTHDATA;
	request->domain   = domain;
	request->user     = user;
	request->password = password;
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/
