/* fling transfer data from stdin over network to destination quickly
 * Copyright 2019 Codethink Ltd.
 *
 * Written by Rob Kendrick <rob.kendrick@codethink.co.uk>
 *
 * Licence: MIT <https://opensource.org/licenses/MIT>
 *
 * Build with: gcc -std=c99 -O2 -o fling fling.c
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/tcp.h>

#define FLING_PROTOCOL "fling 1.0"

static void usage(const char * restrict name, FILE * restrict f)
{
    fprintf(f, "usage: %s [options] where\n", name);
    fprintf(f, "flings data from stdin at a destination quickly over a trusted network.\n\n");
    fprintf(f, "catches data flung at it and sends it to stdout.\n");
    fprintf(f, "options:\n");
    fprintf(f, "  -v\tverbose\n");
    fprintf(f, "  -r\treceive instead of send\n");
    fprintf(f, "  -p\tperiodically print transfer progress\n");
    fprintf(f, "  -o\tspecify an output file rather than stdout\n");
    fprintf(f, "where:\n");
    fprintf(f, "  sending: host port\n");
    fprintf(f, "  sending: [user@]host:destination_file (requires ssh and fling at remote end)\n");
    fprintf(f, "  receiving: host port\n");
    fprintf(f, "  receiving: port\n");
    fprintf(f, "stdin when sending:\n");
    fprintf(f, "  a UNIX pipe\n");
    fprintf(f, "  a regular file\n");
    fprintf(f, "  anything else but that comes with excitement and risk\n");
    fprintf(f, "stdout when receiving:\n");
    fprintf(f, "  probably anything that is not a block device.\n");
}

static bool verbose = false;

typedef enum {
    PROGRESS_NONE,
    PROGRESS_YES,
    PROGRESS_PRINT,
} progress_state;

progress_state progress = PROGRESS_NONE;

static void sig_handler(int sig)
{
    if (sig == SIGALRM && progress == PROGRESS_YES) {
        progress = PROGRESS_PRINT;
    }
}

#define LUMP_SIZE (1024 * 1024)

static void pretty_bytes(off64_t bytes, char * restrict buf, size_t bufz)
{
    double t = bytes;
    static const char *suffix[] = { "B", "kiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    int sidx = 0;
    
    while (t >= 1024 && sidx < 7) {
        t /= 1024;
        sidx++;
    }

    if (t - floor(t) == 0.0) {
        snprintf(buf, bufz, "%d %s", (int)t, suffix[sidx]);
    } else {
        snprintf(buf, bufz, "%.1f %s", t, suffix[sidx]);
    }
}

static void pretty_timespec(const struct timespec * restrict time, char * restrict buf, size_t bufz)
{
    if (time->tv_sec > 0) {
        double passed = time->tv_sec + (time->tv_nsec * 0.000000001);
        snprintf(buf, bufz, "%.2f seconds", passed);
        return;
    }

    static const char *suffix[] = { "ns", "µs", "ms" };
    double t = time->tv_nsec;
    int sidx = 0;

    while (t > 1000 && sidx < 3) {
        t /= 1000;
        sidx++;
    }

    if (t - floor(t) == 0.0) {
        snprintf(buf, bufz, "%d %s", (int) t, suffix[sidx]);
    } else {
        snprintf(buf, bufz, "%0.2f %s", t, suffix[sidx]);
    }
}

static int stats(off64_t bytes, const struct timespec * restrict start_time, char * restrict buf, size_t bufz)
{
    struct timespec current_time = { .tv_sec = 0, .tv_nsec = 0 };
    struct timespec passed;
    double passed_in_sec;
    char pretty_transferred[128], pretty_speed[128], pretty_time[128];

    (void) clock_gettime(CLOCK_MONOTONIC_RAW, &current_time);

    passed.tv_sec = current_time.tv_sec - start_time->tv_sec;
    passed.tv_nsec = current_time.tv_nsec - start_time->tv_sec;
    passed_in_sec = passed.tv_sec + (passed.tv_nsec * 0.000000001);

    pretty_bytes(bytes, pretty_transferred, sizeof pretty_transferred);
    pretty_bytes(bytes / passed_in_sec, pretty_speed, sizeof pretty_speed);
    pretty_timespec(&passed, pretty_time, sizeof pretty_time);

    return snprintf(buf, bufz, "%s (%ld bytes) transferred in %s, %s/sec.", 
        pretty_transferred, bytes, pretty_time, pretty_speed);
}

static void print_stats(FILE *f, off64_t bytes, const struct timespec * restrict start_time)
{
    char buf[128];
    (void) stats(bytes, start_time, buf, sizeof buf);
    (void) fprintf(f, "%s\n", buf);
    fflush(f);
}

static void print_progress(FILE *f, off64_t bytes, const struct timespec * restrict start_time)
{
    char buf[128];
    static int prevz = 0;
    int statz = 0;

    fputc('\r', f);

    for (int i = prevz; i > 0; i--) {
        fputc(' ', f);
    }

    fputc('\r', f);

    if (start_time == NULL) {
        /* we're just removing the progress info */
        fflush(f);
        return;
    }

    statz = stats(bytes, start_time, buf, sizeof buf);
    (void) fprintf(f, "%s", buf);
    
    if (statz < prevz) {
        for (int i = prevz - statz; i > 0; i--) {
            fputc(' ', f);
        }
    }

    fflush(f);

    prevz = (prevz > statz) ? prevz : statz;
}

