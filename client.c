#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <arpa/inet.h>
#include "packet_meta.h"
#include <stdbool.h>
#include <unistd.h>
#include "parse_args.h"
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/time.h>

#define WAIT_TIME ((1000000 /* microseconds in 1 second */ / 60 /* fps */) * 3 /* frames between sends */)

volatile bool stop = false;

struct client_args {
  struct server_args *sargs;
  int client_id;
  int client_count;
};

#define LATENCY_BUFSZ 20
struct client_data {
  struct timeval last_seen_tv;
  int last_seen_id;
  int ms_latency_idx;
  int ms_latency_buffer[LATENCY_BUFSZ];
  int skip_count;
  int dup_count;
  int norm_count;
  int max_ms_latency;
  int min_ms_latency;
  int sent_count;
};

int msdiff(struct timeval *ts1, struct timeval *ts2) {
  int sec_diff = ts1->tv_sec - ts2->tv_sec;
  int usec_diff = ts1->tv_usec - ts2->tv_usec;
  return (usec_diff / 1000) + (sec_diff * 1000);
}

void *client_thread(void *_args) {
  struct client_args *cargs = _args;
  struct server_args  *args = cargs->sargs;
  int sockfd;
  struct sockaddr_in servaddr;
  int port = getport(args);
  char *host = gethost(args);

  // socket create and varification
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
    printf("socket creation failed...\n");
    return NULL;
  }

  // assign IP, PORT
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = inet_addr(host);
  servaddr.sin_port = htons(port);

  // connect the client socket to server socket
  if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
    printf("connection with the server failed...\n");
    return NULL;
  }

  printf("Connected to %s on port %d\n", host, port);
#ifdef AUTH
  char auth_buf[4] = "928";
  int sent_count = 0;
  while (sent_count < sizeof(auth_buf) - 1) {
    int rv = send(sockfd, auth_buf, sizeof(auth_buf) - 1, 0);
    if (rv <= 0) {
      printf("Could not start socket\n");
      close(sockfd);
      return NULL;
    } else {
      sent_count += rv;
    }
  }
  printf("Sent auth packet (%s)\n", auth_buf);
#endif

  struct timeval last_send_tv, current_tv;
  last_send_tv.tv_sec = 0;
  last_send_tv.tv_usec = 0;
  char buf[BUFLEN];
  int my_id = cargs->client_id;
  int my_idx = 0;
  struct client_data *d = malloc(sizeof(struct client_data) * cargs->client_count);
  for (int i = 0; i < cargs->client_count; i++) {
    d[i].min_ms_latency = 9999999;
    d[i].max_ms_latency = 0;
    d[i].dup_count = 0;
    d[i].ms_latency_idx = 0;
    d[i].skip_count = 0;
    d[i].last_seen_id = -1;
    d[i].norm_count = 0;
    memset(d[i].ms_latency_buffer, 0, sizeof(d[i].ms_latency_buffer));
  }
  bool tstop;
  // fcntl(sockfd, F_SETFL, O_NONBLOCK);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 10000;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  while (!(tstop = __atomic_load_n(&stop, __ATOMIC_SEQ_CST))) {
    gettimeofday(&current_tv, NULL);
    memset(buf, 0, BUFLEN);
    if (1000000 * current_tv.tv_sec + current_tv.tv_usec > 1000000 * last_send_tv.tv_sec + last_send_tv.tv_usec + WAIT_TIME) {
      int sent_len = 0;
      int r = snprintf(buf, BUFLEN, "%d %d", my_id, my_idx);
      while (sent_len < BUFLEN) {
        int retval = send(sockfd, buf + sent_len, BUFLEN - sent_len, MSG_NOSIGNAL);
        if (retval < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            sched_yield();
          } else {
            printf("Send error %d\n", errno);
            return NULL;
          }
        } else {
          sent_len += retval;
        }
      }
      //printf("%d: Sent pack %d %d\n", my_id, my_id, my_idx);
      last_send_tv = current_tv;
      my_idx += 1;
    } else {
      int retval = recv(sockfd, buf, sizeof(buf), MSG_WAITALL);
      int recv_id, recv_idx;
      if (retval < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          sched_yield();
        } else {
          printf("Recieve error %d\n", errno);
          return NULL;
        }
      } else {
        sscanf(buf, "%d %d", &recv_id, &recv_idx);
        // printf("%d: Got pack %d %d\n", my_id, recv_id, recv_idx);
        if (d[recv_id].last_seen_id == -1) {
          d[recv_id].last_seen_id = recv_idx;
          d[recv_id].last_seen_tv = current_tv;
        } else {
          if (recv_idx == d[recv_id].last_seen_id) {
            d[recv_id].dup_count += 1;
          } else if (recv_idx > d[recv_id].last_seen_id + 1) {
            d[recv_id].skip_count += 1;
          } else {
            d[recv_id].norm_count += 1;
          }
          d[recv_id].last_seen_id = recv_idx;
          int ms_diff = msdiff(&current_tv, &d[recv_id].last_seen_tv);
          d[recv_id].ms_latency_buffer[d[recv_id].ms_latency_idx] = ms_diff;
          d[recv_id].ms_latency_idx = (d[recv_id].ms_latency_idx + 1) % LATENCY_BUFSZ;
          if (ms_diff < d[recv_id].min_ms_latency) {
            d[recv_id].min_ms_latency = ms_diff;
          }
          if (ms_diff > d[recv_id].max_ms_latency) {
            d[recv_id].max_ms_latency = ms_diff;
          }
          d[recv_id].last_seen_tv = current_tv;
        }
      }
    }
  }
  close(sockfd);
  return d;
}

