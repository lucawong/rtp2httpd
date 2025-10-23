#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include "stream.h"
#include "rtp2httpd.h"
#include "rtp.h"
#include "multicast.h"
#include "fcc.h"
#include "http.h"
#include "connection.h"
#include "rtsp.h"
#include "service.h"
#include "snapshot.h"
#include "status.h"
#include "worker.h"
#include "zerocopy.h"

/*
 * Wrapper for join_mcast_group that also resets the multicast data timeout timer.
 * This ensures that every time we join/rejoin a multicast group, the timeout
 * detection starts fresh, preventing false timeout triggers.
 * This function should be used instead of join_mcast_group() directly in all
 * stream-related code to ensure proper timeout handling.
 */
int stream_join_mcast_group(stream_context_t *ctx)
{
    int sock = join_mcast_group(ctx->service);
    if (sock > 0)
    {
        /* Register socket with epoll immediately after creation */
        struct epoll_event ev;
        ev.events = EPOLLIN; /* Level-triggered mode for read events */
        ev.data.fd = sock;
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0)
        {
            logger(LOG_ERROR, "Multicast: Failed to add socket to epoll: %s", strerror(errno));
            close(sock);
            exit(RETVAL_SOCK_READ_FAILED);
        }
        fdmap_set(sock, ctx->conn);
        logger(LOG_DEBUG, "Multicast: Socket registered with epoll");

        /* Reset timeout and rejoin timers when joining multicast group */
        int64_t now = get_time_ms();
        ctx->last_mcast_data_time = now;
        ctx->last_mcast_rejoin_time = now;
    }
    return sock;
}

/*
 * Process RTP payload - either forward to client (streaming) or capture I-frame (snapshot)
 * Returns: bytes forwarded (>= 0) for streaming, 1 if I-frame captured for snapshot, -1 on error
 */
int stream_process_rtp_payload(stream_context_t *ctx, buffer_ref_t *buf_ref, uint16_t *old_seqn, uint16_t *not_first)
{
    /* In snapshot mode, delegate to snapshot module */
    if (ctx->snapshot.enabled)
    {
        return snapshot_process_packet(&ctx->snapshot, buf_ref->data_size, (uint8_t *)buf_ref->data + buf_ref->data_offset, ctx->conn);
    }
    else
    {
        /* Normal streaming mode - use reordering if enabled */
        return rtp_reorder_and_queue(ctx, buf_ref);
    }
}

/*
 * Handle an event-ready fd that belongs to this stream context
 * Note: Client socket events are handled by worker.c,
 * this function only handles media stream sockets (multicast, FCC, RTSP)
 */
