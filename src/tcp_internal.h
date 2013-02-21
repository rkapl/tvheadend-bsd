#ifndef TCP_INTERNAL_H_
#define TCP_INTERNAL_H_

#include "tcp.h"
#include "tvheadend.h"

/**
 * @brief Will be run on separate pthread and will handle client connection.
 * @param aux tcp_server_t
 * @return NULL
 */
void* tcp_server_start(void *aux);

int tcp_create_server_socket(const char* bindaddr,int port);


/**
 * @brief TCP server structure. One for each listening socket
 */
struct tcp_server {
  tcp_server_callback_t *start;
  void *opaque;
  int serverfd;
};
/**
 * @brief TCP connection structure. One for each client;
 */
typedef struct tcp_server_launch_t {
  tcp_server_callback_t *start;
  void *opaque;
  int fd;
  struct sockaddr_storage peer;
  struct sockaddr_storage self;
} tcp_server_launch_t;

#endif
