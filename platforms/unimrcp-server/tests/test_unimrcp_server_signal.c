#include <stdio.h>

#include "unimrcp_server_signal.h"

#if defined(WIN32) || defined(_WIN32)

int main(void)
{
	printf("SKIP SIGPIPE is not available on Windows\n");
	return 0;
}

#else

#include <errno.h>
#include <apr_general.h>
#include <apr_network_io.h>
#include <apr_portable.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for_child(pid_t pid, int *status)
{
	int attempts;
	for(attempts = 0; attempts < 500; attempts++) {
		pid_t result = waitpid(pid, status, WNOHANG);
		if(result == pid) {
			return 1;
		}
		if(result < 0) {
			if(errno == EINTR) {
				continue;
			}
			kill(pid, SIGKILL);
			waitpid(pid, status, 0);
			return 0;
		}
		usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, status, 0);
	return 0;
}

static int child_write_result(int initialize_signals)
{
	int sockets[2];
	pid_t pid;
	int status;

	if(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		return 0;
	}

	/* Close the peer before fork so the child write deterministically has no reader. */
	close(sockets[0]);
	pid = fork();
	if(pid == 0) {
		char byte = 'x';
		signal(SIGALRM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		alarm(5);
		if(initialize_signals && unimrcp_server_signals_init() != 0) {
			_exit(10);
		}
		if(write(sockets[1], &byte, 1) != -1 || errno != EPIPE) {
			_exit(11);
		}
		if(write(sockets[1], &byte, 1) != -1 || errno != EPIPE) {
			_exit(12);
		}
		_exit(0);
	}

	close(sockets[1]);
	if(pid < 0 || !wait_for_child(pid, &status)) {
		return 0;
	}
	if(initialize_signals) {
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;
}

static int child_apr_send_result(void)
{
	int sockets[2];
	pid_t pid;
	int status;

	if(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		return 0;
	}
	close(sockets[0]);
	pid = fork();
	if(pid == 0) {
		apr_pool_t *pool = NULL;
		apr_socket_t *socket = NULL;
		apr_os_sock_t os_socket = sockets[1];
		char byte = 'x';
		int attempt;

		signal(SIGALRM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		alarm(5);
		if(unimrcp_server_signals_init() != 0 || apr_initialize() != APR_SUCCESS ||
			apr_pool_create(&pool, NULL) != APR_SUCCESS ||
			apr_os_sock_put(&socket, &os_socket, pool) != APR_SUCCESS) {
			_exit(20);
		}
		for(attempt = 0; attempt < 2; attempt++) {
			apr_size_t length = 1;
			apr_status_t rv = apr_socket_send(socket, &byte, &length);
			if(!APR_STATUS_IS_EPIPE(rv)) {
				_exit(21 + attempt);
			}
		}
		apr_pool_destroy(pool);
		apr_terminate();
		_exit(0);
	}

	close(sockets[1]);
	if(pid < 0 || !wait_for_child(pid, &status)) {
		return 0;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
	int passed = 0;

	if(!child_write_result(0)) {
		fprintf(stderr, "default disconnected write must terminate with SIGPIPE\n");
		return 1;
	}
	passed++;

	if(!child_write_result(1)) {
		fprintf(stderr, "protected disconnected write must return EPIPE\n");
		return 1;
	}
	passed++;

	if(!child_apr_send_result()) {
		fprintf(stderr, "protected APR disconnected sends must return EPIPE\n");
		return 1;
	}
	passed++;

	printf("%d/3 signal tests passed\n", passed);
	return 0;
}

#endif
