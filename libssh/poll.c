/*
 * poll.c - poll wrapper
 *
 * This file is part of the SSH Library
 *
 * Copyright (c) 2003-2008 by Aris Adamantiadis
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 *
 * vim: ts=2 sw=2 et cindent
 */

/* This code is based on glib's gpoll */

#include "config.h"
#include "libssh/priv.h"
#include "libssh/libssh.h"

#ifndef SSH_POLL_CTX_CHUNK
#define SSH_POLL_CTX_CHUNK			5
#endif

struct ssh_poll {
  SSH_POLL_CTX *ctx;
  union {
    socket_t fd;
    size_t idx;
  };
  short events;
  ssh_poll_callback cb;
  void *cb_data;
};

struct ssh_poll_ctx {
  SSH_POLL **pollptrs;
  pollfd_t *pollfds;
  size_t polls_allocated;
  size_t polls_used;
  size_t chunk_size;
};

#ifdef HAVE_POLL
#include <poll.h>

int ssh_poll(pollfd_t *fds, nfds_t nfds, int timeout) {
  return poll((struct pollfd *) fds, nfds, timeout);
}

#else /* HAVE_POLL */
#ifdef _WIN32

#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0600)

#include <winsock2.h>

int ssh_poll(pollfd_t *fds, nfds_t nfds, int timeout) {
  return WSAPoll(fds, nfds, timeout);
}

#else /* _WIN32_WINNT */

#ifndef STRICT
#define STRICT
#endif

#include <stdio.h>
#include <windows.h>

static int poll_rest (HANDLE *handles, int nhandles,
    pollfd_t *fds, nfds_t nfds, int timeout) {
  DWORD ready;
  pollfd_t *f;
  int recursed_result;

  if (nhandles == 0) {
    /* No handles to wait for, just the timeout */
    if (timeout == INFINITE) {
      ready = WAIT_FAILED;
    } else {
      SleepEx(timeout, 1);
      ready = WAIT_TIMEOUT;
    }
  } else {
    /* Wait for just handles */
    ready = WaitForMultipleObjectsEx(nhandles, handles, FALSE, timeout, TRUE);
#if 0
    if (ready == WAIT_FAILED)  {
      fprintf(stderr, "WaitForMultipleObjectsEx failed: %d\n", GetLastError());
    }
#endif
  }

  if (ready == WAIT_FAILED) {
    return -1;
  } else if (ready == WAIT_TIMEOUT || ready == WAIT_IO_COMPLETION) {
    return 0;
  } else if (ready >= WAIT_OBJECT_0 && ready < WAIT_OBJECT_0 + nhandles) {
    for (f = fds; f < &fds[nfds]; f++) {
      if ((HANDLE) f->fd == handles[ready - WAIT_OBJECT_0]) {
        f->revents = f->events;
      }
    }

    /*
     * If no timeout and polling several handles, recurse to poll
     * the rest of them.
     */
    if (timeout == 0 && nhandles > 1) {
      /* Remove the handle that fired */
      int i;
      if (ready < nhandles - 1) {
        for (i = ready - WAIT_OBJECT_0 + 1; i < nhandles; i++) {
          handles[i-1] = handles[i];
        }
      }
      nhandles--;
      recursed_result = poll_rest(handles, nhandles, fds, nfds, 0);
      if (recursed_result < 0) {
        return -1;
      }
      return recursed_result + 1;
    }
    return 1;
  }

  return 0;
}