int main(int argc, char *argv[]) {
             sigset_t set;
           int s;

           /* Block SIGQUIT and SIGUSR1; other threads created by main()
              will inherit a copy of the signal mask. */

           sigemptyset(&set);
           sigaddset(&set, SIGINT);
           pthread_sigmask(SIG_BLOCK, &set, NULL);
  struct server_args *args = parse_args(argc, argv);
  int thread_count = getthread_count(args);
  pthread_t pids[thread_count];
  struct client_data *cdatas[thread_count];

  printf("Launching %d threads\n", thread_count);
  struct client_args *cargs;
  for (int i = 0; i < thread_count; i++) {
    cargs = malloc(sizeof(struct client_args));
    cargs->sargs = args;
    cargs->client_id = i;
    cargs->client_count = thread_count;
    pthread_create(&pids[i], NULL, &client_thread, (void*)cargs);
  }

  printf("WAIT SIG\n");
  int sig;
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigwait(&signal_set, &sig);
  __atomic_store_n(&stop, true, __ATOMIC_SEQ_CST);
  printf("GOT SIG\n");
  for (int i = 0; i < thread_count; i++)
    pthread_join(pids[i], (void**)&cdatas[i]);
  for (int i = 0; i < thread_count; i++) {
    printf("Thread %d POV\n", i);
    if (cdatas[i] != NULL) {
      printf("Min latencies: ");
      for (int j = 0; j < thread_count; j++) {
        printf(" %d", cdatas[i][j].min_ms_latency);
      }
      printf("\n");
      printf("Max latencies: ");
      for (int j = 0; j < thread_count; j++) {
        printf(" %d", cdatas[i][j].max_ms_latency);
      }
      printf("\n");
      printf("Avj latencies: ");
      for (int j = 0; j < thread_count; j++) {
        int sum = 0;
        for (int l = 0; l < LATENCY_BUFSZ; l++) {
          sum += cdatas[i][j].ms_latency_buffer[l];
        }
        printf(" %d", sum / LATENCY_BUFSZ);
      }
      printf("\n");
      printf("Skip counts:   ");
      for (int j = 0; j < thread_count; j++) {
        printf(" %d", cdatas[i][j].skip_count);
      }
      printf("\n");
      printf("Dup counts:    ");
      for (int j = 0; j < thread_count; j++) {
        printf(" %d", cdatas[i][j].dup_count);
      }
      printf("\n");
      printf("Norm counts:   ");
      for (int j = 0; j < thread_count; j++) {
        printf(" %d", cdatas[i][j].norm_count);
      }
      printf("\n");
    }
  }
  return 0;
}
