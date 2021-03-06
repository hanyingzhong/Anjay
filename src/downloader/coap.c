/*
 * Copyright 2017-2020 AVSystem <avsystem@avsystem.com>
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

#include <anjay_config.h>

#include <inttypes.h>

#include <avsystem/commons/errno.h>
#include <avsystem/commons/utils.h>

#include <avsystem/coap/coap.h>
#include <avsystem/coap/config.h>

#include <avsystem/commons/shared_buffer.h>

#define ANJAY_DOWNLOADER_INTERNALS

#include "private.h"

VISIBILITY_SOURCE_BEGIN

AVS_STATIC_ASSERT(offsetof(anjay_etag_t, value)
                          == offsetof(avs_coap_etag_t, bytes),
                  coap_etag_layout_compatible);
AVS_STATIC_ASSERT(AVS_ALIGNOF(anjay_etag_t) == AVS_ALIGNOF(avs_coap_etag_t),
                  coap_etag_alignment_compatible);

typedef struct {
    anjay_download_ctx_common_t common;

    anjay_downloader_t *dl;

    anjay_socket_transport_t transport;
    anjay_url_t uri;
    size_t bytes_downloaded;
    size_t initial_block_size;
    avs_coap_etag_t etag;

    avs_net_socket_t *socket;
    avs_net_resolved_endpoint_t preferred_endpoint;
    char dtls_session_buffer[ANJAY_DTLS_SESSION_BUFFER_SIZE];

    avs_coap_exchange_id_t exchange_id;
#ifdef WITH_AVS_COAP_UDP
    avs_coap_udp_tx_params_t tx_params;
#endif // WITH_AVS_COAP_UDP
    avs_coap_ctx_t *coap;

    avs_sched_handle_t job_start;
} anjay_coap_download_ctx_t;

typedef struct {
    anjay_t *anjay;
    avs_coap_ctx_t *coap_ctx;
    avs_net_socket_t *socket;
} cleanup_coap_context_args_t;

static void cleanup_coap_context(avs_sched_t *sched, const void *args_) {
    (void) sched;
    cleanup_coap_context_args_t args =
            *(const cleanup_coap_context_args_t *) args_;
    _anjay_coap_ctx_cleanup(args.anjay, &args.coap_ctx);
#ifndef ANJAY_TEST
    _anjay_socket_cleanup(args.anjay, &args.socket);
#endif // ANJAY_TEST
}

static void cleanup_coap_transfer(AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    avs_sched_del(&ctx->job_start);
    _anjay_url_cleanup(&ctx->uri);

    anjay_t *anjay = _anjay_downloader_get_anjay(ctx->dl);
    /**
     * HACK: this is necessary, because CoAP context may be destroyed while
     * handling a response, and when the control returns, it may access some of
     * its internal fields.
     */
    const cleanup_coap_context_args_t args = {
        .anjay = anjay,
        .coap_ctx = ctx->coap,
        .socket = ctx->socket
    };
    if (ctx->coap
            && AVS_SCHED_NOW(anjay->sched, NULL, cleanup_coap_context, &args,
                             sizeof(args))) {
        dl_log(WARNING, _("could not schedule cleanup of CoAP context"));
    }
    AVS_LIST_DELETE(ctx_ptr);
}

static int read_etag(const avs_coap_response_header_t *hdr,
                     avs_coap_etag_t *out_etag) {
    switch (avs_coap_options_get_etag(&hdr->options, out_etag)) {
    case 0:
        break;
    case AVS_COAP_OPTION_MISSING:
        dl_log(TRACE, _("no ETag option"));
        return 0;
    default:
        dl_log(DEBUG, _("invalid ETag option size"));
        return -1;
    }

    dl_log(TRACE, _("ETag: ") "%s", AVS_COAP_ETAG_HEX(out_etag));
    return 0;
}

static inline bool etag_matches(const avs_coap_etag_t *a,
                                const avs_coap_etag_t *b) {
    return a->size == b->size && !memcmp(a->bytes, b->bytes, a->size);
}

