#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "network_shared.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- Structs ---- */

/* Data to send to the 'interaction' function. */
struct server_interact_data {
	int server_sockfd; /* Server socket or file descriptor */
	char *interact_message; /* The interaction message or 0-index terminator for a kick message. */
	int interact_target; /* The target of the interaction or 0 for all clients. */
	size_t interact_message_bytes; /* The size in bytes of the actual message */
};


/* ---- Globals ---- */

/* The current state of the server:
   0: Inactive, not running  ----  1: Active, running main loop  ----  2: Interaction data ready */
static volatile sig_atomic_t server_state = 0;


/* ---- Function declarations ---- */

/* Initializes the server in the given port, returning the newly opened server socket/file descriptor. */
int init_server(char *server_port);
/* Begins the main loop for listening and responding to clients. The server must be initialized beforehand. */
void begin_serving(int server_sockfd, long maximum_requests, long is_interactive);

/* Allows interacting with clients through input. Input format: '<ID/all> <Message/kick>' */
void *begin_interaction(void *v_interact_data);
/* Executes command given from interaction mode. Returns the new poll requests list and NULL if the server closed. */
static struct pollfd *handle_interaction_result(
	struct pollfd *poll_sockfds,
	struct server_interact_data *interact_data,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
);

/* Send a 'pulse' message to all connected clients to get a response from them to be captured by their
   corresponding poll request in the main server loop. Returns the new poll requests list and NULL if the server closed. */
static struct pollfd *check_clients_pulse(
	struct pollfd *poll_sockfds,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
);

/* Accept a new client and add them to the poll requests list, returning the new poll requests list.
   If deny_connection is set, the client's socket is immediately closed and not added. */
static struct pollfd *accept_new_client(
	int server_sockfd,
	struct pollfd *poll_sockfds,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count,
	int deny_connection
);
/* Reads the data sent from a client socket and prints out the response if no error occurs.
   If the client disconnected instead, it will remove them from the poll requests list. Returns the new poll requests list. */
static struct pollfd *handle_client_request(
	struct pollfd *poll_sockfds,
	struct pollfd *client_sockfd,
	char *client_response_buffer,
	size_t client_response_buffer_bytes,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_request_count
);

/* Adds the given client socket to the poll requests list. The list is expanded if it is too small to store all the requests.
   Returns the new poll requests list. If an error occurs whilst expanding the list, NULL is returned and the list is not modified. */
static struct pollfd *add_pollfds_list(
	struct pollfd *poll_sockfds,
	int new_client_sockfd,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
);
/* Removes the given poll request from the poll requests list. The list is shrinked if it is much larger than the number of requests.
   Returns the new poll requests list. If an error occurs whilst shrinking, the original list is returned. */
static struct pollfd *remove_pollfds_list(
	struct pollfd *poll_sockfds,
	struct pollfd *toremove_poll_sockfd,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
);

/* Ctrl+C handler to stop server gracefully */
static void signal_server_end(int param);


int main(int argc, char *argv[])
{
	if (argc != 4) {
		fprintf(stderr, "Usage:  %s <port> <max.clients> <interactive>\n", argv[0]);
		fprintf(stderr, "\tPort: What port this server will be hosted on. [1024, 65535]\n");
		fprintf(stderr, "\tMaximum clients: The maximum amount of clients that can be connected. A negative value removes this limit.\n");
		fprintf(stderr, "\tInteractive: Non-zero enables inputting messages to send to specified client(s) or to 'kick' them.\n");
		return EXIT_FAILURE;
	}
	
	/* Check for a valid port argument */
	const long server_port = strtol(argv[1], NULL, 10);
	if (server_port < 1024 || server_port > 65535) {
		fprintf(stderr, "Server port must be between 1024 and 65535.\n");
		return EXIT_FAILURE;
	}

	/* Initialize server to accept connections */
	const int server_sockfd = init_server(argv[1]);
	/* Begin main server loop of listening for client events and sending data */
	begin_serving(server_sockfd, strtol(argv[2], NULL, 10), strtol(argv[3], NULL, 10));

	return EXIT_SUCCESS;
}


