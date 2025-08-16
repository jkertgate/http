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
#include <signal.h>
#include <pthread.h>

#define POOL_SIZE 20
#define ROOT   (1u<<0)
#define ECHO   (1u<<1)
#define FILES  (1u<<2)
#define UA     (1u<<3)

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


struct sockaddr_in serv_addr;

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
	const char *filename = target + 7;
	char full[PATH_MAX], header[256], buf[8192];
	int fd, n, h;
   	struct stat st;
    	size_t sent, to_send; 

    if (strncmp(target, "/files/", 7) != 0) {
        (void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        return;
    }
    
    if (*filename == '\0' || *filename == '/' || strstr(filename, "..")) {
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
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) 
	{
        	close(fd);
        	(void)send(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
        	return;
    	}   
    h = snprintf(header, sizeof(header),
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
    	sent = 0; 
	to_send = (size_t)h;
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
        	if (r == 0)
			break;
        	if (r < 0)
			break;
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

static inline unsigned route_mask(const char *t)
{
    if (!t || t[0] != '/') 
	return 0u;
    if (t[1] == '\0')
      return ROOT;                  
    if (strncmp(t + 1, "echo/", 5) == 0) 
	return ECHO;   
    if (strncmp(t + 1, "files/", 6) == 0) 
	return FILES;  
    if (strcmp (t + 1, "user-agent")  == 0) 
	return UA;        
    return 0u;
}

static void responsehandle(char *request, int client_fd)
{
    char method[8] = {0}, target[1024] = {0};

    if (sscanf(request, "%7s %1023s", method, target) != 2) 
	{
        	(void)send(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
        	return;
    	}
    if (memcmp(method, "GET", 3) != 0 || method[3] != '\0')
	{
        	(void)send(client_fd, "HTTP/1.1 405 Method Not Allowed\r\n\r\n", 36, 0);
        	return;
    	}

    unsigned m = route_mask(target);
    switch (m)
    {
        case ROOT:   
	{
		const char *res = "HTTP/1.1 200 OK\r\n\r\n";
		send(client_fd, res, strlen(res), 0);
		break;
	}
	
	 case FILES:
	{
	 	handle_files_get_path(target, client_fd);
	 	break; 
	}
  
        case ECHO:
	{
            const char *msg = target + 6; 
            size_t len = strlen(msg);
            char header[256];
            int h = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: %zu\r\n"
                             "\r\n", len);
            if (h > 0 && h < (int)sizeof(header)) 
		{
                size_t off = 0;
                while (off < (size_t)h) {
                    ssize_t w = send(client_fd, header + off, (size_t)h - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
                off = 0;
                while (off < len) 
		{
                    ssize_t w = send(client_fd, msg + off, len - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
            }
            break;
        }
        case UA: 
	{            
            char ua[1024] = {0};
            if (sscanf(request, "%*[^U]User-Agent: %1023[^\r\n]", ua) == 1) 
		{
            		size_t ua_len = strlen(ua);
                	char header[256];
                	int h = snprintf(header, sizeof(header),
                        	         "HTTP/1.1 200 OK\r\n"
                        	         "Content-Type: text/plain\r\n"
                        	         "Content-Length: %zu\r\n"
                                	 "\r\n", ua_len);
                size_t off = 0;
                while (off < (size_t)h) 
		{
                    ssize_t w = send(client_fd, header + off, (size_t)h - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
                off = 0;
                while (off < ua_len) 
		{
                    ssize_t w = send(client_fd, ua + off, ua_len - off, 0);
                    if (w <= 0) return;
                    off += (size_t)w;
                }
            } else {
                (void)send(client_fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28, 0);
            }
            break;
        }
    default:
	{ 
	const char *res = "HTTP/1.1 404 Not Found\r\n\r\n"; 
	send(client_fd, res, strlen(res), 0); 
	break;
	}
    }
}

static void *threadfunction(void *arg) 
{
	(void)arg; 
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
		close(client_fd);
		}
	}
	return NULL;
}

int main(int ac, char *av[]) 
{

	int reuse = 1;
	int server_fd, client_fd;
	char tmp[PATH_MAX];

 	signal(SIGPIPE, SIG_IGN);

    	memset(&serv_addr, 0, sizeof(serv_addr));
    	serv_addr.sin_family      = AF_INET;
    	serv_addr.sin_port        = htons(4221);
    	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);


    for (int i = 1; i + 1 < ac; i++) 										//find directory
	{
        if (strcmp(av[i], "--directory") == 0)
		{
            		strncpy(g_dir, av[i+1], sizeof(g_dir) - 1);
            		g_dir[sizeof(g_dir) - 1] = '\0';
            		i++;
        	}
    	}
	if (realpath(g_dir, tmp))
	{
	        strncpy(g_dir, tmp, sizeof(g_dir) - 1);
        	g_dir[sizeof(g_dir) - 1] = '\0';
    	}
	for (int i = 0; i < POOL_SIZE; i++) 									//thread pool
	{
		if (pthread_create(&thread_pool[i], NULL, threadfunction, NULL) != 0)
		{
        		perror("pthread_create");
        		exit(1);
    		}
    		pthread_detach(thread_pool[i]);   
	}
	server_fd = socket(AF_INET, SOCK_STREAM, 0); 								//socket
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


	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0)				//bind
	{
        	printf("Bind failed: %s \n", strerror(errno));
        	return 1;
	}
	
	if (listen(server_fd, 5) != 0)
    	{
        	printf("Listen failed: %s \n", strerror(errno));
        	return 1;
    	}

	for (;;) 												//intial loop
	{
        	client_fd = accept(server_fd, NULL, NULL);
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
	return 0; 												//unused ctrl c to quit
}