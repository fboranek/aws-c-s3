#include "aws/s3/private/s3_default_meta_request.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>
#include <inttypes.h>

#ifdef _MSC_VER
/* sscanf warning (not currently scanning for strings) */
#    pragma warning(disable : 4996)
#endif

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request);

static void s_s3_meta_request_default_next_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request **out_request,
    uint32_t flags);

static int s_s3_meta_request_default_prepare_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_client *client,
    struct aws_s3_vip_connection *vip_connection,
    bool is_initial_prepare);

static int s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static struct aws_s3_meta_request_vtable s_s3_meta_request_default_vtable = {
    .next_request = s_s3_meta_request_default_next_request,
    .send_request_finish = aws_s3_meta_request_send_request_finish_default,
    .prepare_request = s_s3_meta_request_default_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = aws_s3_meta_request_sign_request_default,
    .finished_request = s_s3_meta_request_default_request_finished,
    .delivered_requests = aws_s3_meta_request_delivered_requests_default,
    .destroy = s_s3_meta_request_default_destroy,
    .finish = aws_s3_meta_request_finish_default,
};

/* Allocate a new default meta request. */
struct aws_s3_meta_request *aws_s3_meta_request_default_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    uint64_t content_length,
    const struct aws_s3_meta_request_options *options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);

    struct aws_byte_cursor request_method;
    if (aws_http_message_get_request_method(options->message, &request_method)) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; could not get request method from message.");

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (content_length > SIZE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; content length of %" PRIu64 " bytes is too large for platform.",
            content_length);

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3_meta_request_default *meta_request_default =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_meta_request_default));

    /* Try to initialize the base type. */
    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            0,
            options,
            meta_request_default,
            &s_s3_meta_request_default_vtable,
            &meta_request_default->base)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not initialize base type for Default Meta Request.",
            (void *)meta_request_default);
        goto error_clean_up;
    }

    meta_request_default->content_length = (size_t)content_length;

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Created new Default Meta Request.", (void *)meta_request_default);

    return &meta_request_default->base;

error_clean_up:

    aws_s3_meta_request_release(&meta_request_default->base);
    meta_request_default = NULL;

    return NULL;
}

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    aws_mem_release(meta_request->allocator, meta_request_default);
}

/* Try to get the next request that should be processed. */
static void s_s3_meta_request_default_next_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request **out_request,
    uint32_t flags) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    aws_s3_meta_request_lock_synced_data(meta_request);

    if ((flags & AWS_S3_META_REQUEST_NEXT_REQUEST_FLAG_NO_ENDPOINT_CONNECTIONS) > 0) {
        if (!meta_request_default->synced_data.request_sent) {
            aws_s3_meta_request_set_fail_synced(meta_request, NULL, AWS_ERROR_S3_NO_ENDPOINT_CONNECTIONS);
        }
    }

    if (!aws_s3_meta_request_is_finishing_synced(meta_request)) {

        /* If the request hasn't been sent, then create and send it now. */
        if (!meta_request_default->synced_data.request_sent) {
            if (out_request == NULL) {
                goto has_work_remaining;
            }

            request = aws_s3_request_new(
                meta_request,
                0,
                1,
                AWS_S3_REQUEST_DESC_RECORD_RESPONSE_HEADERS | AWS_S3_REQUEST_DESC_STREAM_RESPONSE_BODY);

            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p: Meta Request Default created request %p",
                (void *)meta_request,
                (void *)request);

            meta_request_default->synced_data.request_sent = true;
            goto has_work_remaining;
        }

        /* If the request hasn't been completed, then wait for it to be completed. */
        if (!meta_request_default->synced_data.request_completed) {
            goto has_work_remaining;
        }

        /* If delivery hasn't been attempted yet for the response body, wait for that to happen. */
        if (meta_request->synced_data.num_parts_delivery_completed < 1) {
            goto has_work_remaining;
        }

        goto no_work_remaining;

    } else {

        /* If we are canceling, and the request hasn't been sent yet, then there is nothing to wait for. */
        if (!meta_request_default->synced_data.request_sent) {
            goto no_work_remaining;
        }

        /* If the request hasn't been completed yet, then wait for that to happen. */
        if (!meta_request_default->synced_data.request_completed) {
            goto has_work_remaining;
        }

        /* If some parts are still being delievered to the caller, then wait for those to finish. */
        if (meta_request->synced_data.num_parts_delivery_completed <
            meta_request->synced_data.num_parts_delivery_sent) {
            goto has_work_remaining;
        }

        goto no_work_remaining;
    }