/* ---- Function definitions ---- */


int init_server(char *server_port)
{
	/* Most errors here will exit the program, since there isn't a way to recover in those cases. */

	/* Get linked list of local device's address information */
	struct addrinfo address_info_hints, *server_address_info;
	memset(&address_info_hints, 0, sizeof address_info_hints);
	address_info_hints.ai_family = PF_UNSPEC;
	address_info_hints.ai_socktype = SOCK_STREAM;
	address_info_hints.ai_flags = AI_PASSIVE;

	check_error(getaddrinfo(
		NULL,
		server_port,
		&address_info_hints,
		&server_address_info
	), "(Init) Failed to get address info", 1);

	/* Create the server socket for listening to clients */
	int server_sockfd;
	check_error(server_sockfd = socket(
		server_address_info->ai_family,
		server_address_info->ai_socktype ,
		server_address_info->ai_protocol
	), "(Init) Failed to create server socket", 1);
	
	/* Allow reusing a port to avoid getting "address already in use" errors when restarting a server */
	const int allow_port_reuse = 1;
	check_error(setsockopt(
		server_sockfd,
		SOL_SOCKET,
		SO_REUSEADDR,
		&allow_port_reuse,
		(socklen_t)(sizeof allow_port_reuse)
	), "(Init) Port reuse option failed", 0);

	signal(SIGINT, signal_server_end); /* Clean shutdown on Ctrl+C */

	/* Bind the server address to the socket */
	check_error(bind(
		server_sockfd,
		server_address_info->ai_addr,
		server_address_info->ai_addrlen
	), "(Init) Bind failed to given port", 1);
	check_error(listen(server_sockfd, 20), "Listen failed", 1); /* Prepare to queue connections */

	freeaddrinfo(server_address_info); /* Free memory allocated for the server's 'address info' object */

	printf("(Main) Server started at port %s.\n", server_port);
	return server_sockfd;
}

