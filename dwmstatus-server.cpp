#include <array>
#include <vector>
#include <tuple>
#include <numeric>
#include <fmt/core.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifndef NO_X11
#include <X11/Xlib.h>
#endif

/* enums */
enum FieldIndexes {
        R_TIME = 0,
        R_LOAD,
        R_TEMP,
        R_VOL,
        R_MIC,
        R_MEM,
        R_GOV,
        R_LANG,
        R_WTH,
        R_DATE,
        R_SIZE
};

/* global constexpr variables */
static constexpr int BUFFER_MAX_SIZE = 255;
static constexpr int ROOT_BUFFER_MAX_SIZE = R_SIZE * BUFFER_MAX_SIZE;
static constexpr int MAX_REQUESTS = 10;
static constexpr std::string_view SOCKET_NAME = "/tmp/dwmstatus.socket";
static constexpr auto fmt_format_str = []()
{
        std::string str = "[{}";

        for(int i = 0; i < R_SIZE - 1; ++i)
        {
                str += " |{}";
        }

        str += "]";

        std::array<char, R_SIZE * 15> fmt_str = {};
        std::char_traits<char>::copy(fmt_str.data(), str.c_str(), str.size());

        return fmt_str;
}();        

/* using declarations */
using CharBuffer     = std::array<char, BUFFER_MAX_SIZE + 1>;
using RootCharBuffer = std::array<char, ROOT_BUFFER_MAX_SIZE + 1>;

/* global variables */
static std::array<CharBuffer, R_SIZE> buffers = {};
static bool running = true;
#ifndef NO_X11
static Display* dpy = nullptr;
static int screen;
static Window root;
#endif

/* struct declarations */
struct Response;

/* template function declarations */
template<const auto& resp_table, std::size_t... indexes>
static void run_meta_response();

/* constexpr function declarations */
static constexpr Response make_shell_response(const char* command, char* buffer);
static constexpr Response make_builtin_response(void (*fptr)(char*), char* buffer);
static constexpr Response make_meta_response(void (*fptr)());

/* function declarations */
static void cexit(const char* why);
static int exec_cmd(const char* cmd, char* output_buf);
static void do_response(const Response* response);
static void toggle_lang(char* output_buf);
static void toggle_cpu_gov(char* output_buf);
static void toggle_mic(char* output_buf);
static void terminator();
static void setup();
static void init_statusbar();
static void set_root();
static void handle_received(const std::uint32_t id);
static void cleanup_and_exit(const int);
static void run();

/* struct definitions */
struct Response
{
        enum Type {
                Shell,
                Builtin,
                Meta
        };

        struct ShellArgs {
                const char* command;
                char* buffer;
        };

        struct BuiltinArgs {
                void (*fptr)(char*);
                char* buffer;
        };

        struct MetaArgs {
                void (*fptr)();
        };

        int type;
        union {
                ShellArgs shell;
                BuiltinArgs builtin;
                MetaArgs meta;
        } args;
};

/* template function definitions */
template<const auto& resp_table, std::size_t... indexes>
void
run_meta_response()
{
        (do_response(&resp_table[indexes]), ...);
}

/* consetval function definitions */
constexpr Response
make_shell_response(const char* command, char* buffer)
{
        Response r;

        r.type               = Response::Type::Shell;
        r.args.shell.command = command;
        r.args.shell.buffer  = buffer;

        return r;
}

constexpr Response
make_builtin_response(void (*fptr)(char*), char* buffer)
{
        Response r;

        r.type                = Response::Type::Builtin;
        r.args.builtin.fptr   = fptr;
        r.args.builtin.buffer = buffer;

        return r;
}

constexpr Response
make_meta_response(void (*fptr)())
{
        Response r;

        r.type           = Response::Type::Meta;
        r.args.meta.fptr = fptr;

        return r;
}

/* function definitions */
void
cexit(const char* why)
{
        perror(why);
        exit(EXIT_FAILURE);
}