static void abort_download_transfer(anjay_coap_download_ctx_t *dl_ctx,
                                    anjay_download_status_t status) {
    avs_coap_exchange_cancel(dl_ctx->coap, dl_ctx->exchange_id);

    AVS_LIST(anjay_download_ctx_t) *dl_ctx_ptr =
            _anjay_downloader_find_ctx_ptr_by_id(dl_ctx->dl, dl_ctx->common.id);
    if (dl_ctx_ptr) {
        _anjay_downloader_abort_transfer(dl_ctx->dl, dl_ctx_ptr, status);
    }
}

static void
handle_coap_response(avs_coap_ctx_t *ctx,
                     avs_coap_exchange_id_t id,
                     avs_coap_client_request_state_t result,
                     const avs_coap_client_async_response_t *response,
                     avs_error_t err,
                     void *arg) {
    (void) ctx;
    if (result == AVS_COAP_CLIENT_REQUEST_CANCEL) {
        return;
    }

    anjay_coap_download_ctx_t *dl_ctx = (anjay_coap_download_ctx_t *) arg;

    assert(dl_ctx->exchange_id.value == id.value);
    (void) id;

    switch (result) {
    case AVS_COAP_CLIENT_REQUEST_OK:
    case AVS_COAP_CLIENT_REQUEST_PARTIAL_CONTENT: {
        const uint8_t code = response->header.code;
        if (code != AVS_COAP_CODE_CONTENT) {
            dl_log(DEBUG,
                   _("server responded with ") "%s" _(" (expected ") "%s" _(
                           ")"),
                   AVS_COAP_CODE_STRING(code),
                   AVS_COAP_CODE_STRING(AVS_COAP_CODE_CONTENT));
            abort_download_transfer(
                    dl_ctx, _anjay_download_status_invalid_response(code));
            return;
        }
        avs_coap_etag_t etag;
        if (read_etag(&response->header, &etag)) {
            dl_log(DEBUG, _("could not parse CoAP response"));
            abort_download_transfer(dl_ctx,
                                    _anjay_download_status_failed(
                                            avs_errno(AVS_EPROTO)));
            return;
        }
        // NOTE: avs_coap normally performs ETag validation for blockwise
        // transfers. However, if we resumed the download from persistence
        // information, avs_coap wouldn't know about the ETag used before, and
        // would blindly accept any ETag.
        if (dl_ctx->etag.size == 0) {
            dl_ctx->etag = etag;
        } else if (!etag_matches(&dl_ctx->etag, &etag)) {
            dl_log(DEBUG, _("remote resource expired, aborting download"));
            abort_download_transfer(dl_ctx, _anjay_download_status_expired());
            return;
        }
        const void *payload = response->payload;
        size_t payload_size = response->payload_size;
        // Resumption from a non-multiple block-size
        size_t offset = dl_ctx->bytes_downloaded - response->payload_offset;
        if (offset) {
            payload = (const char *) payload + offset;
            payload_size -= offset;
        }

        if (avs_is_err((err = dl_ctx->common.on_next_block(
                                _anjay_downloader_get_anjay(dl_ctx->dl),
                                (const uint8_t *) payload, payload_size,
                                (const anjay_etag_t *) &etag,
                                dl_ctx->common.user_data)))) {
            abort_download_transfer(dl_ctx, _anjay_download_status_failed(err));
            return;
        }
        dl_ctx->bytes_downloaded += payload_size;
        if (result == AVS_COAP_CLIENT_REQUEST_OK) {
            dl_log(INFO, _("transfer id = ") "%" PRIuPTR _(" finished"),
                   dl_ctx->common.id);
            abort_download_transfer(dl_ctx, _anjay_download_status_success());
        } else {
            dl_log(TRACE,
                   _("transfer id = ") "%" PRIuPTR _(": ") "%lu" _(
                           " B downloaded"),
                   dl_ctx->common.id, (unsigned long) dl_ctx->bytes_downloaded);
        }
        break;
    }
    case AVS_COAP_CLIENT_REQUEST_FAIL: {
        dl_log(DEBUG, _("download failed: ") "%s", AVS_COAP_STRERROR(err));
        if (err.category == AVS_COAP_ERR_CATEGORY
                && err.code == AVS_COAP_ERR_ETAG_MISMATCH) {
            abort_download_transfer(dl_ctx, _anjay_download_status_expired());
        } else {
            abort_download_transfer(dl_ctx, _anjay_download_status_failed(err));
        }
        break;
    }
    case AVS_COAP_CLIENT_REQUEST_CANCEL:
        AVS_UNREACHABLE("This case shall already be handler above.");
        break;
    }
}