int stream_handle_fd_event(stream_context_t *ctx, int fd, uint32_t events, int64_t now)
{
    int actualr;
    struct sockaddr_in peer_addr;
    socklen_t slen = sizeof(peer_addr);

    /* Process FCC socket events */
    if (ctx->fcc.fcc_sock > 0 && fd == ctx->fcc.fcc_sock)
    {
        /* Allocate a fresh buffer from pool for this receive operation */
        buffer_ref_t *recv_buf = buffer_pool_alloc();
        if (!recv_buf)
        {
            /* Buffer pool exhausted - drop this packet */
            logger(LOG_DEBUG, "FCC: Buffer pool exhausted, dropping packet");
            ctx->last_fcc_data_time = now;
            /* Drain the socket to prevent event loop spinning */
            uint8_t dummy[BUFFER_POOL_BUFFER_SIZE];
            ssize_t drained = recvfrom(ctx->fcc.fcc_sock, dummy, sizeof(dummy), 0, NULL, NULL);
            if (drained < 0 && errno != EAGAIN)
            {
                logger(LOG_DEBUG, "FCC: Dummy recv failed while dropping packet: %s", strerror(errno));
            }
            return 0;
        }

        /* Receive directly into zero-copy buffer (true zero-copy receive) */
        actualr = recvfrom(ctx->fcc.fcc_sock, recv_buf->data, BUFFER_POOL_BUFFER_SIZE,
                           0, (struct sockaddr *)&peer_addr, &slen);
        if (actualr < 0 && errno != EAGAIN)
        {
            logger(LOG_ERROR, "FCC: Receive failed: %s", strerror(errno));
            buffer_ref_put(recv_buf);
            return 0;
        }

        /* Verify packet comes from expected FCC server */
        if (peer_addr.sin_addr.s_addr != ctx->fcc.fcc_server->sin_addr.s_addr)
        {
            buffer_ref_put(recv_buf);
            return 0;
        }

        ctx->last_fcc_data_time = now;
        recv_buf->data_size = (size_t)actualr;

        /* Handle different types of FCC packets */
        uint8_t *recv_data = (uint8_t *)recv_buf->data;
        int result = 0;
        if (peer_addr.sin_port == ctx->fcc.fcc_server->sin_port)
        {
            /* RTCP control message */
            if (recv_data[0] == 0x83)
            {
                int res = fcc_handle_server_response(ctx, recv_data, actualr, &peer_addr);
                if (res == 1)
                {
                    /* FCC redirect - retry request with new server */
                    if (fcc_initialize_and_request(ctx) < 0)
                    {
                        logger(LOG_ERROR, "FCC redirect retry failed");
                        buffer_ref_put(recv_buf);
                        return -1;
                    }
                    buffer_ref_put(recv_buf);
                    return 0; /* Redirect handled successfully */
                }
                result = res;
            }
            else if (recv_data[0] == 0x84)
            {
                /* Sync notification (FMT 4) */
                result = fcc_handle_sync_notification(ctx, 0);
            }
        }
        else if (peer_addr.sin_port == ctx->fcc.media_port)
        {
            /* RTP media packet from FCC unicast stream */
            result = fcc_handle_unicast_media(ctx, recv_buf);
        }

        /* Release our reference to the buffer */
        buffer_ref_put(recv_buf);
        return result;
    }

    /* Process multicast socket events */
    if (ctx->mcast_sock > 0 && fd == ctx->mcast_sock)
    {
        /* Allocate a fresh buffer from pool for this receive operation */
        buffer_ref_t *recv_buf = buffer_pool_alloc();
        if (!recv_buf)
        {
            /* Buffer pool exhausted - drop this packet */
            logger(LOG_DEBUG, "Multicast: Buffer pool exhausted, dropping packet");
            ctx->last_mcast_data_time = now;
            /* Drain the socket to prevent event loop spinning */
            uint8_t dummy[BUFFER_POOL_BUFFER_SIZE];
            ssize_t drained = recv(ctx->mcast_sock, dummy, sizeof(dummy), 0);
            if (drained < 0 && errno != EAGAIN)
            {
                logger(LOG_DEBUG, "Multicast: Dummy recv failed while dropping packet: %s", strerror(errno));
            }
            return 0;
        }

        /* Receive directly into zero-copy buffer (true zero-copy receive) */
        actualr = recv(ctx->mcast_sock, recv_buf->data, BUFFER_POOL_BUFFER_SIZE, 0);
        if (actualr < 0 && errno != EAGAIN)
        {
            logger(LOG_DEBUG, "Multicast receive failed: %s", strerror(errno));
            buffer_ref_put(recv_buf);
            return 0;
        }

        /* Update last data receive timestamp for timeout detection */
        ctx->last_mcast_data_time = now;
        recv_buf->data_size = (size_t)actualr;

        int result = 0;

        /* Handle multicast data based on FCC state */
        switch (ctx->fcc.state)
        {
        case FCC_STATE_MCAST_ACTIVE:
            result = fcc_handle_mcast_active(ctx, recv_buf);
            break;

        case FCC_STATE_MCAST_REQUESTED:
            result = fcc_handle_mcast_transition(ctx, recv_buf);
            break;

        default:
            /* Shouldn't receive multicast in other states */
            logger(LOG_DEBUG, "Received multicast data in unexpected state: %d", ctx->fcc.state);
            break;
        }

        /* Release our reference to the buffer */
        buffer_ref_put(recv_buf);
        return result;
    }

    /* Process RTSP socket events */
    if (ctx->rtsp.socket > 0 && fd == ctx->rtsp.socket)
    {
        /* Handle RTSP socket events (handshake and RTP data in PLAYING state) */
        int result = rtsp_handle_socket_event(&ctx->rtsp, events);
        if (result < 0)
        {
            /* -2 indicates graceful TEARDOWN completion, not an error */
            if (result == -2)
            {
                logger(LOG_DEBUG, "RTSP: Graceful TEARDOWN completed");
                return -1; /* Signal connection should be closed */
            }
            /* Real error */
            logger(LOG_ERROR, "RTSP: Socket event handling failed");
            return -1;
        }
        if (result > 0)
        {
            ctx->total_bytes_sent += (uint64_t)result;
        }
        return 0; /* Success - processed data, continue with other events */
    }

    /* Process RTSP RTP socket events (UDP mode) */
    if (ctx->rtsp.rtp_socket > 0 && fd == ctx->rtsp.rtp_socket)
    {
        int result = rtsp_handle_udp_rtp_data(&ctx->rtsp, ctx->conn);
        if (result < 0)
        {
            return -1; /* Error */
        }
        if (result > 0)
        {
            ctx->total_bytes_sent += (uint64_t)result;
        }
        return 0; /* Success - processed data, continue with other events */
    }

    /* Handle UDP RTCP socket (for future RTCP processing) */
    if (ctx->rtsp.rtcp_socket > 0 && fd == ctx->rtsp.rtcp_socket)
    {
        /* RTCP data processing could be added here in the future */
        /* For now, just consume the data to prevent buffer overflow */
        uint8_t rtcp_buffer[RTCP_BUFFER_SIZE];
        recv(ctx->rtsp.rtcp_socket, rtcp_buffer, sizeof(rtcp_buffer), 0);
        return 0;
    }

    return 0;
}

