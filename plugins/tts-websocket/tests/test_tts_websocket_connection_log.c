#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apr_general.h>
#include <apr_network_io.h>

static char* read_file(const char *path)
{
	FILE *file;
	long size;
	char *source;

	file = fopen(path, "rb");
	if(!file) {
		return NULL;
	}
	if(fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	size = ftell(file);
	if(size < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	source = (char*)malloc((size_t)size + 1);
	if(!source) {
		fclose(file);
		return NULL;
	}
	if(fread(source, 1, (size_t)size, file) != (size_t)size) {
		free(source);
		fclose(file);
		return NULL;
	}
	source[size] = '\0';
	fclose(file);
	return source;
}

static int test_local_port_is_logged_after_connect(const char *source)
{
	const char *connect_call = strstr(source, "rv = apr_socket_connect(sock, sa);");
	const char *local_addr_call;
	const char *local_port_log;

	if(!connect_call) {
		return 0;
	}
	local_addr_call = strstr(connect_call, "apr_socket_addr_get(&local_addr, APR_LOCAL, sock)");
	local_port_log = strstr(connect_call, "client local port: %d");
	return local_addr_call && local_port_log && local_addr_call < local_port_log;
}

static int test_local_port_is_available_after_connect(apr_pool_t *pool)
{
	apr_socket_t *server = NULL;
	apr_socket_t *client = NULL;
	apr_socket_t *accepted = NULL;
	apr_sockaddr_t *server_addr = NULL;
	apr_sockaddr_t *client_addr = NULL;
	apr_status_t rv;
	int passed = 0;

	rv = apr_socket_create(&server, APR_INET, SOCK_STREAM, APR_PROTO_TCP, pool);
	if(rv != APR_SUCCESS) {
		goto cleanup;
	}
	rv = apr_sockaddr_info_get(&server_addr, "127.0.0.1", APR_INET, 0, 0, pool);
	if(rv != APR_SUCCESS || apr_socket_bind(server, server_addr) != APR_SUCCESS ||
	   apr_socket_listen(server, 1) != APR_SUCCESS) {
		goto cleanup;
	}
	if(apr_socket_addr_get(&server_addr, APR_LOCAL, server) != APR_SUCCESS ||
	   server_addr->port == 0) {
		goto cleanup;
	}

	if(apr_socket_create(&client, APR_INET, SOCK_STREAM, APR_PROTO_TCP, pool) != APR_SUCCESS ||
	   apr_socket_connect(client, server_addr) != APR_SUCCESS) {
		goto cleanup;
	}
	if(apr_socket_addr_get(&client_addr, APR_LOCAL, client) == APR_SUCCESS &&
	   client_addr->port != 0 &&
	   apr_socket_accept(&accepted, server, pool) == APR_SUCCESS) {
		passed = 1;
	}

cleanup:
	if(accepted) {
		apr_socket_close(accepted);
	}
	if(client) {
		apr_socket_close(client);
	}
	if(server) {
		apr_socket_close(server);
	}
	return passed;
}

int main(int argc, char **argv)
{
	apr_pool_t *pool = NULL;
	char *source;
	int passed = 0;

	if(argc > 2) {
		fprintf(stderr, "usage: %s [tts_websocket_engine.c]\n", argv[0]);
		return 1;
	}
	if(apr_initialize() != APR_SUCCESS || apr_pool_create(&pool, NULL) != APR_SUCCESS) {
		fprintf(stderr, "failed to initialize APR\n");
		return 1;
	}

	if(!test_local_port_is_available_after_connect(pool)) {
		fprintf(stderr, "local source port must be available after connect\n");
		apr_pool_destroy(pool);
		apr_terminate();
		return 1;
	}
	passed++;
	if(argc == 2) {
		source = read_file(argv[1]);
		if(!source) {
			fprintf(stderr, "failed to read %s\n", argv[1]);
			apr_pool_destroy(pool);
			apr_terminate();
			return 1;
		}
		if(!test_local_port_is_logged_after_connect(source)) {
			fprintf(stderr, "local source port must be queried and logged after connect\n");
			free(source);
			apr_pool_destroy(pool);
			apr_terminate();
			return 1;
		}
		free(source);
		passed++;
	}
	apr_pool_destroy(pool);
	apr_terminate();
	printf("%d/%d connection log tests passed\n", passed, argc == 2 ? 2 : 1);
	return 0;
}
