/*
 * Copyright 2008 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_hash.h>
#include "rtsp_client.h"
#include "apt_net_client_task.h"
#include "apt_text_stream.h"
#include "apt_obj_list.h"
#include "apt_log.h"

#define RTSP_MESSAGE_MAX_SIZE 2048

typedef struct rtsp_client_connection_t rtsp_client_connection_t;

/** RTSP client */
struct rtsp_client_t {
	apr_pool_t                 *pool;
	apt_net_client_task_t      *task;

	apr_pool_t                 *sub_pool;
	apt_obj_list_t             *connection_list;

	void                       *obj;
	const rtsp_client_vtable_t *vtable;
};

/** RTSP connection */
struct rtsp_client_connection_t {
	/** Connection base */
	apt_net_client_connection_t *base;

	/** Element of the connection list in agent */
	apt_list_elem_t             *it;

	/** Session table (rtsp_client_session_t*) */
	apr_hash_t                  *session_table;
	
	/** Session table (rtsp_client_session_t*) */
	apt_obj_list_t              *pending_session_queue;
};

/** RTSP session */
struct rtsp_client_session_t {
	apr_pool_t               *pool;
	void                     *obj;
	rtsp_client_connection_t *connection;

	/** Session identifier */
	apt_str_t                 id;
	apt_str_t                 url;

	apt_str_t                 server_ip;
	apr_port_t                server_port;

	apt_str_t                 resource_location;

	/** Last cseq sent */
	apr_size_t                last_cseq;

	/** In-progress request */
	rtsp_message_t           *active_request;
	/** request queue */
	apt_obj_list_t           *request_queue;

	/** In-progress termination request */
	apt_bool_t                terminating;
};

typedef enum {
	TASK_MSG_SEND_MESSAGE,
	TASK_MSG_TERMINATE_SESSION
} task_msg_data_type_e;

typedef struct task_msg_data_t task_msg_data_t;

struct task_msg_data_t {
	task_msg_data_type_e   type;
	rtsp_client_t         *client;
	rtsp_client_session_t *session;
	rtsp_message_t        *message;
};

static apt_bool_t rtsp_client_task_msg_process(apt_task_t *task, apt_task_msg_t *msg);

static apt_bool_t rtsp_client_on_connect(apt_net_client_task_t *task, apt_net_client_connection_t *connection, apt_bool_t status);
static apt_bool_t rtsp_client_on_disconnect(apt_net_client_task_t *task, apt_net_client_connection_t *connection, apt_bool_t status);
static apt_bool_t rtsp_client_message_receive(apt_net_client_task_t *task, apt_net_client_connection_t *connection);

static const apt_net_client_vtable_t client_vtable = {
	rtsp_client_on_connect,
	rtsp_client_on_disconnect,
	rtsp_client_message_receive
};

static apt_bool_t rtsp_client_message_send(rtsp_client_t *client, apt_net_client_connection_t *connection, rtsp_message_t *message);

/** Create RTSP client */
RTSP_DECLARE(rtsp_client_t*) rtsp_client_create(
										apr_size_t max_connection_count,
										void *obj,
										const rtsp_client_vtable_t *handler,
										apr_pool_t *pool)
{
	apt_task_vtable_t vtable;
	apt_task_msg_pool_t *msg_pool;
	rtsp_client_t *client;
	
	apt_log(APT_PRIO_NOTICE,"Create RTSP client [%d]",max_connection_count);
	client = apr_palloc(pool,sizeof(rtsp_client_t));
	client->pool = pool;
	client->obj = obj;
	client->vtable = handler;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(task_msg_data_t),pool);

	apt_task_vtable_reset(&vtable);
	vtable.process_msg = rtsp_client_task_msg_process;
	client->task = apt_net_client_task_create(max_connection_count,client,&vtable,&client_vtable,msg_pool,pool);
	if(!client->task) {
		return NULL;
	}

	apr_pool_create(&client->sub_pool,pool);
	client->connection_list = NULL;
	return client;
}

/** Destroy RTSP client */
RTSP_DECLARE(apt_bool_t) rtsp_client_destroy(rtsp_client_t *client)
{
	apt_log(APT_PRIO_NOTICE,"Destroy RTSP client");
	return apt_net_client_task_destroy(client->task);
}

