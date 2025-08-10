.SILENT:

.PHONY: all
all: server client

CFLAGS = -Wall -Wconversion -Wextra -Wpedantic

server: .FORCE
	cc server.c -O2 $(CFLAGS) -o server
client: .FORCE
	cc client.c -O2 $(CFLAGS) -o client

.PHONY: .FORCE
.FORCE:

.PHONY:clean
clean:
	rm -f server
	rm -f client
