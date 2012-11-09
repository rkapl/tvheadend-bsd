#include "tcp_internal.h"
#include "tcp.h"

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <poll.h>
#include "tvheadend.h"


#define MAX_SERVERS 10

pthread_mutex_t tcp_mutex;
int tcp_servers_used;
tcp_server_t tcp_servers[MAX_SERVERS];
/**
 * @brief Pipe for waking up the server (by writing anything) write to tcp_control_pipe[1]
 */
int tcp_control_pipe[2];
void* tcp_server_loop(void* aux);

void
tcp_server_init(void){
  pthread_t tid;
  tcp_servers_used=0;
  if(pipe(tcp_control_pipe)!=0){
      perror("pipe");
      return;
  }
  pthread_mutex_init(&tcp_mutex,NULL);
  pthread_create(&tid, NULL, tcp_server_loop, NULL);
}

tcp_server_t* tcp_server_create(int port, tcp_server_callback_t *start, void *opaque){
  struct sockaddr_in s;
  int one = 1;
  tcp_server_t* server;
  int fd,x;
  pthread_mutex_lock(&tcp_mutex);
    server=&tcp_servers[tcp_servers_used];
    tcp_servers_used++;
    server->opaque=opaque;
    server->start=start;
    fd = tvh_socket(AF_INET, SOCK_STREAM, 0);
    server->serverfd=fd;
    if(fd == -1){
      pthread_mutex_unlock(&tcp_mutex);
      return NULL;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_port = htons(port);

    x = bind(fd, (struct sockaddr *)&s, sizeof(s));
    if(x < 0) {
      close(fd);
      pthread_mutex_unlock(&tcp_mutex);
      return NULL;
    }

    listen(fd, 1);
  pthread_mutex_unlock(&tcp_mutex);

  //wakeup our server thread and force it to start listening on our new socket
  char wakeup='n';
  //printf("WAKEUP\n");
  write(tcp_control_pipe[1],&wakeup,1);
  return server;
}

void* tcp_server_loop(void *aux){
  pthread_attr_t attr;
  tcp_server_t* polled_servers[MAX_SERVERS];
  struct pollfd poll_list[MAX_SERVERS+1];
  int servers_used;
  poll_list[0].fd=tcp_control_pipe[0];
  poll_list[0].events=POLLIN;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  while(1){
      poll_list[0].revents=0;
      pthread_mutex_lock(&tcp_mutex);
      servers_used=tcp_servers_used;
      pthread_mutex_unlock(&tcp_mutex);
      int i;
      int server_number=1;
      for(i=0;i<servers_used;i++){
          if(tcp_servers[i].serverfd==0) continue;
          poll_list[server_number].fd=tcp_servers[i].serverfd;
          poll_list[server_number].events=POLLIN;
          poll_list[server_number].revents=0;
          polled_servers[server_number-1]=&tcp_servers[i];
          server_number++;
      }
      poll(poll_list,server_number,-1);
      if(poll_list[0].revents & POLLIN){
          char c;
          //the byte is just or wake-up
          read(tcp_control_pipe[0],&c,1);
          continue;
      }
      for(i=1;i<server_number;i++){
          int events=poll_list[i].revents;
          tcp_server_t* server=polled_servers[i-1];
          if(events & POLLIN){
              tcp_server_launch_t* tsl=malloc(sizeof(tcp_server_launch_t));
              socklen_t slen=sizeof(struct sockaddr_in);
              pthread_t tid;
              tsl->start = server->start;
              tsl->opaque = server->opaque;
              tsl->fd = accept(server->serverfd,(struct sockaddr *)&tsl->peer, &slen);
              if(tsl->fd == -1){
                  perror("accept");
                  free(tsl);
                  sleep(1);
                  continue;
              }
              slen = sizeof(struct sockaddr_in);
              if(getsockname(tsl->fd, (struct sockaddr *)&tsl->self, &slen)) {
                  close(tsl->fd);
                  free(tsl);
                  continue;
                }

              pthread_create(&tid, &attr, tcp_server_start, tsl);
           }else if(events!=0){
              close(server->serverfd);
              server->serverfd=0;
              continue;
           }
      }
  }
  return NULL;
}
