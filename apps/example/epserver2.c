#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"
#include "rte_malloc.h"
#include "onvm_nflib.h"

#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 256
#define FULLNAME_LIMIT 512

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS        16
#endif

/*----------------------------------------------------------------------------*/
struct file_cache {
        char name[NAME_LIMIT];
        char fullname[FULLNAME_LIMIT];
        uint64_t size;
        char *file;
};
/*----------------------------------------------------------------------------*/
struct server_vars {
        char request[HTTP_HEADER_LEN];
        int recv_len;
        int request_len;
        long int total_read, total_sent;
        uint8_t done;
        uint8_t rspheader_sent;
        uint8_t keep_alive;

        int fidx;                        // file cache index
        char fname[NAME_LIMIT];                // file name
        long int fsize;                    // file size
};
/*----------------------------------------------------------------------------*/
struct thread_context {
        mctx_t mctx;
        int ep;
        struct server_vars *svars;
};

struct nf_files {
        struct server_vars *sv;
        struct mtcp_epoll_event *ev;
        int file_sent;
        char *file_buffer;
        char *response;
};

int keep_running_msg = 1;

/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;
static int finished;


const char *www_main;
static struct file_cache fcache[MAX_FILES];
static int nfiles;

/*----------------------------------------------------------------------------*/
static char *
StatusCodeToString(int scode) {
        switch (scode) {
                case 200:
                        return "OK";
                        break;

                case 404:
                        return "Not Found";
                        break;
        }

        return NULL;
}

/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv) {
        sv->recv_len = 0;
        sv->request_len = 0;
        sv->total_read = 0;
        sv->total_sent = 0;
        sv->done = 0;
        sv->rspheader_sent = 0;
        sv->keep_alive = 0;
}

/*----------------------------------------------------------------------------*/
void
CloseConnection(struct thread_context *ctx, int sockid, struct server_vars *sv) {
        mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, sockid, NULL);
        mtcp_close(ctx->mctx, sockid);
}
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
int
AcceptConnection(struct thread_context *ctx, int listener) {
        mctx_t mctx = ctx->mctx;
        struct server_vars *sv;
        struct mtcp_epoll_event ev;
        int c;

        c = mtcp_accept(mctx, listener, NULL, NULL);

        if (c >= 0) {
                if (c >= MAX_FLOW_NUM) {
                        TRACE_ERROR("Invalid socket id %d.\n", c);
                        return -1;
                }

                sv = &ctx->svars[c];
                CleanServerVariable(sv);
                TRACE_APP("New connection %d accepted.\n", c);
                ev.events = MTCP_EPOLLIN;
                ev.data.sockid = c;
                mtcp_setsock_nonblock(ctx->mctx, c);
                mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);
                TRACE_APP("Socket %d registered.\n", c);

        } else {
                if (errno != EAGAIN) {
                        TRACE_ERROR("mtcp_accept() error %s\n",
                                    strerror(errno));
                }
        }

        return c;
}

/*----------------------------------------------------------------------------*/
struct thread_context *
InitializeServerThread(int core) {
        struct thread_context *ctx;

/* affinitize application thread to a CPU core */
#if HT_SUPPORT
        mtcp_core_affinitize(core + (num_cores / 2));
#else
        mtcp_core_affinitize(core);
#endif /* HT_SUPPORT */

        ctx = (struct thread_context *) rte_zmalloc("ctx_vars", sizeof(struct thread_context), 0);
        if (!ctx) {
                TRACE_ERROR("Failed to create thread context!\n");
                return NULL;
        }

/* create mtcp context: this will spawn an mtcp thread */
        ctx->mctx = mtcp_create_context(core);
        if (!ctx->mctx) {
                TRACE_ERROR("Failed to create mtcp context!\n");
                free(ctx);
                return NULL;
        }

/* create epoll descriptor */
        ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
        if (ctx->ep < 0) {
                mtcp_destroy_context(ctx->mctx);
                free(ctx);
                TRACE_ERROR("Failed to create epoll descriptor!\n");
                return NULL;
        }

/* allocate memory for server variables */
        ctx->svars = (struct server_vars *) rte_zmalloc("svars_ctx", sizeof(struct server_vars) * MAX_FLOW_NUM, 0);
        if (!ctx->svars) {
                mtcp_close(ctx->mctx, ctx->ep);
                mtcp_destroy_context(ctx->mctx);
                free(ctx);
                TRACE_ERROR("Failed to create server_vars struct!\n");
                return NULL;
        }

        return ctx;
}

