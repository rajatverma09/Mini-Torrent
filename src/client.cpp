#include "includes.h"
#include "common.h"

#include <cstdio>
#include <openssl/sha.h>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <algorithm>
#include <set>
#include <sstream>

#include <sys/socket.h> 
#include <cstdlib> 
#include <netinet/in.h> 
#include <arpa/inet.h>
#include <string.h> 
#include <signal.h>
#include <pthread.h>

using namespace std;

#define PIECE_SIZE     (512 * 1024)
#define pb             push_back

#define FAILURE             -1
#define SUCCESS             0
#define ENTER               10
#define ESC                 27
#define UP                  11
#define DOWN                12
#define RIGHT               13
#define LEFT                14
#define BACKSPACE           127
#define SEEDING_FILES_LIST  "seeding_files.txt"
#define TRACKER1            0
#define TRACKER2            1
#define CLIENT              2
#define MAX_DOWNLOADS       100
#define MAX_CONNS           100

static int cursor_r_pos;
static int cursor_c_pos;
static int cursor_left_limit = 1;
static int cursor_right_limit = 1;
string working_dir;
static struct termios prev_attr, new_attr;

map<string, string>   seeding_files_map;
map<string, set<int>> seeding_file_chunks;
map<string, string>   files_downloading;
map<string, string>   files_downloaded;

static bool is_status_on;

static string log_file_path, seeding_files_path, client_exec_path;
static string addr[3];
static string ip[3];
static int port[3];
static int curr_tracker_id;
static mutex download_mtx[MAX_DOWNLOADS];
static mutex g_mtx;
static bool mtx_inuse[MAX_DOWNLOADS];

enum operation
{
    ADD,
    REMOVE
};

enum client_to_tracker_req
{
    SHARE,
    GET,
    REMOVE_TORRENT,
    REMOVE_ALL
};

enum client_to_client_req
{
    GET_CHUNK_IDS,
    GET_CHUNKS
};

void ip_and_port_split(string addr, string &ip, int &port);
void data_read(int sock, char* read_buffer, int size_to_read);

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

void print_mode()
{
    cursor_r_pos = 1;
    cursor_c_pos = 1;
    cursor_init();
    from_cursor_line_clear();

    stringstream ss;
    ss << "[Enter Command] :";

    cout << "\033[1;33;40m" << ss.str() << "\033[0m" << " ";    // YELLOW text and BLACK background
    cout.flush();
    cursor_c_pos = ss.str().length() + 2;       // two spaces
    cursor_init();
    cursor_left_limit = cursor_right_limit = cursor_c_pos;
}

void status_print(int result, string msg)
{
    if(is_status_on)
        return;

    is_status_on = true;
    cursor_c_pos = cursor_left_limit;
    cursor_init();

    from_cursor_line_clear();
    if(FAILURE == result)
        cout << "\033[1;31m" << msg << "\033[0m";	// RED color
    else
        cout << "\033[1;32m" << msg << "\033[0m";	// GREEN color

    cout.flush();
}

string current_timestamp_get()
{
    time_t tt;
    struct tm *ti;

    time (&tt);
    ti = localtime(&tt);
    return asctime(ti);
}

void error_print(string err_msg)
{
    stringstream ss;
    ss << __func__ << " (" << __LINE__ << "): socket failed!!";
}

void fprint_log(string msg)
{
    ofstream out(log_file_path, ios_base::app);
    if(!out)
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
        return;
    }
    string curr_timestamp = current_timestamp_get();
    curr_timestamp.pop_back();
    out << curr_timestamp << " : " << "\"" << msg << "\"" << "\n";
}


int download_id_get()
{
    for(int i = 0; i < MAX_DOWNLOADS; ++i)
    {
        if(!mtx_inuse[i])
            return i;
    }
    return FAILURE;
}


int command_size_check(vector<string> &v, unsigned int min_size, unsigned int max_size, string error_msg)
{
    if(v.size() < min_size || v.size() > max_size)
    {
        status_print(FAILURE, error_msg);
        return FAILURE;
    }
    return SUCCESS;
}