static void maximise_pipe_length(int fd)
{
    int pipez, npipez;
    int pagez = (int) sysconf(_SC_PAGESIZE);

    if (pagez < 1) {
        pagez = 4096;
    }

    pipez = fcntl(fd, F_GETPIPE_SZ);
    if (pipez != -1 && pipez < LUMP_SIZE) {
        if (pipez < LUMP_SIZE) {
            npipez = LUMP_SIZE;
            while (fcntl(fd, F_SETPIPE_SZ, npipez) == -1 && npipez >= pipez) {
                npipez -= pagez;
            }
        }
    }
}

static int read_number_from_file(const char *path)
{
    FILE *f = fopen(path, "r");
    int v;

    if (f == NULL) {
        return -1;
    }

    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

static void maximise_socket_buffers(int fd)
{
    int rmem_max = read_number_from_file("/proc/sys/net/core/rmem_max");
    int wmem_max = read_number_from_file("/proc/sys/net/core/wmem_max");

    if (rmem_max > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rmem_max, sizeof rmem_max);
    }

    if (wmem_max > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &wmem_max, sizeof wmem_max);
    }

    int v = 1;
    setsockopt(fd, SOL_TCP, TCP_QUICKACK, &v, sizeof v);
}

static int connect_dest(const char * restrict host, const char * restrict port) 
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;
    char ahost[256], aport[256];

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    hints.ai_protocol = 0;

#ifdef AI_IDN
    hints.ai_flags |= AI_IDN;
