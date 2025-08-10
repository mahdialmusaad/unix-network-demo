#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "network_shared.h"

#ifdef __cplusplus
extern "C" {
#endif


volatile sig_atomic_t client_running = 0; /* Determines the 'active' state of the client. */ 

/* ---- Function declarations ---- */

/* Attempts to connect to the server with the given port and address strings, returning the server's socket file descriptor if found.
   Exits on failure to find or connect to a server. */
int init_server_connection(const char *server_address, const char *server_port);
/* The main loop for sending messages to the connected server. */
void begin_client_loop(int server_sockfd);
/* Seperate handler for interpreting and printing server responses or messages. */
static void *handle_server_responses(void *v_server_sockfd);

/* Ctrl+C handler to stop client gracefully */
static void signal_client_end(int param);


int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "Usage:  %s <server_address> <server_port>\n", argv[0]);
		fprintf(stderr, "\tAddress: The address or device name to connect to.\n");
		fprintf(stderr, "\tPort: The port of the server to connect to. [1024, 65535]\n");
		return EXIT_FAILURE;
	}

	/* Convert given server port to a numerical value for bounds checking */
	const long server_port_long = strtol(argv[2], NULL, 10);
	if (server_port_long < 1024 || server_port_long > 65535) {
		fprintf(stderr, "Server port must be a number between 1024 and 65535.\n");
		return EXIT_FAILURE;
	}
	const int server_sockfd = init_server_connection(argv[1], argv[2]); /* Attempt to connect to given server */
	begin_client_loop(server_sockfd); /* Send encryption details to server and begin main message loop */

	return EXIT_SUCCESS;
}


/*  ---- Function definitions ---- */


int init_server_connection(const char *server_address, const char *server_port)
{
	/* Initial values to be filled by server address conncections */
	struct addrinfo addr_info_hints, *server_address_list, *server_address_info_iterator;
	char found_server_ip_buffer[INET6_ADDRSTRLEN];
	const socklen_t server_ip_buffer_len = (socklen_t)(sizeof found_server_ip_buffer);
	int found_server_sockfd;

	/* Specify values for address type hints (TCP, any address family) */
	memset(&addr_info_hints, 0, sizeof addr_info_hints);
	addr_info_hints.ai_family = AF_UNSPEC;
	addr_info_hints.ai_socktype = SOCK_STREAM;

	/* Get all the different linked addresses to attempt a connection */
	check_error(getaddrinfo(
		server_address,
		server_port,
		&addr_info_hints,
		&server_address_list
	), "Failed to get server address information", 1);

	/* Go through each address in the linked list of server addresses, connecting to the first one that works. */
	int address_found_counter = 0;
	for (server_address_info_iterator = server_address_list;
	     server_address_info_iterator != NULL;
	     server_address_info_iterator = server_address_info_iterator->ai_next
	) {
		++address_found_counter;

		/* Try to open a socket/file descriptor with the current server address */
		if (check_error(found_server_sockfd = socket(
			server_address_info_iterator->ai_family,
			server_address_info_iterator->ai_socktype,
			server_address_info_iterator->ai_protocol
		), "Failed to create a socket for a found address", 0) == -1) continue;
		
		/* Open a connection on created socket */
		if (check_error(connect(
			found_server_sockfd,
			server_address_info_iterator->ai_addr,
			server_address_info_iterator->ai_addrlen
		), "Failed to connect to a found address", 0) == -1) {
			close(found_server_sockfd);
			continue;
		}

		/* Try to convert the found address into a printable format */
		if (check_error_null(inet_ntop(
			server_address_info_iterator->ai_family,
			get_ipvx_address((struct sockaddr*)server_address_info_iterator->ai_addr),
			found_server_ip_buffer,
			server_ip_buffer_len
		), "Failed to convert a found address to presentation form", 0) != -1) {
			printf("Connecting to address '%s' on port %s.\n", found_server_ip_buffer, server_port);
		}

		/* Found a valid address and was able to connect, continuing with this server. */
		goto address_search_success;
	}

	/* If the loop ended at a NULL address pointer, none of the addresses in the given linked list worked. */
	if (server_address_info_iterator == NULL) {
		fprintf(stderr, "Failed to connect to the %d found address(es).\n", address_found_counter);
		exit(EXIT_FAILURE);
	}

address_search_success:
	signal(SIGINT, signal_client_end); /* Clean client shutdown on Ctrl+C */
	freeaddrinfo(server_address_list); /* Only the server socket is needed after this. */
	return found_server_sockfd;
}

void begin_client_loop(int server_sockfd)
{
	client_running = 1; /* Set client as active */

	const size_t client_input_buffer_size = 0xFFF;
	char *client_input_buffer = calloc(sizeof(char), client_input_buffer_size);
	check_error_null(client_input_buffer, "Calloc failed on input buffer", 1);

	/* Create thread for handling server messages */
	pthread_t response_handler_thread;
	pthread_create(&response_handler_thread, NULL, handle_server_responses, &server_sockfd);

	printf("Type messages to be sent to server:\n");

	do {
		/* Get user input from stdin */
		const size_t input_message_len = get_stdin_input(
			client_input_buffer,
			client_input_buffer_size
		);
		if (input_message_len == 0) continue;

		check_error((int)send_bytes(
			server_sockfd,
			client_input_buffer,
			input_message_len
		), "Failed to send message", 0); /* Send input to server */
	} while (client_running);

	if (client_running == 0) printf("\nClosing connection with server...\n");

	close(server_sockfd); /* Close server socket */
	free(client_input_buffer); /* Free allocated input buffer */
}

void *handle_server_responses(void *v_server_sockfd)
{
	const int server_sockfd = *(int*)v_server_sockfd; /* Get server socket from thread pointer argument */

	/* Allocate a buffer to store messages from the server */
	const size_t server_response_buffer_size = 0xFFFF;
	char *server_response_buffer = calloc(sizeof(char), server_response_buffer_size);

	do {
		/* Block and wait to recieve buffer from server */
		const ssize_t total_bytes_recieved = recieve_bytes(
			server_sockfd,
			server_response_buffer,
			server_response_buffer_size
		);

		if (total_bytes_recieved == 0) {
			/* Recieving '0 bytes' means the connection has been closed, stop client as well */
			printf("Connection with server lost, exiting...\n");
			close(server_sockfd);
			exit(EXIT_SUCCESS);
			return NULL;
		}

		check_error((int)total_bytes_recieved, "Failed to recieve server message", 0);

		/* If the message recieved is the 'pulse' message, respond so the server knows the 
		   client is still connected to avoid disconnection during large periods of inactivity */
		if (*server_response_buffer == network_global_pulse_message) {
			check_error((int)send_bytes(
				server_sockfd,
				&network_global_pulse_null_response,
				network_global_pulse_bytes
			), "Failed to reply to pulse message", 0);
		} else printf("Message recieved from server: %s\n", server_response_buffer);
	} while (client_running);

	return NULL;
}


void signal_client_end(int param)
{
	(void)param; /* Avoid unused parameter warning */
	client_running = 0; /* Stop client loop and begin resource cleanup */
}

#ifdef __cplusplus
}
#endif