int make_connection(string ip, uint16_t port)
{
    struct sockaddr_in serv_addr; 
    int sock = 0;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    { 
        status_print(FAILURE, "Socket connection error!!");
        return FAILURE; 
    } 

    memset(&serv_addr, '0', sizeof(serv_addr)); 

    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_port = htons(port);

    // Convert IPv4 and IPv6 addresses from text to binary form 
    if(inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr)<=0)  
    { 
        status_print(FAILURE, "Invalid address/ Address not supported");
        return FAILURE;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) 
    { 
        status_print(FAILURE, "Connection with tracker failed!!");
        return FAILURE; 
    } 
 
    return sock;
}

int make_connection_with_tracker()
{
    int sock = make_connection(ip[curr_tracker_id], port[curr_tracker_id]);

    if(FAILURE == sock)
    {
        curr_tracker_id = (curr_tracker_id == TRACKER1) ? TRACKER2 : TRACKER1;
        sock = make_connection(ip[curr_tracker_id], port[curr_tracker_id]);
        if(FAILURE == sock)
        {
            stringstream ss;
            ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
            status_print(FAILURE, ss.str());
            return FAILURE;
        }
    }
    return sock;
}

int send_request(int sock, int req, string str)
{
    string req_op = to_string(req);
    str = req_op + "$" + str;
    int sz = str.length();
    send(sock, &sz, sizeof(sz), 0);
    send(sock, str.c_str(), str.length(), 0); 
    return SUCCESS;
}

void seeding_files_recreate()
{
    ofstream out(seeding_files_path, ios_base::trunc);
    if(!out)
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
        return;
    }
    for(auto itr = seeding_files_map.begin(); itr != seeding_files_map.end(); ++itr)
    {
        out << itr->first << "$" << itr->second << "\n";
    }
}

void update_seeding_map_n_file(operation opn, string double_sha1_str, string file_path = "", string mtorrent_file_path = "")
{
    switch(opn)
    {
        case ADD:
        {
            ofstream out(seeding_files_path, ios_base::app);
            if(mtorrent_file_path.empty())
            {
                seeding_files_map[double_sha1_str] = file_path;
                out << double_sha1_str << "$" << file_path << "\n";
            }
            else
            {
                seeding_files_map[double_sha1_str] = file_path + "$" + mtorrent_file_path;
                out << double_sha1_str << "$" << file_path << "$" << mtorrent_file_path << "\n";
            }
            break;
        }
        case REMOVE:
        {
            seeding_files_map.erase(double_sha1_str);
            seeding_files_recreate();
            break;
        }
        default:
            break;
    }
}

string get_sha1_str(const unsigned char* ibuf, int blen)
{
    char sha1_buff[3] = {'\0'};
    unsigned char obuf[21] = {'\0'};
    string str;

    SHA1(ibuf, blen, obuf);
    for (int i = 0; i < 10; i++) {
        snprintf(sha1_buff, sizeof(sha1_buff), "%02x", obuf[i]);
        str = str + sha1_buff;			// str becomes a string of 40 characters
    }
    return str;
}

