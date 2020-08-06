/*
 * Copyright 2010-2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/event-stream/event_stream_channel_handler.h>
#include <aws/event-stream/event_stream_rpc_server.h>

#include <aws/common/atomics.h>
#include <aws/common/hash_table.h>

#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>

static const uint32_t s_bit_scrambling_magic = 0x45d9f3bU;
static const uint32_t s_bit_shift_magic = 16U;

static const struct aws_byte_cursor s_message_type_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":message-type");
static const struct aws_byte_cursor s_message_flags_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":message-flags");
static const struct aws_byte_cursor s_stream_id_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":stream-id");
static const struct aws_byte_cursor s_operation_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("operation");

static const struct aws_byte_cursor s_invalid_stream_id_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"non-zero stream-id field is only allowed for messages of type APPLICATION_MESSAGE\"; }");

static const struct aws_byte_cursor s_invalid_client_stream_id_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"stream-id values must be monotonically incrementing. A "
                                          "stream-id arrived that was lower than the last seen stream-id.\"; }");

static const struct aws_byte_cursor s_invalid_new_client_stream_id_error =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("{ \"message\": \"stream-id values must be monotonically incrementing. A new "
                                          "stream-id arrived that was incremented by more than 1.\"; }");

static const struct aws_byte_cursor s_invalid_message_type_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"an invalid value for message-type field was received.\"; }");

static const struct aws_byte_cursor s_server_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"an error occured on the server. This is not likely caused by your client.\"; }");

static const struct aws_byte_cursor s_invalid_message_error = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "{ \"message\": \"A message was recieved with missing required fields. Check that your client is sending at least, "
    ":message-type, :message-flags, and :stream-id\"; }");

/* this is a repurposed hash function based on the technique in splitmix64. The magic number was a result of numerical
 * analysis on maximum bit entropy. */
static uint64_t s_hash_uint32(const void *to_hash) {
    uint32_t int_to_hash = *(const uint32_t *)to_hash;
    uint32_t hash = ((int_to_hash >> s_bit_shift_magic) ^ int_to_hash) * s_bit_scrambling_magic;
    hash = ((hash >> s_bit_shift_magic) ^ hash) * s_bit_scrambling_magic;
    hash = (hash >> s_bit_shift_magic) ^ hash;
    return (uint64_t)hash;
}

static bool s_uint32_eq(const void *a, const void *b) {
    return *(const int32_t *)a == *(const int32_t *)b;
}

struct aws_event_stream_rpc_server_listener {
    struct aws_allocator *allocator;
    struct aws_socket *listener;
    struct aws_server_bootstrap *bootstrap;
    struct aws_atomic_var ref_count;
    aws_event_stream_rpc_server_on_new_connection_fn *on_new_connection;
    aws_event_stream_rpc_server_on_connection_shutdown_fn *on_connection_shutdown;
    aws_event_stream_rpc_server_on_listener_destroy_fn *on_destroy_callback;
    size_t initial_window_size;
    bool enable_read_backpressure;
    void *user_data;
};

struct aws_event_stream_rpc_connection {
    struct aws_allocator *allocator;
    struct aws_hash_table continuation_table;
    struct aws_event_stream_rpc_server_listener *server;
    struct aws_atomic_var ref_count;
    aws_event_stream_rpc_server_on_incoming_stream_fn *on_incoming_stream;
    aws_event_stream_rpc_server_connection_protocol_message_fn *on_connection_protocol_message;
    struct aws_channel *channel;
    struct aws_channel_handler *event_stream_handler;
    uint32_t latest_stream_id;
    void *user_data;
    struct aws_atomic_var is_closed;
};

struct aws_event_stream_rpc_server_continuation_token {
    uint32_t stream_id;
    struct aws_event_stream_rpc_connection *connection;
    aws_event_stream_rpc_server_stream_continuation_fn *continuation_fn;
    aws_event_stream_rpc_server_stream_continuation_closed_fn *closed_fn;
    void *user_data;
    struct aws_atomic_var ref_count;
    struct aws_atomic_var is_closed;
};

