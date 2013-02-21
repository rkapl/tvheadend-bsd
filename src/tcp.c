/*
 *  tvheadend, TCP common functions
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "tcp.h"
#include "tcp_internal.h"
#include "tvheadend.h"

int tcp_preferred_address_family = AF_INET;

void tcp_common_init(int opt_ipv6)
{
  if(opt_ipv6)
    tcp_preferred_address_family = AF_INET6;
}
int tcp_create_server_socket(const char* bindaddr,int port)
{
   int fd, x;
   struct addrinfo hints, *res, *ressave, *use = NULL;
   char *portBuf = (char*)malloc(6);
   int one = 1;
   int zero = 0;


   snprintf(portBuf, 6, "%d", port);

   memset(&hints, 0, sizeof(struct addrinfo));
   hints.ai_flags = AI_PASSIVE;
   if (bindaddr != NULL)
      hints.ai_flags |= AI_NUMERICHOST;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   x = getaddrinfo(bindaddr, portBuf, &hints, &res);
   free(portBuf);

   if(x != 0) {
      tvhlog(LOG_ERR, "tcp", "getaddrinfo: %s: %s", bindaddr != NULL ? bindaddr : "*",
         x == EAI_SYSTEM ? strerror(errno) : gai_strerror(x));
      return -1;
   }

   ressave = res;
   while(res)
   {
      if(res->ai_family == tcp_preferred_address_family)
      {
         use = res;
         break;
      }
      else if(use == NULL)
      {
         use = res;
      }
      res = res->ai_next;
   }

   fd = tvh_socket(use->ai_family, use->ai_socktype, use->ai_protocol);
   if(fd == -1)
      return -1;

   if(use->ai_family == AF_INET6)
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(int));

   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

   x = bind(fd, use->ai_addr, use->ai_addrlen);
   freeaddrinfo(ressave);

   if(x != 0)
   {
      tvhlog(LOG_ERR, "tcp", "bind: %s: %s", bindaddr != NULL ? bindaddr : "*", strerror(errno));
      close(fd);
      return -1;
   }

   listen(fd, 1);
   return fd;
}

/**
 *
 */
int
tcp_connect(const char *hostname, int port, char *errbuf, size_t errbufsize,
	    int timeout)
{
  const char *errtxt;
  struct hostent hostbuf, *hp;
  char *tmphstbuf;
  size_t hstbuflen;
  int herr, fd, r, res, err;
  struct sockaddr_in6 in6;
  struct sockaddr_in in;
  socklen_t errlen = sizeof(int);

  hstbuflen = 1024;
  tmphstbuf = malloc(hstbuflen);

  while((res = gethostbyname_r(hostname, &hostbuf, tmphstbuf, hstbuflen,
			       &hp, &herr)) == ERANGE) {
    hstbuflen *= 2;
    tmphstbuf = realloc(tmphstbuf, hstbuflen);
  }
  
  if(res != 0) {
    snprintf(errbuf, errbufsize, "Resolver internal error");
    free(tmphstbuf);
    return -1;
  } else if(herr != 0) {
    switch(herr) {
    case HOST_NOT_FOUND:
      errtxt = "The specified host is unknown";
      break;
    case NO_ADDRESS:
      errtxt = "The requested name is valid but does not have an IP address";
      break;
      
    case NO_RECOVERY:
      errtxt = "A non-recoverable name server error occurred";
      break;
      
    case TRY_AGAIN:
      errtxt = "A temporary error occurred on an authoritative name server";
      break;
      
    default:
      errtxt = "Unknown error";
      break;
    }

    snprintf(errbuf, errbufsize, "%s", errtxt);
    free(tmphstbuf);
    return -1;
  } else if(hp == NULL) {
    snprintf(errbuf, errbufsize, "Resolver internal error");
    free(tmphstbuf);
    return -1;
  }
  fd = tvh_socket(hp->h_addrtype, SOCK_STREAM, 0);
  if(fd == -1) {
    snprintf(errbuf, errbufsize, "Unable to create socket: %s",
	     strerror(errno));
    free(tmphstbuf);
    return -1;
  }

  /**
   * Switch to nonblocking
   */
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

  switch(hp->h_addrtype) {
  case AF_INET:
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    memcpy(&in.sin_addr, hp->h_addr_list[0], sizeof(struct in_addr));
    r = connect(fd, (struct sockaddr *)&in, sizeof(struct sockaddr_in));
    break;

  case AF_INET6:
    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(port);
    memcpy(&in6.sin6_addr, hp->h_addr_list[0], sizeof(struct in6_addr));
    r = connect(fd, (struct sockaddr *)&in, sizeof(struct sockaddr_in6));
    break;

  default:
    snprintf(errbuf, errbufsize, "Invalid protocol family");
    free(tmphstbuf);
    return -1;
  }

  free(tmphstbuf);

  if(r == -1) {
    if(errno == EINPROGRESS) {
      struct pollfd pfd;

      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;

      r = poll(&pfd, 1, timeout * 1000);
      if(r == 0) {
	/* Timeout */
	snprintf(errbuf, errbufsize, "Connection attempt timed out");
	close(fd);
	return -1;
      }
      
      if(r == -1) {
	snprintf(errbuf, errbufsize, "poll() error: %s", strerror(errno));
	close(fd);
	return -1;
      }

      getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
    } else {
      err = errno;
    }
  } else {
    err = 0;
  }

  if(err != 0) {
    snprintf(errbuf, errbufsize, "%s", strerror(err));
    close(fd);
    return -1;
  }
  
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  return fd;
}


