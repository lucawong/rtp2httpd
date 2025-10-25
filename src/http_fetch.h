#ifndef __HTTP_FETCH_H__
#define __HTTP_FETCH_H__

#include <stddef.h>

/* Async HTTP fetch context (opaque) */
typedef struct http_fetch_ctx_s http_fetch_ctx_t;

/**
 * Synchronously fetch content from HTTP(S) URL using curl (blocking, zero-copy)
 * This function blocks until the fetch completes or times out.
 * Returns a tmpfs file descriptor for zero-copy transmission.
 * The file is unlinked but the fd remains valid until closed.
 *
 * @param url HTTP(S) URL to fetch
 * @param out_size Pointer to receive content size (required)
 * @return File descriptor on success (caller must close), or -1 on error
 */
int http_fetch_fd_sync(const char *url, size_t *out_size);

/**
 * Synchronously fetch content from HTTP(S) URL using curl (blocking)
 * This function blocks until the fetch completes or times out.
 * Suitable for use during initialization or in contexts where blocking is acceptable.
 * Note: This internally uses http_fetch_fd_sync and reads the entire content into memory.
 *
 * @param url HTTP(S) URL to fetch
 * @param out_size Pointer to receive content size (required)
 * @return Newly allocated buffer containing content (caller must free), or NULL on error
 */
char *http_fetch_sync(const char *url, size_t *out_size);

/* Callback type for async HTTP fetch completion
 * ctx: fetch context
 * content: fetched content (caller must free), or NULL on error
 * content_size: size of fetched content in bytes (0 if content is NULL)
 * user_data: user-provided data passed to http_fetch_start_async
 */
typedef void (*http_fetch_callback_t)(http_fetch_ctx_t *ctx, char *content, size_t content_size, void *user_data);

/* Callback type for async HTTP fetch completion with file descriptor (zero-copy)
 * ctx: fetch context
 * fd: tmpfs file descriptor containing fetched content (caller must close), or -1 on error
 * content_size: size of fetched content in bytes (0 if fd is -1)
 * user_data: user-provided data passed to http_fetch_start_async_fd
 *
 * Note: The file is stored in tmpfs (/tmp) and remains open. The caller is responsible
 * for closing the file descriptor when done. The file will be unlinked before this
 * callback is called, so it will be automatically deleted when all fds are closed.
 */
typedef void (*http_fetch_fd_callback_t)(http_fetch_ctx_t *ctx, int fd, size_t content_size, void *user_data);

/**
 * Start async HTTP fetch using popen and curl
 * This function starts a non-blocking HTTP(S) fetch using curl via popen.
 * The pipe is added to the provided epoll instance for async I/O.
 *
 * @param url HTTP(S) URL to fetch
 * @param callback Function to call when fetch completes (required)
 * @param user_data User-provided data passed to callback (can be NULL)
 * @param epfd epoll file descriptor for async I/O (required)
 * @return Fetch context on success, NULL on error
 */
http_fetch_ctx_t *http_fetch_start_async(const char *url, http_fetch_callback_t callback,
                                          void *user_data, int epfd);

/**
 * Start async HTTP fetch using popen and curl (zero-copy with file descriptor)
 * This function starts a non-blocking HTTP(S) fetch using curl via popen.
 * The pipe is added to the provided epoll instance for async I/O.
 * Upon completion, a tmpfs file descriptor is passed to the callback for zero-copy transmission.
 *
 * @param url HTTP(S) URL to fetch
 * @param callback Function to call when fetch completes (required)
 * @param user_data User-provided data passed to callback (can be NULL)
 * @param epfd epoll file descriptor for async I/O (required)
 * @return Fetch context on success, NULL on error
 */
http_fetch_ctx_t *http_fetch_start_async_fd(const char *url, http_fetch_fd_callback_t callback,
                                            void *user_data, int epfd);

/**
 * Find HTTP fetch context by file descriptor
 * This is used by the epoll event loop to identify HTTP fetch events.
 *
 * @param fd File descriptor from epoll event
 * @return Fetch context if fd belongs to an HTTP fetch, NULL otherwise
 */
http_fetch_ctx_t *http_fetch_find_by_fd(int fd);

/**
 * Handle epoll event for async HTTP fetch
 * This should be called when epoll reports an event on an HTTP fetch fd.
 * The function handles reading data, detecting completion, and invoking callbacks.
 *
 * @param ctx Fetch context
 * @return 0 if more data expected, 1 if fetch completed, -1 on error
 *         Note: On completion or error, the context is automatically cleaned up
 */
int http_fetch_handle_event(http_fetch_ctx_t *ctx);

/**
 * Cancel and cleanup async HTTP fetch
 * This terminates the curl process, removes it from epoll, and frees resources.
 * The completion callback is invoked with NULL content to signal cancellation.
 *
 * @param ctx Fetch context to cancel
 */
void http_fetch_cancel(http_fetch_ctx_t *ctx);

#endif /* __HTTP_FETCH_H__ */
