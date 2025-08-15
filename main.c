#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#define POOL_SIZE 20

typedef struct node {
    int client_fd;
    struct node *next;
} node;


static node *head = NULL;  
static node *tail = NULL;   

static char g_dir[PATH_MAX] = ".";

pthread_t thread_pool[POOL_SIZE];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  condition_var = PTHREAD_COND_INITIALIZER;

static void enqueue(int client_fd) 
{
	node *n = (node *)malloc(sizeof(*n));
	if (!n)
	{
		close(client_fd);
		return;
	}
	n->client_fd = client_fd;
	n->next = NULL;
	if (tail) 
	{
		tail->next = n;
		tail = n;
	}
	else { 
		head = tail = n;
	}
}

static int dequeue(void) 
{
	if (!head)
		return -1;
	node *n = head;
	head = n->next;
	if (!head)
		tail = NULL;
	int fd = n->client_fd;
	free(n);
	return fd;
}

static void handle_files_get_path(const char *target, int client_fd)
{
	const char *filename;
	char full[PATH_MAX];
	int fd, n;
 	char header[256];
	char buf[8192];


    if (strncmp(target, "/files/", 7) != 0)
	{
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}

    filename = target + 7;
    if (*filename == '\0' || *filename == '/' || strstr(filename, "..")) 
	{
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}

    
    n = snprintf(full, sizeof(full), "%s/%s", g_dir, filename);
    if (n < 0 || n >= (int)sizeof(full)) 
	{
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}

    fd = open(full, O_RDONLY);
    if (fd < 0) 
	{
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) 
	{
        	close(fd);
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}

   
    int h = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lld\r\n"
        "\r\n",
        (long long)st.st_size);
    if (h <= 0 || h >= (int)sizeof(header))
	{
        	close(fd);
        	(void)send(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 40, 0);
        	return;
    	}


    size_t sent = 0, to_send = (size_t)h;
    while (sent < to_send) 
	{
        ssize_t w = send(client_fd, header + sent, to_send - sent, 0);
        if (w <= 0) 
		{ 
			close(fd);
	 		return;
		}
        sent += (size_t)w;
    	}


    
    for (;;)
	{
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0)  break;
        size_t off = 0;
        while (off < (size_t)r)
	{
            ssize_t w = send(client_fd, buf + off, (size_t)r - off, 0);
            if (w <= 0)
		{
			 close(fd);
		 	return; 
		}
            off += (size_t)w;
        }
    }
    close(fd);
}