int ssh_poll(pollfd_t *fds, nfds_t nfds, int timeout) {
  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  pollfd_t *f;
  int nhandles = 0;
  int rc = -1;

  if (fds == NULL) {
    errno = EFAULT;
    return -1;
  }

  if (nfds >= MAXIMUM_WAIT_OBJECTS) {
    errno = EINVAL;
    return -1;
  }

  for (f = fds; f < &fds[nfds]; f++) {
    if (f->fd > 0) {
      int i;

      /*
       * Don't add the same handle several times into the array, as
       * docs say that is not allowed, even if it actually does seem
       * to work.
       */
      for (i = 0; i < nhandles; i++) {
        if (handles[i] == (HANDLE) f->fd) {
          break;
        }
      }

      if (i == nhandles) {
        if (nhandles == MAXIMUM_WAIT_OBJECTS) {
          break;
        } else {
          handles[nhandles++] = (HANDLE) f->fd;
        }
      }
    }
  }

  if (timeout == -1) {
    timeout = INFINITE;
  }

  if (nhandles > 1) {
    /*
     * First check if one or several of them are immediately
     * available.
     */
    rc = poll_rest(handles, nhandles, fds, nfds, 0);

    /*
     * If not, and we have a significant timeout, poll again with
     * timeout then. Note that this will return indication for only
     * one event, or only for messages. We ignore timeouts less than
     * ten milliseconds as they are mostly pointless on Windows, the
     * MsgWaitForMultipleObjectsEx() call will timeout right away
     * anyway.
     */
    if (rc == 0 && (timeout == INFINITE || timeout >= 10)) {
      rc = poll_rest(handles, nhandles, fds, nfds, timeout);
    }
  } else {
    /*
     * Just polling for one thing, so no need to check first if
     * available immediately
     */
    rc = poll_rest(handles, nhandles, fds, nfds, timeout);
  }

  if (rc < 0) {
    for (f = fds; f < &fds[nfds]; f++) {
      f->revents = 0;
    }
    errno = EBADF;
  }

  return rc;
}

#endif /* _WIN32_WINNT */

#endif /* _WIN32 */

#endif /* HAVE_POLL */

SSH_POLL *ssh_poll_new(socket_t fd, short events, ssh_poll_callback cb,
    void *userdata) {
  SSH_POLL *p;

  p = malloc(sizeof(SSH_POLL));
  if (p != NULL) {
    p->ctx = NULL;
    p->fd = fd;
    p->events = events;
    p->cb = cb;
    p->cb_data = userdata;
  }

  return p;
}

void ssh_poll_free(SSH_POLL *p) {
  SAFE_FREE(p);
}

SSH_POLL_CTX *ssh_poll_get_ctx(SSH_POLL *p) {
  return p->ctx;
}

short ssh_poll_get_events(SSH_POLL *p) {
  return p->events;
}

void ssh_poll_set_events(SSH_POLL *p, short events) {
  p->events = events;
  if (p->ctx != NULL) {
    p->ctx->pollfds[p->idx].events = events;
  }
}

void ssh_poll_add_events(SSH_POLL *p, short events) {
  ssh_poll_set_events(p, ssh_poll_get_events(p) | events);
}

void ssh_poll_remove_events(SSH_POLL *p, short events) {
  ssh_poll_set_events(p, ssh_poll_get_events(p) & ~events);
}

int ssh_poll_get_fd(SSH_POLL *p) {
  if (p->ctx != NULL) {
    return p->ctx->pollfds[p->idx].fd;
  }

  return p->fd;
}

void ssh_poll_set_callback(SSH_POLL *p, ssh_poll_callback cb, void *userdata) {
  if (cb != NULL) {
    p->cb = cb;
    p->cb_data = userdata;
  }
}

SSH_POLL_CTX *ssh_poll_ctx_new(size_t chunk_size) {
  SSH_POLL_CTX *ctx;

  ctx = malloc(sizeof(SSH_POLL_CTX));
  if (ctx != NULL) {
    if (!chunk_size) {
      chunk_size = SSH_POLL_CTX_CHUNK;
    }

    ctx->chunk_size = chunk_size;
    ctx->pollptrs = NULL;
    ctx->pollfds = NULL;
    ctx->polls_allocated = 0;
    ctx->polls_used = 0;
  }

  return ctx;
}