int
exec_cmd(const char* cmd, char* output_buf)
{
        FILE* pipe = popen(cmd, "r");
        if(pipe == nullptr)
        {
                cexit("popen");
        }

        output_buf[0] = '\0';
        if(fgets(output_buf, BUFFER_MAX_SIZE + 1, pipe) != nullptr)
        {
                auto len = strlen(output_buf);

                /* check for trailing newline and delete it */
                if(len > 0 && output_buf[len - 1] == '\n')
                {
                        output_buf[len - 1] = '\0';
                }
        }

        return pclose(pipe);
}

void
do_response(const Response* response)
{
        switch(response->type)
        {
        case Response::Type::Shell:
        {
                auto& args = response->args.shell;
                exec_cmd(args.command, args.buffer);

                break;
        }
        case Response::Type::Builtin:
        {
                auto& args = response->args.builtin;
                args.fptr(args.buffer);

                break;
        }
        case Response::Type::Meta:
        {
                auto& args = response->args.meta;
                args.fptr();

                break;
        }
        default:
        {
                __builtin_unreachable();
        }
        }
}

void
toggle_lang(char* output_buf)
{
        static constexpr std::string_view ltable[2] = {
            {"US"},
            {"RO"}
        };

        static constexpr const char* commands[2] = {
            "setxkbmap us; setxkbmap -option numpad:mac",
            "setxkbmap ro -variant std"
        };

        static std::size_t idx = 1;

        idx = !idx;
        std::system(commands[idx]);
        memcpy(output_buf, ltable[idx].data(), 2);
}

void
toggle_cpu_gov(char* output_buf)
{
        static constexpr std::string_view freq_table[2] = {
            {"*"},
            {"$"}
        };

        static constexpr const char* commands[2] = {
            "xss-set-save",
            "xss-set-perf"
        };

        static std::size_t idx = 1;

        idx = !idx;
        std::system(commands[idx]);
        memcpy(output_buf, freq_table[idx].data(), 1);
}

void
toggle_mic(char* output_buf)
{
        static constexpr std::string_view mic_status_table[2] = {
            {"0"},
            {"1"}
        };

        static constexpr const char* command = "pactl set-source-mute @DEFAULT_SOURCE@ toggle";

        static std::size_t idx = 1;

        idx = !idx;
        std::system(command);
        memcpy(output_buf, mic_status_table[idx].data(), 1);
}

void
terminator()
{
        fmt::print(stderr, "Received id 0. Terminating...\n");
        running = false;
}

static constexpr auto sr_table = []()
{
        std::array arr = std::to_array<std::pair<const char*, char*>>({
            {   /* time */
                R"(date +%H:%M:%S)",        /* shell command */
                buffers[R_TIME].data()      /* reference to root buffer */
            },
            {   /* sys load*/
                R"(uptime | grep -wo "average: .*," | cut --delimiter=' ' -f2 | head -c4)",
                buffers[R_LOAD].data()
            },
            {   /* cpu temp*/
                R"(sensors | grep -F "Core 0" | awk '{print $3}' | cut -c2-5)",
                buffers[R_TEMP].data()
            },
            {   /* volume */
                R"(amixer sget Master | tail -n1 | get-from-to '[' ']' '--amixer')",
                buffers[R_VOL].data()
            },
            {   /* memory usage */
                R"(xss-get-mem)",
                buffers[R_MEM].data()
            },
            {   /* date */
                R"(date "+%d.%m.%Y")",
                buffers[R_DATE].data()
            },
            {   /* weather */
                R"(curl wttr.in/Bucharest?format=1 2>/dev/null | get-from '+')",
                buffers[R_WTH].data()
            }
        });

        std::array<Response, arr.size()> responses;
        for(std::size_t i = 0; i < responses.size(); ++i)
        {
                responses[i] = make_shell_response(arr[i].first, arr[i].second);
        }

        return responses;
}();