static void responsehandle(char *request, int client_fd)
{
    enum Route { R_FILE, ROOT, ECHO, USER_AGENT, NOT_FOUND } route = NOT_FOUND;

    char method[8] = {0}, target[PATH_MAX] = {0};
    if (sscanf(request, "%7s %1023s", method, target) != 2) {
        (void)send(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        (void)send(client_fd, "HTTP/1.1 405 Method Not Allowed\r\n\r\n", 36, 0);
        return;
    }

    if (strcmp(target, "/") == 0) {
        route = ROOT;
    } else if (strncmp(target, "/echo/", 6) == 0) {
        route = ECHO;
    } else if (strcmp(target, "/user-agent") == 0) {
        route = USER_AGENT;
    } else if (strncmp(target, "/files/", 7) == 0) {
        route = R_FILE;
    } else {
        route = NOT_FOUND;
    }

    switch (route)
    {
        case R_FILE: {
            handle_files_get_path(target, client_fd); // <-- pass parsed path
            break;
        }

        case ROOT: {
            const char *res = "HTTP/1.1 200 OK\r\n\r\n";
            (void)send(client_fd, res, strlen(res), 0);
            break;
        }

        case ECHO: {
            // Use the already-parsed target, not request offsets
            const char *msg = target + 6; // after "/echo/"
            size_t len = strlen(msg);
            char header[256];
            int h = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: %zu\r\n"
                             "\r\n", len);
            if (h > 0 && h < (int)sizeof(header)) {
                size_t off = 0;
                while (off < (size_t)h) {
                    ssize_t w = send(client_fd, header + off, (size_t)h - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
                off = 0;
                while (off < len) {
                    ssize_t w = send(client_fd, msg + off, len - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
            }
            break;
        }

        case USER_AGENT: {
            // ok to read from the raw request buffer for headers
            char ua[1024] = {0};
            if (sscanf(request, "%*[^U]User-Agent: %1023[^\r\n]", ua) == 1) {
                size_t ua_len = strlen(ua);
                char header[256];
                int h = snprintf(header, sizeof(header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "Content-Length: %zu\r\n"
                                 "\r\n", ua_len);
                size_t off = 0;
                while (off < (size_t)h) {
                    ssize_t w = send(client_fd, header + off, (size_t)h - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
                off = 0;
                while (off < ua_len) {
                    ssize_t w = send(client_fd, ua + off, ua_len - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
            } else {
                (void)send(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
            }
            break;
        }

        default: {
            const char *res = "HTTP/1.1 404 Not Found\r\n\r\n";
            (void)send(client_fd, res, strlen(res), 0);
            break;
        }
    }
}

static void *threadfunction(void *arg) //mem leak in here i think
{
	(void)arg; //unsure whyi need todo this but i do
	char buf[4096];

	for (;;) //again with the infinite loops
	{
        	pthread_mutex_lock(&mutex);
        	while (head == NULL) 
			{
            	pthread_cond_wait(&condition_var, &mutex);
        		}
        	int client_fd = dequeue();   
        	pthread_mutex_unlock(&mutex);
        	if (client_fd >= 0) 
		{
			ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
			if (n > 0)
			{
                		buf[n] = '\0';
                		responsehandle(buf, client_fd);
			}
		shutdown(client_fd, SHUT_WR);
		close(client_fd);
		}
	}
	return NULL;
}



int main(int ac, char *av[]) 
{

	int reuse = 1;
	int server_fd, client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;
	int connection_backlog = 5;

	setbuf(stdout, NULL); //you dont actuually need todo this
	setbuf(stderr, NULL); 

    // Parse --directory <path>
    for (int i = 1; i + 1 < ac; i++) {
        if (strcmp(av[i], "--directory") == 0) {
            strncpy(g_dir, av[i+1], sizeof(g_dir) - 1);
            g_dir[sizeof(g_dir) - 1] = '\0';
            i++;
        }
    }

    // Canonicalize g_dir if possible (optional)
    char tmp[PATH_MAX];
    if (realpath(g_dir, tmp)) {
        strncpy(g_dir, tmp, sizeof(g_dir) - 1);
        g_dir[sizeof(g_dir) - 1] = '\0';
    }

	for (int i = 0; i < POOL_SIZE; i++) 
	{
		if (pthread_create(&thread_pool[i], NULL, threadfunction, NULL) != 0)
		{
        		perror("pthread_create");
        		exit(1);
    		}
    	pthread_detach(thread_pool[i]);   
	}
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) 
	{
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
	{
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = {
					.sin_family = AF_INET ,
					.sin_port = htons(4221), //dont use port 80
					.sin_addr = { .s_addr = htonl(INADDR_ANY) },
					};

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0)
	{
        	printf("Bind failed: %s \n", strerror(errno));
        	return 1;
	}
	
	if (listen(server_fd, connection_backlog) != 0)
    	{
        	printf("Listen failed: %s \n", strerror(errno));
        	return 1;
    	}
	client_addr_len = sizeof(client_addr);
	for (;;) //please god dont use an infinite loop
	{
        	client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
        	if (client_fd < 0) 
		{
			perror("except");
			continue;
		}
        	pthread_mutex_lock(&mutex);
        	enqueue(client_fd);
        	pthread_cond_signal(&condition_var);
        	pthread_mutex_unlock(&mutex);
	}
	close(server_fd); //unused
	return 0; //unused ctrl c to quit
}