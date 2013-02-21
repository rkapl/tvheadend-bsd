/*
 *  tvheadend, TCP common functions
 *  Copyright (C) 2007 Andreas man
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

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include "tvheadend.h"
#include "tcp.h"
#include "tcp_internal.h"

static int tcp_server_epoll_fd;

/**
 * @brief TCP listening loop that runs in the thread started by tcp_server_init
 */
static void* tcp_server_loop(void *aux);

void
tcp_server_init(void)
{
  pthread_t tid;

  tcp_server_epoll_fd = epoll_create(10);
  pthread_create(&tid, NULL, tcp_server_loop, NULL);
  
}

void *
tcp_server_loop(void *aux)
{
  int r, i;
  struct epoll_event ev[1];
  tcp_server_t *ts;
  tcp_server_launch_t *tsl;
  pthread_attr_t attr;
  pthread_t tid;
  socklen_t slen;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  while(1) {
      r = epoll_wait(tcp_server_epoll_fd, ev, sizeof(ev) / sizeof(ev[0]), -1);
      if(r == -1) {
          perror("tcp_server: epoll_wait");
          continue;
        }

      for(i = 0; i < r; i++) {
          ts = ev[i].data.ptr;

          if(ev[i].events & EPOLLHUP) {
              close(ts->serverfd);
              free(ts);
              continue;
            }

          if(ev[i].events & EPOLLIN) {
              tsl = malloc(sizeof(tcp_server_launch_t));
              tsl->start  = ts->start;
              tsl->opaque = ts->opaque;
              slen = sizeof(struct sockaddr_storage);

              tsl->fd = accept(ts->serverfd,
                               (struct sockaddr *)&tsl->peer, &slen);
              if(tsl->fd == -1) {
                  perror("accept");
                  free(tsl);
                  sleep(1);
                  continue;
                }


              slen = sizeof(struct sockaddr_storage);
              if(getsockname(tsl->fd, (struct sockaddr *)&tsl->self, &slen)) {
                  close(tsl->fd);
                  free(tsl);
                  continue;
                }

              pthread_create(&tid, &attr, tcp_server_start, tsl);
            }
        }
    }
  return NULL;
}
tcp_server_t* tcp_server_create(const char* bindaddr,int port, tcp_server_callback_t *start, void *opaque)
{
  struct epoll_event e;
  tcp_server_t *ts;
  int fd;

  memset(&e, 0, sizeof(e));

  fd=tcp_create_server_socket(bindaddr,port);
  ts = malloc(sizeof(tcp_server_t));
  ts->serverfd = fd;
  ts->start = start;
  ts->opaque = opaque;


  e.events = EPOLLIN;
  e.data.ptr = ts;

  epoll_ctl(tcp_server_epoll_fd, EPOLL_CTL_ADD, fd, &e);
  return ts;
}