/** Start connection agent */
RTSP_DECLARE(apt_bool_t) rtsp_client_start(rtsp_client_t *client)
{
	return apt_net_client_task_start(client->task);
}

/** Terminate connection agent */
RTSP_DECLARE(apt_bool_t) rtsp_client_terminate(rtsp_client_t *client)
{
	return apt_net_client_task_terminate(client->task);
}

/** Get task */
RTSP_DECLARE(apt_task_t*) rtsp_client_task_get(rtsp_client_t *client)
{
	return apt_net_client_task_object_get(client->task);
}

/** Get external object */
RTSP_DECLARE(void*) rtsp_client_object_get(rtsp_client_t *client)
{
	return client->obj;
}

/** Get object associated with the session */
RTSP_DECLARE(void*) rtsp_client_session_object_get(const rtsp_client_session_t *session)
{
	return session->obj;
}

/** Set object associated with the session */
RTSP_DECLARE(void) rtsp_client_session_object_set(rtsp_client_session_t *session, void *obj)
{
	session->obj = obj;
}

/** Get the session identifier */
RTSP_DECLARE(const apt_str_t*) rtsp_client_session_id_get(const rtsp_client_session_t *session)
{
	return &session->id;
}

/** Signal task message */
static apt_bool_t rtsp_client_control_message_signal(
								task_msg_data_type_e type,
								rtsp_client_t *client,
								rtsp_client_session_t *session,
								rtsp_message_t *message)
{
	apt_task_t *task = apt_net_client_task_base_get(client->task);
	apt_task_msg_t *task_msg = apt_task_msg_get(task);
	if(task_msg) {
		task_msg_data_t *data = (task_msg_data_t*)task_msg->data;
		data->type = type;
		data->client = client;
		data->session = session;
		data->message = message;
		apt_task_msg_signal(task,task_msg);
	}
	return TRUE;
}

/** Create RTSP session */
RTSP_DECLARE(rtsp_client_session_t*) rtsp_client_session_create(
											rtsp_client_t *client,
											const char *server_ip, 
											apr_port_t server_port,
											const char *resource_location)
{
	rtsp_client_session_t *session;
	apr_pool_t *pool;
	apr_pool_create(&pool,NULL);
	session = apr_palloc(pool,sizeof(rtsp_client_session_t));
	session->pool = pool;
	session->obj = NULL;
	session->connection = NULL;
	session->last_cseq = 0;
	session->active_request = NULL;
	session->request_queue = apt_list_create(pool);
	session->terminating = FALSE;

	apt_string_assign(&session->server_ip,server_ip,pool);
	session->server_port = server_port;

	apt_string_assign(&session->resource_location,resource_location,pool);

	apt_string_reset(&session->url);
	apt_string_reset(&session->id);
	apt_log(APT_PRIO_NOTICE,"Create RTSP Session <new>");
	return session;
}

/** Destroy RTSP session */
RTSP_DECLARE(void) rtsp_client_session_destroy(rtsp_client_session_t *session)
{
	apt_log(APT_PRIO_NOTICE,"Destroy RTSP Session <%s>",session->id.buf ? session->id.buf : "new");
	if(session && session->pool) {
		apr_pool_destroy(session->pool);
	}
}

/** Signal terminate request */
RTSP_DECLARE(apt_bool_t) rtsp_client_session_terminate(rtsp_client_t *client, rtsp_client_session_t *session)
{
	return rtsp_client_control_message_signal(TASK_MSG_TERMINATE_SESSION,client,session,NULL);
}

/** Signal RTSP message */
RTSP_DECLARE(apt_bool_t) rtsp_client_session_request(rtsp_client_t *client, rtsp_client_session_t *session, rtsp_message_t *message)
{
	return rtsp_client_control_message_signal(TASK_MSG_SEND_MESSAGE,client,session,message);
}

/* Create RTSP connection */
static apt_bool_t rtsp_client_connection_create(rtsp_client_t *client, rtsp_client_session_t *session)
{
	rtsp_client_connection_t *rtsp_connection;
	apt_net_client_connection_t *connection = apt_net_client_connect(client->task,session->server_ip.buf,session->server_port);
	if(!connection) {
		return FALSE;
	}
	rtsp_connection = apr_palloc(connection->pool,sizeof(rtsp_client_connection_t));
	rtsp_connection->session_table = apr_hash_make(connection->pool);
	rtsp_connection->pending_session_queue = apt_list_create(connection->pool);
	rtsp_connection->base = connection;
	connection->obj = rtsp_connection;
	if(!client->connection_list) {
		client->connection_list = apt_list_create(client->sub_pool);
	}
	rtsp_connection->it = apt_list_push_back(client->connection_list,rtsp_connection);
	session->connection = rtsp_connection;
	return TRUE;
}