static constexpr auto br_table = []()
{
        std::array arr = std::to_array<std::pair<void(*)(char*), char*>>({
           /* pointer to function   reference to root buffer */
            { &toggle_lang,         buffers[R_LANG].data() },
            { &toggle_cpu_gov,      buffers[R_GOV].data()  },
            { &toggle_mic,          buffers[R_MIC].data()  }
        });

        std::array<Response, arr.size()> responses;
        for(std::size_t i = 0; i < responses.size(); ++i)
        {
                responses[i] = make_builtin_response(arr[i].first, arr[i].second);
        }

        return responses;
}();

static constexpr auto mr_table = []()
{
        std::array arr = std::to_array<void (*)()>({
           /* pointer to function */
            &run_meta_response<sr_table, 0, 1, 2, 4>,
            &terminator
        });

        std::array<Response, arr.size()> responses;
        for(std::size_t i = 0; i < responses.size(); ++i)
        {
                responses[i] = make_meta_response(arr[i]);
        }

        return responses;
}();

static const std::vector<const Response*> rt_responses = {
        &mr_table[1], // 0
        &sr_table[3], // 1
        &sr_table[6], // 2
        &br_table[0], // 3
        &br_table[1], // 4
        &br_table[2], // 5
        &mr_table[0]  // 6
};

void
setup()
{
        signal(SIGTERM, &cleanup_and_exit);
        signal(SIGINT,  &cleanup_and_exit);

#ifndef NO_X11
        dpy = XOpenDisplay(nullptr);
        if(!dpy)
        {
                fmt::print(stderr, "XOpenDisplay(): Failed to open display\n");
                exit(EXIT_FAILURE);
        }
        screen = DefaultScreen(dpy);
        root = RootWindow(dpy, screen);
#endif
}

void
init_statusbar()
{
        for(const auto& r : sr_table)
        {
                do_response(&r);
        }

        for(const auto& r : br_table)
        {
                do_response(&r);
        }

        set_root();
}

void
set_root()
{
        RootCharBuffer buf;

        const auto format_res = std::apply(
            [&](auto&&... args)
            {
                    return fmt::format_to_n(buf.data(),
                                            BUFFER_MAX_SIZE,
                                            std::string_view(fmt_format_str.data()),
                                            std::string_view(args.data())...);
            },
            buffers);
        *format_res.out = '\0';

#ifndef NO_X11
        XStoreName(dpy, root, buf.data());
        XFlush(dpy);
#else
        fmt::print("{}\n", buf.data());
#endif
}

void
handle_received(const std::uint32_t id)
{
        if(id >= rt_responses.size())
        {
                fmt::print(stderr, "Received id out of bounds: {}\n", id);
                return;
        }

        do_response(rt_responses[id]);
        set_root();
}

void
cleanup_and_exit(const int)
{
        unlink(SOCKET_NAME.data());
        _exit(EXIT_SUCCESS);
}

void
run()
{
        const int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if(sockfd < 0)
        {
                cexit("socket");
        }

        struct sockaddr_un name;
        memset(&name, 0, sizeof(name));

        name.sun_family = AF_UNIX;
        strncpy(name.sun_path, SOCKET_NAME.data(), sizeof(name.sun_path) - 1);

        int ret = bind(sockfd, (const struct sockaddr*)&name, sizeof(name));
        if(ret < 0)
        {
                cexit("bind");
        }

        ret = listen(sockfd, MAX_REQUESTS);
        if(ret < 0)
        {
                cexit("listen");
        }

        setup();
        init_statusbar();

        while(running)
        {
                const int datafd = accept(sockfd, nullptr, nullptr);
                if(datafd < 0)
                {
                        cexit("accept");
                }

                std::uint32_t id;

                ret = read(datafd, &id, sizeof(id));
                if(ret < 0)
                {
                        cexit("read");
                }

                if(ret != sizeof(id))
                {
                        fmt::print(
                            stderr,
                            "read(): Received {} out of {} bytes needed for table index\n",
                            ret,
                            sizeof(id)
                        );
                }
                else
                {
                        handle_received(id);
                }

                close(datafd);
        }

        close(sockfd);
        unlink(SOCKET_NAME.data());
}

int
main()
{
        run();
}