/* Initialize context for unified worker epoll (non-blocking, no own loop) */
int stream_context_init_for_worker(stream_context_t *ctx, connection_t *conn, service_t *service,
                                   int epoll_fd, int status_index, int is_snapshot)
{
    if (!ctx || !conn || !service)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn = conn;
    ctx->service = service;
    ctx->epoll_fd = epoll_fd;
    ctx->status_index = status_index;
    fcc_session_init(&ctx->fcc);
    ctx->fcc.status_index = status_index;
    rtsp_session_init(&ctx->rtsp);
    ctx->rtsp.status_index = status_index;
    ctx->rtsp.parent_ctx = ctx; /* Set parent pointer for RTSP */
    ctx->total_bytes_sent = 0;
    ctx->last_bytes_sent = 0;
    ctx->last_status_update = get_time_ms();
    ctx->last_mcast_data_time = get_time_ms();
    ctx->last_fcc_data_time = get_time_ms();
    ctx->last_mcast_rejoin_time = get_time_ms();

    /* Initialize RTP reordering buffer */
    ctx->reorder_enabled = 1; /* Enabled by default, may be disabled for RTSP TCP */
    memset(ctx->reorder_slots, 0, sizeof(ctx->reorder_slots));
    ctx->reorder_expected_seqn = 0;
    ctx->reorder_base_seqn = 0;
    ctx->reorder_first_packet = 1;
    ctx->reorder_waiting = 0;
    ctx->reorder_wait_start = 0;
    ctx->reorder_drops = 0;
    ctx->reorder_duplicates = 0;
    ctx->reorder_out_of_order = 0;
    ctx->reorder_recovered = 0;

    /* Initialize snapshot context if this is a snapshot request */
    if (is_snapshot)
    {
        if (snapshot_init(&ctx->snapshot) < 0)
        {
            logger(LOG_ERROR, "Snapshot: Failed to initialize snapshot context");
            return -1;
        }
        if (is_snapshot == 2) /* X-Request-Snapshot or Accept: image/jpeg */
        {
            ctx->snapshot.fallback_to_streaming = 1;
        }
    }

    /* Initialize media path depending on service type */
    if (service->service_type == SERVICE_RTSP)
    {
        ctx->rtsp.epoll_fd = ctx->epoll_fd;
        ctx->rtsp.conn = conn;
        if (!service->rtsp_url)
        {
            logger(LOG_ERROR, "RTSP URL not found in service configuration");
            return -1;
        }

        /* Parse URL and initiate connection */
        if (rtsp_parse_server_url(&ctx->rtsp, service->rtsp_url,
                                  service->playseek_param, service->user_agent,
                                  NULL, NULL) < 0)
        {
            logger(LOG_ERROR, "RTSP: Failed to parse URL");
            return -1;
        }

        if (rtsp_connect(&ctx->rtsp) < 0)
        {
            logger(LOG_ERROR, "RTSP: Failed to initiate connection");
            return -1;
        }

        /* Connection initiated - handshake will proceed asynchronously via event loop */
        logger(LOG_DEBUG, "RTSP: Async connection initiated, state=%d", ctx->rtsp.state);
    }
    else if (service->fcc_addr)
    {
        /* use Fast Channel Change for quick stream startup */
        if (fcc_initialize_and_request(ctx) < 0)
        {
            logger(LOG_ERROR, "FCC initialization failed");
            return -1;
        }
    }
    else
    {
        /* Direct multicast join */
        /* Note: Both /rtp/ and /udp/ endpoints now use unified packet detection */
        /* Packets are automatically detected as RTP or raw UDP at receive time */
        ctx->mcast_sock = stream_join_mcast_group(ctx);
        fcc_session_set_state(&ctx->fcc, FCC_STATE_MCAST_ACTIVE, "Direct multicast");
    }

    return 0;
}