/* Process session termination request */
static apt_bool_t rtsp_client_session_terminate_process(rtsp_client_t *client, rtsp_client_session_t *session)
{
	return TRUE;
}

static apt_bool_t rtsp_client_session_url_generate(rtsp_client_session_t *session, rtsp_message_t *message)
{
	apt_str_t *url = &message->start_line.common.request_line.url;
	if(session->resource_location.length) {
		url->buf = apr_psprintf(message->pool,"rtsp://%s:%d/%s/%s",
						session->server_ip.buf,
						session->server_port,
						session->resource_location.buf,
						message->start_line.common.request_line.resource_name);
	}
	else {
		url->buf = apr_psprintf(message->pool,"rtsp://%s:%d/%s",
						session->server_ip.buf,
						session->server_port,
						message->start_line.common.request_line.resource_name);
	}
	url->length = strlen(url->buf);
	return TRUE;
}

/* Process outgoing RTSP request */
static apt_bool_t rtsp_client_session_request_process(rtsp_client_t *client, rtsp_client_session_t *session, rtsp_message_t *message)
{
	if(!session->connection) {
		if(rtsp_client_connection_create(client,session) == FALSE) {
			return FALSE;
		}
		if(message->start_line.common.request_line.method_id == RTSP_METHOD_SETUP) {
			/* create new session */
			apt_log(APT_PRIO_INFO,"Add RTSP Session <new>");
			apt_list_push_back(session->connection->pending_session_queue,session);
		}
	}

	rtsp_client_session_url_generate(session,message);
	
	message->header.cseq = ++session->last_cseq;
	rtsp_header_property_add(&message->header.property_set,RTSP_HEADER_FIELD_CSEQ);

	return rtsp_client_message_send(client,session->connection->base,message);
}

static apt_bool_t rtsp_client_session_message_process(rtsp_client_t *client, rtsp_client_session_t *session, rtsp_message_t *message)
{
	if(session->active_request) {
		apt_log(APT_PRIO_DEBUG,"Push RTSP Request to Queue <%s>",session->id.buf);
		apt_list_push_back(session->request_queue,message);
		return TRUE;
	}

	session->active_request = message;
	return rtsp_client_session_request_process(client,session,message);
}

/* Process incoming RTSP event (request) */
static apt_bool_t rtsp_client_session_event_process(rtsp_client_t *client, rtsp_client_connection_t *rtsp_connection, rtsp_message_t *message)
{
	rtsp_message_t *response = NULL;
	rtsp_client_session_t *session = NULL;
	if(rtsp_header_property_check(&message->header.property_set,RTSP_HEADER_FIELD_SESSION_ID) == TRUE) {
		/* find existing session */
		session = apr_hash_get(
					rtsp_connection->session_table,
					message->header.session_id.buf,
					message->header.session_id.length);
	}

	if(session) {
		response = rtsp_response_create(message,RTSP_STATUS_CODE_OK,RTSP_REASON_PHRASE_OK,message->pool);
		if(rtsp_header_property_check(&message->header.property_set,RTSP_HEADER_FIELD_SESSION_ID) == TRUE) {
			response->header.session_id = message->header.session_id;
			rtsp_header_property_add(&response->header.property_set,RTSP_HEADER_FIELD_SESSION_ID);
		}
		client->vtable->on_session_event(client,session,message);
		return rtsp_client_message_send(client,rtsp_connection->base,response);
	}
	else {
		response = rtsp_response_create(message,RTSP_STATUS_CODE_NOT_FOUND,RTSP_REASON_PHRASE_NOT_FOUND,message->pool);
	}

	if(rtsp_header_property_check(&message->header.property_set,RTSP_HEADER_FIELD_CSEQ) == TRUE) {
		response->header.cseq = message->header.cseq;
		rtsp_header_property_add(&response->header.property_set,RTSP_HEADER_FIELD_SESSION_ID);
	}
	return rtsp_client_message_send(client,rtsp_connection->base,response);
}