int share_request(vector<string> &cmd)
{
    string str;
    string local_file_path, mtorrent_file_path;
    local_file_path = abs_path_get(cmd[1]);
    mtorrent_file_path = abs_path_get(cmd[2]);

    int bytes_read, total_bytes_read = 0, nchunks = 0;
    string sha1_str;

    ifstream infile (local_file_path.c_str(), ios::binary | ios::in);
    if(!infile)
    {
        string err_str = "FAILURE: ";
        err_str = err_str + strerror(errno);
        status_print(FAILURE, err_str);
        return FAILURE;
    }

    while(infile)
    {
        unsigned char ibuf[PIECE_SIZE + 1] = {'\0'};
        infile.read((char*)ibuf, PIECE_SIZE);

        if(infile.fail() && !infile.eof())
        {
            stringstream ss;
            ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
            status_print(FAILURE, ss.str());
            return FAILURE;
        }

        bytes_read = infile.gcount();
        total_bytes_read += bytes_read;

        sha1_str += get_sha1_str(ibuf, bytes_read);            // taking the first 20 characters
    }

    ofstream out(mtorrent_file_path, ios::out);
    int pos = local_file_path.find_last_of("/");
    string local_file_name = local_file_path.substr(pos+1);
    if(out)
    {
        out << addr[TRACKER1] << "\n";
        out << addr[TRACKER2] << "\n";
        out << local_file_name << "\n";
        out << total_bytes_read << "\n";
        out << sha1_str << "\n";
    }

    int sock = make_connection_with_tracker();
    if(FAILURE == sock)
        return FAILURE;

    // applying SHA1 on already created SHA1 string.
    string double_sha1_str = get_sha1_str((const unsigned char*)sha1_str.c_str(), sha1_str.length());

    send_request(sock, SHARE, double_sha1_str + "$" + addr[CLIENT]);
    update_seeding_map_n_file(ADD, double_sha1_str, local_file_path, mtorrent_file_path);
    close(sock);

    int filesize = total_bytes_read; 
    if(filesize % PIECE_SIZE)
        nchunks = (filesize / PIECE_SIZE) + 1;
    else
        nchunks = filesize / PIECE_SIZE;

    auto itr = seeding_file_chunks.find(double_sha1_str);
    if(itr == seeding_file_chunks.end())
    {
        seeding_file_chunks[double_sha1_str] = set<int>();
        set<int> &id_set = seeding_file_chunks[double_sha1_str];
        for(int i = 0; i < nchunks; ++i)
        {
            id_set.insert(i);
        }
    }

    status_print(SUCCESS, "SUCCESS: " + cmd[2]);
    return SUCCESS;
}

void file_chunk_ids_get(string seeder_addr, string double_sha1_str, vector<vector<int>>& chunk_ids_vec, int download_id)
{
    string seeder_ip;
    int seeder_port;
    ip_and_port_split(seeder_addr, seeder_ip, seeder_port);
    int sock = make_connection(seeder_ip, seeder_port);

    send_request(sock, GET_CHUNK_IDS, double_sha1_str);

    int read_size;
    if(FAILURE == read(sock, &read_size, sizeof(read_size)))
    {
        status_print(FAILURE, "Read failed!!");
        fprint_log("Read failed!! file_chunk_ids_get() ");
        close(sock);
        return;
    }
    char file_chunk_ids[read_size + 1] = {'\0'};
    data_read(sock, file_chunk_ids, read_size);
    close(sock);

    string chunk_ids(file_chunk_ids);
    fprint_log(seeder_addr + " has these chunks: " + chunk_ids);

    unsigned int dollar_pos, id;
    vector<int> ids_vec;
    while((dollar_pos = chunk_ids.find('$')) != string::npos)
    {
        id = stoi(chunk_ids.substr(0, dollar_pos));
        ids_vec.push_back(id);
        chunk_ids.erase(0, dollar_pos+1);
    }
    id = stoi(chunk_ids);
    ids_vec.push_back(id);

    lock_guard<mutex> lg(download_mtx[download_id]);
    chunk_ids_vec.push_back(ids_vec);
}

