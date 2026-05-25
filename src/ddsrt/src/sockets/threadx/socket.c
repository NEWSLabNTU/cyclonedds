/*
 * Copyright(c) 2026 ZettaScale Technology and others
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "sockets_priv.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/log.h"
#include "dds/ddsrt/misc.h"

static dds_return_t errno_to_retcode(int errnum)
{
  switch (errnum) {
    case EACCES:
    case EPERM:
      return DDS_RETCODE_NOT_ALLOWED;
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
    case EALREADY:
      return DDS_RETCODE_TRY_AGAIN;
    case EBADF:
    case EFAULT:
    case EINVAL:
    case ENOTSOCK:
    case EPROTOTYPE:
      return DDS_RETCODE_BAD_PARAMETER;
    case EADDRINUSE:
    case EADDRNOTAVAIL:
      return DDS_RETCODE_PRECONDITION_NOT_MET;
    case ECONNREFUSED:
    case ECONNRESET:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    case ENETUNREACH:
      return DDS_RETCODE_NO_CONNECTION;
    case EINPROGRESS:
      return DDS_RETCODE_IN_PROGRESS;
    case EINTR:
      return DDS_RETCODE_INTERRUPTED;
    case EMSGSIZE:
      return DDS_RETCODE_NOT_ENOUGH_SPACE;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
      return DDS_RETCODE_OUT_OF_RESOURCES;
    case ETIMEDOUT:
      return DDS_RETCODE_TIMEOUT;
    case EDESTADDRREQ:
    case EISCONN:
    case ENOTCONN:
    case EOPNOTSUPP:
    case EPIPE:
      return DDS_RETCODE_ILLEGAL_OPERATION;
    default:
      return DDS_RETCODE_ERROR;
  }
}

static dds_return_t threadx_errno_to_retcode(void)
{
  return errno_to_retcode(_nxd_get_errno());
}

dds_return_t ddsrt_socket(ddsrt_socket_t *sockptr, int domain, int type, int protocol)
{
  ddsrt_socket_t sock;

  assert(sockptr != NULL);
  sock = nx_bsd_socket(domain, type, protocol);
  if (sock != DDSRT_INVALID_SOCKET) {
    *sockptr = sock;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_close(ddsrt_socket_t sock)
{
  return nx_bsd_soc_close(sock) == 0 ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_bind(ddsrt_socket_t sock, const struct sockaddr *addr, socklen_t addrlen)
{
  return nx_bsd_bind(sock, addr, (INT)addrlen) == 0 ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_listen(ddsrt_socket_t sock, int backlog)
{
  return nx_bsd_listen(sock, backlog) == 0 ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_connect(ddsrt_socket_t sock, const struct sockaddr *addr, socklen_t addrlen)
{
  return nx_bsd_connect(sock, (struct sockaddr *)addr, (INT)addrlen) == 0
    ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_accept(ddsrt_socket_t sock, struct sockaddr *addr, socklen_t *addrlen, ddsrt_socket_t *connptr)
{
  INT len = addrlen != NULL ? (INT)*addrlen : 0;
  ddsrt_socket_t conn = nx_bsd_accept(sock, addr, addrlen != NULL ? &len : NULL);

  if (conn != DDSRT_INVALID_SOCKET) {
    if (addrlen != NULL) {
      *addrlen = (socklen_t)len;
    }
    *connptr = conn;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_getsockname(ddsrt_socket_t sock, struct sockaddr *addr, socklen_t *addrlen)
{
  INT len = addrlen != NULL ? (INT)*addrlen : 0;
  INT rc = nx_bsd_getsockname(sock, addr, &len);
  if (rc == 0) {
    *addrlen = (socklen_t)len;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_getsockopt(ddsrt_socket_t sock, int32_t level, int32_t optname, void *optval, socklen_t *optlen)
{
  INT len = optlen != NULL ? (INT)*optlen : 0;
  if (level == SOL_SOCKET && (optname == SO_RCVBUF || optname == SO_SNDBUF)) {
    return DDS_RETCODE_UNSUPPORTED;
  }
  INT rc = nx_bsd_getsockopt(sock, level, optname, optval, &len);
  if (rc == 0) {
    *optlen = (socklen_t)len;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_setsockopt(ddsrt_socket_t sock, int32_t level, int32_t optname, const void *optval, socklen_t optlen)
{
  if (level == SOL_SOCKET && optname == SO_REUSEPORT) {
    return DDS_RETCODE_UNSUPPORTED;
  }
  return nx_bsd_setsockopt(sock, level, optname, optval, (INT)optlen) == 0
    ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_setsocknonblocking(ddsrt_socket_t sock, bool nonblock)
{
  INT flags = nx_bsd_fcntl(sock, F_GETFL, 0);
  if (flags < 0) {
    return threadx_errno_to_retcode();
  }
  if (nonblock) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  return nx_bsd_fcntl(sock, F_SETFL, flags) == 0 ? DDS_RETCODE_OK : threadx_errno_to_retcode();
}

dds_return_t ddsrt_setsockreuse(ddsrt_socket_t sock, bool reuse)
{
  INT opt = reuse ? 1 : 0;
  return ddsrt_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

dds_return_t ddsrt_recv(ddsrt_socket_t sock, void *buf, size_t len, int flags, ssize_t *rcvd)
{
  INT n = nx_bsd_recv(sock, buf, (INT)len, flags);
  if (n >= 0) {
    *rcvd = n;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_recvmsg(ddsrt_socket_t sock, ddsrt_msghdr_t *msg, int flags, ssize_t *rcvd)
{
  INT n = nx_bsd_recvmsg(sock, msg, flags);
  if (n >= 0) {
    *rcvd = n;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_send(ddsrt_socket_t sock, const void *buf, size_t len, int flags, ssize_t *sent)
{
  INT n = nx_bsd_send(sock, (CHAR *)buf, (INT)len, flags);
  if (n >= 0) {
    *sent = n;
    return DDS_RETCODE_OK;
  }
  return threadx_errno_to_retcode();
}

dds_return_t ddsrt_sendmsg(ddsrt_socket_t sock, const ddsrt_msghdr_t *msg, int flags, ssize_t *sent)
{
  ssize_t total = 0;
  (void) flags;

  assert(msg != NULL);

  if (msg->msg_name != NULL && msg->msg_iovlen == 1) {
    INT n = nx_bsd_sendto(sock, (CHAR *)msg->msg_iov[0].iov_base,
                          (INT)msg->msg_iov[0].iov_len, flags,
                          (struct sockaddr *)msg->msg_name,
                          (INT)msg->msg_namelen);
    if (n >= 0) {
      *sent = n;
      return DDS_RETCODE_OK;
    }
    return threadx_errno_to_retcode();
  }

  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    INT n = nx_bsd_send(sock, (CHAR *)msg->msg_iov[i].iov_base,
                        (INT)msg->msg_iov[i].iov_len, flags);
    if (n < 0) {
      return threadx_errno_to_retcode();
    }
    total += n;
    if ((size_t)n != msg->msg_iov[i].iov_len) {
      break;
    }
  }
  *sent = total;
  return DDS_RETCODE_OK;
}

dds_return_t ddsrt_select(int32_t nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, dds_duration_t reltime)
{
  INT n;
  struct timeval tv, *tvp = NULL;

  tvp = ddsrt_duration_to_timeval_ceil(reltime, &tv);
  n = nx_bsd_select(nfds, readfds, writefds, errorfds, tvp);
  if (n >= 0) {
    return n == 0 ? DDS_RETCODE_TIMEOUT : n;
  }
  return threadx_errno_to_retcode();
}
