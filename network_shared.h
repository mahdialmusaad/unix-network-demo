/*
	Copyright 2025 Mahdi Almusaad (https://github.com/mahdialmusaad)
	under the MIT License (https://opensource.org/license/mit)
*/

#pragma once
#ifndef NETWORK_DEMO_SHARED_H
#define NETWORK_DEMO_SHARED_H

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

char network_global_pulse_message = '\3';
char network_global_pulse_null_response = '\3';
const size_t network_global_pulse_bytes = sizeof network_global_pulse_message;

/* ---- Helper functions for client and server ---- */

/* Repeatedly recieves a limited amount data from the target socket/file descriptor until there is none left.
   Returns recieved bytes on success, 0 on disconnect and -1 on error. */
static ssize_t recieve_bytes(int target_sockfd, char *target_buffer, size_t max_operation_bytes) {
	size_t total_bytes_operated = 0;
	ssize_t recent_bytes_operated = 0;

	do {
		const size_t next_operation_size = max_operation_bytes - total_bytes_operated;
		char *next_buffer_operation = target_buffer + total_bytes_operated;
		recent_bytes_operated = recv(target_sockfd, next_buffer_operation, next_operation_size, 0);

		if (recent_bytes_operated == 0) return 0; /* Disconnected */
		if (recent_bytes_operated == -1) return -1; /* Recieve error */
		if ((total_bytes_operated += (size_t)recent_bytes_operated) >= max_operation_bytes) goto place_null_terminator_return; /* Maximum buffer size reached */

		char last_operated_char = target_buffer[total_bytes_operated - 1];
		/* End of buffer reached */
		if (last_operated_char == '\0' || last_operated_char == network_global_pulse_message) goto no_place_terminator_return;
		else if (last_operated_char == '\n') goto place_null_terminator_return;
	} while (1);


place_null_terminator_return:
	/* Place null terminator at the end of the buffer */
	target_buffer[total_bytes_operated - 1] = '\0';
no_place_terminator_return:
	/* Return the total number of bytes operated on */
	return (ssize_t)total_bytes_operated;
}

/* Repeatedly sends a limited amount data to the target socket/file descriptor until there is none left from the given buffer.
   Returns sent bytes on success and -1 on error. */
static ssize_t send_bytes(int target_sockfd, const char *target_buffer, size_t max_operation_bytes)
{
	size_t total_bytes_operated = 0;
	ssize_t recent_bytes_operated = 0;

	do {
		const size_t next_operation_size = max_operation_bytes - total_bytes_operated;
		const char *next_buffer_operation = target_buffer + total_bytes_operated;
		recent_bytes_operated = send(target_sockfd, next_buffer_operation, next_operation_size, 0);

		if (recent_bytes_operated < 1) return -1; /* Send error */
		if ((total_bytes_operated += (size_t)recent_bytes_operated) >= max_operation_bytes) break; /* Maximum buffer size reached */

		const char last_operated_char = target_buffer[total_bytes_operated - 1];
		if (last_operated_char == '\0' || last_operated_char == '\n') break; /* End of buffer reached */
	} while (1);

	/* Return the total number of bytes operated on */
	return (ssize_t)total_bytes_operated;
}

/* Get null-terminated input from stdin. Returns 0 on error and the length of the input otherwise */
size_t get_stdin_input(char *input_buffer, size_t max_input_size)
{
	if (fgets(input_buffer, (int)max_input_size, stdin) == NULL) return 0;
	size_t input_message_len = strlen(input_buffer);
	if (input_message_len > max_input_size) input_message_len = max_input_size;
	input_buffer[input_message_len - 1] = '\0';
	return input_message_len;
}

/* Prints the given error message if the result of a function was equal to -1 (an error occurred) and the errno description.
   Exits the program if '_exit' evaluates to true and returns the given result otherwise. */
int check_error(int func_result, const char *onerror_message, int _exit)
{
	/* Check if the given function failed */
	if (func_result == -1) {
		perror(onerror_message);
		if (_exit) exit(EXIT_FAILURE);
	}

	/* Return normal result */
	return func_result;
}
/* Same as 'check_error(...)' but for functions where a null pointer is passed rather than -1 on failure. */
int check_error_null(const void *func_result, const char *onerror_message, int _exit)
{
	return check_error(-(func_result == NULL), onerror_message, _exit);
}
/* Returns either the IPv4 or IPv6 address of the given socket address depending on the set socket family. */
void *get_ipvx_address(struct sockaddr *in_socket_address)
{
	if (in_socket_address->sa_family == AF_INET) return &(((struct sockaddr_in*)in_socket_address)->sin_addr);
	return &(((struct sockaddr_in6*)in_socket_address)->sin6_addr);
}

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_DEMO_SHARED_H */
