## Overview
A simple network client-and-server demo written in C for UNIX-compliant systems, allowing simple chatting between a server and its clients. Features a command system, connection checking and dynamic allocation for the server.
## Usage
### Client
Run the client executable with the following arguments:
- address: The server's address or name. An example could be `localhost` or your device's name to connect to a server running on the same device, or an IP address.
- port: The port of the server. This can be a number between 1024 and 65535.

After connecting, you can type in a message to be sent to the server. Any incoming messages from the server will be shown as well.
### Server
Run the server executable with the following arguments:
- port: The port to start the server on. Follows the same rules as that of the client above.
- max-clients: The maximum number of clients allowed to be connected. A negative value will remove this limit.
- interactive-mode: A non-zero value will enable interactive mode, where you can type in commands as input, as specified below.
### Commands (server)
Commands written in the 'interactive' mode of the server are as follows (keywords are case-sensitive):
- exit: Initiates a clean shutdown of the server.
- stopint: Exits interactive mode. The server will continue running but no more commands can be issued.
- "\<ID\> \<Message\>": Sends the given client ID the following message.
- "\<ID\> kick": Kicks the given client ID.
The 'ID' can instead be 'all' to operate on all connected clients.
## Build
To compile the client and server source files, you can run `make` with the provided [Makefile](Makefile).