void begin_serving(int server_sockfd, long maximum_requests, long is_interactive)
{
	/* Check if the given server socket is valid */
	if (fcntl(server_sockfd, F_GETFD) == -1) {
		fprintf(stderr, "(Init) The given server socket is invalid. Make sure you have called 'init_server' first.\n");
		return;
	};

	server_state = 1; /* Server is now active */
	++maximum_requests; /* Include server poll request */
	
	/* Counter for how many valid request objects are *present* in the poll requests list (1 for only the server) */
	size_t poll_sockfds_requests_count = 1;
	/* Count of how many request objects are *allocated* in the poll requests list */
	size_t poll_sockfds_alloc_count = 4;
	/* (Start off with some amount of allocated request objects to avoid excessive reallocating at the start) */

	/* Create poll requests list with initial count */
	struct pollfd *poll_sockfds = malloc(sizeof *poll_sockfds * poll_sockfds_alloc_count);
	check_error_null(poll_sockfds, "(Main) Allocation failed for poll requests list", 1);

	/* Set the server pollfd values at the first index */
	poll_sockfds[0].fd = server_sockfd; /* Using the server's file descriptor */
	poll_sockfds[0].events = POLLIN; /* Listening for available reads (in this case, it means an incoming connection) */
	poll_sockfds[0].revents = 0; /* Clear recieved events to see what listened events occurred after polling */

	/* Character buffer for storing client responses */
	const size_t client_response_buffer_size = 0xFFFF;
	char *client_response_buffer = malloc(client_response_buffer_size);
	check_error_null(client_response_buffer, "(Main) Allocation failed for client response buffer", 1);
	
	/* Timer values for 'pulse' check and polling */
	const int poll_timeout_milliseconds = 200;
	time_t previous_pulse_send_time = time(NULL);
	const double pulse_check_frequency_secs = 30.0;

	struct server_interact_data interactive_mode_data;

	/* Initiate interactive mode if specified on a seperate thread. */
	if (is_interactive) {
		interactive_mode_data.server_sockfd = server_sockfd;
		pthread_t interactive_mode_thread;
		pthread_create(&interactive_mode_thread, NULL, begin_interaction, &interactive_mode_data);
	}

	do {
		/* Wait for any specified events on all given poll requests */
		const int poll_events_recieved = poll(poll_sockfds, poll_sockfds_requests_count, poll_timeout_milliseconds);
		if (server_state == 0) break; /* Close on Ctrl+C */

		/* Check each client's 'pulse' at a fixed interval to see if any connections are 'dead' */
		const time_t current_time = time(NULL);
		if (difftime(current_time, previous_pulse_send_time) >= pulse_check_frequency_secs) {
			previous_pulse_send_time = current_time;
			if ((poll_sockfds = check_clients_pulse(
				poll_sockfds,
				&poll_sockfds_alloc_count,
				&poll_sockfds_requests_count
			)) == NULL) break; /* Returns NULL if server closed */
		}

		/* Handle interaction result inputted by user in interactive mode */
		if (server_state == 2) {
			if (handle_interaction_result(
				poll_sockfds,
				&interactive_mode_data,
				&poll_sockfds_alloc_count,
				&poll_sockfds_requests_count
			) == NULL) return; /* Returns NULL if server closed. */
			server_state = 1; /* Reset server to default state */
			continue;
		}

		if (check_error(poll_events_recieved, "(Main) Error encountered whilst polling", 0) == -1) continue;
		if (poll_events_recieved == 0) continue; /* Poll timeout */

		/* If the server socket is ready to read (first pollfd object), a new connection is available.
		   The new client socket is immediately closed if the server reached the client limit. */
		const size_t original_requests_count = poll_sockfds_requests_count;
		if ((poll_sockfds->revents & POLLIN)) {
			poll_sockfds = accept_new_client(
				server_sockfd,
				poll_sockfds,
				&poll_sockfds_alloc_count,
				&poll_sockfds_requests_count,
				(maximum_requests > 0) &&
				        (poll_sockfds_requests_count >= (size_t)maximum_requests) 
			);
			poll_sockfds->revents = 0; /* Reset server's 'recieved events' bitmask */
		}

		/* 
		   All other pollfd objects after the initial server index refer to connected clients.
		   Each client poll request is checked to see if a read or disconnect event occurred and acts accordingly.
		   Using original request count avoids iterating through a newly added client, which will initally have no events.
		   (Need pointers to each poll request, so pointer iteration is more practical)
		*/
		for (struct pollfd *current_poll_sockfd = poll_sockfds + 1,
		     *poll_sockfds_end = poll_sockfds + original_requests_count;
		     current_poll_sockfd != poll_sockfds_end;
		     ++current_poll_sockfd
		) {
			if (server_state == 0) break; /* Check if server closed whilst handling clients */
			if ((current_poll_sockfd->revents & (POLLIN | POLLHUP)) == 0) continue; /* Check for valid events */
			poll_sockfds = handle_client_request(
				poll_sockfds,
				current_poll_sockfd,
				client_response_buffer,
				client_response_buffer_size,
				&poll_sockfds_alloc_count,
				&poll_sockfds_requests_count
			);
		}
	} while (server_state);

	printf("\n(Main) Closing server...\n");

	/* Close all sockets and free allocated memory */
	for (size_t i = 0; i < poll_sockfds_requests_count; ++i) close(poll_sockfds[i].fd);
	free(poll_sockfds);
}