has_work_remaining:
    work_remaining = true;

no_work_remaining:

    if (!work_remaining) {
        aws_s3_meta_request_set_success_synced(meta_request, meta_request_default->synced_data.cached_response_status);
    }

    aws_s3_meta_request_unlock_synced_data(meta_request);

    if (work_remaining) {
        if (request == NULL) {
            return;
        }

        /* If the request is not sending a body, then we can return the request now. */
        if (meta_request_default->content_length == 0) {
            *out_request = request;
            return;
        }

        aws_byte_buf_init(&request->request_body, meta_request->allocator, meta_request_default->content_length);

        /* If reading the stream is successful, then we can return the request now. */
        if (!aws_s3_meta_request_read_body(meta_request, &request->request_body)) {
            *out_request = request;
            return;
        }

        /* In the event of stream read failure, first release the request. */
        aws_s3_request_release(request);
        request = NULL;

        /* Record failure and rewind the "sent" state. */
        aws_s3_meta_request_lock_synced_data(meta_request);
        aws_s3_meta_request_set_fail_synced(meta_request, NULL, aws_last_error_or_unknown());
        meta_request_default->synced_data.request_sent = false;
        aws_s3_meta_request_unlock_synced_data(meta_request);

        /* Call our function again, triggering the "work remaining" logic, and then finishing the meta request if
         * possible. */
        s_s3_meta_request_default_next_request(meta_request, out_request, flags);

    } else {
        AWS_ASSERT(request == NULL);

        aws_s3_meta_request_finish(meta_request);
    }
}

/* Given a request, prepare it for sending based on its description. */
static int s_s3_meta_request_default_prepare_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_client *client,
    struct aws_s3_vip_connection *vip_connection,
    bool is_initial_prepare) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(vip_connection);
    (void)client;
    (void)is_initial_prepare;

    struct aws_s3_request *request = vip_connection->request;
    AWS_PRECONDITION(request);

    struct aws_http_message *message = aws_s3_message_util_copy_http_message(
        meta_request->allocator, meta_request->initial_request_message, AWS_S3_COPY_MESSAGE_INCLUDE_SSE);

    aws_s3_message_util_assign_body(meta_request->allocator, &request->request_body, message);

    aws_s3_request_setup_send_data(request, message);

    aws_http_message_release(message);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST, "id=%p: Meta Request prepared request %p", (void *)meta_request, (void *)request);

    return AWS_OP_SUCCESS;
}

static int s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    if (!aws_s3_meta_request_finished_request_default(meta_request, request, error_code)) {
        return AWS_OP_ERR;
    }

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    AWS_PRECONDITION(meta_request_default);

    if (error_code == AWS_ERROR_SUCCESS && meta_request->headers_callback != NULL &&
        meta_request->headers_callback(
            meta_request,
            request->send_data.response_headers,
            request->send_data.response_status,
            meta_request->user_data)) {

        error_code = aws_last_error_or_unknown();
    }

    aws_s3_meta_request_lock_synced_data(meta_request);
    meta_request_default->synced_data.cached_response_status = request->send_data.response_status;
    meta_request_default->synced_data.request_completed = true;
    meta_request_default->synced_data.request_error_code = error_code;

    if (error_code != AWS_ERROR_SUCCESS) {
        aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
    }

    aws_s3_meta_request_unlock_synced_data(meta_request);

    return error_code == AWS_ERROR_SUCCESS ? AWS_OP_SUCCESS : AWS_OP_ERR;
}