int stream_tick(stream_context_t *ctx, int64_t now)
{
    if (!ctx)
        return 0;

    /* Periodic multicast rejoin (if enabled) */
    if (config.mcast_rejoin_interval > 0 && ctx->mcast_sock > 0)
    {
        int64_t elapsed_ms = now - ctx->last_mcast_rejoin_time;
        if (elapsed_ms >= config.mcast_rejoin_interval * 1000)
        {
            logger(LOG_DEBUG, "Multicast: Periodic rejoin (interval: %d seconds)", config.mcast_rejoin_interval);

            /* Rejoin multicast group on existing socket (LEAVE + JOIN to send IGMP Report) */
            if (rejoin_mcast_group(ctx->mcast_sock, ctx->service) == 0)
            {
                ctx->last_mcast_rejoin_time = now;
            }
            else
            {
                logger(LOG_ERROR, "Multicast: Failed to rejoin group, will retry next interval");
            }
        }
    }

    /* Check for multicast stream timeout */
    if (ctx->mcast_sock > 0)
    {
        int64_t elapsed_ms = now - ctx->last_mcast_data_time;
        if (elapsed_ms >= MCAST_TIMEOUT_SEC * 1000)
        {
            logger(LOG_ERROR, "Multicast: No data received for %d seconds, closing connection",
                   MCAST_TIMEOUT_SEC);
            return -1; /* Signal connection should be closed */
        }
    }

    /* Check for FCC timeouts */
    if (ctx->fcc.fcc_sock > 0)
    {
        int64_t elapsed_ms = now - ctx->last_fcc_data_time;
        int timeout_ms = 0;

        /* Different timeouts for different FCC states */
        if (ctx->fcc.state == FCC_STATE_REQUESTED || ctx->fcc.state == FCC_STATE_UNICAST_PENDING)
        {
            /* Signaling phase - waiting for server response */
            timeout_ms = FCC_TIMEOUT_SIGNALING_MS;

            if (elapsed_ms >= timeout_ms)
            {
                logger(LOG_WARN, "FCC: Server response timeout (%d ms), falling back to multicast",
                       FCC_TIMEOUT_SIGNALING_MS);
                if (ctx->fcc.state == FCC_STATE_REQUESTED)
                {
                    fcc_session_set_state(&ctx->fcc, FCC_STATE_MCAST_ACTIVE, "Signaling timeout");
                }
                else
                {
                    fcc_session_set_state(&ctx->fcc, FCC_STATE_MCAST_ACTIVE, "First unicast packet timeout");
                }
                ctx->mcast_sock = stream_join_mcast_group(ctx);
            }
        }
        else if (ctx->fcc.state == FCC_STATE_UNICAST_ACTIVE || ctx->fcc.state == FCC_STATE_MCAST_REQUESTED)
        {
            /* Already receiving unicast, check for stream interruption */
            timeout_ms = (int)(FCC_TIMEOUT_UNICAST_SEC * 1000);

            if (elapsed_ms >= timeout_ms)
            {
                logger(LOG_WARN, "FCC: Unicast stream interrupted (%.1f seconds), falling back to multicast",
                       FCC_TIMEOUT_UNICAST_SEC);
                fcc_session_set_state(&ctx->fcc, FCC_STATE_MCAST_ACTIVE, "Unicast interrupted");
                if (!ctx->mcast_sock)
                {
                    ctx->mcast_sock = stream_join_mcast_group(ctx);
                }
            }

            /* Check if we've been waiting too long for sync notification */
            if (ctx->fcc.state == FCC_STATE_UNICAST_ACTIVE && ctx->fcc.unicast_start_time > 0)
            {
                int64_t unicast_duration_ms = now - ctx->fcc.unicast_start_time;
                int64_t sync_wait_timeout_ms = (int64_t)(FCC_TIMEOUT_SYNC_WAIT_SEC * 1000);

                if (unicast_duration_ms >= sync_wait_timeout_ms)
                {
                    fcc_handle_sync_notification(ctx, FCC_TIMEOUT_SYNC_WAIT_SEC * 1000); /* Indicate timeout */
                }
            }
        }
    }

    /* Send periodic RTSP OPTIONS keepalive when using UDP transport */
    if (ctx->rtsp.state == RTSP_STATE_PLAYING &&
        ctx->rtsp.transport_mode == RTSP_TRANSPORT_UDP &&
        ctx->rtsp.keepalive_interval_ms > 0 &&
        ctx->rtsp.session_id[0] != '\0')
    {
        if (ctx->rtsp.last_keepalive_ms == 0)
        {
            ctx->rtsp.last_keepalive_ms = now;
        }

        int64_t keepalive_elapsed = now - ctx->rtsp.last_keepalive_ms;
        if (keepalive_elapsed >= ctx->rtsp.keepalive_interval_ms)
        {
            int ka_status = rtsp_send_keepalive(&ctx->rtsp);
            if (ka_status == 0)
            {
                ctx->rtsp.last_keepalive_ms = now;
            }
            else if (ka_status < 0)
            {
                logger(LOG_WARN, "RTSP: Failed to queue OPTIONS keepalive");
            }
        }
    }

    /* Check RTP reordering wait timeout */
    if (ctx->reorder_waiting)
    {
        int64_t wait_duration = now - ctx->reorder_wait_start;
        if (wait_duration >= RTP_REORDER_TIMEOUT_MS)
        {
            logger(LOG_DEBUG, "RTP: Reorder timeout (%lld ms), recovering",
                   (long long)wait_duration);

            /* Timeout recovery: skip missing packet and flush available packets */
            rtp_reorder_timeout_recovery(ctx);

            ctx->reorder_waiting = 0;
        }
    }

    /* Check snapshot timeout (5 seconds) */
    if (ctx->snapshot.enabled)
    {
        int64_t snapshot_elapsed = now - ctx->snapshot.start_time;
        if (snapshot_elapsed > SNAPSHOT_TIMEOUT_SEC * 1000) /* 5 seconds */
        {
            logger(LOG_WARN, "Snapshot: Timeout waiting for I-frame (%lld ms)",
                   (long long)snapshot_elapsed);
            snapshot_fallback_to_streaming(&ctx->snapshot, ctx->conn);
        }
    }

    /* Update bandwidth calculation every second (skip for snapshot mode) */
    if (!ctx->snapshot.enabled && now - ctx->last_status_update >= 1000)
    {
        /* Calculate bandwidth based on bytes sent since last update */
        uint64_t bytes_diff = ctx->total_bytes_sent - ctx->last_bytes_sent;
        int64_t elapsed_ms = now - ctx->last_status_update;
        uint32_t current_bandwidth = 0;

        if (elapsed_ms > 0)
        {
            /* Convert to bytes per second: (bytes * 1000) / elapsed_ms */
            current_bandwidth = (uint32_t)((bytes_diff * 1000) / elapsed_ms);
        }

        /* Update bytes and bandwidth in status */
        status_update_client_bytes(ctx->status_index, ctx->total_bytes_sent, current_bandwidth);

        /* Save current bytes for next calculation */
        ctx->last_bytes_sent = ctx->total_bytes_sent;
        ctx->last_status_update = now;
    }

    return 0; /* Success */
}