#endif
    
    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {

        if (verbose) {
            getnameinfo(rp->ai_addr, rp->ai_addrlen, 
                ahost, sizeof ahost,
                aport, sizeof aport,
                NI_NUMERICHOST | NI_NUMERICSERV);
            fprintf(stdout, "trying %s %s... ", ahost, aport);
            fflush(stdout);
        }

        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            if (verbose) {
                fprintf(stdout, "unable to create socket: %s\n", strerror(errno));
            }
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            if (verbose) {
                fprintf(stdout, "connected.\n");
            }
            break;
        } else {
            if (verbose) {
                fprintf(stdout, "unable to connect: %s\n", strerror(errno));
            }
        }
        close(sfd);
    }

    if (rp == NULL) {
        fprintf(stderr, "unable to connect\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);

    s = 1;
    setsockopt(sfd, IPPROTO_TCP, TCP_CORK, &s, sizeof(s));

    maximise_socket_buffers(sfd);

    return sfd;
}

typedef enum {
    FLING_PANIC,
    FLING_SPLICE,
    FLING_SENDFILE,
    FLING_READWRITE,
    FLING_COMPLETE,
} fling_state;

static int fling(const char * restrict host, const char * restrict port, int fd)
{
    int sock = connect_dest(host, port);
    
    fling_state state = FLING_PANIC;
    off64_t total_written = 0;
    int r, w;
    char buf[BUFSIZ]; /* only used for read/write mode */
    struct timespec start_time = { .tv_sec = 0, .tv_nsec = 0 };

    if (verbose || progress == PROGRESS_YES) {
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &start_time) != 0) {
            fprintf(stdout, "unable to obtain start time, statistics will be nonsense.\n");
        }
    }

    if (progress == PROGRESS_YES) {
        alarm(1);
    }

    maximise_pipe_length(fd);

    if ((w = splice(fd, NULL, sock, NULL, LUMP_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE)) == -1) {
        /* splicing not possible */
        if (verbose) {
            fprintf(stdout, "splicing not possible: %s, trying sendfile\n", strerror(errno));
        }
    } else {
        if (w != -1) {
            state = FLING_SPLICE;
            total_written = w;
        }
    }

    if (state == FLING_PANIC) {
        if ((w = sendfile(sock, fd, NULL, LUMP_SIZE)) == -1) {
            /* sendfile is not possible */
            if (verbose) {
                fprintf(stdout, "sendfile is not possible: %s, trying read/write\n", strerror(errno));
            }
        } else {
            state = FLING_SENDFILE;
            total_written = w;
        }
    }

    if (state == FLING_PANIC) {
        state = FLING_READWRITE;
        total_written = 0;
    }

    do {
        if (progress == PROGRESS_PRINT) {
            progress = PROGRESS_YES;
            print_progress(stdout, total_written, &start_time);
            alarm(1);
        }

        switch (state) {
        case FLING_SPLICE:
            w = splice(fd, NULL, sock, NULL, LUMP_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE);
            if (w == -1) {
                fprintf(stderr, "splice: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
            }

            if (w == 0) {
                /* no more to write */
                state = FLING_COMPLETE;
                continue;
            }

            total_written += w;

            /* splice next bit */
            continue;
        
        case FLING_SENDFILE:
            w = sendfile(sock, fd, NULL, LUMP_SIZE);
            if (w == -1) {
                fprintf(stderr, "sendfile: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
            }

            if (w == 0) {
                /* this isn't defined to mean no more data, so let's check */
                struct stat statbuf;
                if (fstat(0, &statbuf) == -1) {
                    /* um.  let's assume we're done */
                    state = FLING_COMPLETE;
                    continue;
                }

                if (total_written >= statbuf.st_size) {
                    state = FLING_COMPLETE;
                    continue;
                }
            }

            total_written += w;
            continue;

        case FLING_READWRITE:
            r = read(fd, buf, BUFSIZ);
            if (r == -1) {
                state = FLING_COMPLETE;
                continue;
            }
            int w = write(sock, buf, r);
            if (w == -1) {
                fprintf(stderr, "write: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
            }

            if (w != r) {
                fprintf(stderr, "write: short write to blocking socket\n");
                close(sock);
                return EXIT_FAILURE;
            }

            total_written += w;
            continue;
        
        case FLING_PANIC:
        case FLING_COMPLETE:
            continue;
        }
    } while (state != FLING_COMPLETE);

    close(sock);

    if (progress != PROGRESS_NONE) {
        print_progress(stdout, 0, NULL);
    }

    if (verbose) {
        print_stats(stdout, total_written, &start_time);
    }

    return EXIT_SUCCESS;
}

static int bind_listen(const char * restrict host, const char * restrict port, int boundport[1])
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;
    char ahost[256], aport[256];

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    s = getaddrinfo(host, port != NULL ? port : "0", &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return EXIT_FAILURE;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (verbose) {
             getnameinfo(rp->ai_addr, rp->ai_addrlen, 
                ahost, sizeof ahost,
                aport, sizeof aport,
                NI_NUMERICHOST | NI_NUMERICSERV);
            fprintf(stderr, "trying %s %s... ", ahost, aport);
            fflush(stderr);           
        }

        sfd = socket(rp->ai_family, rp->ai_socktype,
                rp->ai_protocol);
        if (sfd == -1) {
            if (verbose) {
                fprintf(stderr, "unable to create socket: %s\n", strerror(errno));
            }
            continue;
        }

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            if (listen(sfd, 16) != 0) {
                fprintf(stderr, "unable to listen: %s\n", strerror(errno));
            } else {
                break;
            }
        } else {
            if (verbose) {
                fprintf(stderr, "unable to bind: %s\n", strerror(errno));
            }
        }

        close(sfd);
    }

    if (rp == NULL) {
        fprintf(stderr, "could not bind\n");
        exit(EXIT_FAILURE);
    }

    if (verbose) {
        fprintf(stderr, "listening.\n");
    }

    freeaddrinfo(result);

    /* obtain port actually listened on */
    if (port == NULL || strcmp(port, "0") == 0) {
        struct sockaddr addr;
        socklen_t addrlen = sizeof addr;
        getsockname(sfd, &addr, &addrlen);

        switch (addr.sa_family) {
        case AF_INET:
            *boundport = ntohs(((struct sockaddr_in *)(&addr))->sin_port);
            break;
        case AF_INET6:
            *boundport = ntohs(((struct sockaddr_in6 *)(&addr))->sin6_port);
            break;
        default:
            *boundport = -1;
        }

        fprintf(stderr, "fling ephemeral port %d\n", *boundport);
        fflush(stderr);
        fclose(stderr);
    }

    return sfd;
}