static void handle_coap_message(anjay_downloader_t *dl,
                                AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    (void) dl;

    // NOTE: The return value is ignored as there is not a lot we can do with
    // it.
    (void) avs_coap_async_handle_incoming_packet(
            ((anjay_coap_download_ctx_t *) *ctx_ptr)->coap, NULL, NULL);
}

static int get_coap_socket(anjay_downloader_t *dl,
                           anjay_download_ctx_t *ctx,
                           avs_net_socket_t **out_socket,
                           anjay_socket_transport_t *out_transport) {
    (void) dl;
    if (!(*out_socket = ((anjay_coap_download_ctx_t *) ctx)->socket)) {
        return -1;
    }
    *out_transport = ANJAY_SOCKET_TRANSPORT_UDP;
    return 0;
}

#ifdef ANJAY_TEST
#    include "test/downloader_mock.h"
#endif // ANJAY_TEST

static inline size_t initial_block2_option_size(anjay_coap_download_ctx_t *ctx,
                                                uint8_t code) {
    char buffer[64];
    avs_coap_options_t expected_options =
            avs_coap_options_create_empty(buffer, sizeof(buffer));
    avs_error_t err;
    // We expect BLOCK2 and ETag in response.
    (void) (avs_is_err((err = avs_coap_options_add_block(
                                &expected_options,
                                &(avs_coap_option_block_t) {
                                    .type = AVS_COAP_BLOCK2,
                                    .seq_num = UINT16_MAX,
                                    .size = AVS_COAP_BLOCK_MAX_SIZE
                                })))
            || avs_is_err((err = avs_coap_options_add_etag(
                                   &expected_options,
                                   &(avs_coap_etag_t) {
                                       .size = AVS_COAP_MAX_ETAG_LENGTH,
                                       .bytes = { 0 }
                                   }))));
    assert(avs_is_ok(err));

    size_t block_size = avs_max_power_of_2_not_greater_than(
            avs_coap_max_incoming_message_payload(ctx->coap, &expected_options,
                                                  code));
    if (block_size > AVS_COAP_BLOCK_MAX_SIZE) {
        block_size = AVS_COAP_BLOCK_MAX_SIZE;
    } else if (block_size < AVS_COAP_BLOCK_MIN_SIZE) {
        block_size = AVS_COAP_BLOCK_MIN_SIZE;
    }
    return block_size;
}