int stream_context_cleanup(stream_context_t *ctx)
{
    if (!ctx)
        return 0;

    /* Clean up RTP reordering buffer */
    for (int i = 0; i < RTP_REORDER_BUFFER_SIZE; i++)
    {
        if (ctx->reorder_slots[i])
        {
            buffer_ref_put(ctx->reorder_slots[i]);
            ctx->reorder_slots[i] = NULL;
        }
    }

    /* Log reordering statistics if any activity occurred */
    if (ctx->reorder_out_of_order > 0 || ctx->reorder_duplicates > 0 ||
        ctx->reorder_drops > 0 || ctx->reorder_recovered > 0)
    {
        logger(LOG_DEBUG, "RTP reorder stats: out_of_order=%llu, duplicates=%llu, "
                          "recovered=%llu, drops=%llu",
               (unsigned long long)ctx->reorder_out_of_order,
               (unsigned long long)ctx->reorder_duplicates,
               (unsigned long long)ctx->reorder_recovered,
               (unsigned long long)ctx->reorder_drops);
    }

    /* Clean up snapshot resources if in snapshot mode */
    if (ctx->snapshot.enabled)
    {
        snapshot_free(&ctx->snapshot);
    }

    /* Clean up FCC session (always safe to cleanup immediately) */
    fcc_session_cleanup(&ctx->fcc, ctx->service, ctx->epoll_fd);

    /* Clean up RTSP session - this may initiate async TEARDOWN */
    int rtsp_async = rtsp_session_cleanup(&ctx->rtsp);

    /* Close multicast socket if active (always safe to cleanup immediately) */
    if (ctx->mcast_sock)
    {
        worker_cleanup_socket_from_epoll(ctx->epoll_fd, ctx->mcast_sock);
        ctx->mcast_sock = 0;
        logger(LOG_DEBUG, "Multicast socket closed");
    }

    if (rtsp_async)
    {
        /* RTSP async TEARDOWN initiated - defer final cleanup */
        logger(LOG_DEBUG, "Stream: RTSP async TEARDOWN initiated, deferring final cleanup");
        /* Do NOT clear ctx->service - still needed for RTSP */
        return 1; /* Indicate async cleanup in progress */
    }

    /* NOTE: Do NOT free ctx->service here!
     * The service pointer is shared with the parent connection (c->service).
     * The connection owns the service and will free it in connection_free()
     * based on the c->service_owned flag.
     * Freeing it here would cause double-free when connection_free() is called.
     */
    ctx->service = NULL; /* Clear pointer but don't free */

    return 0; /* Cleanup completed */
}