/*----------------------------------------------------------------------------*/
int
CreateListeningSocket(struct thread_context *ctx) {
        int listener;
        struct mtcp_epoll_event ev;
        struct sockaddr_in saddr;
        int ret;

/* create socket and set it as nonblocking */
        listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
        if (listener < 0) {
                TRACE_ERROR("Failed to create listening socket!\n");
                return -1;
        }
        ret = mtcp_setsock_nonblock(ctx->mctx, listener);
        if (ret < 0) {
                TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
                return -1;
        }

/* bind to port 80 */
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = INADDR_ANY;
        saddr.sin_port = htons(80);
        ret = mtcp_bind(ctx->mctx, listener,
                        (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
        if (ret < 0) {
                TRACE_ERROR("Failed to bind to the listening socket!\n");
                return -1;
        }

/* listen (backlog: can be configured) */
        ret = mtcp_listen(ctx->mctx, listener, backlog);
        if (ret < 0) {
                TRACE_ERROR("mtcp_listen() failed!\n");
                return -1;
        }

/* wait for incoming accept events */
        ev.events = MTCP_EPOLLIN;
        ev.data.sockid = listener;
        mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

        return listener;
}

/*----------------------------------------------------------------------------*/

static int
SendUntilAvailable(struct thread_context *ctx, int sockid, struct nf_files *nf_files) {
        int ret;
        int sent;
        int len;
        struct server_vars *sv;
        sv = nf_files->sv;

        if (sv->done || !sv->rspheader_sent) {
                return 0;
        }
        //printf("File size: %d Buffer size: %d", sv->fsize, sizeof(nf_files->file_buffer));

        sent = 0;
        ret = 1;
        while (ret > 0) {
                len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
                if (len <= 0) {
                        break;
                }
                ret = mtcp_write(ctx->mctx, sockid, nf_files->file_buffer + sv->total_sent, len);
                //printf("Ret = %d\n\n", ret);
                if (ret < 0) {
                        TRACE_APP("Connection closed with client.\n");
                        break;
                }
                TRACE_APP("Socket %d: mtcp_write try: %d, ret: %d\n", sockid, len, ret);
                sent += ret;
                sv->total_sent += ret;
        }

        if (sv->total_sent >= sv->fsize) {
                struct mtcp_epoll_event ev;
                sv->done = 1;
                finished++;

                if (sv->keep_alive) {
                        /* if keep-alive connection, wait for the incoming request */
                        ev.events = MTCP_EPOLLIN;
                        ev.data.sockid = sockid;
                        mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

                        CleanServerVariable(sv);
                } else {
                        /* else, close connection */
                        CloseConnection(ctx, sockid, sv);
                }
        }

        return sent;
}

static int
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv) {
        struct mtcp_epoll_event ev;
        char buf[HTTP_HEADER_LEN];
        char url[URL_LEN];
        char response[HTTP_HEADER_LEN];
        int scode;                        // status code
        time_t t_now;
        char t_str[128];
        char keepalive_str[128];
        int rd;
        int i;
        int len;
        int sent;

/* HTTP request handling */
        rd = mtcp_read(ctx->mctx, sockid, buf, HTTP_HEADER_LEN);
        if (rd <= 0) {
                return rd;
        }
        memcpy(sv->request + sv->recv_len, (char *) buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
        sv->recv_len += rd;
//sv->request[rd] = '\0';
//fprintf(stderr, "HTTP Request: \n%s", request);
        sv->request_len = find_http_header(sv->request, sv->recv_len);
        if (sv->request_len <= 0) {
                TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
                            "read bytes: %d, recv_len: %d, "
                            "request_len: %d, strlen: %ld, request: \n%s\n",
                            sockid, rd, sv->recv_len,
                            sv->request_len, strlen(sv->request), sv->request);
                return rd;
        }

        http_get_url(sv->request, sv->request_len, url, URL_LEN);
        TRACE_APP("Socket %d URL: %s\n", sockid, url);
        sprintf(sv->fname, "%s%s", www_main, url);
        TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);

        sv->keep_alive = FALSE;
        if (http_header_str_val(sv->request, "Connection: ",
                                strlen("Connection: "), keepalive_str, 128)) {
                if (strstr(keepalive_str, "Keep-Alive")) {
                        sv->keep_alive = TRUE;
                } else if (strstr(keepalive_str, "Close")) {
                        sv->keep_alive = FALSE;
                }
        }

/* Find file in cache */
        scode = 404;
        for (i = 0; i < nfiles; i++) {
                if (strcmp(sv->fname, fcache[i].fullname) == 0) {
                        sv->fsize = fcache[i].size;
                        sv->fidx = i; // File index from fcache
                        scode = 200;
                        break;
                }
        }
        TRACE_APP("Socket %d File size: %ld (%ldMB)\n",
                  sockid, sv->fsize, sv->fsize / 1024 / 1024);

/* Response header handling */
        time(&t_now);
        strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
        if (sv->keep_alive)
                sprintf(keepalive_str, "Keep-Alive");
        else
                sprintf(keepalive_str, "Close");

        sprintf(response, "HTTP/1.1 %d %s\r\n"
                          "Date: %s\r\n"
                          "Server: Webserver on Middlebox TCP (Ubuntu)\r\n"
                          "Content-Length: %ld\r\n"
                          "Connection: %s\r\n\r\n",
                scode, StatusCodeToString(scode), t_str, sv->fsize, keepalive_str);
        len = strlen(response);
        TRACE_APP("Socket %d HTTP Response: \n%s", sockid, response);
        sent = mtcp_write(ctx->mctx, sockid, response, len);
        TRACE_APP("Socket %d Sent response header: try: %d, sent: %d\n",
                  sockid, len, sent);
        assert(sent == len);
        sv->rspheader_sent = TRUE;

        ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
        ev.data.sockid = sockid;
        mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

        //SendUntilAvailable(ctx, sockid, sv);

        return rd;
}