void s_continuation_destroy(void *value) {
    struct aws_event_stream_rpc_server_continuation_token *continuation = value;
    continuation->closed_fn(continuation, continuation->user_data);
    aws_event_stream_rpc_server_continuation_release(continuation);
}

static void s_on_message_received(struct aws_event_stream_message *message, int error_code, void *user_data);

static struct aws_event_stream_rpc_connection *s_create_connection_on_channel(
    struct aws_event_stream_rpc_server_listener *server,
    struct aws_channel *channel) {
    struct aws_event_stream_rpc_connection *connection =
        aws_mem_calloc(server->allocator, 1, sizeof(struct aws_event_stream_rpc_connection));
    struct aws_channel_handler *event_stream_handler = NULL;
    struct aws_channel_slot *slot = NULL;

    if (!connection) {
        return NULL;
    }

    aws_atomic_init_int(&connection->ref_count, 1);

    if (aws_hash_table_init(
            &connection->continuation_table,
            server->allocator,
            64,
            s_hash_uint32,
            s_uint32_eq,
            NULL,
            s_continuation_destroy)) {
        goto error;
    }

    struct aws_event_stream_channel_handler_options handler_options = {
        .on_message_received = s_on_message_received,
        .user_data = connection,
        .initial_window_size = server->initial_window_size,
        .manual_window_management = server->enable_read_backpressure,
    };

    event_stream_handler = aws_event_stream_channel_handler_new(server->allocator, &handler_options);

    if (!event_stream_handler) {
        goto error;
    }

    slot = aws_channel_slot_new(channel);

    if (!slot) {
        goto error;
    }

    aws_channel_slot_insert_end(channel, slot);
    if (aws_channel_slot_set_handler(slot, event_stream_handler)) {
        goto error;
    }

    aws_event_stream_rpc_server_listener_acquire(server);
    connection->server = server;

    connection->event_stream_handler = event_stream_handler;
    connection->channel = channel;
    aws_channel_acquire_hold(channel);

    return connection;

error:
    if (!slot && event_stream_handler) {
        aws_channel_handler_destroy(event_stream_handler);
    }

    if (connection) {
        aws_event_stream_rpc_server_connection_release(connection);
    }

    return NULL;
}

struct aws_event_stream_rpc_connection *aws_event_stream_rpc_server_connection_from_existing_channel(
    struct aws_event_stream_rpc_server_listener *server,
    struct aws_channel *channel,
    const struct aws_event_stream_rpc_connection_options *connection_options) {
    AWS_FATAL_ASSERT(
        connection_options->on_connection_protocol_message && "on_connection_protocol_message must be specified!");
    AWS_FATAL_ASSERT(connection_options->on_incoming_stream && "on_connection_protocol_message must be specified");

    struct aws_event_stream_rpc_connection *connection = s_create_connection_on_channel(server, channel);

    if (!connection) {
        return NULL;
    }

    connection->on_incoming_stream = connection_options->on_incoming_stream;
    connection->on_connection_protocol_message = connection_options->on_connection_protocol_message;
    connection->user_data = connection_options->user_data;

    return connection;
}

void aws_event_stream_rpc_server_connection_acquire(struct aws_event_stream_rpc_connection *connection) {
    aws_atomic_fetch_and_explicit(&connection->ref_count, 1, aws_memory_order_relaxed);
}

void aws_event_stream_rpc_server_connection_release(struct aws_event_stream_rpc_connection *connection) {
    size_t value = aws_atomic_fetch_sub_explicit(&connection->ref_count, 1, aws_memory_order_seq_cst);

    if (value == 0) {
        aws_channel_release_hold(connection->channel);
        aws_hash_table_clean_up(&connection->continuation_table);
        aws_mem_release(connection->allocator, connection);
    }
}

