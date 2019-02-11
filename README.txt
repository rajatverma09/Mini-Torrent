1) Synchronization between two trackers has not been implemented
    Assumption: Only a single tracker exists.

2) Command to build tracker and client executables:
    g++ -std=c++11 -o tracker common.cpp tracker.cpp
    g++ -std=c++11 -o client common.cpp client.cpp -lpthread -lcrypto
