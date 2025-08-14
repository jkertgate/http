#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#define POOL_SIZE 20

typedef struct node {
    int client_fd;
    struct node *next;
} node;


static node *head = NULL;  
static node *tail = NULL;   

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

static void responsehandle(char *request, int client_fd) 
{
	enum Route { ROOT, ECHO, USER_AGENT, NOT_FOUND } route = NOT_FOUND; // use a bitfield instead
	if (strncmp(request, "GET / ", 6) == 0)  //parse better
	{
        	route = ROOT;
	} else if (strncmp(request, "GET /echo/", 10) == 0) {
        	route = ECHO;
	} else if (strncmp(request, "GET /user-agent", strlen("GET /user-agent")) == 0)
		route = USER_AGENT;
	else {
		route = NOT_FOUND;
	}

	switch (route) //add more functionality
 	{
        	case ROOT: 
		{
			const char *res = "HTTP/1.1 200 OK\r\n\r\n";
			send(client_fd, res, strlen(res), 0);
            		break;
        	}
        	case ECHO: 
		{
 			char *msg = request + 10;                   
			size_t len = strcspn(msg, " ");
			msg[len] = '\0';
			char header[256];
			int h = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: %zu\r\n"
                             "\r\n", len);
		if (h > 0 && h < (int)sizeof(header)) 
		{
                	send(client_fd, header, h, 0);
                	if (len) send(client_fd, msg, len, 0);
		}
		break;
        	}
		case USER_AGENT:
		{
    			char ua[1024] = {0};
      			if (sscanf(request, "%*[^U]User-Agent: %1023[^\r\n]", ua) == 1) // this is bad. change this before you go to jail
			{
        			size_t ua_len = strlen(ua);
        			char header[256];
        			int h = snprintf(header, sizeof(header),
                         		"HTTP/1.1 200 OK\r\n"
                         		"Content-Type: text/plain\r\n"
                         		"Content-Length: %zu\r\n"
                         		"\r\n", ua_len);
        			send(client_fd, header, h, 0);
        			if (ua_len) send(client_fd, ua, ua_len, 0);
    			} else {
        		const char *res = "HTTP/1.1 400 Bad Request\r\n\r\n";
        		send(client_fd, res, strlen(res), 0);
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

static void *threadfunction(void *arg) //mem leak in here i think
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
		shutdown(client_fd, SHUT_WR);
		close(client_fd);
		}
	}
	return NULL;
}



int main() 
{

	int reuse = 1;
	int server_fd, client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;

	setbuf(stdout, NULL); //you dont actuually need todo this
	setbuf(stderr, NULL); 

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
					.sin_port = htons(4221),
					.sin_addr = { .s_addr = htonl(INADDR_ANY) },
					};

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0)
	{
        	printf("Bind failed: %s \n", strerror(errno));
        	return 1;
	}
	int connection_backlog = 5;
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
			perror("accept");
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