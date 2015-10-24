#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define ERR_EXIT(a) { perror(a); exit(1); }

typedef struct {
    char hostname[512];  // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;  // fd to wait for a new connection
} server;

typedef struct {
    char host[512];  // client's host
    int conn_fd;  // fd to talk with client
    char buf[512];  // data sent by/to client
    size_t buf_len;  // bytes used by buf
    // you don't need to change this.
	int item;
    int wait_for_write;  // used by handle_read to know if the header is read or not.
} request;

typedef struct {
    int id;
    int amount;
    int price;
} Item;
int locked[20];

server svr;  // server
request* requestP = NULL;  // point to a list of requests
int maxfd;  // size of open file descriptor table, size of request list

const char* accept_read_header = "ACCEPT_FROM_READ";
const char* accept_write_header = "ACCEPT_FROM_WRITE";

// Forwards

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP, int itemfd, fd_set* set);
// free resources used by a request instance

static int handle_read(request* reqP);
// return 0: socket ended, request done.
// return 1: success, message (without header) got this time is in reqP->buf with reqP->buf_len bytes. read more until got <= 0.
// It's guaranteed that the header would be correctly set after the first read.
// error code:
// -1: client connection error

int new_connection()
{
    struct sockaddr_in cliaddr;
    int clilen = sizeof(cliaddr);
    int conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliaddr, (socklen_t*)&clilen);
    if (conn_fd < 0) {
        if (errno == EINTR || errno == EAGAIN) return -1;  // try again
        if (errno == ENFILE) {
            (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
            return -1;
        }
        ERR_EXIT("accept")
    }
    requestP[conn_fd].conn_fd = conn_fd;
    strcpy(requestP[conn_fd].host, inet_ntoa(cliaddr.sin_addr));
    fprintf(stderr, "getting a new request... fd %d from %s\n", conn_fd, requestP[conn_fd].host);
    return conn_fd;
}

void print_error(request *p, char *errormsg) {
    fprintf(stderr, "%s from %s\n", errormsg, p->host);
}

int set_lock(int item_fd, int index) {
    struct flock lock;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = index * sizeof(Item);
    lock.l_len = sizeof(Item);
    locked[index] = 1;
    return fcntl(item_fd, F_SETLK, &lock);
}

int un_lock(int item_fd, int index) {
    struct flock lock;
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = index * sizeof(Item);
    lock.l_len = sizeof(Item);
    locked[index] = 0;
    return fcntl(item_fd, F_SETLK, &lock);
}

int check_lock(int item_fd, int index) {
    struct flock lock;
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = index * sizeof(Item);
    lock.l_len = sizeof(Item);
    if (fcntl(item_fd, F_GETLK, &lock) < 0) return -1;
    return (lock.l_type != F_UNLCK || locked[index]);
}

void print_client(request *p, char *msg) {
    write(p->conn_fd, msg, strlen(msg));
}

// mode 0: read, 1: write
void handle_cmd(request *p, int item_fd, fd_set *set, int mode) {
    char buffer[1024];
    if (mode == 0) {
        p->item = atoi(p->buf);
        int index = p->item - 1;
        int flag = check_lock(item_fd, index);
        if (flag < 0) {
            print_error(p, "Unable to check lock");
            free_request(p, item_fd, set);
            return;
        }
        else if (flag == 0) {
            Item now;
            lseek(item_fd, index * sizeof(Item), SEEK_SET);
            read(item_fd, &now, sizeof(Item));
            sprintf(buffer, "item%d $%d remain: %d\n", now.id, now.price, now.amount);
        }
        else sprintf(buffer, "This item is locked.\n");
        print_client(p, buffer);
        free_request(p, item_fd, set);
    }
    else {
        if (!p->item) {
            p->item = atoi(p->buf);
            int index = p->item - 1;
            int flag = check_lock(item_fd, index);
            if (flag < 0) {
                print_error(p, "Unable to check lock");
                free_request(p, item_fd, set);
                return;
            }
            else if (flag == 0) {
                if (set_lock(item_fd, index) < 0) {
                    print_error(p, "Unable to set lock");
                    free_request(p, item_fd, set);
                    return;
                }
                print_client(p, "This item is modifiable.\n");
            }
            else {
                print_client(p, "This item is locked.\n");
                free_request(p, item_fd, set);
            }
        }
        else {
            int index = p->item - 1;
            Item now;
            lseek(item_fd, index * sizeof(Item), SEEK_SET);
            read(item_fd, &now, sizeof(Item));
            char cmd[1024];
            int amount;
            sscanf(p->buf, "%s%d", cmd, &amount);
            if (strcmp(cmd, "buy") == 0) {
                if (now.amount - amount < 0) {
                    print_client(p, "Operation failed.\n");
                    free_request(p, item_fd, set);
                    return;
                }
                now.amount -= amount;
            }
            else if (strcmp(cmd, "sell") == 0) {
                if (amount < 0) {
                    print_client(p, "Operation failed.\n");
                    free_request(p, item_fd, set);
                    return;
                }
                now.amount += amount;
            } 
            else if (strcmp(cmd, "price") == 0) {
                if (amount < 0) {
                    print_client(p, "Operation failed.\n");
                    free_request(p, item_fd, set);
                    return;
                }
                now.price = amount;
            }
            else {
                print_error(p, "Bad request");
                free_request(p, item_fd, set);
                return;
            }
            lseek(item_fd, index * sizeof(Item), SEEK_SET);
            write(item_fd, (char*)&now, sizeof(Item));
            free_request(p, item_fd, set);
        }
    }

}

