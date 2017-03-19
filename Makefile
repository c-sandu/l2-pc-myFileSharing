server: server.cpp utils.h utils.cpp commands.h commands.cpp
	g++ -g server.cpp utils.h utils.cpp commands.h commands.cpp -o server -Wall

client: client.cpp utils.h utils.cpp commands.h commands.cpp
	g++ -g client.cpp utils.h utils.cpp commands.h commands.cpp -o client -Wall

build: server client

clean:
	rm -f server client