/* build with: gcc -std=c99 -O2 -o fling fling.c */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>

static void usage(const char * restrict name, FILE * restrict f)
{
    fprintf(f, "usage: %s [options] where\n", name);
    fprintf(f, "flings data from stdin at a destination quickly over a trusted network.\n\n");
    fprintf(f, "catches data flung at it and sends it to stdout.\n");
    fprintf(f, "options:\n");
    fprintf(f, "  -v\tverbose\n");
    fprintf(f, "  -r\treceive instead of send\n");
    fprintf(f, "where:\n");
    fprintf(f, "  sending: host port");
    fprintf(f, "  receiving: host port");
    fprintf(f, "  receiving: port");
    fprintf(f, "file:\n");
    fprintf(f, "  a UNIX pipe\n");
    fprintf(f, "  a regular file\n");
    fprintf(f, "  anything else but that comes with excitement and risk\n");
}

static bool verbose = false;

#define LUMP_SIZE (1024 * 1024)

static void pretty_bytes(off64_t bytes, char * restrict buff, size_t buffz)
{
    double count = bytes;
    static const char *suffix[] = { "B", "kB", "MB", "GB", "TB", "PB", "EB" };
    int sidx = 0;
    
    while (count >= 1024 && sidx < 7) {
        count /= 1024;
        sidx++;
    }

    if (count - floor(count) == 0.0) {
        snprintf(buff, buffz, "%d %s", (int)count, suffix[sidx]);
    } else {
        snprintf(buff, buffz, "%.1f %s", count, suffix[sidx]);
    }
}

static void print_stats(FILE *f, off64_t bytes, const struct timespec * restrict start_time)
{
    struct timespec current_time = { .tv_sec = 0, .tv_nsec = 0 };
    double start, current, passed;
    char pretty_transferred[128], pretty_speed[128];

    (void) clock_gettime(CLOCK_MONOTONIC_RAW, &current_time);

    current = current_time.tv_sec + (current_time.tv_nsec * 0.000000001);
    start = start_time->tv_sec + (start_time->tv_nsec * 0.000000001);
    passed = current - start;

    pretty_bytes(bytes, pretty_transferred, sizeof pretty_transferred);
    pretty_bytes(bytes / passed, pretty_speed, sizeof pretty_speed);

    fprintf(f, "%s (%ld bytes) transferred in %f seconds, %s/sec.\n", 
        pretty_transferred, bytes, passed, pretty_speed);
}

static void maximise_pipe_length(int fd)
{
    int pipez, npipez;

    pipez = fcntl(fd, F_GETPIPE_SZ);
    if (pipez != -1 && pipez < LUMP_SIZE) {
        if (pipez < LUMP_SIZE) {
            npipez = LUMP_SIZE;
            while (fcntl(fd, F_SETPIPE_SZ, npipez) == -1 && npipez >= pipez) {
                npipez -= 4096; /* should really query page size, but meh */
            }
        }
    }
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

    if (verbose) {
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &start_time) != 0) {
            fprintf(stdout, "unable to obtain start time, statistics will be nonsense.\n");
        }
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

    if (verbose) {
        print_stats(stdout, total_written, &start_time);
    }

    return EXIT_SUCCESS;
}

static int bind_listen(const char * restrict host, const char * restrict port) 
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

    s = getaddrinfo(host, port, &hints, &result);
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

    fprintf(stderr, "listening.\n");

    freeaddrinfo(result);

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
    int srv = bind_listen(host, port);
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

    if (verbose) {
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

    close(srv);

    if (pipe(p) == -1) {
        if (verbose) {
            fprintf(stderr, "unable to create pipe, falling back to read/read\n");
        }
        state = CATCH_READWRITE;
    } else {
        maximise_pipe_length(p[0]);
    }

    do {
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
                /* erk, writing failed, abort */
                fprintf(stderr, "splicing to output failed: %s\n", strerror(errno));
                close(sock);
                return EXIT_FAILURE;
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

        case CATCH_COMPLETE:

        case CATCH_PANIC:
            fprintf(stderr, "on dear.\n");
            close(sock);
            return EXIT_FAILURE;
        }
    } while (state != CATCH_COMPLETE);

    close(sock);

    if (verbose) {
        print_stats(stderr, total_read, &start_time);
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    int opt;
    bool receiving = false;

    while ((opt = getopt(argc, argv, "hvr")) != -1) {
        switch (opt) {
            case 'h':
                usage(argv[0], stdout);
                exit(EXIT_SUCCESS);
                break;
            case 'v':
                verbose = true;
                break;
            case 'r':
                receiving = true;
                break;
            default:
                fprintf(stderr, "unknown option: %c\n", opt);
                usage(argv[0], stderr);
                exit(EXIT_FAILURE);
                break;
        }
    }  

    if (receiving == false) {
        if (argc - optind != 2) {
            fprintf(stderr, "error: host and port expected.\n");
            usage(argv[0], stderr);
            exit(EXIT_FAILURE);
        }

        exit(fling(argv[optind], argv[optind + 1], 0));
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

        exit(catch(host, port, 1));
    }
}