void chunks_download(string double_sha1_str, string reqd_ids_str, string seeder_addr, string dest_file_path, 
                     int download_id, ofstream& fout, string mtorrent_file_path)
{
    string seeder_ip;
    int seeder_port, id;
    unsigned int dollar_pos;
    ip_and_port_split(seeder_addr, seeder_ip, seeder_port);

    int sock = make_connection(seeder_ip, seeder_port);
    if(FAILURE == sock)
        return;

    send_request(sock, GET_CHUNKS, double_sha1_str + "$" + reqd_ids_str);

    auto itr = seeding_file_chunks.find(double_sha1_str);
    if(itr == seeding_file_chunks.end())
    {
        seeding_file_chunks[double_sha1_str] = set<int>();
        itr = seeding_file_chunks.find(double_sha1_str);
    }
    set<int> &s = itr->second;

    bool done = false;
    while(!done)
    {
        if((dollar_pos = reqd_ids_str.find('$')) == string::npos)
        {
            id = stoi(reqd_ids_str);
            done = true;
        }
        else
        {
            id = stoi(reqd_ids_str.substr(0, dollar_pos));
            reqd_ids_str.erase(0, dollar_pos+1);
        }
        {
            lock_guard<mutex> lg(download_mtx[download_id]);

            int read_size;
            stringstream ss;
            ss << this_thread::get_id();

            if(FAILURE == read(sock, &read_size, sizeof(read_size)))
            {
                status_print(FAILURE, "Read failed!!");

                fprint_log("Read failed!! thread id: " + ss.str());
                close(sock);
                return;
            }
            char downloaded_chunk[read_size + 1] = {'\0'};
            fprint_log("chunk id: " + to_string(id));
            data_read(sock, downloaded_chunk, read_size);

            fout.seekp(id * PIECE_SIZE, ios::beg);
            fout.write(downloaded_chunk, read_size);

            if(s.empty())
            {
                int tracker_sock = make_connection_with_tracker();
                if(FAILURE == tracker_sock)
                {
                    close(sock);
                    return;
                }
                send_request(tracker_sock, SHARE, double_sha1_str + "$" + addr[CLIENT]);
                update_seeding_map_n_file(ADD, double_sha1_str, dest_file_path, mtorrent_file_path);
                close(tracker_sock);
            }
            s.insert(id);
        }
    }
    close(sock);
}

void file_download(string double_sha1_str, string seeder_addrs, unsigned long long filesize, string dest_file_path, int download_id, string mtorrent_file_path)
{
    unsigned int dollar_pos, ndollars;
    string addr;
    vector<vector<int>> seeder_chunk_ids;
    vector<string>      seeder_addr_vec;

    int nchunks;
    if(filesize % PIECE_SIZE)
        nchunks = (filesize / PIECE_SIZE) + 1;
    else
        nchunks = filesize / PIECE_SIZE;

    fprint_log("There are total " + to_string(nchunks) + " chunks");

    ndollars = count(seeder_addrs.begin(), seeder_addrs.end(), '$');
    int nseeders = ndollars + 1;
    thread* seeder_thread_arr = new thread[nseeders];

    string ip;
    int i = 0;
    while((dollar_pos = seeder_addrs.find('$')) != string::npos)
    {
        addr = seeder_addrs.substr(0, dollar_pos);
        seeder_addr_vec.push_back(addr);
        seeder_addrs.erase(0, dollar_pos+1);

        seeder_thread_arr[i++] = thread(file_chunk_ids_get, addr, double_sha1_str, ref(seeder_chunk_ids), download_id);
    }
    seeder_addr_vec.push_back(seeder_addrs);
    seeder_thread_arr[i++] = thread(file_chunk_ids_get, seeder_addrs, double_sha1_str, ref(seeder_chunk_ids), download_id);

    for(int j = 0; j < nseeders; ++j)
        seeder_thread_arr[j].join();

    int idx[nseeders] = {0};
    bool alldone = false;

    bool visited[nchunks] = {0};
    vector<string> distributed_ids(nseeders);

    while(!alldone)
    {
        alldone = true;
        for(int i = 0; i < nseeders; ++i)
        {
            vector<int> &id_vec = seeder_chunk_ids[i];
            int sz = id_vec.size();
            while(idx[i] < sz && visited[id_vec[idx[i]]])
                ++idx[i];

            if(idx[i] < sz)
            {
                if(!distributed_ids[i].empty())
                    distributed_ids[i] += "$";

                alldone = false;
                visited[id_vec[idx[i]]] = true;
                distributed_ids[i] += to_string(id_vec[idx[i]]);
                ++idx[i];
            }
        }
    }

    ofstream fout(dest_file_path, ios::binary);
    if(!fout)
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
        return;
    }
    fout.seekp(filesize - 1);
    fout << '\0';

    {
        lock_guard<mutex> lg(g_mtx);
        files_downloading[double_sha1_str] = dest_file_path;
    }

    for(int i = 0; i < nseeders; ++i)
    {
        if(!distributed_ids[i].empty())
        {
            seeder_thread_arr[i] = thread(chunks_download, double_sha1_str, distributed_ids[i], seeder_addr_vec[i],
                                          dest_file_path, download_id, ref(fout), mtorrent_file_path);
        }
    }

    for(int j = 0; j < nseeders; ++j)
        seeder_thread_arr[j].join();

    {
        lock_guard<mutex> lg(g_mtx);
        files_downloading.erase(double_sha1_str);
        files_downloaded[double_sha1_str] = dest_file_path;
    }
    mtx_inuse[download_id] = false;
}

