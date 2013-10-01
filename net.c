#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "dat.h"
#include "sd-daemon.h"

/*
 * See if we got a listen fd from systemd. If so, all socket options etc are
 * already set, so we check that the fd is a TCP or UNIX listen socket and
 * return.  The parameter determines if we want a UNIX (check_local true) or a
 * TCP (check_local false) socket from systemd.
 */
static int
try_systemd_socket(char check_local)
{
    int r, last, fd;

    r = sd_listen_fds(0);
    if (r < 0) {
        return twarn("getaddrinfo()"), -1;
    } else if (r == 0) {
        return 0;
    }

    if (r > 2) {
        twarnx("inherited mor than one listen socket;"
               " ignoring all but the first two");
        r = 2;
    }
    last = SD_LISTEN_FDS_START + r;
    for (fd = SD_LISTEN_FDS_START; fd < last; fd++) {
        if (check_local) {
            r = sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0);
        } else {
            r = sd_is_socket_inet(fd, 0, SOCK_STREAM, 1, 0);
        }
        if (r < 0) {
            errno = -r;
            twarn("sd_is_socket_%s", check_local ? "unix" : "inet");
        }
        if (r) {
            return fd;
        }
    }
    twarnx("none of the inherited fds is usable");
    return -1;
}

static int
set_nonblocking(int fd)
{
    int flags, r;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        twarn("getting flags");
        return -1;
    }
    r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (r == -1) {
        twarn("setting O_NONBLOCK");
        return -1;
    }
    return 0;
}

int
make_local_socket(char *path)
{
    int fd = -1, r;
    struct stat st;
    struct sockaddr_un addr;

    fd = try_systemd_socket(1);
    if (fd) {
        return fd;
    }

    r = stat(path, &st);
    if (r == 0) {
        if (S_ISSOCK(st.st_mode)) {
            warnx("removing existing local socket to replace it");
            r = unlink(path);
            if (r == -1) {
                twarn("unlink");
                return -1;
            }
        } else {
            warnx("another file already exists in the given path");
            return -1;
        }
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        twarn("socket()");
        return -1;
    }

    r = set_nonblocking(fd);
    if (r == -1) {
        close(fd);
        return -1;
    }

    if (verbose) {
        printf("bind %s\n", path);
    }
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);  // length is safe, checked earlier
    r = bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
    if (r == -1) {
        twarn("bind()");
        close(fd);
        return -1;
    }

    r = listen(fd, 1024);
    if (r == -1) {
        twarn("listen()");
        close(fd);
        return -1;
    }

    return fd;
}

int
make_inet_socket(char *host, char *port)
{
    int fd = -1, flags, r;
    struct linger linger = {0, 0};
    struct addrinfo *airoot, *ai, hints;

    /* See if we got a listen fd from systemd. If so, all socket options etc
     * are already set, so we check that the fd is a TCP listen socket and
     * return. */
    r = sd_listen_fds(1);
    if (r < 0) {
        return twarn("sd_listen_fds"), -1;
    }
    if (r > 0) {
        if (r > 1) {
            twarnx("inherited more than one listen socket;"
                   " ignoring all but the first");
        }
        fd = SD_LISTEN_FDS_START;
        r = sd_is_socket_inet(fd, 0, SOCK_STREAM, 1, 0);
        if (r < 0) {
            errno = -r;
            twarn("sd_is_socket_inet");
            return -1;
        }
        if (!r) {
            twarnx("inherited fd is not a TCP listen socket");
            return -1;
        }
        return fd;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    r = getaddrinfo(host, port, &hints, &airoot);
    if (r == -1)
      return twarn("getaddrinfo()"), -1;

    for(ai = airoot; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd == -1) {
        twarn("socket()");
        continue;
      }

      flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        twarn("getting flags");
        close(fd);
        continue;
      }

      r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      if (r == -1) {
        twarn("setting O_NONBLOCK");
        close(fd);
        continue;
      }

      flags = 1;
      r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting SO_REUSEADDR on fd %d", fd);
        close(fd);
        continue;
      }
      r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting SO_KEEPALIVE on fd %d", fd);
        close(fd);
        continue;
      }
      r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof linger);
      if (r == -1) {
        twarn("setting SO_LINGER on fd %d", fd);
        close(fd);
        continue;
      }
      r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting TCP_NODELAY on fd %d", fd);
        close(fd);
        continue;
      }

      if (verbose) {
          char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV], *h = host, *p = port;
          r = getnameinfo(ai->ai_addr, ai->ai_addrlen,
                  hbuf, sizeof hbuf,
                  pbuf, sizeof pbuf,
                  NI_NUMERICHOST|NI_NUMERICSERV);
          if (!r) {
              h = hbuf;
              p = pbuf;
          }
          if (ai->ai_family == AF_INET6) {
              printf("bind %d [%s]:%s\n", fd, h, p);
          } else {
              printf("bind %d %s:%s\n", fd, h, p);
          }
      }
      r = bind(fd, ai->ai_addr, ai->ai_addrlen);
      if (r == -1) {
        twarn("bind()");
        close(fd);
        continue;
      }

      r = listen(fd, 1024);
      if (r == -1) {
        twarn("listen()");
        close(fd);
        continue;
      }

      break;
    }

    freeaddrinfo(airoot);

    if(ai == NULL)
      fd = -1;

    return fd;
}