static void s_on_accept_channel_setup(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {
    (void)bootstrap;

    struct aws_event_stream_rpc_server_listener *server = user_data;

    if (!error_code) {
        AWS_FATAL_ASSERT(channel && "Channel should never be null with a 0 error code.");

        struct aws_event_stream_rpc_connection *connection = s_create_connection_on_channel(server, channel);

        if (!connection) {
            int error = aws_last_error();
            server->on_new_connection(NULL, error, NULL, server->user_data);
            aws_channel_shutdown(channel, error);
        }

        struct aws_event_stream_rpc_connection_options connection_options;
        AWS_ZERO_STRUCT(connection_options);

        server->on_new_connection(connection, AWS_ERROR_SUCCESS, &connection_options, server->user_data);
        AWS_FATAL_ASSERT(
            connection_options.on_connection_protocol_message && "on_connection_protocol_message must be specified!");
        AWS_FATAL_ASSERT(connection_options.on_incoming_stream && "on_connection_protocol_message must be specified");
        connection->on_incoming_stream = connection_options.on_incoming_stream;
        connection->on_connection_protocol_message = connection_options.on_connection_protocol_message;
        connection->user_data = connection_options.user_data;
    } else {
        server->on_new_connection(NULL, error_code, NULL, server->user_data);
    }
}

static inline struct aws_event_stream_rpc_connection *s_rpc_connection_from_channel(struct aws_channel *channel) {
    struct aws_channel_slot *our_slot = NULL;
    struct aws_channel_slot *current_slot = aws_channel_get_first_slot(channel);
    AWS_FATAL_ASSERT(
        current_slot &&
        "It should be logically impossible to have a channel in this callback that doesn't have a slot in it");
    while (current_slot->adj_right) {
        current_slot = current_slot->adj_right;
    }
    our_slot = current_slot;
    struct aws_channel_handler *our_handler = our_slot->handler;
    return aws_event_stream_channel_handler_get_user_data(our_handler);
}

static void s_on_accept_channel_shutdown(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {
    (void)bootstrap;

    struct aws_event_stream_rpc_server_listener *server = user_data;

    struct aws_event_stream_rpc_connection *connection = s_rpc_connection_from_channel(channel);
    server->on_connection_shutdown(connection, error_code, server->user_data);
    aws_event_stream_rpc_server_connection_release(connection);
}

static void s_on_server_listener_destroy(struct aws_server_bootstrap *bootstrap, void *user_data) {
    struct aws_event_stream_rpc_server_listener *listener = user_data;
    aws_server_bootstrap_destroy_socket_listener(bootstrap, listener->listener);
}

struct aws_event_stream_rpc_server_listener *aws_event_stream_rpc_server_new_listener(
    struct aws_allocator *allocator,
    struct aws_event_stream_rpc_server_listener_options *options) {
    struct aws_event_stream_rpc_server_listener *server =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_event_stream_rpc_server_listener));

    if (!server) {
        return NULL;
    }

    aws_atomic_init_int(&server->ref_count, 1);

    struct aws_server_socket_channel_bootstrap_options bootstrap_options = {
        .bootstrap = options->bootstrap,
        .socket_options = options->socket_options,
        .tls_options = options->tls_options,
        .enable_read_back_pressure = options->enable_read_backpressure,
        .host_name = options->host_name,
        .port = options->port,
        .incoming_callback = s_on_accept_channel_setup,
        .shutdown_callback = s_on_accept_channel_shutdown,
        .destroy_callback = s_on_server_listener_destroy,
        .user_data = server,
    };

    server->bootstrap = options->bootstrap;
    server->listener = aws_server_bootstrap_new_socket_listener(&bootstrap_options);

    if (!server->listener) {
        goto error;
    }

    return server;

error:
    if (server->listener) {
        aws_server_bootstrap_destroy_socket_listener(options->bootstrap, server->listener);
    }

    aws_mem_release(server->allocator, server);
    return NULL;
}

