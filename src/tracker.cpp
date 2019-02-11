#include "includes.h"
#include "common.h"

using namespace std;

#include <cstdio>
#include <openssl/sha.h>
#include <vector>
#include <map>
#include <set>

#include <stdio.h>  
#include <string.h>       //strlen  
#include <stdlib.h>  
#include <errno.h>  
#include <unistd.h>       //close  
#include <arpa/inet.h>    //close  
#include <sys/types.h>  
#include <sys/socket.h>  
#include <netinet/in.h>  
#include <sys/time.h>     //FD_SET, FD_ISSET, FD_ZERO macros  
     
#define TRUE       1  
#define FALSE      0  
#define PORT       4500
#define MAX_CONNS  100


#define PIECE_SIZE     (512 * 1024)
#define pb             push_back

#define FAILURE        -1
#define SUCCESS        0

static int cursor_r_pos;
static int cursor_c_pos;

static bool is_status_on;
static string seeder_file_path, log_file_path;
static string my_tracker_addr, other_tracker_addr;
static string my_tracker_ip, other_tracker_ip;
static int my_tracker_port, other_tracker_port;
string working_dir;

map<string, set<string>> seeder_map;

enum client_request
{
    SHARE,
    GET,
    REMOVE_TORRENT,
    REMOVE_ALL
};

void status_print(int result, string msg);

void cursor_init()
{
    cout << "\033[" << cursor_r_pos << ";" << cursor_c_pos << "H";
    cout.flush();
}

void screen_clear()
{
    cout << "\033[3J" << "\033[H\033[J";
    cout.flush();
    cursor_r_pos = cursor_c_pos = 1;
    cursor_init();
}

string current_timestamp_get()
{
    time_t tt; 
    struct tm *ti; 

    time (&tt); 
    ti = localtime(&tt);
    return asctime(ti);
}

void fprint_log(string msg)
{
    ofstream out(log_file_path, ios_base::app);
    if(!out)
    {
        string err_str = "Error: ";
        err_str = err_str + strerror(errno);
        status_print(FAILURE, err_str);
        return;
    }
    string curr_timestamp = current_timestamp_get();
    curr_timestamp.pop_back();
    out << curr_timestamp << " : " << "\"" << msg << "\"" << "\n";
}

void fprint_seeder_info(string msg)
{
    ofstream out(seeder_file_path, ios_base::app);
    if(!out)
    {
        string err_str = "Error: ";
        err_str = err_str + strerror(errno);
        status_print(FAILURE, err_str);
        return;
    }
    out << msg << "\n";
}

void seederlist_recreate()
{
    ofstream out(seeder_file_path, ios_base::trunc);
    if(!out)
    {
        string err_str = "Error: ";
        err_str = err_str + strerror(errno);
        status_print(FAILURE, err_str);
        return;
    }
    for(auto mitr = seeder_map.begin(); mitr != seeder_map.end(); ++mitr)
    {
        string double_sha1_str = mitr->first;
        set<string>& s = mitr->second;
        for(auto sitr = s.begin(); sitr != s.end(); ++sitr)
        {
            out << double_sha1_str << "$" << *sitr << "\n";
        }
    }
}