void *begin_interaction(void *v_interact_data)
{
	struct server_interact_data *interact_data = (struct server_interact_data*)v_interact_data;

	const size_t interact_message_size = 0xFFFF;
	interact_data->interact_message = malloc(interact_message_size);
	if (check_error_null(
		interact_data->interact_message,
		"(Interactive) Failed to allocate message buffer", 0
	) == -1) return NULL;

	const char all_interact_message[] = "all";
	const char kick_interact_message[] = "kick";
	const char exit_interact_message[] = "exit";
	const char stopint_interact_message[] = "stopint";

	printf("(Interactive) Format: \"<id> <message>\"\n");
	printf("(Interactive) 'ID' can be 'all' to specify all connected clients, 'Message' can be 'kick' to disconnect the target client(s).\n");
	printf("(Interactive) 'stopint' exits interactive mode and 'exit' stops the server.\n");

	do {
		/* Attempt to get input from stdin */
		size_t input_message_length = get_stdin_input(interact_data->interact_message, interact_message_size);
		if (check_error((int)(input_message_length - 1), "(Interactive) Failed to get input message", 0) == -1) continue;

		/* Determine 'target' of input */
		size_t input_space_index = 0;
		while (interact_data->interact_message[input_space_index] > ' ') ++input_space_index;
		if (input_space_index == 0) goto warn_invalid_input;

		/* Check for 'all' target, otherwise get the client ID by converting to a number. */
		interact_data->interact_target = -1; /* Will remain -1 if an invalid target is specified */
		if (strstr(
			interact_data->interact_message,
			all_interact_message
		) != NULL) interact_data->interact_target = 0;
		else {
			const long input_target_client = strtol(interact_data->interact_message, NULL, 10);
			if (input_target_client != 0) interact_data->interact_target = (int)input_target_client;
		}

		/* Check for server exit message */
		if (strstr(
			interact_data->interact_message,
			exit_interact_message
		) != NULL) {
			server_state = 0; /* Server has ended */
			break;
		}
		/* Check for interactive mode exit message */
		else if (strstr(
			interact_data->interact_message,
			stopint_interact_message
		) != NULL) {
			printf("(Interactive) The server will no longer accept input.\n");
			break;
		}
		/* Could not determine target AND string was not a specific command */
		else if (interact_data->interact_target == -1) goto warn_invalid_input;

		/* Determine if input is a kick command or a message to send to the client(s) */
		interact_data->interact_message += input_space_index + 1;
		if (strcasecmp(
			interact_data->interact_message,
			kick_interact_message
		) == 0) *interact_data->interact_message = '\0';
		else interact_data->interact_message_bytes = strlen(interact_data->interact_message) + 1;

		server_state = 2; /* Set server as ready to execute given input */
		while (server_state == 2) sleep(1); /* Wait for execution to finish */
		continue;
	warn_invalid_input:
		printf("(Interactive) Invalid input.\n");
		continue;
	} while (server_state);

	/* Free memory allocated by message string */
	free(interact_data->interact_message);
	return NULL;
}

struct pollfd *handle_interaction_result(
	struct pollfd *poll_sockfds,
	struct server_interact_data *interact_data,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
) {
	const int is_single_client = interact_data->interact_target != 0;
	const int is_kick_command = *interact_data->interact_message == '\0';

	/* Get pointers to each poll request */
	for (struct pollfd *current_poll_sockfd = poll_sockfds + 1, /* Avoid initial server poll request */
	     *poll_sockfds_end = poll_sockfds + *poll_sockfds_requests_count;
	     current_poll_sockfd != poll_sockfds_end;
	     ++current_poll_sockfd
	) {
		if (server_state == 0) return NULL; /* Server has ended, stop execution */
		/* Only operate on a specific clients if specified (target of 0 means all) */
		if (interact_data->interact_target != 0 &&
		    interact_data->interact_target != current_poll_sockfd->fd
		) continue;

		/* A kick command is specifed with a NULL message */
		if (is_kick_command) {
			const int original_sockfd = current_poll_sockfd->fd;

			poll_sockfds = remove_pollfds_list(
				poll_sockfds,
				current_poll_sockfd,
				poll_sockfds_alloc_count,
				poll_sockfds_requests_count
			);
			/* Update 'end' pointer as poll requests list has updated */
			poll_sockfds_end = poll_sockfds + *poll_sockfds_requests_count;
			/* Current index now points to a different client due to removal, avoiding skipping */
			--current_poll_sockfd;

			if (is_single_client) {
				printf("(Interactive) Kicked client %d.\n", original_sockfd);
				return poll_sockfds;
			}
		}
		/* Send message to target client(s) */
		else if (check_error((int)send_bytes(
			current_poll_sockfd->fd,
			interact_data->interact_message,
			interact_data->interact_message_bytes
		), "(Interactive) Failed to send message to target client", 0) != -1) {
			if (is_single_client) {
				printf("(Interactive) Sent message to client %d.\n", current_poll_sockfd->fd);
				return poll_sockfds;
			}
		} else if (is_single_client) {
			/* An error occurred whilst sending a message to a single client, return normally. */
			return poll_sockfds;
		}
	}

	/* In the case of a specific client, it returns on completion, so reaching here */
	if (is_single_client) printf("(Interactive) Client %d does not exist.\n", interact_data->interact_target);
	/* Result messages for operating on all clients */
	else if (is_kick_command) printf("(Interactive) Kicked %d client(s).\n", (int)*poll_sockfds_requests_count - 1);
	else printf("(Interactive) Sent message to %d client(s).\n", (int)*poll_sockfds_requests_count - 1);

	return poll_sockfds;
}