void aws_event_stream_rpc_server_listener_acquire(struct aws_event_stream_rpc_server_listener *server) {
    aws_atomic_fetch_add_explicit(&server->ref_count, 1, aws_memory_order_relaxed);
}

static void s_destroy_server(struct aws_event_stream_rpc_server_listener *server) {
    if (server) {
        aws_server_bootstrap_destroy_socket_listener(server->bootstrap, server->listener);
        aws_mem_release(server->allocator, server);
    }
}

void aws_event_stream_rpc_server_listener_release(struct aws_event_stream_rpc_server_listener *server) {
    size_t ref_count = aws_atomic_fetch_sub_explicit(&server->ref_count, 1, aws_memory_order_seq_cst);

    if (ref_count == 0) {
        s_destroy_server(server);
    }
}

struct event_stream_connection_send_message_args {
    struct aws_allocator *allocator;
    struct aws_event_stream_message message;
    struct aws_event_stream_rpc_connection *connection;
    struct aws_event_stream_rpc_server_continuation_token *continuation;
    aws_event_stream_rpc_server_message_flush_fn *flush_fn;
    void *user_data;
    bool end_stream;
};

static void s_on_protocol_message_written_fn(
    struct aws_event_stream_message *message,
    int error_code,
    void *user_data) {
    (void)message;

    struct event_stream_connection_send_message_args *message_args = user_data;
    message_args->flush_fn(error_code, message_args->user_data);

    aws_event_stream_rpc_server_connection_release(message_args->connection);

    if (message_args->continuation) {
        aws_event_stream_rpc_server_continuation_release(message_args->continuation);
    }

    if (message_args->end_stream) {
        AWS_FATAL_ASSERT(message_args->continuation && "end stream flag was set but it wasn't on a continuation");
        aws_atomic_store_int(&message_args->continuation->is_closed, 1U);
        aws_hash_table_remove(
            &message_args->connection->continuation_table, &message_args->continuation->stream_id, NULL, NULL);
    }

    aws_event_stream_message_clean_up(&message_args->message);
    aws_mem_release(message_args->allocator, message_args);
}

static int s_send_protocol_message(
    struct aws_event_stream_rpc_connection *connection,
    struct aws_event_stream_rpc_server_continuation_token *continuation,
    const struct aws_event_stream_rpc_message_args *message_args,
    int32_t stream_id,
    aws_event_stream_rpc_server_message_flush_fn *flush_fn,
    void *user_data) {
    struct event_stream_connection_send_message_args *args =
        aws_mem_calloc(connection->allocator, 1, sizeof(struct event_stream_connection_send_message_args));

    if (!message_args) {
        return AWS_OP_ERR;
    }

    args->allocator = connection->allocator;
    args->user_data = user_data;

    args->connection = connection;
    aws_event_stream_rpc_server_connection_acquire(connection);

    if (continuation) {
        args->continuation = continuation;
        aws_event_stream_rpc_server_continuation_acquire(continuation);

        if (message_args->message_flags & AWS_EVENT_STREAM_RPC_MESSAGE_FLAG_TERMINATE_STREAM) {
            args->end_stream = true;
        }
    }

    args->flush_fn = flush_fn;

    size_t headers_count = message_args->headers_count + 3;
    struct aws_array_list headers_list;
    AWS_ZERO_STRUCT(headers_list);

    if (aws_array_list_init_dynamic(
            &headers_list, connection->allocator, headers_count, sizeof(struct aws_event_stream_header_value_pair))) {
        goto args_allocated_before_failure;
    }

    /* since we preallocated the space for the headers, these can't fail, but we'll go ahead an assert on them just in
     * case */
    for (size_t i = 0; i < message_args->headers_count; ++i) {
        AWS_ASSERT(!aws_array_list_push_back(&headers_list, &message_args->headers[i]));
    }

    AWS_ASSERT(!aws_event_stream_add_int32_header(
        &headers_list, (const char *)s_message_type_name.ptr, s_message_type_name.len, message_args->message_type));
    AWS_ASSERT(!aws_event_stream_add_int32_header(
        &headers_list, (const char *)s_message_flags_name.ptr, s_message_flags_name.len, message_args->message_flags));
    AWS_ASSERT(!aws_event_stream_add_int32_header(
        &headers_list, (const char *)s_stream_id_name.ptr, s_stream_id_name.len, stream_id));

    int message_init_err_code =
        aws_event_stream_message_init(&args->message, connection->allocator, &headers_list, message_args->payload);
    aws_array_list_clean_up(&headers_list);

    if (message_init_err_code) {
        goto args_allocated_before_failure;
    }

    args->allocator = connection->allocator;
    args->user_data = user_data;
    args->connection = connection;
    args->flush_fn = flush_fn;
    aws_event_stream_rpc_server_connection_acquire(connection);

    if (aws_event_stream_channel_handler_write_message(
            connection->event_stream_handler, &args->message, s_on_protocol_message_written_fn, args)) {
        goto message_initialized_before_failure;
    }

    return AWS_OP_SUCCESS;

message_initialized_before_failure:
    aws_event_stream_message_clean_up(&args->message);

args_allocated_before_failure:
    aws_mem_release(args->allocator, args);
    aws_event_stream_rpc_server_connection_release(connection);

    return AWS_OP_ERR;
}

