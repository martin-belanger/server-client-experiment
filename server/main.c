// SERVER
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

#define RED     "\x1b[1;31m"
#define GREEN   "\x1b[1;32m"
#define CYAN    "\x1b[1;36m"
#define NORMAL  "\x1b[0m"

static void syntax()
{
    fprintf(stderr, "Syntax:  %s PORT\n", program_invocation_short_name);
}

static volatile int stop = 0;
static void sig_handler(int signo)
{
    stop = 1;
}

static int get_listen_sock4(uint16_t port)
{
    int rc = 0;

    printf("listensock4 = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0) -> ");
    int listensock4 = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    printf("%d\n", listensock4);

    if (listensock4 < 0)
    {
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    printf("setsockopt(listensock4, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof opt) -> ");
    rc = setsockopt(listensock4, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof opt);
    printf("%d\n", rc);
    if (rc != 0)
    {
        fprintf(stderr, RED "setsockopt() failed: %m" NORMAL "\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in  address;
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    printf("bind(listensock4, (struct sockaddr *)&address, sizeof(address)) -> ");
    rc = bind(listensock4, (struct sockaddr *)&address, sizeof(address));
    printf("%d\n", rc);
    if (rc < 0)
    {
        fprintf(stderr, RED "bind() failed: %m" NORMAL "\n");
        exit(EXIT_FAILURE);
    }

    printf("listen(listensock4, 1) -> ");
    rc = listen(listensock4, 1);
    printf("%d\n", rc);
    if (rc < 0)
    {
        fprintf(stderr, "\x1b[1;31listen() failed: %m\n");
        exit(EXIT_FAILURE);
    }

    return listensock4;
}

static int get_listen_sock6(uint16_t port)
{
    int rc = 0;

    printf("listensock6 = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP) -> ");
    int listensock6 = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    printf("%d\n", listensock6);

    if (listensock6 < 0)
    {
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    printf("setsockopt(listensock6, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof opt) -> ");
    rc = setsockopt(listensock6, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof opt);
    printf("%d\n", rc);
    if (rc != 0)
    {
        fprintf(stderr, RED "setsockopt() failed: %m" NORMAL "\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in6 address;
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);

    printf("bind(listensock6, (struct sockaddr *)&address, sizeof(address)) -> ");
    rc = bind(listensock6, (struct sockaddr *)&address, sizeof(address));
    printf("%d\n", rc);
    if (rc < 0)
    {
        fprintf(stderr, RED "bind() failed: %m" NORMAL "\n");
        exit(EXIT_FAILURE);
    }

    printf("listen(listensock6, 1) -> ");
    rc = listen(listensock6, 1);
    printf("%d\n", rc);
    if (rc < 0)
    {
        fprintf(stderr, "\x1b[1;31listen() failed: %m\n");
        exit(EXIT_FAILURE);
    }

    return listensock6;
}

static int get_client_connection(uint16_t port, sigset_t sigmsk, struct sockaddr_storage * client_address)
{
    int rc = 0;

    int listensock4 = get_listen_sock4(port);
    int listensock6 = get_listen_sock6(port);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
    {
        fprintf(stderr, "\x1b[1;31Could not create the listen epoll FD list. Aborting!\n");
        exit(EXIT_FAILURE);
    }

    struct epoll_event  newPeerConnectionEvent;

    memset(&newPeerConnectionEvent, 0, sizeof newPeerConnectionEvent);
    newPeerConnectionEvent.data.fd = listensock4;
    newPeerConnectionEvent.events  = EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listensock4, &newPeerConnectionEvent) == -1)
    {
        fprintf(stderr, "\x1b[1;31Could not add listensock4 to the epoll FD list. Aborting!\n");
        exit(EXIT_FAILURE);
    }

    memset(&newPeerConnectionEvent, 0, sizeof newPeerConnectionEvent);
    newPeerConnectionEvent.data.fd = listensock6;
    newPeerConnectionEvent.events  = EPOLLOUT | EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listensock6, &newPeerConnectionEvent) == -1)
    {
        fprintf(stderr, "\x1b[1;31Could not add listensock6 to the epoll FD list. Aborting!\n");
        exit(EXIT_FAILURE);
    }

    struct epoll_event  processableEvents;
    memset(&processableEvents, 0, sizeof processableEvents);
    int numfds = epoll_pwait(epfd, &processableEvents, 1, -1, &sigmsk);
    if (numfds < 0)
    {
        if (errno == EINTR)
        {
            return -1;
        }
        fprintf(stderr, "\x1b[1;31Serious error in epoll setup: epoll_wait() returned < 0 status! %m\n");
        exit(EXIT_FAILURE);
    }

    if (numfds == 0)
    {
        fprintf(stderr, "\x1b[1;31Connection timed out!\n");
        exit(EXIT_FAILURE);
    }

    socklen_t rc_len = sizeof rc;
    if (getsockopt(processableEvents.data.fd, SOL_SOCKET, SO_ERROR, (void *)&rc, &rc_len) < 0)
    {
        fprintf(stderr, "\x1b[1;31getsockopt() failed\n");
        exit(EXIT_FAILURE);
    }

    if (rc != 0)
    {
        fprintf(stderr, "\x1b[1;31connect did not go through. rc=%d\n", rc);
        exit(EXIT_FAILURE);
    }


    socklen_t    addrlen = sizeof(*client_address);
    const char * sock_name_p = processableEvents.data.fd == listensock4 ? "listensock4" : "listensock6";

    printf("clientfd = accept4(%s, (struct sockaddr *)&client_address, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC) -> ", sock_name_p);
    int clientfd = accept4(processableEvents.data.fd, (struct sockaddr *)client_address, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    printf("%d\n", clientfd);
    if (clientfd < 0)
    {
        fprintf(stderr, "\x1b[1;31accept() failed: %m\n");
        exit(EXIT_FAILURE);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, listensock4, NULL);
    epoll_ctl(epfd, EPOLL_CTL_DEL, listensock6, NULL);
    close(listensock4);
    close(listensock6);
    close(epfd);

    return clientfd;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        syntax();
        exit(EXIT_FAILURE);
    }

    sigset_t    sigmsk;
    sigfillset(&sigmsk);
    sigdelset(&sigmsk, SIGINT); // SIGINT -> CTRL-c
    sigprocmask(SIG_SETMASK, &sigmsk, NULL);
    signal(SIGINT, sig_handler); // CTRL-c
    setvbuf(stdout, NULL, _IONBF, 0);

    int status = EXIT_SUCCESS;
    do
    {
        printf("\n-------------------------------------------------------------------------------\n");
        struct sockaddr_storage client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
        int clientfd = get_client_connection((uint16_t)atoi(argv[1]), sigmsk, &client_addr);

        if (stop) break;

        printf(GREEN "%m" NORMAL "\n");

        char buf[INET6_ADDRSTRLEN];
        void     * src  = &((struct sockaddr_in *)&client_addr)->sin_addr;
        uint16_t   port = ((struct sockaddr_in *)&client_addr)->sin_port;
        if (client_addr.ss_family == AF_INET6)
        {
            src  = &((struct sockaddr_in6 *)&client_addr)->sin6_addr;
            port = ((struct sockaddr_in6 *)&client_addr)->sin6_port;
        }
        printf("New client: " CYAN "%s" NORMAL ":%hu\n",
               inet_ntop(client_addr.ss_family, src, buf, sizeof(buf)),
               ntohs(port));

        struct epoll_event  newPeerConnectionEvent;
        memset(&newPeerConnectionEvent, 0, sizeof newPeerConnectionEvent);

        int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1)
        {
            fprintf(stderr, "\x1b[1;31Could not create the read epoll FD list. Aborting!\n");
            exit(EXIT_FAILURE);
        }

        newPeerConnectionEvent.data.fd = clientfd;
        newPeerConnectionEvent.events  = EPOLLIN | EPOLLERR | EPOLLRDHUP | EPOLLHUP;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &newPeerConnectionEvent) == -1)
        {
            fprintf(stderr, RED "Could not add the socket FD to the epoll FD list. Aborting!" NORMAL "\n");
            exit(EXIT_FAILURE);
        }

        char buffer[1024] = {0};
        while (!stop)
        {
            struct epoll_event  processableEvents;
            memset(&processableEvents, 0, sizeof processableEvents);
            int numfds = epoll_pwait(epfd, &processableEvents, 1, -1, &sigmsk);

            if (stop) break;

            if (numfds < 0)
            {
                fprintf(stderr, RED "Serious error in epoll setup: epoll_wait() returned < 0 status! %m" NORMAL "\n");
                status = EXIT_FAILURE;
                stop = 1;
                break;
            }

            if (numfds == 0)
            {
                fprintf(stderr, RED "Timeout" NORMAL "\n");
                status = EXIT_FAILURE;
                stop = 1;
                break;
            }

            printf("recv(clientfd, buffer, 1024, 0) -> ");
            int n = recv(clientfd, buffer, 1024, 0);
            printf("%d", n);
            if (n > 0)
            {
                printf(" - %.*s\n", n, buffer);
            }
            else if (n == 0)
            {
                printf(" - Connection closed by client\n");
                break;
            }
            else
            {
                printf(" - " RED "%m" NORMAL "\n");
                break;
            }
        }

        close(clientfd);
        close(epfd);

    } while (!stop);

    exit(status);
}

