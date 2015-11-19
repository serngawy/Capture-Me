#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define _GNU_SOURCE
#include <getopt.h>

#include "common.h"

#define PORT 42380
#define BUF_SIZE 256
#define SCREENSHOT_CMD "SCREEN"

volatile sig_atomic_t end = 0;
void sig_INT(int sig)	{ fprintf(logFile, "------ Caught signal %d -----\n", sig); if (sig == SIGINT || sig == SIGSEGV) end = 1; }


ssize_t Receive(int sfd, char* buf, size_t count, int flags)
{
	int c;
	size_t len = 0;

	do
	{
		c = recv(sfd, buf, count, flags);
		if (c < 0)	return c;
		if (c == 0)	return len;

		buf += c;
		len += c;
		count -= c;
	} while (count > 0);

	return len;
}

ssize_t Send(int sfd, const char* buf, ssize_t count, int flags)
{
	int c;
	size_t len = 0;

	do
	{
		c = send(sfd, buf, count, flags);
		if (c < 0)	return c;

		buf += c;
		len += c;
		count -= c;
	} while (count > 0);

	return len;
}


int start_server()
{
	//Starting server.
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0)	return -1;
	 //Socket creation
	
	struct sockaddr_in sin;
	sin.sin_family = PF_INET;
	sin.sin_port = htons(PORT);
	sin.sin_addr.s_addr =  htonl(INADDR_ANY);
	if (bind (sfd, (struct sockaddr*)&sin, sizeof(struct sockaddr_in)) < 0)
		return -1;
	// Socket binding"

	if (listen (sfd, 5) < 0)	return -1;
	//Socket in listening mode
	struct linger l = { 0, 0 };
	if (setsockopt(sfd, SOL_SOCKET, SO_LINGER, (const void*)&l, sizeof(struct linger)) < 0)
		return -1;
	//Server started

	return sfd;
}

int setup_signals()
{
	//Signal handling setup

	struct sigaction sa;
	int i;
	for (i = 1; i < 30; ++i) {
		sa.sa_handler = sig_INT;
		sigemptyset (&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(i, &sa, NULL);
	}
	errno = 0;

	return 0;
}

int accept_client(int servfd, int** client_fd, int* client_count)
{
	//Incoming client connection

	int cfd = accept(servfd, NULL, NULL);
	if (cfd < 0)	return -1;
	//- Connection accepted

	/* check whether the client comes from local system; detach if not */
	struct sockaddr_in client_addr;
	socklen_t ca_len = sizeof(struct sockaddr_in);
	if (getpeername(cfd, (struct sockaddr*)&client_addr, &ca_len) < 0)	return -1;
	if (strcmp(inet_ntoa(client_addr.sin_addr), "127.0.0.1") != 0) {
		//- Remote client detected -- closing connection.
		shutdown (cfd, SHUT_RDWR);
		close (cfd);
		return 0;
	}

	*client_fd = (int*)realloc(*client_fd, sizeof(int) * (*client_count + 1));
	(*client_fd)[(*client_count)++] = cfd;
	return cfd;
}

int handle_client_input(int cfd, char* fddev)
{
	//Client socket signaled for input
	struct picture pict;
	char buf[BUF_SIZE];
	int c;	
	
	/* read input and parse it */
	//- Retreiving data
	c = Receive(cfd, buf, strlen(SCREENSHOT_CMD), 0);
	if (c == 0 || (c < 0 && errno == EINTR))	return 0;
	if (c < 0)	return -1;
	if (c >= strlen(SCREENSHOT_CMD) && (buf[strlen(SCREENSHOT_CMD)] = '\0', strcmp(buf, SCREENSHOT_CMD) == 0))
	{
		//- Command identified as " SCREENSHOT_CMD);

		/* screenshot command read; take screenshot and post it through socket */
		//- Taking screenshot
		if (TakeScreenshot(fddev, &pict) < 0)	return -1;
		//- Screenshot taken
		
		/* header: width height BPP */
		memset (buf, 0, BUF_SIZE * sizeof(char));
		snprintf (buf, BUF_SIZE, "%d %d %d", pict.xres, pict.yres, pict.bps);
		if (Send(cfd, buf, (strlen(buf) + 1) * sizeof(char), 0) < 0)	/* incl. \0 */
			return -1;
		/* content */
		if (Send(cfd, pict.buffer, pict.xres * pict.yres * pict.bps / 8, 0) < 0)
			return -1;
	}

	return c;
}

int cleanup(int servfd, int* client_fd, int client_count)
{
	//Shutdown
	int i;
	for (i = 0; i < client_count; ++i)
		if (close(client_fd[i]) < 0)	return -1;
	free (client_fd);

	//- Closing server socket
	if (close(servfd) < 0)	return -1;

	//Shutdown complete
	
	return 0;
}

int do_work(int servfd, char* fbDevice)
{
	int* client_fd = NULL;
	int client_count = 0;
	int max_fd;
	fd_set readfs;
	int i, c;

	//Starting main loop.
	while (!end)
	{
		/* fill fd_set to check sockets for reading (client and server ones) */
		FD_ZERO(&readfs); max_fd = 0;
		FD_SET(servfd, &readfs);
		if (servfd > max_fd)	max_fd = servfd;
		for (i = 0; i < client_count; ++i)
		{
			if (client_fd[i] < 0)	continue;

			FD_SET (client_fd[i], &readfs);
			if (client_fd[i] > max_fd)	max_fd = client_fd[i];
		}

		if (select(max_fd + 1, &readfs, NULL, NULL, NULL) == -1)
		{
			if (errno == EINTR)	{ errno = 0; continue; }
			return -1;
		}

		/* check for input on client socket and handle it */
		for (i = 0; i < client_count; ++i)
		{
			if (client_fd[i] < 0)	continue;
			if (FD_ISSET(client_fd[i], &readfs))
			{
				c = handle_client_input (client_fd[i], fbDevice);
				if (c < 0 && errno != EPIPE && errno!= ECONNRESET)	return -1;
				
				/* connection finished */
				close (client_fd[i]);
				client_fd[i] = -1;	// no socket
			}
		}

		/* check whether we have incoming connection */
		if (FD_ISSET(servfd, &readfs))
			if (accept_client (servfd, &client_fd, &client_count) < 0)
				return -1;
	}
	//- Caught SIGINT

	return cleanup(servfd, client_fd, client_count);
}


int main(int argc, char* argv [])
{
	//Program initialized
	char* device;
	device = "/dev/graphics/fb0";
	int server_socket = start_server();
	if (server_socket < 0)
	    device = "/dev/fb0";
	start_server();
	
}
