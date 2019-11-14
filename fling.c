/* build with: gcc -std=c99 -O2 -o fling fling.c */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netdb.h>

static void usage(const char * restrict name, FILE * restrict f)
{
    fprintf(f, "usage: %s [options] where < input\n", name);
    fprintf(f, "flings data piped to it at a destination quickly over a trusted network.\n\n");
    fprintf(f, "options:\n");
    fprintf(f, "  -v\tverbose\n");
    fprintf(f, "where:\n");
    fprintf(f, "  host port");
    fprintf(f, "file:\n");
    fprintf(f, "  a UNIX pipe\n");
    fprintf(f, "  a regular file\n");
    fprintf(f, "  anything else but that comes with excitement and risk\n");
}

static bool verbose = false;

#define LUMP_SIZE (1024 * 1024)

typedef enum {
    FLING_PANIC,
    FLING_SPLICE,
    FLING_SENDFILE,
    FLING_READWRITE,
    FLING_COMPLETE,
} fling_state;

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

static int fling(const char * restrict host, const char * restrict port, int fd)
{
    int sock = connect_dest(host, port);
    
    fling_state state = FLING_PANIC;
    off64_t total_written = 0;
    int r, w;
    char buf[BUFSIZ]; /* only used for read/write mode */

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
        fprintf(stdout, "bytes flung: %ld\n", total_written);
    }

    return EXIT_SUCCESS;

}

int main(int argc, char *argv[])
{
    int opt;

    while ((opt = getopt(argc, argv, "hv")) != -1) {
        switch (opt) {
            case 'h':
                usage(argv[0], stdout);
                exit(EXIT_SUCCESS);
                break;
            case 'v':
                verbose = true;
                break;
            default:
                usage(argv[0], stderr);
                exit(EXIT_FAILURE);
                break;
        }
    }

    if (argc - optind != 2) {
        fprintf(stderr, "error: host and port expected.\n");
        usage(argv[0], stderr);
        exit(EXIT_FAILURE);
    }

    exit(fling(argv[optind], argv[optind + 1], 0));
}