int aws_event_stream_rpc_server_connection_send_protocol_message(
    struct aws_event_stream_rpc_connection *connection,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_server_message_flush_fn *flush_fn,
    void *user_data) {
    if (aws_event_stream_rpc_server_connection_is_closed(connection)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_CONNECTION_CLOSED);
    }

    return s_send_protocol_message(connection, NULL, message_args, 0, flush_fn, user_data);
}

bool aws_event_stream_rpc_server_continuation_is_closed(
    struct aws_event_stream_rpc_server_continuation_token *continuation) {
    return aws_atomic_load_int(&continuation->is_closed) == 1U;
}

bool aws_event_stream_rpc_server_connection_is_closed(struct aws_event_stream_rpc_connection *connection) {
    return aws_atomic_load_int(&connection->is_closed) == 1U;
}

void aws_event_stream_rpc_server_continuation_acquire(
    struct aws_event_stream_rpc_server_continuation_token *continuation) {
    aws_atomic_fetch_add_explicit(&continuation->ref_count, 1, aws_memory_order_relaxed);
}

void aws_event_stream_rpc_server_continuation_release(
    struct aws_event_stream_rpc_server_continuation_token *continuation) {
    size_t value = aws_atomic_fetch_sub_explicit(&continuation->ref_count, 1, aws_memory_order_seq_cst);

    if (value == 0) {
        struct aws_allocator *allocator = continuation->connection->allocator;
        aws_event_stream_rpc_server_connection_release(continuation->connection);
        aws_mem_release(allocator, continuation);
    }
}

int aws_event_stream_rpc_server_continuation_send_message(
    struct aws_event_stream_rpc_server_continuation_token *continuation,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_server_message_flush_fn *flush_fn,
    void *user_data) {
    if (aws_event_stream_rpc_server_continuation_is_closed(continuation)) {
        return aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_STREAM_CLOSED);
    }

    return s_send_protocol_message(
        continuation->connection, continuation, message_args, continuation->stream_id, flush_fn, user_data);
}