struct pollfd *check_clients_pulse(
	struct pollfd *poll_sockfds,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
) {
	/*
	   This should be run occassionally to check for any 'dead' sockets where
	   the client disconnected but no message reached the server. A message
	   is sent to each client to warrant an eventual response from them.

	   If a client takes too long to respond to any of the repeated 'pulse' messages,
	   it can safely be assumed that the client has disconnected through unexpected
	   means, so they are removed from the poll requests list.
	*/

	/* Need pointers to each poll request, so pointer iteration is more practical */
	for (struct pollfd *current_poll_sockfd = poll_sockfds + 1, /* Avoid initial server poll request */
	     *poll_sockfds_end = poll_sockfds + *poll_sockfds_requests_count;
	     current_poll_sockfd != poll_sockfds_end;
	     ++current_poll_sockfd
	) {
		/* Server could be stopped at any moment, so this needs to be checked every iteration.
		   Return NULL as the new poll requests list to warn of this. */
		if (server_state == 0) return NULL;

		/* If a read event is available for this client, ignore this pulse check
		   as it could either mean a response or a disconnect event. */
		if (current_poll_sockfd->revents & POLLIN) continue;

		/* 
		   To track the client's pulse without having to store another array (since the 'pollfd'
		   objects list must be seperate), we can make use of the 'error' bits "which are always
		   implicitly polled for", and therefore have no effect in the 'events' field.
		*/
		int client_current_pulse = (current_poll_sockfd->events >> 3) & 3;

		/*
		   Subtract from the pulse counter, deleting the client if it has 'died' (pulse < 1).
		   The index should not change next iteration as the same index now refers to a
		   different client (the order of previous clients is not affected).
		*/
		if (--client_current_pulse <= 0) {
			printf("(Main) Disconnecting client %d: Not responding to pulse checks\n", current_poll_sockfd->fd);
			poll_sockfds = remove_pollfds_list(
				poll_sockfds,
				current_poll_sockfd--, /* Decrement to operate on new client at the same index due to removal*/
				poll_sockfds_alloc_count,
				poll_sockfds_requests_count
			);

			/* Poll request list changed; update 'end' pointer to reflect this. */
			poll_sockfds_end = poll_sockfds + *poll_sockfds_requests_count;
			continue; /* Client no longer exists, move on to new client at the same index */
		}

		/* Decrement client pulse by clearing the bits and storing the new value */
		current_poll_sockfd->events &= ~(3 << 3);
		current_poll_sockfd->events |= (short)(client_current_pulse << 3);

		/* Attempt to send the 'pulse' message to the client */
		check_error((int)send_bytes(
			current_poll_sockfd->fd,
			&network_global_pulse_message,
			network_global_pulse_bytes
		), "(Main) Failed to send pulse to client", 0);
	}
	
	return poll_sockfds;
}


