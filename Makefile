all: server.c
	gcc server.c -o write_server
	gcc server.c -D READ_SERVER -o read_server
clean:
	rm -f read_server write_server