void client_request_handle(int sock, string req_str)
{
    int dollar_pos = req_str.find('$');
    string cmd = req_str.substr(0, dollar_pos);
    req_str = req_str.substr(dollar_pos + 1);
    int req = stoi(cmd);

    switch(req)
    {
        case SHARE:
        {
            dollar_pos = req_str.find('$');
            string double_sha1_str = req_str.substr(0, dollar_pos);
            req_str.erase(0, dollar_pos+1);

            string client_addr_str = req_str;

            auto itr = seeder_map.find(double_sha1_str);
            if(itr == seeder_map.end())
            {
                seeder_map[double_sha1_str] = set<string>();
                itr = seeder_map.find(double_sha1_str);
            }
            set<string>& s = itr->second;
            s.insert(client_addr_str);

            fprint_log(double_sha1_str + " shared by client " + client_addr_str); 
            fprint_seeder_info(double_sha1_str + "$" + client_addr_str);
            break;
        }

        case GET:
        {
            string double_sha1_str = req_str;

            auto itr = seeder_map.find(double_sha1_str);
            set<string>& s = itr->second;
            auto sitr = s.begin();
            string seeder_addrs = *sitr;
            ++sitr;
            for(; sitr != s.end(); ++sitr)
            {
                seeder_addrs += "$" + *sitr;
            }

            int sz = seeder_addrs.length();
            send(sock, &sz, sizeof(sz), 0);
            send(sock, seeder_addrs.c_str(), seeder_addrs.length(), 0);
            fprint_log("For sha1: " + double_sha1_str + ", seeder List: " + seeder_addrs); 
            break;
        }

        case REMOVE_TORRENT:
        {
            dollar_pos = req_str.find('$');
            string double_sha1_str = req_str.substr(0, dollar_pos);
            req_str.erase(0, dollar_pos+1);

            string client_addr_str = req_str;

            auto itr = seeder_map.find(double_sha1_str);
            set<string>& s = itr->second;
            s.erase(client_addr_str);
            fprint_log("Client: " + client_addr_str + " stopped seeding " + double_sha1_str); 
            seederlist_recreate();
            break;
        }

        case REMOVE_ALL:
        {
            string client_addr_str = req_str;
            for(auto mitr = seeder_map.begin(); mitr != seeder_map.end(); ++mitr)
            {
                set<string>& s = mitr->second;
                s.erase(client_addr_str);
            }
            seederlist_recreate();
            break;
        }

        default:
            break;
    }
}

void status_print(int result, string msg)
{
    if(is_status_on)
        return;

    is_status_on = true;
    if(FAILURE == result)
        cout << "\033[1;31m" << msg << "\033[0m";   // RED color
    else
        cout << "\033[1;32m" << msg << "\033[0m";   // GREEN color

    cout.flush();
}

void data_read(int sock, char* read_buffer, int size_to_read)
{
    int bytes_read = 0;
    do
    {
        bytes_read += read(sock, read_buffer + bytes_read, size_to_read);   // read in a loop
        fprint_log("Bytes read: " + to_string(bytes_read));
    }while(bytes_read < size_to_read);
}

void seeder_list_fetch()
{
    int pos = seeder_file_path.find_last_of('/');
    string temp_file_path = seeder_file_path.substr(0, pos+1) + "temp_file.txt";
    rename(seeder_file_path.c_str(), temp_file_path.c_str());

    ifstream in(temp_file_path);
    if(!in)
    {
        return;
    }

    string line_str;

    while(in)
    {
        in >> line_str;
        client_request_handle(0, to_string(SHARE) + "$" + line_str);
    }
    if(FAILURE == unlinkat(0, temp_file_path.c_str(), 0))
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
    }
}