struct pollfd *accept_new_client(
	int server_sockfd,
	struct pollfd *poll_sockfds,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count,
	int deny_connection
) {
	struct sockaddr_in client_address;
	struct sockaddr *client_address_ptr = (struct sockaddr*)&client_address;
	socklen_t sockaddr_in_bytes = sizeof client_address;

	/* Accept a valid connection from a new client */
	int new_client_sockfd;
	if (check_error(new_client_sockfd = accept(
		server_sockfd,
		client_address_ptr,
		&sockaddr_in_bytes
	), "(Main) Connection accept failed", 0) == -1) return poll_sockfds;

	/* Check if the server wants to deny this request for any reason, usually due to client limit. */
	if (deny_connection) {
		close(new_client_sockfd);
		printf("(Main) Failed to connect client: Reached client limit\n");
		return poll_sockfds;
	}
	
	/* Add the new client to the poll requests list */
	struct pollfd* new_poll_sockfds = add_pollfds_list(
		poll_sockfds,
		new_client_sockfd,
		poll_sockfds_alloc_count,
		poll_sockfds_requests_count
	);
	
	/* Error occurred whilst expanding the poll request list to fit a new one;
	   cannot accommodate the new client */
	if (new_poll_sockfds == NULL) {
		close(new_client_sockfd);
		printf("(Main) Failed to connect client: Data allocation error\n");
		return poll_sockfds; /* Return original list */
	}

	/* Get the client's IP address string from the given address object for printing.
	   Use fallback instead if conversion failed. 
	*/
	char client_ip_buffer[INET_ADDRSTRLEN];
	if (check_error_null(inet_ntop(
		client_address.sin_family,
		&client_address.sin_addr,
		client_ip_buffer,
		(socklen_t)(sizeof client_ip_buffer)
	), "Failed to convert client address", 0)) {
		const char client_fallback_ip_buffer[] = "Unknown";
		memcpy(client_ip_buffer, client_fallback_ip_buffer, sizeof client_fallback_ip_buffer);
	};

	printf("(Main) Connected with client '%s' (socket ID %d)\n", client_ip_buffer, new_client_sockfd);

	return new_poll_sockfds;
}

struct pollfd *handle_client_request(
	struct pollfd *poll_sockfds,
	struct pollfd *client_sockfd,
	char *client_response_buffer,
	size_t client_response_buffer_bytes,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_request_count
) {
	ssize_t total_bytes_recieved;

	/* Close the connection if the 'recieved events' bitmask includes a 'disconnect' event. */
	if (client_sockfd->revents & POLLHUP) goto delete_client_request;

	/* 
	   Continuously reads the data the client sent to the given buffer until there
	   is none left (terminator/new line) or the maximum buffer size was reached.
	   A return value of 0 bytes means the client has disconnected, -1 means an error.
	*/
	total_bytes_recieved = recieve_bytes(client_sockfd->fd, client_response_buffer, client_response_buffer_bytes);
	if (total_bytes_recieved == 0) goto delete_client_request;

	check_error((int)total_bytes_recieved, "(Main) Failed to recieve client data", 0);

	/* Reset 'pulse' counter of client as the client is still connected, stored as 2 bits in the 'events' field
	(specifically where error bits are set) for reasons explained in the 'pulse check' function. */
	client_sockfd->events |= (3 << 3);
	client_sockfd->revents = 0; /* Reset 'recieved' event bitmask */

	if (*client_response_buffer != network_global_pulse_message) {
		printf("(Client %d message) %s\n", client_sockfd->fd, client_response_buffer);
	}

	goto client_response_completed; /* Don't remove client, only return from function */

delete_client_request:
	/* Remove client from the poll requests list */
	printf("(Main) Disconnected client %d: External disconnection\n", client_sockfd->fd);
	poll_sockfds = remove_pollfds_list(
		poll_sockfds,
		client_sockfd,
		poll_sockfds_alloc_count,
		poll_sockfds_request_count
	);
client_response_completed:
	return poll_sockfds;
}


