CC = g++
CFLAGS = -g -Wall -std=c++1z
CLIENT_SRC = common.cpp client.cpp
TRACKER_SRC = common.cpp tracker.cpp

all: client tracker

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC) -lpthread -lcrypto

tracker: $(TRACKER_SRC)
	$(CC) $(CFLAGS) -o $@ $(TRACKER_SRC)

clean:
	rm client tracker