typedef enum {
    CATCH_PANIC,
    CATCH_SPLICE,
    CATCH_SPLICEWRITE,
    CATCH_READWRITE,
    CATCH_COMPLETE,
} catch_state;

static int catch(const char * restrict host, const char * restrict port, int fd)
{
    int boundport;
    int srv = bind_listen(host, port, &boundport);
    int sock = accept(srv, NULL, NULL);
    int pr;

    catch_state state = CATCH_SPLICE;
    off64_t total_read = 0;
    int r, w;
    char buf[BUFSIZ]; /* only used for read/write mode */
    int p[2];
    struct pollfd pfd = {
        .fd = sock,
        .events = POLLIN | POLLHUP,
        .revents = 0,
    };

    struct timespec start_time = { .tv_sec = 0, .tv_nsec = 0 };

    if (verbose || progress == PROGRESS_YES) {
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &start_time) != 0) {
            fprintf(stdout, "unable to obtain start time, statistics will be nonsense.\n");
        }
    }

    if (sock == -1) {
        fprintf(stderr, "accept return failure: %s\n", strerror(errno));
        close(srv);
        return EXIT_FAILURE;
    }

    if (verbose) {
        fprintf(stderr, "connection accepted.\n");
    }

    maximise_socket_buffers(sock);

    close(srv);

    if (progress == PROGRESS_YES) {
        alarm(1);
    }

    if (pipe(p) == -1) {
        if (verbose) {
            fprintf(stderr, "unable to create pipe, falling back to read/read\n");
        }
        state = CATCH_READWRITE;
    } else {
        maximise_pipe_length(p[0]);
    }

    do {
        if (progress == PROGRESS_PRINT) {
            progress = PROGRESS_YES;
            print_progress(stderr, total_read, &start_time);
            alarm(1);
        }

        switch (state) {
        case CATCH_SPLICE:
            /* read data from the socket into the pipe */
            r = splice(sock, NULL, p[1], NULL, LUMP_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE);
            if (r == -1) {
                /* splicing failed, fall back to read/write */
                state = CATCH_READWRITE;
                continue;
            }

            if (r == 0) {
                /* no more input - has the remote end hung up? */
                pr = poll(&pfd, 1, 0);

                if (pr == -1) {
                    fprintf(stderr, "poll: %s\n", strerror(errno));
                    close(sock);
                    return EXIT_FAILURE;
                }

                if (pr == 0) {
                    continue;
                }

                /* check if there is data waiting */
                if (recv(sock, buf, sizeof buf, MSG_PEEK | MSG_DONTWAIT) == 0) {
                    close(p[1]);
                    state = CATCH_SPLICEWRITE;
                    continue;
                }
            }

            /* write data fro the pipe to the output */
            w = splice(p[0], NULL, fd, NULL, LUMP_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE);
            if (w == -1) {
                /* writing failed, probably a tty or similar.
                 * read the data back out of the pipe the old fasioned way,
                 * and write it out before falling back to read/write mode.
                 */
                int spliceerr = errno;
                char *fbuff = malloc(r);

                if (fbuff == NULL) {
                    fprintf(stderr, "splicing to output failed: %s\n", strerror(spliceerr));
                    fprintf(stderr, "and then allocating memory for fallback failed: %s\n", strerror(errno));
                    close(sock);
                    return EXIT_FAILURE;
                }

                int fbr = read(p[0], fbuff, r);

                if (fbr != r) {
                    fprintf(stderr, "fallback mode failed, short read from pipe.\n");
                    close(sock);
                    free(fbuff);
                    return EXIT_FAILURE;
                }

                int fbw = write(fd, fbuff, fbr);

                if (fbw != r) {
                    fprintf(stderr, "fallback mode failed, short write to output.\n");
                    close(sock);
                    free(fbuff);
                    return EXIT_FAILURE;
                }

                free(fbuff);

                state = CATCH_READWRITE;
            }

            total_read += r;

            continue;

        case CATCH_SPLICEWRITE:
            w = splice(p[0], NULL, fd, NULL, LUMP_SIZE, SPLICE_F_MOVE | SPLICE_F_MORE);
            if (w == -1) {
                /* erk, writing failed, abort */
                fprintf(stderr, "splicing to output failed: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
            }

            if (w == 0) {
                state = CATCH_COMPLETE;
            }

            continue;

        case CATCH_READWRITE:
            r = read(sock, buf, BUFSIZ);
            if (r <= 0) {
                state = CATCH_COMPLETE;
                continue;
            }

            w = write(fd, buf, r);
            if (w == -1) {
                fprintf(stderr, "write: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
            }

            if (w != r) {
                    fprintf(stderr, "write: short write to blocking file\n");
                    close(sock);
                    return EXIT_FAILURE;
            }

            total_read += r;
            continue;

        case CATCH_COMPLETE:
            break;

        case CATCH_PANIC:
            close(sock);
            return EXIT_FAILURE;
        }
    } while (state != CATCH_COMPLETE);

    close(sock);

    if (progress != PROGRESS_NONE) {
        print_progress(stderr, 0, NULL);
    }

    if (verbose) {
        print_stats(stderr, total_read, &start_time);
    }

    return EXIT_SUCCESS;
}

#define PIPER 0
#define PIPEW 1

static inline void close_pipe(int pipe[2])
{
    close(pipe[PIPER]);
    close(pipe[PIPEW]);
}

static pid_t spawn_child(const char prog[1], char *const argv[], int fds[3])
{
    int stdinpipe[2], stdoutpipe[2], stderrpipe[2], sigpipe[2];
    pid_t child;
    int oerr = 0;

    if (pipe(stdinpipe) == -1) {
        return -1;
    }

    if (pipe(stdoutpipe) == -1) {
        goto errout_stdoutpipe;
    }

    if (pipe(stderrpipe) == -1) {
        goto errout_stderrpipe;
    }

    if (pipe(sigpipe) == -1) {
        goto errout_sigpipe;
    }

    switch (child = fork()) {
    case -1:
        goto errout_fork;
    case 0:
        if (dup2(stdinpipe[PIPER], STDIN_FILENO) == -1) {
            oerr = errno;
            write(sigpipe[PIPEW], "dup2\n", 5);
            exit(oerr);
        }
        if (dup2(stdoutpipe[PIPEW], STDOUT_FILENO) == -1) {
            oerr = errno;
            write(sigpipe[PIPEW], "dup2\n", 5);
            exit(oerr);
        }
        if (dup2(stderrpipe[PIPEW], STDERR_FILENO) == -1) {
            oerr = errno;
            write(sigpipe[PIPEW], "dup2\n", 5);
            exit(oerr);
        }

        close_pipe(stdinpipe);
        close_pipe(stdoutpipe);
        close_pipe(stderrpipe);
        close(sigpipe[PIPER]);
        fcntl(sigpipe[PIPEW], F_SETFD, FD_CLOEXEC);

        (void) execvp(prog, argv);
        oerr = errno;
        write(sigpipe[PIPEW], "exec\n", 5);
        exit(oerr);

    default:
        fds[0] = stdinpipe[PIPEW];
        fds[1] = stdoutpipe[PIPER];
        fds[2] = stderrpipe[PIPER];
        close(stdinpipe[PIPER]);
        close(stdoutpipe[PIPEW]);
        close(stderrpipe[PIPEW]);
        close(sigpipe[PIPEW]);

        /* wait on the read end of the signalling pipe - it will either
         * return an error reason, or hang up on succesful exec.
         */

        char buf[BUFSIZ];
        ssize_t r = read(sigpipe[PIPER], buf, BUFSIZ);

        if (r == 0) {
            /* EOF, exec happened */
            close(sigpipe[PIPER]);
            return child;
        }

        /* there was an error, we don't do anything with the reason but
         * we return the exit code to the caller
         */
        
        int wstatus;
        (void) waitpid(child, &wstatus, 0);

        if (WIFEXITED(wstatus)) {
            errno = WEXITSTATUS(wstatus);
        }

        return -1;
    }
 
errout_fork:
    close_pipe(sigpipe);
errout_sigpipe:
    close_pipe(stderrpipe);
errout_stderrpipe:
    close_pipe(stdoutpipe);
errout_stdoutpipe:
    close_pipe(stdinpipe);
    return -1;
}

#undef PIPER
#undef PIPEW

static int prep_ssh(const char * restrict hostspec, char * restrict hostout,
    size_t hostz, char * restrict portout, size_t portz)
{
    char *speccpy = strdup(hostspec);
    char *host = NULL;
    char *user = NULL;
    char *path = NULL;
    int fds[3];
    int control, child, status, eport, controlr;
    char *sshbin = getenv("FLING_SSH") ? getenv("FLING_SSH") : "ssh";
    char *flingbin = getenv("FLING_REMOTE_EXE") ? getenv("FLING_REMOTE_EXE") : "fling";
    char *argv[16];
    unsigned int argc = 0;
    char controlbuf[BUFSIZ];

    if (speccpy == NULL) {
        return -1;
    }

    path = strchr(speccpy, ':'); /* existance of : is guarded by caller */
    *path = '\0';
    path++;

    user = strchr(speccpy, '@');
    if (user != NULL) {
        host = user + 1;
        *user = '\0';
        user = speccpy;
    } else {
        host = speccpy;
    }

    if (strlen(path) == 0) {
        fprintf(stderr, "no destination filename specified\n");
        free(speccpy);
        return -1;
    }

    strncpy(hostout, host, hostz);

#define ADD_ARG(x) do {\
    assert(argc < sizeof argv / sizeof (char *));\
    argv[argc++] = ((x)); \
    } while(0)

    /* run ssh in control socket mode first to authenticate and daemonise */

    ADD_ARG(sshbin);
    ADD_ARG("-oControlMaster=auto");
    ADD_ARG("-oControlPath=/tmp/fling.%i.%u.%C");
    ADD_ARG("-oControlPersist=5s");  
    if (user != NULL) {
        ADD_ARG("-l");
        ADD_ARG(user);
    }
    ADD_ARG(host);
    snprintf(controlbuf, sizeof controlbuf, "%s -!", flingbin);
    ADD_ARG(controlbuf);
    ADD_ARG(NULL);

    control = spawn_child(sshbin, argv, fds);
    if (control == -1) {
        fprintf(stderr, "unable to spawn control ssh: %s\n", strerror(errno));
        goto errout;
    }

    controlr = read(fds[2], controlbuf, sizeof controlbuf);
    controlbuf[controlr] = '\0';

    if (controlr < 1) {
        /* error reading or eof */
        fprintf(stderr, "unable to spawn control ssh and check remote fling version\n");
        goto errout_spawn_control;
    }

    waitpid(control, &status, 0);

    close(fds[0]);
    close(fds[1]);
    close(fds[2]);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "control ssh returned error %d: %s\n", WEXITSTATUS(status), controlbuf);
        goto errout;
    }

    if (strcmp(controlbuf, FLING_PROTOCOL) != 0) {
        fprintf(stderr, "mismatched fling protocols, remote end reports %s\n",
            controlbuf);
        goto errout;
    }

    /* We have a daemonised ssh client running, spawn a new connection through
     * it to launch remote fling
     */
    argc = 0;

    ADD_ARG(sshbin);
    ADD_ARG("-oControlPath=/tmp/fling.%i.%u.%C");
    if (user != NULL) {
        ADD_ARG("-l");
        ADD_ARG(user);
    }
    ADD_ARG(host);
    snprintf(controlbuf, sizeof controlbuf, "%s -r 0 -o '%s'", flingbin, path);
    ADD_ARG(controlbuf);
       
    ADD_ARG(NULL);

    child = spawn_child(sshbin, argv, fds);
    if (child == -1) {
        fprintf(stderr, "unable to spawn remote fling: %s\n", strerror(errno));
        goto errout_spawn_control;
    }

    controlr = read(fds[2], controlbuf, sizeof controlbuf);
    controlbuf[controlr] = '\0';

    if (controlr < 1) {
        /* error reading or eof */
        fprintf(stderr, "unable to spawn remote fling\n");
        goto errout_spawn_fling;
    }

    /* if the response already contains a newline, get rid of it */
    if (controlbuf[controlr - 1] == '\n') {
        controlbuf[controlr - 1] = '\0';
    }

    controlr = sscanf(controlbuf, "fling ephemeral port %d\n", &eport);
    if (controlr <= 0 || controlr == EOF) {
        fprintf(stderr, "unable to parse repsonse of remote fling: %s\n", controlbuf);
        goto errout_spawn_fling;
    }

    snprintf(portout, portz, "%d", eport);
    
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);

    free(speccpy);
    return child;