struct pollfd *add_pollfds_list(
	struct pollfd *poll_sockfds,
	int new_client_sockfd,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
) {	
	/* 
	   This will double the size of the poll requests list if the number of clients has reached the
	   element count of the list, to accomodate possible future additions to the list. If the expansion
	   (realloc) fails, return 0 to signal this. Using '0' is fine as an error since in normal circumstances,
	   there is always at least one request (that of the server, which is never removed except for closing).
	*/
	if (*poll_sockfds_requests_count >= *poll_sockfds_alloc_count) {
		void *new_poll_sockfds = realloc(
			poll_sockfds,
			sizeof *poll_sockfds * (*poll_sockfds_alloc_count *= 2)
		);
		if (check_error_null(
			new_poll_sockfds,
			"(Main) Failed to expand poll requests list", 0
		) == -1) return NULL;
		/* If realloc returned a new pointer, set it as the new list */
		if (poll_sockfds != new_poll_sockfds) poll_sockfds = new_poll_sockfds;
	}

	/* 
	   Add the new client socket to the end of the poll requests list and set it to
	   listen for read events, and with their 'pulse' counter at the maximum, stored as
	   2 bits representing the 'error' bits as explained in the 'pulse check' function.
	   This should be done AFTER extension to avoid possibly modifying outside the requests list.
	*/
	struct pollfd *new_pollfd_entry = poll_sockfds + (*poll_sockfds_requests_count)++;
	new_pollfd_entry->fd = new_client_sockfd;
	new_pollfd_entry->events = POLLIN | (3 << 3);
	new_pollfd_entry->revents = 0;

	return poll_sockfds; /* Return the (possibly changed or expanded) poll requests list */
}

struct pollfd *remove_pollfds_list(
	struct pollfd *poll_sockfds,
	struct pollfd *toremove_poll_sockfd,
	size_t *poll_sockfds_alloc_count,
	size_t *poll_sockfds_requests_count
) {
	size_t poll_sockfds_threshold_count = 4;
	void *new_poll_sockfds;

	/* Attempt to close the given socket to disable further interactions */
	close(toremove_poll_sockfd->fd);

	/* Decrement the total number of clients ('--' operation on the value, not the pointer) */
	const size_t new_poll_sockfds_requests_count = --(*poll_sockfds_requests_count);

	/* 
	   Only the file descriptor in each pollfd object is different, so make the current (obsolete) pollfd object
	   use the last one, which is not accessed otherwise due to decrementing the number of connected clients above.
	   This should be done BEFORE shrinking to avoid invalidating the element(s) at the end and then accessing them.
	*/
	toremove_poll_sockfd->fd = poll_sockfds[new_poll_sockfds_requests_count].fd;

	/* 
	   If the poll requests list is too large compared to the number of clients, shrink it (half)
	   to save on memory. A single 'pollfd' object is only 1 int and 2 shorts, so this should not 
	   be done excessively as the performance implications of reallocating outweighs saving a few bytes.
	   No shrinking is done on 'realloc' failure to reduce the number of points of failure for the server.
	*/
	poll_sockfds_threshold_count = *poll_sockfds_alloc_count / 2;

	if (new_poll_sockfds_requests_count < poll_sockfds_threshold_count) {
		/* If 'realloc' returns NULL, continue without shrinking. If it returns a different pointer, use it as
		   the new poll requests list. Otherwise, realloc succeeded and the poll requests list has been shrunk. */
		new_poll_sockfds = realloc(
			poll_sockfds,
			sizeof *poll_sockfds * (*poll_sockfds_alloc_count = poll_sockfds_threshold_count)
		);
		if (poll_sockfds != new_poll_sockfds) poll_sockfds = new_poll_sockfds;
	}

	return poll_sockfds; /* Return the (possibly modified) poll requests list */
}


void signal_server_end(int param)
{
	(void)param; /* Hide unused argument warning */
	if (server_state == 2) return; /* Ignore interrupt from 'interactive' mode */
	server_state = 0; /* Stop the server as soon as possible. */
}


#ifdef __cplusplus
}
#endif
