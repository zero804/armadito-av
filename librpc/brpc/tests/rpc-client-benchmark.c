/*
  compile with:
  - with debug:
  gcc -Wall -Wno-unused -g -I../include/ -DDEBUG -o rpc-client-benchmark ../buffer.c ../hash.c ../brpc.c unix.c rpc-client-benchmark.c -lpthread
  - without debug:
  gcc -Wall -Wno-unused -g -I../include/ -o rpc-client-benchmark ../buffer.c ../hash.c ../brpc.c unix.c rpc-client-benchmark.c -lpthread
*/

#include <brpc.h>

#include "test.h"
#include "unix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static size_t call_countdown;
static int32_t r;

static void simple_cb(const struct brpc_msg *result, void *user_data)
{
	r = brpc_msg_get_int32(result, 0, NULL);

	call_countdown--;
}

static int test_call(struct brpc_connection *conn, uint8_t method, int op1, int op2, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		int ret = brpc_call(conn, method, simple_cb, NULL, "ii", op1, op2);

		if (ret) {
			fprintf(stderr, "call failed: %d\n", ret);
			return ret;
		}
	}

	return BRPC_OK;
}

static void *cb_thread_fun(void *arg)
{
	struct brpc_connection *conn = (struct brpc_connection *)arg;

	while (brpc_connection_process(conn) != BRPC_EOF && call_countdown > 0)
		;

	return NULL;
}

static void client_error_handler(struct brpc_connection *conn, uint32_t id, int code, const char *message)
{
	if (BRPC_ERR_IS_METHOD_ERROR(code))
		code = BRPC_ERR_CODE_TO_METHOD(code);

	fprintf(stderr, "error handler: id %d code %d message \"%s\"\n", id, code, message);
}

static void usage(int argc, char **argv)
{
	fprintf(stderr, "Usage: %s COUNT\n", argv[0]);
	exit(EXIT_FAILURE);
}

static size_t get_from_arg(int argc, char **argv)
{
	size_t l;

	if (argc < 2)
		usage(argc, argv);

	if (sscanf(argv[1], "%ld", &l) < 1)
		usage(argc, argv);

	return l;
}

int main(int argc, char **argv)
{
	struct brpc_connection *conn;
	int client_sock;
	int *p_client_sock;
	pthread_t cb_thread;
	size_t count;

	count = get_from_arg(argc, argv);

	client_sock = unix_client_connect(SOCKET_PATH, 10);

	if (client_sock < 0) {
		perror("cannot connect to " SOCKET_PATH);
		exit(EXIT_FAILURE);
	}

	conn = brpc_connection_new(NULL, NULL);

	p_client_sock = malloc(sizeof(int));
	*p_client_sock = client_sock;

	brpc_connection_set_read_cb(conn, unix_fd_read_cb, p_client_sock);
	brpc_connection_set_write_cb(conn, unix_fd_write_cb, p_client_sock);

	brpc_connection_set_error_handler(conn, client_error_handler);

	call_countdown = count;

	if (pthread_create(&cb_thread, NULL, cb_thread_fun, conn))
		perror("pthread_create");

	test_call(conn, METHOD_ADD, 58, 11, count);

	if (pthread_join(cb_thread, NULL))
		perror("pthread_join");

	printf("the result is: %d\n", r);

	return 0;
}