/* Process incoming RTSP response */
static apt_bool_t rtsp_client_session_response_process(rtsp_client_t *client, rtsp_client_connection_t *rtsp_connection, rtsp_message_t *message)
{
	if(message->start_line.message_type == RTSP_MESSAGE_TYPE_REQUEST) {
		return rtsp_client_session_event_process(client,rtsp_connection,message);
	}

	if(rtsp_header_property_check(&message->header.property_set,RTSP_HEADER_FIELD_SESSION_ID) == TRUE) {
		/* find existing session */
		rtsp_client_session_t *session = apr_hash_get(
					rtsp_connection->session_table,
					message->header.session_id.buf,
					message->header.session_id.length);
		if(!session) {
			/* find new session */
			rtsp_client_session_t *new_session;
			apt_list_elem_t *elem = apt_list_first_elem_get(rtsp_connection->pending_session_queue);
			while(elem) {
				new_session = apt_list_elem_object_get(elem);
				if(new_session && new_session->last_cseq == message->header.cseq) {
					session = new_session;
					session->id = message->header.session_id;
					apt_log(APT_PRIO_INFO,"Move RTSP Session <%s>",session->id.buf);
					apt_list_elem_remove(rtsp_connection->pending_session_queue,elem);
					apr_hash_set(rtsp_connection->session_table,session->id.buf,session->id.length,session);
					break;
				}
				elem = apt_list_next_elem_get(rtsp_connection->pending_session_queue,elem);
			}
		}
		
		if(session) {
			if(!session->active_request) {
				return FALSE;
			}
			client->vtable->on_session_response(client,session,session->active_request,message);

			/* process pending requests */
			session->active_request = apt_list_pop_front(session->request_queue);
			if(session->active_request) {
				rtsp_client_session_request_process(client,session,session->active_request);
			}
		}
	}
	else {
		/* invalid response, no session-id specified */
	}

	return TRUE;
}

/* Send RTSP message through RTSP connection */
static apt_bool_t rtsp_client_message_send(rtsp_client_t *client, apt_net_client_connection_t *connection, rtsp_message_t *message)
{
	apt_bool_t status = FALSE;
	if(connection && connection->sock) {
		char buffer[RTSP_MESSAGE_MAX_SIZE];
		apt_text_stream_t text_stream;
		
		text_stream.text.buf = buffer;
		text_stream.text.length = sizeof(buffer)-1;
		text_stream.pos = text_stream.text.buf;

		if(rtsp_message_generate(message,&text_stream) == TRUE) {
			*text_stream.pos = '\0';
			apt_log(APT_PRIO_INFO,"Send RTSP Message size=%lu\n%s",
				text_stream.text.length,text_stream.text.buf);
			if(apr_socket_send(connection->sock,text_stream.text.buf,&text_stream.text.length) == APR_SUCCESS) {
				status = TRUE;
			}
			else {
				apt_log(APT_PRIO_WARNING,"Failed to Send RTSP Message");
			}
		}
		else {
			apt_log(APT_PRIO_WARNING,"Failed to Generate RTSP Message");
		}
	}
	else {
		apt_log(APT_PRIO_WARNING,"No RTSP Connection");
	}

	return status;
}

