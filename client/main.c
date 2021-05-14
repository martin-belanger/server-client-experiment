// CLIENT
#define _GNU_SOURCE
#include <stdio.h>      /* printf() */
#include <stdlib.h>     /* atoi(), exit(), EXIT_SUCCESS */
#include <unistd.h>     /* pause() */
#include <errno.h>      /* errno, program_invocation_short_name */
#include <string.h>     /* strerror() */
#include <sys/socket.h> /* socket(), setsockopt(), connect() */
#include <arpa/inet.h>  /* htons(), inet_pton() */
#include <signal.h>     /* signal(), SIGINT */
#include <sys/epoll.h>
#include <argp.h>

#define RED     "\x1b[1;31m"
#define GREEN   "\x1b[1;32m"
#define CYAN    "\x1b[1;36m"
#define NORMAL  "\x1b[0m"


const char *argp_program_version = "1.0";
const char *argp_program_bug_address = "";
static char doc[] = "Test bind() before connect() and SO_BINDTODEVICE.";
static char args_doc[] = "DEST-ADDR PORT";
static struct argp_option options[] =
{
    { "interface",      'i', "IFACE", OPTION_ARG_OPTIONAL, "Interface passed to SO_BINDTODEVICE" },
    { "source-address", 's', "ADDR",  OPTION_ARG_OPTIONAL, "IP Address passed to bind()" },
    { 0 }
};

struct arguments
{
    uint16_t     port;
    const char * addr_p;
    char       * interface_p;
    char       * srce_addr_p;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    struct arguments * arguments = state->input;
    switch (key) {
    case 'i': arguments->interface_p = arg; break;
    case 's': arguments->srce_addr_p = arg; break;

    case ARGP_KEY_ARG:
        switch (state->arg_num)
        {
        case 0: arguments->addr_p = arg; break;
        case 1: arguments->port   = (uint16_t)atoi(arg); break;
        default:
            fprintf(stderr, RED "Too many arguments" NORMAL "\n");
            argp_usage(state);  /* Too many arguments. */
        }
        break;

    case ARGP_KEY_END:
        if (state->arg_num < 2)
        {
            fprintf(stderr, RED "Missing arguments" NORMAL "\n");
            argp_usage(state);
        }
        break;

    default: return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };


static volatile int stop = 0;
static void sig_handler(int signo)
{
    stop = 1;
}

static int connect_to_server(const char *addr_p, uint16_t port, const char *interface_p, const char *srce_addr_p)
{
    int                 rc = 0;
    struct sockaddr_in  serv_addr;

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, addr_p, &serv_addr.sin_addr) <= 0)
    {
        fprintf(stderr, RED "Invalid address %s" NORMAL "\n", addr_p);
        exit(EXIT_FAILURE);
    }

    printf("serverfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) -> ");
    int serverfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    printf("%d\n", serverfd);

    // =================================================================
    // Force interface: SO_BINDTODEVICE
    if (interface_p)
    {
        size_t len = strlen(interface_p);
        printf("setsockopt(serverfd, SOL_SOCKET, SO_BINDTODEVICE, %s, %lu) -> ", interface_p, len);
        rc = setsockopt(serverfd, SOL_SOCKET, SO_BINDTODEVICE, interface_p, len);
        printf("%s%m" NORMAL "\n", rc ? RED : GREEN);
    }

    // =================================================================
    // Set source address: bind()-before-connect()
    struct sockaddr_in addr;
    socklen_t          addrlen = sizeof(addr);
    if (srce_addr_p)
    {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(srce_addr_p);

        printf("bind(serverfd, %s, %d) -> ", srce_addr_p, addrlen);
        errno = 0;
        rc = bind(serverfd, (struct sockaddr *)&addr, addrlen);
        printf("%s%m" NORMAL "\n", rc ? RED : GREEN);
    }

    printf("connect(serverfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr) -> ");
    rc = connect(serverfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    printf("%s%m" NORMAL "\n", rc ? errno == EINPROGRESS ? "\x1b[1;93m" : RED : GREEN);

    if (rc != 0)
    {
        if (errno != EINPROGRESS)
        {
            fprintf(stderr, RED "Failed to connect" NORMAL "\n");
            exit(EXIT_FAILURE);
        }
        else
        {
            int epollfd = epoll_create1(EPOLL_CLOEXEC);
            if (epollfd == -1)
            {
                fprintf(stderr, RED "Could not create the epoll FD list. Aborting!" NORMAL "\n");
                exit(EXIT_FAILURE);
            }

            struct epoll_event  newPeerConnectionEvent;
            newPeerConnectionEvent.data.fd = serverfd;
            newPeerConnectionEvent.events  = EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP;

            if (epoll_ctl(epollfd, EPOLL_CTL_ADD, serverfd, &newPeerConnectionEvent) == -1)
            {
                fprintf(stderr, RED "Could not add the socket FD to the epoll FD list. Aborting!" NORMAL "\n");
                exit(EXIT_FAILURE);
            }

            sigset_t    sigmsk;
            sigfillset(&sigmsk);
            sigdelset(&sigmsk, SIGINT); // SIGINT -> CTRL-c
            sigprocmask(SIG_SETMASK, &sigmsk, NULL);
            static const int timeout_msec = 7000; // 7 seconds

            struct epoll_event  processableEvents;
            int numfds = epoll_pwait(epollfd, &processableEvents, 1, timeout_msec, &sigmsk);

            if (numfds < 0)
            {
                if (errno == EINTR)
                {
                    return -1;
                }
                fprintf(stderr, RED "Serious error in epoll setup: epoll_wait() returned < 0 status! %m" NORMAL "\n");
                exit(EXIT_FAILURE);
            }

            if (numfds == 0)
            {
                fprintf(stderr, RED "Connection timed out!" NORMAL "\n");
                exit(EXIT_FAILURE);
            }

            socklen_t rc_len = sizeof rc;
            if (getsockopt(serverfd, SOL_SOCKET, SO_ERROR, (void *)&rc, &rc_len) < 0)
            {
                fprintf(stderr, RED "getsockopt() failed" NORMAL "\n");
                exit(EXIT_FAILURE);
            }

            if (rc != 0)
            {
                fprintf(stderr, RED "Failed to connect. rc=%d" NORMAL "\n", rc);
                exit(EXIT_FAILURE);
            }
        }
    }

    printf("Connected to server\n");

    printf("getsockname(serverfd, (struct sockaddr *)&addr, &addrlen) -> ");
    errno = 0;
    rc = getsockname(serverfd, (struct sockaddr *)&addr, &addrlen);
    printf("%s%m" NORMAL "\n", rc != 0 ? RED : GREEN);
    printf("This sock is: " CYAN "%s" NORMAL ":%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    return serverfd;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler); // CTRL-c

    struct arguments arguments;

    arguments.port        = 0;
    arguments.addr_p      = NULL;
    arguments.interface_p = NULL;
    arguments.srce_addr_p = NULL;

    argp_parse (&argp, argc, argv, 0, 0, &arguments);

    int serverfd = connect_to_server(arguments.addr_p, arguments.port, arguments.interface_p, arguments.srce_addr_p);
    if (serverfd > 0)
    {
        while (!stop)
        {
            printf("send(serverfd, \"hello\", 5, 0) -> ");
            ssize_t l = send(serverfd, "hello", 5, 0);
            printf("%ld", l);
            if (l < 0)
            {
                printf(" - " RED "%m" NORMAL "\n");
                break;
            }

            printf("\n");
            sleep(2);
        }

        close(serverfd);
    }

    printf("\n\n");

    exit(EXIT_SUCCESS);
}