void data_read(int sock, char* read_buffer, int size_to_read)
{
    int bytes_read = 0;
    stringstream ss;
    ss << this_thread::get_id();
    do
    {
        bytes_read += read(sock, read_buffer + bytes_read, size_to_read);   // read in a loop
        fprint_log("Bytes read: " + to_string(bytes_read) + " thread id: " + ss.str());
    }while(bytes_read < size_to_read);
}

void get_request(vector<string> &cmd)
{
    string mtorrent_file_path = abs_path_get(cmd[1]);
    string dest_file_path = abs_path_get(cmd[2]);

    ifstream in(mtorrent_file_path);
    if(!in)
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
        return;
    }

    int line_no = 0;
    string line_str;
    while(line_no != 4 && getline(in, line_str))
        ++line_no;

    unsigned long long filesize = 0;
    if(line_no == 4)    // filesize
    {
        filesize = stoi(line_str);
        getline(in, line_str);
        ++line_no;
    }

    string double_sha1_str;
    if(line_no == 5)    // sha1 string
    {
        double_sha1_str = get_sha1_str((const unsigned char*)line_str.c_str(), line_str.length());
    }

    int sock = make_connection_with_tracker();
    send_request(sock, GET, double_sha1_str);

    int read_size;
    if(FAILURE == read(sock, &read_size, sizeof(read_size)))
    {
        status_print(FAILURE, "Read failed!!");
        fprint_log("Read failed!! get_request() ");
        close(sock);
        return;
    }
    char seeder_list[read_size + 1] = {'\0'};
    data_read(sock, seeder_list, read_size);
    close(sock);

    int download_id = download_id_get();
    mtx_inuse[download_id] = true;
    thread download_thread(file_download, double_sha1_str, (string)seeder_list, filesize, dest_file_path, download_id, mtorrent_file_path);
    download_thread.detach();
}

void remove_request(vector<string> &cmd)
{
    string mtorrent_file_path = abs_path_get(cmd[1]);

    ifstream in(mtorrent_file_path);
    if(!in)
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
        return;
    }

    int line_no = 0;
    string line_str;

    string file_name;
    if(line_no == 3)    // filename
    {
        file_name = line_str;
        getline(in, line_str);
        ++line_no;
    }

    while(line_no != 5 && getline(in, line_str))
        ++line_no;

    string double_sha1_str;
    if(line_no == 5)    // sha1 string
    {
        double_sha1_str = get_sha1_str((const unsigned char*)line_str.c_str(), line_str.length());
    }

    if(files_downloading.find(double_sha1_str) != files_downloading.end())
    {
        return;
    }

    update_seeding_map_n_file(REMOVE, double_sha1_str);

    int sock = make_connection_with_tracker();
    send_request(sock, REMOVE_TORRENT, double_sha1_str + "$" + addr[CLIENT]);
    if(FAILURE == unlinkat(0, mtorrent_file_path.c_str(), 0))
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
    }
}

void downloads_show()
{
    screen_clear();
    print_mode();
    if(files_downloaded.empty())
    {
        cout << "\nNo Downloads Available!!\n";
    }
    else
    {
        cout << "\nFiles Downloaded:\n";
        for(auto itr = files_downloaded.begin(); itr != files_downloaded.end(); ++itr)
        {
            cout << "[S] " << itr->second << endl;
        }
    }

    if(files_downloading.empty())
    {
        cout << "\nNo Ongoing Downloads!!\n";
    }
    else
    {
        cout << "\nFiles Downloading:\n";
        for(auto itr = files_downloading.begin(); itr != files_downloading.end(); ++itr)
        {
            cout << "[D] " << itr->second << endl;
        }
    }
}