void tracker_run()
{
    int opt = TRUE;   
    int tracker_socket , addrlen , new_socket , client_socket[MAX_CONNS],
        max_clients = MAX_CONNS , activity, i , valread , sd;   
    int max_sd;
    struct sockaddr_in address;   

    seeder_list_fetch();

    fd_set readfds;

    for (i = 0; i < max_clients; i++)   
    {   
        client_socket[i] = 0;   
    }   
  
    if( (tracker_socket = socket(AF_INET , SOCK_STREAM , 0)) == 0)   
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): socket failed!!";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);
    }   

    //set master socket to allow multiple connections ,  
    //this is just a good habit, it will work without this  
    if( setsockopt(tracker_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 )   
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): setsockopt";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);   
    }   
     
    //type of socket created  
    address.sin_family = AF_INET;   
    address.sin_addr.s_addr = INADDR_ANY;   
    address.sin_port = htons( my_tracker_port );   
         
    //bind the socket to localhost port 4500
    if (bind(tracker_socket, (struct sockaddr *)&address, sizeof(address))<0)   
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): bind failed";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);   
    }   
    stringstream ss;
    ss << "Listening on port " << my_tracker_port;
    fprint_log(ss.str());
         
    //try to specify maximum of 100 pending connections for the master socket  
    if (listen(tracker_socket, MAX_CONNS) < 0)   
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): listen failed";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);   
    }   
         
    //accept the incoming connection  
    addrlen = sizeof(address);

    while(TRUE)
    {   
        //clear the socket set  
        FD_ZERO(&readfds);   

        //add master socket to set  
        FD_SET(tracker_socket, &readfds);   
        max_sd = tracker_socket;   

        //add child sockets to set  
        for ( i = 0 ; i < max_clients ; i++)   
        {   
            //socket descriptor  
            sd = client_socket[i];   

            //if valid socket descriptor then add to read list  
            if(sd > 0)   
                FD_SET( sd , &readfds);   
                 
            //highest file descriptor number, need it for the select function  
            if(sd > max_sd)   
                max_sd = sd;   
        }   
     
        // wait for an activity on one of the sockets , timeout is NULL ,  
        // so wait indefinitely  
        activity = select( max_sd + 1 , &readfds , NULL , NULL , NULL);   
       
        if ((activity < 0) && (errno != EINTR))   
        {   
            continue;
        }

        // If something happened on the master socket ,  
        // then its an incoming connection  
        if (FD_ISSET(tracker_socket, &readfds))   
        {   
            if ((new_socket = accept(tracker_socket,  
                    (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)   
            {   
                stringstream ss;
                ss << __func__ << " (" << __LINE__ << "): accept() failed!!";
                fprint_log(ss.str());
                exit(EXIT_FAILURE);   
            }   
             
            //inform user of socket number - used in send and receive commands
            stringstream ss;
            ss << __func__ << " (" << __LINE__ << "): New connection, socket fd is " << new_socket
               << inet_ntoa(address.sin_addr) << ":" << ntohs(address.sin_port);
            fprint_log(ss.str());

            //add new socket to array of sockets  
            for (i = 0; i < max_clients; i++)   
            {   
                //if position is empty  
                if( client_socket[i] == 0 )   
                {   
                    client_socket[i] = new_socket;   
                    break;
                }   
            }   
        }   
        else
        {
            for (i = 0; i < max_clients; i++)
            {
                sd = client_socket[i];

                if (FD_ISSET( sd , &readfds))
                {
                    int read_size;
                    valread = read(sd, &read_size, sizeof(read_size));
                    if(FAILURE == valread)
                    {
                        status_print(FAILURE, "Read failed!!");
                        fprint_log("Read failed!! seeder_run() ");
                        close(sd);
                        client_socket[i] = 0;
                    }
                    else if(0 == valread)
                    {
                        // Somebody disconnected, get his details and print  
                        getpeername(sd , (struct sockaddr*)&address, (socklen_t*)&addrlen);
                        stringstream ss;
                        ss << "Host disconnected!! ip: " << inet_ntoa(address.sin_addr) << " port: " << ntohs(address.sin_port);
                        fprint_log(ss.str());

                        close(sd);
                        client_socket[i] = 0;
                    }
                    else
                    {
                        char buffer[read_size + 1] = {'\0'};
                        data_read(sd, buffer, read_size);

                        stringstream ss;
                        ss << "Request read: " << buffer;
                        fprint_log(ss.str());
                        getpeername(sd , (struct sockaddr*)&address, (socklen_t*)&addrlen);

                        client_request_handle(sd, buffer);
                    }
                }
            }
        }
    }
}

void ip_and_port_split(string addr, string &ip, int &port)
{
    unsigned int colon_pos = addr.find(':');
    if(colon_pos != string::npos)
    {
        ip = addr.substr(0, colon_pos);
        port = stoi(addr.substr(colon_pos + 1));
    }
}

int main(int argc, char* argv[])
{
    screen_clear();

    if(argc != 5)
    {
        status_print(FAILURE, "Tracker usage: \"./executable <my_tracker_ip>:<my_tracker_port>"
                     " <other_tracker_ip>:<other_tracker_port> <seederlist_file> <log_file>\"\n");
        return 0;
    }

    working_dir = getenv("PWD");
    if(working_dir != "/")
        working_dir = working_dir + "/";

    my_tracker_addr = argv[1];
    other_tracker_addr = argv[2];
    seeder_file_path = abs_path_get(argv[3]);
    log_file_path = abs_path_get(argv[4]);

    ip_and_port_split(my_tracker_addr, my_tracker_ip, my_tracker_port);
    ip_and_port_split(other_tracker_addr, other_tracker_ip, other_tracker_port);

    tracker_run();
    return 0;
}