int main(int argc, char** argv) {
    int conn_fd;  // fd for a new connection with client
    int file_fd;  // fd for file that we open for reading
#ifdef read_server
    int item_fd = open("item_list", O_RDONLY);
#else
    int item_fd = open("item_list", O_RDWR);
#endif

    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }
    memset(locked, 0, sizeof(locked));

    // Initialize server
    init_server((unsigned short) atoi(argv[1]));

    // Get file descripter table size and initize request table
    maxfd = getdtablesize();
    requestP = (request*) malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (int i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    fprintf(stderr, "\nstarting on %.80s, port %d, fd %d, maxconn %d...\n", svr.hostname, svr.port, svr.listen_fd, maxfd);

    fd_set read_master, read_working;
    FD_ZERO(&read_master);
    FD_SET(svr.listen_fd, &read_master);

    while (1) {
        fprintf(stderr, "monitoring: ");
        for (int i=0; i<maxfd; i++){
            if (FD_ISSET(i, &read_master)) {
                fprintf(stderr, "%d ", i);
            }
        }
        fprintf(stderr, "\n");
        memcpy(&read_working, &read_master, sizeof(read_master));
        select(maxfd, &read_working, NULL, NULL, NULL);
        for (conn_fd=0; conn_fd<maxfd; conn_fd++) {
            if (FD_ISSET(conn_fd, &read_working)) {
                if (conn_fd == svr.listen_fd) {
                    int new_fd;
                    if ((new_fd = new_connection()) < 0) continue;
                    FD_SET(new_fd, &read_master);
                }
                else {
                    int ret = handle_read(&requestP[conn_fd]); // parse data from client to requestP[conn_fd].buf
                    if (ret < 0) {
                        print_error(&requestP[conn_fd], "Bad request");
                        free_request(&requestP[conn_fd], item_fd, &read_master);
                        continue;
                    }
                    else if(ret == 0) continue;
#ifdef READ_SERVER
                    handle_cmd(&requestP[conn_fd], item_fd, &read_master, 0);
#else
                    handle_cmd(&requestP[conn_fd], item_fd, &read_master, 1);
#endif
                }
            }
        }
    }
    free(requestP);
    return 0;
}


// ======================================================================================================
// You don't need to know how the following codes are working

static void* e_malloc(size_t size);


static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->buf_len = 0;
    reqP->item = 0;
    reqP->wait_for_write = 0;
}

static void free_request(request* p, int item_fd, fd_set *set) {
    /*if (reqP->filename != NULL) {
        free(reqP->filename);
        reqP->filename = NULL;
    }*/
    if (p->item)
        if (un_lock(item_fd, p->item-1) < 0)
            print_error(p, "Unable to unlock");
    FD_CLR(p->conn_fd, set);
    close(p->conn_fd);
    init_request(p);
}

// return 0: socket ended, request done.
// return 1: success, message (without header) got this time is in reqP->buf with reqP->buf_len bytes. read more until got <= 0.
// It's guaranteed that the header would be correctly set after the first read.
// error code:
// -1: client connection error
static int handle_read(request* reqP) {
    int r;
    char buf[512];

    // Read in request from client
    r = read(reqP->conn_fd, buf, sizeof(buf));
    if (r < 0 || buf[0] == -1) return -1;
    if (r == 0) return 0;
	char* p1 = strstr(buf, "\015\012");
	// be careful that in Windows, line ends with \015\012
	if (p1 == NULL) {
		p1 = strstr(buf, "\012");
	}
	size_t len = p1 - buf + 1;
	memmove(reqP->buf, buf, len);
	reqP->buf[len - 1] = '\0';
	reqP->buf_len = len-1;
    return 1;
}

static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }
}

static void* e_malloc(size_t size) {
    void* ptr;

    ptr = malloc(size);
    if (ptr == NULL) ERR_EXIT("out of memory");
    return ptr;
}