/**
 *
 */
int
tcp_write_queue(int fd, htsbuf_queue_t *q)
{
  htsbuf_data_t *hd;
  int l, r = 0;
  void *p;

  while((hd = TAILQ_FIRST(&q->hq_q)) != NULL) {
    if (!r) {
      l = hd->hd_data_len - hd->hd_data_off;
      p = hd->hd_data + hd->hd_data_off;
      r = tvh_write(fd, p, l);
    }
    htsbuf_data_free(q, hd);
  }
  q->hq_size = 0;
  return r;
}


/**
 *
 */
static int
tcp_fill_htsbuf_from_fd(int fd, htsbuf_queue_t *hq)
{
  htsbuf_data_t *hd = TAILQ_LAST(&hq->hq_q, htsbuf_data_queue);
  int c;

  if(hd != NULL) {
    /* Fill out any previous buffer */
    c = hd->hd_data_size - hd->hd_data_len;

    if(c > 0) {

      c = read(fd, hd->hd_data + hd->hd_data_len, c);
      if(c < 1)
	return -1;

      hd->hd_data_len += c;
      hq->hq_size += c;
      return 0;
    }
  }
  
  hd = malloc(sizeof(htsbuf_data_t));
  
  hd->hd_data_size = 1000;
  hd->hd_data = malloc(hd->hd_data_size);

  c = read(fd, hd->hd_data, hd->hd_data_size);
  if(c < 1) {
    free(hd->hd_data);
    free(hd);
    return -1;
  }
  hd->hd_data_len = c;
  hd->hd_data_off = 0;
  TAILQ_INSERT_TAIL(&hq->hq_q, hd, hd_link);
  hq->hq_size += c;
  return 0;
}


/**
 *
 */
int
tcp_read_line(int fd, char *buf, const size_t bufsize, htsbuf_queue_t *spill)
{
  int len;

  while(1) {
    len = htsbuf_find(spill, 0xa);

    if(len == -1) {
      if(tcp_fill_htsbuf_from_fd(fd, spill) < 0)
	return -1;
      continue;
    }
    
    if(len >= bufsize - 1)
      return -1;

    htsbuf_read(spill, buf, len);
    buf[len] = 0;
    while(len > 0 && buf[len - 1] < 32)
      buf[--len] = 0;
    htsbuf_drop(spill, 1); /* Drop the \n */
    return 0;
  }
}



/**
 *
 */
int
tcp_read_data(int fd, char *buf, const size_t bufsize, htsbuf_queue_t *spill)
{
  int x, tot = htsbuf_read(spill, buf, bufsize);

  if(tot == bufsize)
    return 0;

  x = recv(fd, buf + tot, bufsize - tot, MSG_WAITALL);
  if(x != bufsize - tot)
    return -1;

  return 0;
}

/**
 *
 */
int
tcp_read(int fd, void *buf, size_t len)
{
  int x = recv(fd, buf, len, MSG_WAITALL);

  if(x == -1)
    return errno;
  if(x != len)
    return ECONNRESET;
  return 0;

}

/**
 *
 */
int
tcp_read_timeout(int fd, void *buf, size_t len, int timeout)
{
  int x, tot = 0;
  struct pollfd fds;

  assert(timeout > 0);

  fds.fd = fd;
  fds.events = POLLIN;
  fds.revents = 0;

  while(tot != len) {

    x = poll(&fds, 1, timeout);
    if(x == 0)
      return ETIMEDOUT;

    x = recv(fd, buf + tot, len - tot, MSG_DONTWAIT);
    if(x == -1) {
      if(errno == EAGAIN)
	continue;
      return errno;
    }

    if(x == 0)
      return ECONNRESET;

    tot += x;
  }

  return 0;

}

/**
 *
 */
char *
tcp_get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen)
{
  if(sa == NULL || s == NULL)
    return NULL;

  switch(sa->sa_family)
  {
    case AF_INET:
      inet_ntop(AF_INET, &(((struct sockaddr_in*)sa)->sin_addr), s, maxlen);
      break;
    case AF_INET6:
      inet_ntop(AF_INET6, &(((struct sockaddr_in6*)sa)->sin6_addr), s, maxlen);
      break;
    default:
      strncpy(s, "Unknown AF", maxlen);
      return NULL;
  }

  return s;
}
void* tcp_server_start(void *aux)
{
  tcp_server_launch_t *tsl = aux;
  struct timeval to;
  int val;

  val = 1;
  setsockopt(tsl->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));

#ifdef TCP_KEEPIDLE
  val = 30;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val));
#endif

#ifdef TCP_KEEPINVL
  val = 15;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val));
#endif

#ifdef TCP_KEEPCNT
  val = 5;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val));
#endif

  val = 1;
  setsockopt(tsl->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

  to.tv_sec  = 30;
  to.tv_usec =  0;
  setsockopt(tsl->fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

  tsl->start(tsl->fd, tsl->opaque, &tsl->peer, &tsl->self);
  free(tsl);
  return NULL;
}