static void start_download_job(avs_sched_t *sched, const void *id_ptr) {
    anjay_t *anjay = _anjay_get_from_sched(sched);
    uintptr_t id = *(const uintptr_t *) id_ptr;
    AVS_LIST(anjay_download_ctx_t) *dl_ctx_ptr =
            _anjay_downloader_find_ctx_ptr_by_id(&anjay->downloader, id);
    if (!dl_ctx_ptr) {
        dl_log(DEBUG, _("download id = ") "%" PRIuPTR _("expired"), id);
        return;
    }
    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *dl_ctx_ptr;

    avs_error_t err;
    avs_coap_options_t options;
    const uint8_t code = AVS_COAP_CODE_GET;
    const size_t block_size = initial_block2_option_size(ctx, code);
    if (avs_is_err((err = avs_coap_options_dynamic_init(&options)))) {
        dl_log(ERROR,
               _("download id = ") "%" PRIuPTR _("cannot start: out of memory"),
               id);
        goto end;
    }

    AVS_LIST(const anjay_string_t) elem;
    AVS_LIST_FOREACH(elem, ctx->uri.uri_path) {
        if (avs_is_err((err = avs_coap_options_add_string(
                                &options, AVS_COAP_OPTION_URI_PATH,
                                elem->c_str)))) {
            goto end;
        }
    }
    AVS_LIST_FOREACH(elem, ctx->uri.uri_query) {
        if (avs_is_err((err = avs_coap_options_add_string(
                                &options, AVS_COAP_OPTION_URI_QUERY,
                                elem->c_str)))) {
            goto end;
        }
    }

    // When we start the download, there is no need to ask for a blockwise
    // transfer (by adding a BLOCK option explicitly). If the incoming payload
    // is too large, CoAP layer will negotiate smaller block sizes.
    if (ctx->bytes_downloaded != 0
            && avs_is_err(
                       (err = avs_coap_options_add_block(
                                &options,
                                &(avs_coap_option_block_t) {
                                    .type = AVS_COAP_BLOCK2,
                                    .seq_num = (uint32_t) (ctx->bytes_downloaded
                                                           / block_size),
                                    .size = (uint16_t) block_size
                                })))) {
        goto end;
    }

    err = avs_coap_client_send_async_request(ctx->coap, &ctx->exchange_id,
                                             &(avs_coap_request_header_t) {
                                                 .code = code,
                                                 .options = options
                                             },
                                             NULL, NULL, handle_coap_response,
                                             (void *) ctx);

end:
    avs_coap_options_cleanup(&options);

    if (avs_is_err(err)) {
        _anjay_downloader_abort_transfer(ctx->dl, dl_ctx_ptr,
                                         _anjay_download_status_failed(err));
    }
}

static avs_error_t reset_coap_ctx(anjay_coap_download_ctx_t *ctx) {
    anjay_t *anjay = _anjay_downloader_get_anjay(ctx->dl);

    _anjay_coap_ctx_cleanup(anjay, &ctx->coap);

    switch (ctx->transport) {
#ifdef WITH_AVS_COAP_UDP
    case ANJAY_SOCKET_TRANSPORT_UDP:
        // NOTE: we set udp_response_cache to NULL, because it should never be
        // necessary. It's used to cache responses generated by us whenever we
        // handle an incoming request, and contexts used for downloads don't
        // expect receiving any requests that would need handling.
        ctx->coap = avs_coap_udp_ctx_create(anjay->sched, &ctx->tx_params,
                                            anjay->in_shared_buffer,
                                            anjay->out_shared_buffer, NULL);
        break;
#endif // WITH_AVS_COAP_UDP

    default:
        dl_log(ERROR,
               _("anjay_coap_download_ctx_t is compatible only with "
                 "ANJAY_SOCKET_TRANSPORT_UDP and "
                 "ANJAY_SOCKET_TRANSPORT_TCP (if they are compiled-in)"));
        return avs_errno(AVS_EPROTONOSUPPORT);
    }

    if (!ctx->coap) {
        dl_log(ERROR, _("could not create CoAP context"));
        return avs_errno(AVS_ENOMEM);
    }

    avs_error_t err = avs_coap_ctx_set_socket(ctx->coap, ctx->socket);
    if (avs_is_err(err)) {
        anjay_log(ERROR, _("could not assign socket to CoAP context"));
        _anjay_coap_ctx_cleanup(anjay, &ctx->coap);
    }

    return err;
}

static inline avs_error_t shutdown_and_close(avs_net_socket_t *socket) {
    avs_error_t err = avs_net_socket_shutdown(socket);
    avs_error_t close_err = avs_net_socket_close(socket);
    return avs_is_err(err) ? err : close_err;
}