void seeding_files_removeall()
{
    int sock = make_connection_with_tracker();
    send_request(sock, REMOVE_ALL, addr[CLIENT]);
}

void enter_commands()
{
    bool command_exit = false;

    while(1)
    {
        if(!is_status_on)
            print_mode();

        char ch;
        string cmd;
        bool enter_pressed = false;
        while(!enter_pressed && !command_exit)
        {
            ch = next_input_char_get();
            if(is_status_on)
            {
                is_status_on = false;
                cursor_c_pos = cursor_left_limit;
                cursor_init();
                from_cursor_line_clear();
            }
            switch(ch)
            {
                case ESC:
                    command_exit = true;
                    break;

                case ENTER:
                    enter_pressed = true;
                    break;

                case BACKSPACE:
                    if(cmd.length())
                    {
                        --cursor_c_pos;
                        --cursor_right_limit;
                        cursor_init();
                        from_cursor_line_clear();
                        cmd.erase(cursor_c_pos - cursor_left_limit, 1);
                        cout << cmd.substr(cursor_c_pos - cursor_left_limit);
                        cout.flush();
                        cursor_init();
                    }
                    break;

                case UP:
                case DOWN:
                    break;

                case LEFT:
                    if(cursor_c_pos != cursor_left_limit)
                    {
                        --cursor_c_pos;
                        cursor_init();
                    }
                    break;

                case RIGHT:
                    if(cursor_c_pos != cursor_right_limit)
                    {
                        ++cursor_c_pos;
                        cursor_init();
                    }
                    break;

                default:
                    cmd.insert(cursor_c_pos - cursor_left_limit, 1, ch);
                    cout << cmd.substr(cursor_c_pos - cursor_left_limit);
                    cout.flush();
                    ++cursor_c_pos;
                    cursor_init();
                    ++cursor_right_limit;
                    break;
            }
        }
        if(command_exit)
            break;

        if(cmd.empty())
            continue;

        string part;
        vector<string> command;

        for(unsigned int i = 0; i < cmd.length(); ++i)
        {
            if(cmd[i] == ' ')
            {
                if(!part.empty())
                {
                    command.pb(part);
                    part = "";
                }
            }
            else if(cmd[i] == '\\' && (i < cmd.length() - 1) && cmd[i+1] == ' ')
            {
                part += ' ';
                ++i;
            }
            else
            {
                part += cmd[i];
            }
        }
        if(!part.empty())
            command.pb(part);

        if(command.empty())
            continue;

        if(command[0] == "exit")
            break;

        if(command[0] == "share")
        {
            if(FAILURE == command_size_check(command, 3, 3, "share: (usage):- \"share <local_file_path>"
                                                            " <filename>.<file_extension>.mtorrent\""))
                continue;
            share_request(command);
        }
        else if(command[0] == "get")
        {
            if(FAILURE == command_size_check(command, 3, 3, "get: (usage):- \"get <local_file_path>"
                                                            " <path_to_.mtorrent_file> <destination_path>\""))
                continue;
            get_request(command);
        }
        else if(command[0] == "remove")
        {
            if(FAILURE == command_size_check(command, 2, 2, "remove: (usage):- \"remove <path_to_.mtorrent_file>\""))
                continue;
            remove_request(command);
        }
        else if(command[0] == "show" && command[1] == "downloads")
        {
            downloads_show();
        }
        else
        {
            status_print(FAILURE, "Invalid Command. Please try again!!");
        }
    }

    seeding_files_removeall();
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

void file_chunks_upload(int sock, string req_str)
{
    unsigned int dollar_pos = req_str.find('$');
    string double_sha1_str = req_str.substr(0, dollar_pos);
    req_str.erase(0, dollar_pos+1);

    string file_entry = seeding_files_map[double_sha1_str];
    dollar_pos = file_entry.find('$');
    string local_file_path = file_entry.substr(0, dollar_pos);

    ifstream in (local_file_path.c_str(), ios::binary | ios::in);
    if(!in)
    {
        string err_str = "FAILURE: ";
        err_str = err_str + strerror(errno);
        status_print(FAILURE, err_str);
        return;
    }

    fprint_log("File: " + local_file_path + "\nChunks requested to upload:\n" + req_str); 

    bool done = false;
    int id;
    while(!done)
    {
        if((dollar_pos = req_str.find('$')) == string::npos)
        {
            id = stoi(req_str);
            done = true;
        }
        else
        {
            id = stoi(req_str.substr(0, dollar_pos));
            req_str.erase(0, dollar_pos+1);
        }
        in.seekg(id * PIECE_SIZE);

        char ibuf[PIECE_SIZE + 1] = {'\0'};
        in.read(ibuf, PIECE_SIZE);

        if(in.fail() && !in.eof())
        {
            stringstream ss;
            ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
            status_print(FAILURE, ss.str());
            return;
        }
        int sz = in.gcount();
        send(sock, &sz, sizeof(sz), 0);
        send(sock, ibuf, in.gcount(), 0);
    }
}

void client_request_handle(int sock, string req_str)
{
    int dollar_pos = req_str.find('$');
    string cmd = req_str.substr(0, dollar_pos);
    req_str = req_str.erase(0, dollar_pos+1);
    int req = stoi(cmd);

    switch(req)
    {
        case GET_CHUNK_IDS:
        {
            string double_sha1_str = req_str;
            auto itr = seeding_file_chunks.find(double_sha1_str);
            set<int>& id_set = itr->second;
            auto set_itr = id_set.begin();
            string ids_str;
            ids_str += to_string(*set_itr);
            for(++set_itr; set_itr != id_set.end(); ++set_itr)
            {
                ids_str += "$" + to_string(*set_itr);
            }
            int sz = ids_str.length();
            send(sock, &sz, sizeof(sz), 0);
            send(sock, ids_str.c_str(), ids_str.length(), 0);

            fprint_log("GET_CHUNK_IDS: Sending " + ids_str);
            break;
        }

        case GET_CHUNKS:
        {
            thread th(file_chunks_upload, sock, req_str);
            th.detach();
            break;
        }

        default:
            break;
    }
}

void sigusr1_handler(int sig)
{
    ;
}

void seeding_files_share()
{
    int pos = seeding_files_path.find_last_of('/');
    string temp_file_path = seeding_files_path.substr(0, pos+1) + "temp_file.txt";
    rename(seeding_files_path.c_str(), temp_file_path.c_str());

    ifstream in(temp_file_path);
    if(!in)
    {
        return;
    }

    string line_str, double_sha1_str, file_path, mtorrent_file_path;
    vector<string> cmd;

    int dollar_pos;
    while(getline(in, line_str))
    {
        cmd.push_back("share");
        dollar_pos = line_str.find('$');
        double_sha1_str = line_str.substr(0, dollar_pos);
        line_str.erase(0, dollar_pos + 1);

        dollar_pos = line_str.find('$');
        file_path = line_str.substr(0, dollar_pos);
        line_str.erase(0, dollar_pos + 1);
        mtorrent_file_path = line_str;

        cmd.push_back(file_path);
        cmd.push_back(mtorrent_file_path);

        share_request(cmd);
        cmd.clear();
    }
    if(FAILURE == unlinkat(0, temp_file_path.c_str(), 0))
    {
        stringstream ss;
        ss << "Error: (" << __func__ << ") (" << __LINE__ << "): " << strerror(errno);
        status_print(FAILURE, ss.str());
    }
}

void seeder_run(bool& seeder_exit)
{
    int opt = true;
    int seeder_socket , addrlen , new_socket , client_socket[MAX_CONNS],
        max_clients = MAX_CONNS , activity, i , valread , sd;
    int max_sd;
    struct sockaddr_in address;

    signal(SIGUSR1, sigusr1_handler);

    seeding_files_share();

    fd_set readfds;

    for (i = 0; i < max_clients; i++)
    {
        client_socket[i] = 0;
    }

    if( (seeder_socket = socket(AF_INET , SOCK_STREAM , 0)) == 0)
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): socket failed!!";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);
    }

    //set master socket to allow multiple connections ,
    //this is just a good habit, it will work without this
    if( setsockopt(seeder_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 )
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): setsockopt";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);
    }

    //type of socket created  
    address.sin_family = AF_INET;   
    address.sin_addr.s_addr = INADDR_ANY;   
    address.sin_port = htons( port[CLIENT] );   

    //bind the socket to localhost port 4500
    if (bind(seeder_socket, (struct sockaddr *)&address, sizeof(address))<0)   
    {   
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): bind failed";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);   
    }   
    stringstream ss;
    ss << "Seeder " << ip[CLIENT] << ":" << port[CLIENT] << " listening";
    fprint_log(ss.str());
         
    //try to specify maximum of 100 pending connections for the master socket  
    if (listen(seeder_socket, MAX_CONNS) < 0)   
    {
        stringstream ss;
        ss << __func__ << " (" << __LINE__ << "): listen failed";
        fprint_log(ss.str());
        exit(EXIT_FAILURE);   
    }   
         
    //accept the incoming connection  
    addrlen = sizeof(address);

    while(!seeder_exit)
    {   
        //clear the socket set  
        FD_ZERO(&readfds);   

        //add master socket to set  
        FD_SET(seeder_socket, &readfds);   
        max_sd = seeder_socket;   

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
       
        if (activity < 0)
        {
            if(errno != EINTR)
            {
                stringstream ss;
                ss << __func__ << " (" << __LINE__ << "): select error";
                fprint_log(ss.str());
            }
            continue;
        }

        // If something happened on the master socket ,  
        // then its an incoming connection  
        if (FD_ISSET(seeder_socket, &readfds))   
        {
            if ((new_socket = accept(seeder_socket,  
                    (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)   
            {   
                stringstream ss;
                ss << __func__ << " (" << __LINE__ << "): accept() failed!!";
                fprint_log(ss.str());
                exit(EXIT_FAILURE);   
            }   
             
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
            //else its some IO operation on some other socket 
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

int main(int argc, char* argv[])
{
    screen_clear();

    tcgetattr(STDIN_FILENO, &prev_attr);
    new_attr = prev_attr;
    new_attr.c_lflag &= ~ICANON;
    new_attr.c_lflag &= ~ECHO;
    tcsetattr( STDIN_FILENO, TCSANOW, &new_attr);

    if(argc < 5 || argc > 5)
    {
        status_print(FAILURE, "Client Application Usage: \"/executable <CLIENT_IP>:<UPLOAD_PORT>"
                     " <TRACKER_IP_1>:<TRACKER_PORT_1> <TRACKER_IP_2>:<TRACKER_PORT_2> <log_file>\"");
        cout << endl;
        tcsetattr( STDIN_FILENO, TCSANOW, &prev_attr);
        return 0;
    }

    print_mode();
    working_dir = getenv("PWD");
    if(working_dir != "/")
        working_dir = working_dir + "/";

    client_exec_path = abs_path_get(argv[0]);
    int pos = client_exec_path.find_last_of("/");
    seeding_files_path = client_exec_path.substr(0, pos+1);
    seeding_files_path += SEEDING_FILES_LIST;

    addr[CLIENT] = argv[1];
    addr[TRACKER1] = argv[2];
    addr[TRACKER2] = argv[3];
    log_file_path = abs_path_get(argv[4]);

    ip_and_port_split(addr[CLIENT], ip[CLIENT], port[CLIENT]);
    ip_and_port_split(addr[TRACKER1], ip[TRACKER1], port[TRACKER1]);
    ip_and_port_split(addr[TRACKER2], ip[TRACKER2], port[TRACKER2]);

    curr_tracker_id = TRACKER1;

    bool seeder_exit = false;
    thread t_seeder(seeder_run, ref(seeder_exit));

    enter_commands();
    screen_clear();

    seeder_exit = true;
    pthread_kill(t_seeder.native_handle(), SIGUSR1);
    t_seeder.join();

    tcsetattr( STDIN_FILENO, TCSANOW, &prev_attr);
    return 0;
}