static int s_fetch_message_metadata(
    struct aws_array_list *message_headers,
    int32_t *stream_id,
    int32_t *message_type,
    int32_t *message_flags,
    struct aws_byte_buf *operation_name) {
    size_t length = aws_array_list_length(message_headers);
    uint8_t required_fields_found = 0;

    for (size_t i = 0; i < length; ++i) {
        struct aws_event_stream_header_value_pair *header = NULL;
        aws_array_list_get_at_ptr(message_headers, (void **)&header, i);
        struct aws_byte_buf name_buf = aws_event_stream_header_name(header);

        /* check type first since that's cheaper than a string compare */
        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_INT32) {

            struct aws_byte_buf stream_id_field = aws_byte_buf_from_array(s_stream_id_name.ptr, s_stream_id_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &stream_id_field)) {
                *stream_id = aws_event_stream_header_value_as_int32(header);
                goto found;
            }

            struct aws_byte_buf message_type_field =
                aws_byte_buf_from_array(s_message_type_name.ptr, s_message_type_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &message_type_field)) {
                *message_type = aws_event_stream_header_value_as_int32(header);
                goto found;
            }

            struct aws_byte_buf message_flags_field =
                aws_byte_buf_from_array(s_message_flags_name.ptr, s_message_flags_name.len);
            if (aws_byte_buf_eq_ignore_case(&name_buf, &message_flags_field)) {
                *message_flags = aws_event_stream_header_value_as_int32(header);
                goto found;
            }
        }

        if (header->header_value_type == AWS_EVENT_STREAM_HEADER_STRING) {
            struct aws_byte_buf operation_field = aws_byte_buf_from_array(s_operation_name.ptr, s_operation_name.len);

            if (aws_byte_buf_eq_ignore_case(&name_buf, &operation_field)) {
                *operation_name = aws_event_stream_header_value_as_string(header);
                goto found;
            }
        }

    found:
        required_fields_found += 1;

        if (required_fields_found == 3) {
            return AWS_OP_SUCCESS;
        }
    }

    return AWS_OP_ERR;
}

static void s_connection_error_message_flush_fn(int error_code, void *user_data) {
    (void)error_code;

    struct aws_event_stream_rpc_connection *connection = user_data;
    aws_event_stream_rpc_server_connection_close(connection, AWS_ERROR_EVENT_STREAM_RPC_CONNECTION_CLOSED);
}

static void s_send_connection_level_error(
    struct aws_event_stream_rpc_connection *connection,
    uint32_t message_type,
    uint32_t message_flags,
    const struct aws_byte_cursor *message) {
    struct aws_byte_buf payload_buf = aws_byte_buf_from_array(message->ptr, message->len);

    struct aws_event_stream_rpc_message_args message_args = {
        .message_type = message_type,
        .message_flags = message_flags,
        .payload = &payload_buf,
        .headers_count = 0,
    };

    aws_event_stream_rpc_server_connection_send_protocol_message(
        connection, &message_args, s_connection_error_message_flush_fn, connection);
}