void *
WriteMessages(void *ctx) {

        struct onvm_nf_msg *msg;
        struct mtcp_epoll_event ev;
        struct nf_files *nf_files;
        struct rte_ring *msg_q;
        struct onvm_nf *nf;
        struct server_vars *sv;
        int sockid, sent, len;
        struct thread_context *mtcp_ctx;
        mtcp_ctx = (struct thread_context *) ctx;

        nf = &nfs[1];
        msg_q = nf->msg_q;

        // Check and see if this NF has any messages from the manager
        while (keep_running_msg) {
                if (rte_ring_count(msg_q) > 0) {
                        msg = NULL;
                        rte_ring_dequeue(msg_q, (void **) (&msg));
                        nf_files = (struct nf_files *) msg->msg_data;
                        sockid = nf_files->ev->data.sockid;
                        sv = nf_files->sv;
                        len = strlen(nf_files->response);

                        sent = mtcp_write(mtcp_ctx->mctx, sockid, nf_files->response, len);
                        assert(sent == len);
                        sv->rspheader_sent = 1;

                        ev.events = MTCP_EPOLLOUT;
                        ev.data.sockid = sockid;
                        mtcp_epoll_ctl(mtcp_ctx->mctx, mtcp_ctx->ep, MTCP_EPOLL_CTL_MOD, sockid, &ev);

                        SendUntilAvailable(mtcp_ctx, sockid, nf_files);
                }
        }


}


void *
RunServerThread(void *arg) {
        int core = *(int *) arg;
        struct thread_context *ctx;
        mctx_t mctx;
        int listener;
        int ep;
        struct mtcp_epoll_event *events;
        int nevents;
        int i, ret, thr;
        int do_accept;
        pthread_t msg_thread;

/* initialization */
        ctx = InitializeServerThread(core); // Returns the initialized thread context struct created
        if (!ctx) {
                TRACE_ERROR("Failed to initialize server thread.\n");
                return NULL;
        }
        mctx = ctx->mctx;
        ep = ctx->ep;

        events = (struct mtcp_epoll_event *)
                rte_zmalloc("events_var", sizeof(struct mtcp_epoll_event) * MAX_EVENTS, 0);
        if (!events) {
                TRACE_ERROR("Failed to create event struct!\n");
                exit(-1);
        }

        listener = CreateListeningSocket(ctx);
        if (listener < 0) {
                TRACE_ERROR("Failed to create listening socket.\n");
                exit(-1);
        }

        if ((thr = pthread_create(&msg_thread, NULL, WriteMessages, (void*) ctx) < 0)) {
                printf("Failed to spawn main loop thread, error %d", thr);
        }
        www_main = "www";


        struct nf_files *to_nf = (struct nf_files *) rte_zmalloc("nf_files", sizeof(struct nf_files), 0);
        struct server_vars *sv;
        char buf[HTTP_HEADER_LEN];
        char url[URL_LEN];
        int rd, sockid;

        while (!done[core]) {
                nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
                if (nevents < 0) {
                        if (errno != EINTR)
                                perror("mtcp_epoll_wait");
                        break;
                }

                do_accept = FALSE;
                for (i = 0; i < nevents; i++) {
                        printf("Number of events = %d", nevents);
                        if (events[i].data.sockid == listener) {
                                /* if the event is for the listener, accept connection */
                                do_accept = TRUE;

                        }
                        else if (events[i].events & MTCP_EPOLLIN) {
                                printf("Reading event %d\n", i);
                                to_nf->sv = &ctx->svars[events[i].data.sockid];
                                to_nf->ev = &events[i];
                                sv = to_nf->sv;
                                //CleanServerVariable(sv);
                                sockid = events[i].data.sockid;

                                rd = mtcp_read(ctx->mctx, sockid, buf, HTTP_HEADER_LEN);
                                if (rd <= 0) {
                                        printf("Could not read from socket\n");
                                        break;
                                }
                                memcpy(sv->request + sv->recv_len, (char *) buf,
                                       MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
                                sv->recv_len += rd;

                                sv->request_len = find_http_header(sv->request, sv->recv_len);
                                if (sv->request_len <= 0) {
                                        TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
                                                    "read bytes: %d, recv_len: %d, "
                                                    "request_len: %d, strlen: %ld, request: \n%s\n",
                                                    sockid, rd, sv->recv_len,
                                                    sv->request_len, strlen(sv->request), sv->request);
                                        break;
                                }

                                http_get_url(sv->request, sv->request_len, url, URL_LEN);
                                TRACE_APP("Socket %d URL: %s\n", sockid, url);
                                sprintf(sv->fname, "%s%s", www_main, url);
                                TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);
                                onvm_nflib_send_msg_to_nf(2, (void *) to_nf);
                        }
                        else {
                                SendUntilAvailable(ctx, sockid, to_nf);
                        }

                }
                if (do_accept) {
                        while (1) {
                                ret = AcceptConnection(ctx, listener);
                                if (ret < 0)
                                        break;
                        }
                }
        }

