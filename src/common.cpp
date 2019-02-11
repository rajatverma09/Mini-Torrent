/* Name:    Bhavi Dhingra
 * RollNo.: 2018201058
 */

#include "common.h"
#include "includes.h"

using namespace std;

extern string  working_dir;
extern string  root_dir;
static termios prev_attr, new_attr;

struct winsize w;

char next_input_char_get()
{
    cin.clear();
    char ch = cin.get();
    switch(ch)
    {
        case ESC:
            tcgetattr(STDIN_FILENO, &prev_attr);
            new_attr = prev_attr;
            new_attr.c_cc[VMIN] = 0;
            new_attr.c_cc[VTIME] = 1;
            tcsetattr( STDIN_FILENO, TCSANOW, &new_attr);

            if(FAILURE != cin.get())    // FAILURE is return if ESC is pressed
            {
                ch = cin.get();         // For UP, DOWN, LEFT, RIGHT
                switch(ch)
                {
                    case 'A':
                        ch = UP;
                        break;
                    case 'B':
                        ch = DOWN;
                        break;
                    case 'C':
                        ch = RIGHT;
                        break;
                    case 'D':
                        ch = LEFT;
                        break;
                    default:
                        break;
                }
            }
            tcsetattr( STDIN_FILENO, TCSANOW, &prev_attr);
            break;

        default:
            break;
    }
    return ch;
}

void from_cursor_line_clear()
{
    cout << "\e[0K";
    cout.flush();
}

string abs_path_get(string str)
{
    if(str[0] == '/')
        return str;

    char *str_buf = new char[str.length() + 1];
    strncpy(str_buf, str.c_str(), str.length());
    str_buf[str.length()] = '\0';

    string ret_path = working_dir, prev_tok = working_dir;
    if(str_buf[0] == '/')
        ret_path = "/";

    char *p_str = strtok(str_buf, "/");
    while(p_str)
    {
        string tok(p_str);
        if(tok == ".")
        {
            prev_tok = tok;
            p_str = strtok (NULL, "/");
        }
        else if(tok == "..")
        {
            ret_path.erase(ret_path.length() - 1);
            size_t fwd_slash_pos = ret_path.find_last_of("/");
            ret_path = ret_path.substr(0, fwd_slash_pos + 1);
            prev_tok = tok;
            p_str = strtok (NULL, "/");
        }
        else if (tok == "~")
        {
            ret_path = getenv("HOME");
            ret_path = ret_path + "/";
            prev_tok = tok;
            p_str = strtok (NULL, "/");
        }
        else if(tok == "")
        {
            if(!prev_tok.empty())
                ret_path = "/";
            p_str = strtok (NULL, "/");
        }
        else
        {
            p_str = strtok (NULL, "/");
            if(!p_str)
                ret_path += tok;
            else
                ret_path += tok + "/";
        }
    }

    return ret_path;
}