static void s_route_message_by_type(
    struct aws_event_stream_rpc_connection *connection,
    struct aws_event_stream_message *message,
    uint32_t stream_id,
    uint32_t message_type,
    uint32_t message_flags,
    struct aws_byte_cursor operation_name) {
    struct aws_array_list headers_list;
    aws_event_stream_message_headers(message, &headers_list);
    struct aws_byte_buf payload_buf = aws_byte_buf_from_array(
        aws_event_stream_message_payload(message), aws_event_stream_message_payload_len(message));

    struct aws_event_stream_rpc_message_args message_args = {
        .headers = headers_list.data,
        .headers_count = aws_array_list_length(&headers_list),
        .payload = &payload_buf,
        .message_flags = message_flags,
        .message_type = message_type,
    };

    if (stream_id > 0) {
        struct aws_event_stream_rpc_server_continuation_token *continuation = NULL;
        if (message_type != AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_APPLICATION_MESSAGE) {
            aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR);
            s_send_connection_level_error(
                connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR, 0, &s_invalid_stream_id_error);
            return;
        }

        if (stream_id <= connection->latest_stream_id) {
            struct aws_hash_element *continuation_element = NULL;
            if (aws_hash_table_find(&connection->continuation_table, &stream_id, &continuation_element)) {
                aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR);
                s_send_connection_level_error(
                    connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR, 0, &s_invalid_client_stream_id_error);
                return;
            }

            continuation = continuation_element->value;
            continuation->continuation_fn(continuation, &message_args, continuation->user_data);
        } else {
            if (stream_id != connection->latest_stream_id + 1) {
                aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR);
                s_send_connection_level_error(
                    connection,
                    AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR,
                    0,
                    &s_invalid_new_client_stream_id_error);
                return;
            }

            continuation =
                aws_mem_calloc(connection->allocator, 1, sizeof(struct aws_event_stream_rpc_server_continuation_token));
            if (!continuation) {
                aws_raise_error(AWS_ERROR_EVENT_STREAM_RPC_PROTOCOL_ERROR);
                s_send_connection_level_error(
                    connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_SERVER_ERROR, 0, &s_server_error);
                return;
            }

            continuation->stream_id = stream_id;
            continuation->connection = connection;
            aws_event_stream_rpc_server_connection_acquire(continuation->connection);
            /* one ref count for the user, one for the hash table. */
            aws_atomic_init_int(&continuation->ref_count, 2);

            if (aws_hash_table_put(&connection->continuation_table, &continuation->stream_id, continuation, NULL)) {
                /* continuation release will drop the connection reference as well */
                aws_event_stream_rpc_server_continuation_release(continuation);
                /* one ref was stored for the hash table which didn't get stored. */
                aws_event_stream_rpc_server_continuation_release(continuation);
                s_send_connection_level_error(
                    connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_SERVER_ERROR, 0, &s_server_error);
                return;
            }

            struct aws_event_stream_rpc_server_stream_continuation_options options;
            AWS_ZERO_STRUCT(options);

            connection->on_incoming_stream(continuation, operation_name, &options, connection->user_data);
            AWS_FATAL_ASSERT(options.on_continuation);
            AWS_FATAL_ASSERT(options.on_continuation_closed);

            continuation->continuation_fn = options.on_continuation;
            continuation->closed_fn = options.on_continuation_closed;
            continuation->user_data = options.user_data;

            connection->latest_stream_id = stream_id;
            continuation->continuation_fn(continuation, &message_args, continuation->user_data);
        }

        /* if it was a terminal stream message purge it from the hash table. The delete will decref the continuation. */
        if (message_flags & AWS_EVENT_STREAM_RPC_MESSAGE_FLAG_TERMINATE_STREAM) {
            aws_atomic_store_int(&continuation->is_closed, 1U);
            aws_hash_table_remove(&connection->continuation_table, &stream_id, NULL, NULL);
        }
    } else {
        if (message_type == AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_APPLICATION_MESSAGE ||
            message_type >= AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_COUNT) {
            s_send_connection_level_error(
                connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR, 0, &s_invalid_message_type_error);
            return;
        }

        connection->on_connection_protocol_message(connection, &message_args, connection->user_data);
    }
}

static void s_on_message_received(struct aws_event_stream_message *message, int error_code, void *user_data) {

    if (!error_code) {
        struct aws_event_stream_rpc_connection *connection = user_data;

        struct aws_array_list headers;
        if (aws_array_list_init_dynamic(
                &headers, connection->allocator, 8, sizeof(struct aws_event_stream_header_value_pair))) {
            s_send_connection_level_error(
                connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_SERVER_ERROR, 0, &s_server_error);
            return;
        }

        if (aws_event_stream_message_headers(message, &headers)) {
            s_send_connection_level_error(
                connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_SERVER_ERROR, 0, &s_server_error);
            return;
        }

        int32_t stream_id = -1;
        int32_t message_type = -1;
        int32_t message_flags = -1;

        struct aws_byte_buf operation_name_buf;
        AWS_ZERO_STRUCT(operation_name_buf);
        if (s_fetch_message_metadata(&headers, &stream_id, &message_type, &message_flags, &operation_name_buf)) {
            s_send_connection_level_error(
                connection, AWS_EVENT_STREAM_RPC_MESSAGE_TYPE_PROTOCOL_ERROR, 0, &s_invalid_message_error);
            return;
        }

        s_route_message_by_type(
            connection, message, stream_id, message_type, message_flags, aws_byte_cursor_from_buf(&operation_name_buf));
    }
}