/* destroy mtcp context: this will kill the mtcp thread */
        keep_running_msg = 0;
        mtcp_destroy_context(mctx);
        pthread_exit(NULL);

        return NULL;
}

/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum) {
        int i;

        for (i = 0; i < core_limit; i++) {
                if (app_thread[i] == pthread_self()) {
                        //TRACE_INFO("Server thread %d got SIGINT\n", i);
                        done[i] = TRUE;
                } else {
                        if (!done[i]) {
                                pthread_kill(app_thread[i], signum);
                        }
                }
        }
}
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv) {
        int ret;
        struct mtcp_conf mcfg; // Conf file
        int cores[MAX_CPUS];
        int process_cpu;
        int i, o;

        num_cores = GetNumCPUs();
        core_limit = num_cores;
        process_cpu = -1;

        while (-1 != (o = getopt(argc, argv, "N:f:c:b"))) {
                switch (o) {
                        case 'N':
                                core_limit = mystrtol(optarg, 10);
                                if (core_limit > num_cores) {
                                        TRACE_CONFIG("CPU limit should be smaller than the "
                                                     "number of CPUs: %d\n", num_cores);
                                        return FALSE;
                                }
                                /**
                                 * it is important that core limit is set
                                 * before mtcp_init() is called. You can
                                 * not set core_limit after mtcp_init()
                                 */
                                mtcp_getconf(&mcfg);
                                mcfg.num_cores = core_limit; // Set the core limit. Maybe just 1 for now
                                mtcp_setconf(&mcfg);
                                break;
                        case 'f':
                                conf_file = optarg;
                                break;
                        case 'c':
                                process_cpu = mystrtol(optarg, 10);
                                if (process_cpu > core_limit) {
                                        TRACE_CONFIG("Starting CPU is way off limits!\n");
                                        return FALSE;
                                }
                                break;
                        case 'b':
                                backlog = mystrtol(optarg, 10);
                                break;
                }
        }

/* initialize mtcp */
        if (conf_file == NULL) {
                TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
                exit(EXIT_FAILURE);
        }

        ret = mtcp_init(conf_file);
        if (ret) {
                TRACE_CONFIG("Failed to initialize mtcp\n");
                exit(EXIT_FAILURE);
        }

        mtcp_getconf(&mcfg);
        if (backlog > mcfg.max_concurrency) {
                TRACE_CONFIG("backlog can not be set larger than CONFIG.max_concurrency\n");
                return FALSE;
        }

/* if backlog is not specified, set it to 4K */
        if (backlog == -1) {
                backlog = 4096;
        }

/* register signal handler to mtcp */
        mtcp_register_signal(SIGINT, SignalHandler);

        TRACE_INFO("Application initialization finished.\n");

        for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
                cores[i] = i;
                done[i] = FALSE;

                if (pthread_create(&app_thread[i],
                                   NULL, RunServerThread, (void *) &cores[i])) {
                        perror("pthread_create");
                        TRACE_CONFIG("Failed to create server thread.\n");
                        exit(EXIT_FAILURE);
                }
                if (process_cpu != -1)
                        break;
        }

        for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
                pthread_join(app_thread[i], NULL);

                if (process_cpu != -1)
                        break;
        }

        mtcp_destroy();
        return 0;
}
