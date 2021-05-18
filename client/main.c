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
#include <netdb.h>
#include <net/if.h>

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
    const char *addr_p;
    char       *interface_p;
    char       *srce_addr_p;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;
    switch (key)
    {
    case 'i':
        arguments->interface_p = arg; break;
    case 's':
        arguments->srce_addr_p = arg; break;

    case ARGP_KEY_ARG:
        switch (state->arg_num)
        {
        case 0:
            arguments->addr_p = arg; break;
        case 1:
            arguments->port   = (uint16_t)atoi(arg); break;
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

    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };


static volatile int stop = 0;
static void sig_handler(int signo)
{
    stop = 1;
}

static int inet4_pton(const char *src, uint16_t port, struct sockaddr_storage *addr)
{
    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;

    if (strlen(src) > INET_ADDRSTRLEN) return -EINVAL;

    if (inet_pton(AF_INET, src, &addr4->sin_addr.s_addr) <= 0) return -EINVAL;

    addr4->sin_family = AF_INET;
    addr4->sin_port   = htons(port);

    return 0;
}

static int inet6_pton(const char *src, uint16_t port, struct sockaddr_storage *addr)
{
    int                  ret   = -EINVAL;
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;

    if (strlen(src) > INET6_ADDRSTRLEN) return -EINVAL;

    char  *tmp = strdup(src);
    if (!tmp) fprintf(stderr, RED "cannot copy: %s" NORMAL "\n", src);

    const char *scope = NULL;
    char *p = strchr(tmp, SCOPE_DELIMITER);
    if (p)
    {
        *p = '\0';
        scope = src + (p - tmp) + 1;
    }

    if (inet_pton(AF_INET6, tmp, &addr6->sin6_addr) != 1)
    {
        fprintf(stderr, RED "invalid source address: %s" NORMAL "\n", src);
        goto free_tmp;
    }

    if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr) && scope)
    {
        addr6->sin6_scope_id = if_nametoindex(scope);
        if (addr6->sin6_scope_id == 0)
        {
            fprintf(stderr, RED "can't find iface index for: %s (%m)" NORMAL "\n", scope);
            goto free_tmp;
        }
    }

    addr6->sin6_family = AF_INET6;
    addr6->sin6_port   = htons(port);
    ret = 0;

free_tmp:
    free(tmp);
    return ret;
}

/**
 * inet_pton_with_scope - convert an IPv4/IPv6 to socket address
 * @af: address family, AF_INET, AF_INET6 or AF_UNSPEC for either
 * @src: the start of the address string
 * @addr: output socket address
 *
 * Return 0 on success, errno otherwise.
 */
static int inet_pton_with_scope(int af, const char *src,
                                uint16_t port, struct sockaddr_storage *addr)
{
    int rc = -EINVAL;

    memset(addr, 0, sizeof(*addr));
    switch (af)
    {
    case AF_INET:   return inet4_pton(src, port, addr);
    case AF_INET6:  return inet6_pton(src, port, addr);
    case AF_UNSPEC:
        rc = inet4_pton(src, port, addr);
        if (rc != 0)
        {
            memset(addr, 0, sizeof(*addr));
            rc = inet6_pton(src, port, addr);
        }

        break;
    default:
        fprintf(stderr, RED "unexpected address family %d" NORMAL "\n", af);
    }

    return rc;
}

static int connect_to_server(const char *addr_p, uint16_t port, const char *interface_p, const char *srce_addr_p)
{
    int                     rc = 0;
    struct sockaddr_storage serv_addr;
    rc = inet_pton_with_scope(AF_UNSPEC, addr_p, port, &serv_addr);
    if (rc != 0)
    {
        fprintf(stderr, RED "Invalid address %s" NORMAL "\n", addr_p);
        exit(EXIT_FAILURE);
    }

    printf("serverfd = socket(AF_INET%s, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) -> ", serv_addr.ss_family == AF_INET ? "" : "6");
    int serverfd = socket(serv_addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    if (srce_addr_p)
    {
        struct sockaddr_storage  addr;
        rc = inet_pton_with_scope(AF_UNSPEC, srce_addr_p, 0, &addr);
        if (rc != 0)
        {
            fprintf(stderr, RED "Invalid source address %s" NORMAL "\n", srce_addr_p);
            exit(EXIT_FAILURE);
        }

        socklen_t addrlen = addr.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                                      : sizeof(struct sockaddr_in6);

        printf("bind(serverfd, %s, %d) -> ", srce_addr_p, addrlen);
        errno = 0;
        rc = bind(serverfd, (struct sockaddr *)&addr, addrlen);
        printf("%s%m" NORMAL "\n", rc ? RED : GREEN);
    }

    printf("connect(serverfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr) -> ");
    rc = connect(serverfd, (struct sockaddr *)&serv_addr, serv_addr.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
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

    struct sockaddr_storage client_addr;
    socklen_t               addrlen = sizeof(client_addr);

    printf("getsockname(serverfd, (struct sockaddr *)&client_address, &addrlen) -> ");
    errno = 0;
    rc = getsockname(serverfd, (struct sockaddr *)&client_addr, &addrlen);
    printf("%s%m" NORMAL "\n", rc != 0 ? RED : GREEN);

    char       buf[INET6_ADDRSTRLEN];
    void      *client_addr_p = &((struct sockaddr_in *)&client_addr)->sin_addr;
    uint16_t   client_port   = ((struct sockaddr_in *)&client_addr)->sin_port;
    if (client_addr.ss_family == AF_INET6)
    {
        client_addr_p  = &((struct sockaddr_in6 *)&client_addr)->sin6_addr;
        client_port = ((struct sockaddr_in6 *)&client_addr)->sin6_port;
    }
    printf("This sock is: " CYAN "%s" NORMAL ":%d\n",
           inet_ntop(client_addr.ss_family, client_addr_p, buf, sizeof(buf)),
           ntohs(client_port));

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

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int serverfd = connect_to_server(arguments.addr_p, arguments.port,
                                     arguments.interface_p, arguments.srce_addr_p);
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
