all: server.c bidding
	gcc server.c bidding.o -o write_server
	gcc server.c bidding.o -D READ_SERVER -o read_server

bidding: bidding.c
	gcc bidding.c -c

clean:
	rm -f read_server write_server
	rm *.o