static avs_error_t
reconnect_coap_transfer(anjay_downloader_t *dl,
                        AVS_LIST(anjay_download_ctx_t) *ctx_ptr) {
    (void) dl;
    (void) ctx_ptr;
    anjay_coap_download_ctx_t *ctx = (anjay_coap_download_ctx_t *) *ctx_ptr;
    char hostname[ANJAY_MAX_URL_HOSTNAME_SIZE];
    char port[ANJAY_MAX_URL_PORT_SIZE];

    avs_error_t err;
    if (avs_is_err((err = avs_net_socket_get_remote_hostname(
                            ctx->socket, hostname, sizeof(hostname))))
            || avs_is_err((err = avs_net_socket_get_remote_port(
                                   ctx->socket, port, sizeof(port))))
            || avs_is_err((err = shutdown_and_close(ctx->socket)))
            || avs_is_err((err = avs_net_socket_connect(ctx->socket, hostname,
                                                        port)))) {
        dl_log(WARNING,
               _("could not reconnect socket for download id = ") "%" PRIuPTR,
               ctx->common.id);
        return err;
    } else {
        // A new DTLS session requires resetting the CoAP context.
        // If we manage to resume the session, we can simply continue sending
        // retransmissions as if nothing happened.
        if (!_anjay_was_session_resumed(ctx->socket)) {
            if (avs_is_err((err = reset_coap_ctx(ctx)))) {
                return err;
            }

            anjay_t *anjay = _anjay_downloader_get_anjay(dl);
            if (AVS_SCHED_NOW(anjay->sched, &ctx->job_start, start_download_job,
                              &ctx->common.id, sizeof(ctx->common.id))) {
                dl_log(WARNING,
                       _("could not schedule resumption for download id "
                         "= ") "%" PRIuPTR,
                       ctx->common.id);
                return avs_errno(AVS_ENOMEM);
            }
        }
    }
    return AVS_OK;
}

