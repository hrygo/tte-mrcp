#include <stdio.h>

#include "unimrcp_server_signal.h"

#ifdef WIN32

int main(void)
{
	printf("SKIP SIGPIPE is not available on Windows\n");
	return 0;
}

#else

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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
		signal(SIGPIPE, SIG_DFL);
		if(initialize_signals && unimrcp_server_signals_init() != 0) {
			_exit(10);
		}
		if(write(sockets[1], &byte, 1) == -1 && errno == EPIPE) {
			_exit(0);
		}
		_exit(11);
	}

	close(sockets[1]);
	if(pid < 0 || waitpid(pid, &status, 0) != pid) {
		return 0;
	}
	if(initialize_signals) {
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;
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

	printf("%d/2 signal tests passed\n", passed);
	return 0;
}

#endif