/* Receive RTSP message through RTSP connection */
static apt_bool_t rtsp_client_message_receive(apt_net_client_task_t *task, apt_net_client_connection_t *connection)
{
	rtsp_client_t *client = apt_net_client_task_object_get(task);
	char buffer[RTSP_MESSAGE_MAX_SIZE];
	apt_bool_t more_messages_on_buffer = FALSE;
	apr_status_t status;
	apt_text_stream_t text_stream;
	rtsp_message_t *message;

	if(!connection || !connection->sock) {
		return FALSE;
	}
	
	text_stream.text.buf = buffer;
	text_stream.text.length = sizeof(buffer)-1;
	status = apr_socket_recv(connection->sock, text_stream.text.buf, &text_stream.text.length);
	if(status == APR_EOF || text_stream.text.length == 0) {
		return FALSE;//apt_net_client_connection_close(task,connection);
	}
	text_stream.text.buf[text_stream.text.length] = '\0';
	text_stream.pos = text_stream.text.buf;

	apt_log(APT_PRIO_INFO,"Receive RTSP Message size=%lu\n%s",text_stream.text.length,text_stream.text.buf);
	do {
		message = rtsp_message_create(RTSP_MESSAGE_TYPE_UNKNOWN,connection->pool);
		if(rtsp_message_parse(message,&text_stream) == TRUE) {
			rtsp_client_session_response_process(client,connection->obj,message);
		}
		else {
			rtsp_message_t *response;
			apt_log(APT_PRIO_WARNING,"Failed to Parse RTSP Message");
			response = rtsp_response_create(message,RTSP_STATUS_CODE_BAD_REQUEST,
									RTSP_REASON_PHRASE_BAD_REQUEST,message->pool);
			if(rtsp_client_message_send(client,connection,response) == FALSE) {
				apt_log(APT_PRIO_WARNING,"Failed to Send RTSP Response");
			}
		}

		more_messages_on_buffer = FALSE;
		if(text_stream.text.length > (apr_size_t)(text_stream.pos - text_stream.text.buf)) {
			/* there are more RTSP messages to signal */
			more_messages_on_buffer = TRUE;
			text_stream.text.length -= text_stream.pos - text_stream.text.buf;
			text_stream.text.buf = text_stream.pos;
			apt_log(APT_PRIO_DEBUG,"Saving Remaining Buffer for Next Message");
		}
	}
	while(more_messages_on_buffer);

	return TRUE;
}

/* New RTSP connection accepted */
static apt_bool_t rtsp_client_on_connect(apt_net_client_task_t *task, apt_net_client_connection_t *connection, apt_bool_t status)
{
	rtsp_client_t *client = apt_net_client_task_object_get(task);
	rtsp_client_connection_t *rtsp_connection = apr_palloc(connection->pool,sizeof(rtsp_client_connection_t));
	rtsp_connection->session_table = apr_hash_make(connection->pool);
	rtsp_connection->base = connection;
	connection->obj = rtsp_connection;
	if(!client->connection_list) {
		client->connection_list = apt_list_create(client->sub_pool);
	}
	rtsp_connection->it = apt_list_push_back(client->connection_list,rtsp_connection);
	return TRUE;
}

/* RTSP connection disconnected */
static apt_bool_t rtsp_client_on_disconnect(apt_net_client_task_t *task, apt_net_client_connection_t *connection, apt_bool_t status)
{
	apr_size_t remaining_sessions = 0;
	rtsp_client_t *client = apt_net_client_task_object_get(task);
	rtsp_client_connection_t *rtsp_connection = connection->obj;
	apt_list_elem_remove(client->connection_list,rtsp_connection->it);
	rtsp_connection->it = NULL;
	if(apt_list_is_empty(client->connection_list) == TRUE) {
		apr_pool_clear(client->sub_pool);
		client->connection_list = NULL;
	}

	remaining_sessions = apr_hash_count(rtsp_connection->session_table);
	if(remaining_sessions) {
		rtsp_client_session_t *session;
		void *val;
		apr_hash_index_t *it;
		apt_log(APT_PRIO_NOTICE,"Terminate Remaining RTSP Sessions [%d]",remaining_sessions);
		it = apr_hash_first(connection->pool,rtsp_connection->session_table);
		for(; it; it = apr_hash_next(it)) {
			apr_hash_this(it,NULL,NULL,&val);
			session = val;
			if(session && session->terminating == FALSE) {
//				rtsp_client_session_terminate_request(client,session);
			}
		}
	}
	else {
		apt_net_client_connection_destroy(connection);
	}
	return TRUE;
}

/* Process task message */
static apt_bool_t rtsp_client_task_msg_process(apt_task_t *task, apt_task_msg_t *task_msg)
{
	apt_net_client_task_t *net_task = apt_task_object_get(task);
	rtsp_client_t *client = apt_net_client_task_object_get(net_task);

	task_msg_data_t *data = (task_msg_data_t*) task_msg->data;
	switch(data->type) {
		case TASK_MSG_SEND_MESSAGE:
			rtsp_client_session_message_process(client,data->session,data->message);
			break;
		case TASK_MSG_TERMINATE_SESSION:
			rtsp_client_session_terminate_process(client,data->session);
			break;
	}

	return TRUE;
}