avs_error_t
_anjay_downloader_coap_ctx_new(anjay_downloader_t *dl,
                               AVS_LIST(anjay_download_ctx_t) *out_dl_ctx,
                               const anjay_download_config_t *cfg,
                               uintptr_t id) {
    anjay_t *anjay = _anjay_downloader_get_anjay(dl);
    assert(!*out_dl_ctx);
    AVS_LIST(anjay_coap_download_ctx_t) ctx =
            AVS_LIST_NEW_ELEMENT(anjay_coap_download_ctx_t);
    if (!ctx) {
        dl_log(ERROR, _("out of memory"));
        return avs_errno(AVS_ENOMEM);
    }

    avs_net_ssl_configuration_t ssl_config;
    avs_error_t err = AVS_OK;
    static const anjay_download_ctx_vtable_t VTABLE = {
        .get_socket = get_coap_socket,
        .handle_packet = handle_coap_message,
        .cleanup = cleanup_coap_transfer,
        .reconnect = reconnect_coap_transfer
    };
    ctx->common.vtable = &VTABLE;

    const void *config;

    const anjay_transport_info_t *transport_info =
            _anjay_transport_info_by_uri_scheme(cfg->url);
    if (!transport_info || _anjay_url_parse(cfg->url, &ctx->uri)) {
        dl_log(ERROR, _("invalid URL: ") "%s", cfg->url);
        err = avs_errno(AVS_EINVAL);
        goto error;
    }
    ctx->transport = transport_info->transport;

    if (cfg->etag && cfg->etag->size > sizeof(ctx->etag.bytes)) {
        dl_log(ERROR, _("ETag too long"));
        err = avs_errno(AVS_EPROTO);
        goto error;
    }

    if (!cfg->on_next_block || !cfg->on_download_finished) {
        dl_log(ERROR, _("invalid download config: handlers not set up"));
        err = avs_errno(AVS_EINVAL);
        goto error;
    }

    ssl_config = (avs_net_ssl_configuration_t) {
        .version = anjay->dtls_version,
        .security = cfg->security_config.security_info,
        .session_resumption_buffer = ctx->dtls_session_buffer,
        .session_resumption_buffer_size = sizeof(ctx->dtls_session_buffer),
        .ciphersuites = cfg->security_config.tls_ciphersuites.num_ids
                                ? cfg->security_config.tls_ciphersuites
                                : anjay->default_tls_ciphersuites,
        .backend_configuration = anjay->socket_config
    };
    ssl_config.backend_configuration.reuse_addr = 1;
    ssl_config.backend_configuration.preferred_endpoint =
            &ctx->preferred_endpoint;

    if (!transport_info->socket_type) {
        dl_log(ERROR,
               _("URI scheme ") "%s" _(" uses a non-IP transport, which is not "
                                       "supported for downloads"),
               transport_info->uri_scheme);
        err = avs_errno(AVS_EPROTONOSUPPORT);
        goto error;
    }

    assert(transport_info->security != ANJAY_TRANSPORT_SECURITY_UNDEFINED);
    config = transport_info->security == ANJAY_TRANSPORT_ENCRYPTED
                     ? (const void *) &ssl_config
                     : (const void *) &ssl_config.backend_configuration;

    // Downloader sockets MUST NOT reuse the same local port as LwM2M
    // sockets. If they do, and the client attempts to download anything
    // from the same host:port as is used by an LwM2M server, we will get
    // two sockets with identical local/remote host/port tuples. Depending
    // on the socket implementation, we may not be able to create such
    // socket, packets might get duplicated between these "identical"
    // sockets, or we may get some kind of load-balancing behavior. In the
    // last case, the client would randomly handle or ignore LwM2M requests
    // and CoAP download responses.
    if (avs_is_err((err = avs_net_socket_create(&ctx->socket,
                                                *transport_info->socket_type,
                                                config)))) {
        dl_log(ERROR, _("could not create CoAP socket"));
    } else if (avs_is_err((err = avs_net_socket_connect(ctx->socket,
                                                        ctx->uri.host,
                                                        ctx->uri.port)))) {
        dl_log(ERROR, _("could not connect CoAP socket"));
        _anjay_socket_cleanup(anjay, &ctx->socket);
    }
    if (!ctx->socket) {
        assert(avs_is_err(err));
        dl_log(ERROR, _("could not create CoAP socket"));
        goto error;
    }

    ctx->common.id = id;
    ctx->common.on_next_block = cfg->on_next_block;
    ctx->common.on_download_finished = cfg->on_download_finished;
    ctx->common.user_data = cfg->user_data;
    ctx->dl = dl;
    ctx->bytes_downloaded = cfg->start_offset;

    if (cfg->etag) {
        ctx->etag.size = cfg->etag->size;
        memcpy(ctx->etag.bytes, cfg->etag->value, ctx->etag.size);
    }

#ifdef WITH_AVS_COAP_UDP
    if (!cfg->coap_tx_params) {
        ctx->tx_params = anjay->udp_tx_params;
    } else {
        const char *error_string = NULL;
        if (avs_coap_udp_tx_params_valid(cfg->coap_tx_params, &error_string)) {
            ctx->tx_params = *cfg->coap_tx_params;
        } else {
            dl_log(ERROR, _("invalid tx_params: ") "%s", error_string);
            goto error;
        }
    }
#endif // WITH_AVS_COAP_UDP

    if (avs_is_err((err = reset_coap_ctx(ctx)))) {
        goto error;
    }

    if (AVS_SCHED_NOW(anjay->sched, &ctx->job_start, start_download_job,
                      &ctx->common.id, sizeof(ctx->common.id))) {
        dl_log(ERROR, _("could not schedule download job"));
        err = avs_errno(AVS_ENOMEM);
        goto error;
    }

    *out_dl_ctx = (AVS_LIST(anjay_download_ctx_t)) ctx;
    return AVS_OK;
error:
    cleanup_coap_transfer((AVS_LIST(anjay_download_ctx_t) *) &ctx);
    return err;
}

#ifdef ANJAY_TEST
#    include "test/downloader.c"
#endif // ANJAY_TEST