void ssh_poll_ctx_free(SSH_POLL_CTX *ctx) {
  if (ctx->polls_allocated > 0) {
    register size_t i, used;

    used = ctx->polls_used;
    for (i = 0; i < used; ) {
      SSH_POLL *p = ctx->pollptrs[i];
      int fd = ctx->pollfds[i].fd;

      /* force poll object removal */
      if (p->cb(p, fd, POLLERR, p->cb_data) < 0) {
        used = ctx->polls_used;
      } else {
        i++;
      }
    }

    SAFE_FREE(ctx->pollptrs);
    SAFE_FREE(ctx->pollfds);
  }

  SAFE_FREE(ctx);
}

static int ssh_poll_ctx_resize(SSH_POLL_CTX *ctx, size_t new_size) {
  SSH_POLL **pollptrs;
  pollfd_t *pollfds;

  pollptrs = realloc(ctx->pollptrs, sizeof(SSH_POLL *) * new_size);
  if (pollptrs == NULL) {
    return -1;
  }

  pollfds = realloc(ctx->pollfds, sizeof(pollfd_t) * new_size);
  if (pollfds == NULL) {
    ctx->pollptrs = realloc(pollptrs, sizeof(SSH_POLL *) * ctx->polls_allocated);
    return -1;
  }

  ctx->pollptrs = pollptrs;
  ctx->pollfds = pollfds;
  ctx->polls_allocated = new_size;

  return 0;
}

int ssh_poll_ctx_add(SSH_POLL_CTX *ctx, SSH_POLL *p) {
  int fd;

  if (p->ctx != NULL) {
    /* already attached to a context */
    return -1;
  }

  if (ctx->polls_used == ctx->polls_allocated &&
      ssh_poll_ctx_resize(ctx, ctx->polls_allocated + ctx->chunk_size) < 0) {
    return -1;
  }

  fd = p->fd;
  p->idx = ctx->polls_used++;
  ctx->pollptrs[p->idx] = p;
  ctx->pollfds[p->idx].fd = fd;
  ctx->pollfds[p->idx].events = p->events;
  ctx->pollfds[p->idx].revents = 0;
  p->ctx = ctx;

  return 0;
}

void ssh_poll_ctx_remove(SSH_POLL_CTX *ctx, SSH_POLL *p) {
  size_t i;

  i = p->idx;
  p->fd = ctx->pollfds[i].fd;
  p->ctx = NULL;

  ctx->polls_used--;

  /* fill the empty poll slot with the last one */
  if (ctx->polls_used > 0 && ctx->polls_used != i) {
    ctx->pollfds[i] = ctx->pollfds[ctx->polls_used];
    ctx->pollptrs[i] = ctx->pollptrs[ctx->polls_used];
  }

  /* this will always leave at least chunk_size polls allocated */
  if (ctx->polls_allocated - ctx->polls_used > ctx->chunk_size) {
    ssh_poll_ctx_resize(ctx, ctx->polls_allocated - ctx->chunk_size);
  }
}

int ssh_poll_ctx(SSH_POLL_CTX *ctx, int timeout) {
  int rc;

  if (!ctx->polls_used)
    return 0;

  rc = ssh_poll(ctx->pollfds, ctx->polls_used, timeout);
  if (rc > 0) {
    register size_t i, used;

    used = ctx->polls_used;
    for (i = 0; i < used && rc > 0; ) {
      if (!ctx->pollfds[i].revents) {
        i++;
      } else {
        SSH_POLL *p = ctx->pollptrs[i];
        int fd = ctx->pollfds[i].fd;
        int revents = ctx->pollfds[i].revents;

        if (p->cb(p, fd, revents, p->cb_data) < 0) {
          /* the poll was removed, reload the used counter and stall the loop */
          used = ctx->polls_used;
        } else {
          ctx->pollfds[i].revents = 0;
          i++;
        }

        rc--;
      }
    }
  }

  return rc;
}