#undef ADD_ARG

errout_spawn_fling:
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
errout_spawn_control:
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);
    kill(control, SIGTERM);
errout:
    free(speccpy);
    return -1;
}

int main(int argc, char *argv[])
{
    int opt;
    bool receiving = false;
    const char *output = NULL;

    while ((opt = getopt(argc, argv, "hvrpo:!")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0], stdout);
            exit(EXIT_SUCCESS);
            break;
        case 'v':
            verbose = true;
            break;
        case 'p':
            progress = PROGRESS_YES;
            break;
        case 'r':
            receiving = true;
            break;
        case 'o':
            output = optarg;
            break;
        case '!':
            fprintf(stderr, "%s", FLING_PROTOCOL);
            exit(EXIT_SUCCESS);
        default:
            fprintf(stderr, "unknown option: %c\n", opt);
            usage(argv[0], stderr);
            exit(EXIT_FAILURE);
            break;
        }
    }

    signal(SIGALRM, sig_handler);

    if (receiving == false) {
        switch (argc - optind) {
        case 2:
            exit(fling(argv[optind], argv[optind + 1], 0));
        case 1:
            if (strchr(argv[optind], ':')) {
                /* establish via ssh */
                char host[128];
                char port[128];
                int pid;
                pid = prep_ssh(argv[optind], host, sizeof host, port, sizeof port);
                if (pid < 1) {
                    exit(EXIT_FAILURE);
                }

                int r = fling(host, port, 0);
                kill(pid, SIGTERM);
                waitpid(pid, NULL, 0);
                exit(r);
            }
            /* fallthrough */
        default:
            fprintf(stderr, "error: host and port expected.\n");
            usage(argv[0], stderr);
            exit(EXIT_FAILURE);
        }
    } else {
        /* receiving */
        const char *host = NULL, *port = NULL;
        switch (argc - optind) {
            case 1:
                port = argv[optind];
                break;
            case 2:
                host = argv[optind];
                port = argv[optind + 1];
                break;
            default:
                fprintf(stderr, "unparsable listening location.\n");
                usage(argv[0], stderr);
                exit(EXIT_FAILURE);
        }

        int fd;

        if (output == NULL) {
            fd = STDOUT_FILENO;
        } else {
            fd = open(output, O_CREAT | O_WRONLY);
            if (fd == -1) {
                fprintf(stderr, "unable to open %s: %s\n", output, strerror(errno));
                exit(EXIT_FAILURE);
            }
        }

        exit(catch(host, port, fd));
    }
}
