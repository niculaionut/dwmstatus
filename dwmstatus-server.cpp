#include <array>
#include <tuple>
#include <fmt/core.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifndef NO_X11
#include <X11/Xlib.h>
#endif

/* macros */
#define DWMSTATUS_NORETURN    __attribute__((__noreturn__))
#define DWMSTATUS_UNREACHABLE __builtin_unreachable()

/* enums */
enum {
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
static constexpr std::string_view SOCKET_PATH = "/tmp/dwmstatus.socket";
static constexpr auto STATUS_FMT = []()
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
static constexpr int STATUS_FMT_LEN = std::char_traits<char>::length(STATUS_FMT.data());

/* struct definitions */
struct FieldBuffer
{
        std::uint32_t length           = 0;
        char data[BUFFER_MAX_SIZE + 1] = {};
};

struct FieldUpdate
{
        enum Type {
                Shell = 0,
                Builtin,
                Meta
        };

        struct ShellArgs {
                const char* command;
                FieldBuffer* field_buffer;
        };

        struct BuiltinArgs {
                void (*fptr)(FieldBuffer*);
                FieldBuffer* field_buffer;
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

/* constexpr function declarations */
static constexpr FieldUpdate make_shell_update(const char* command, FieldBuffer* buffer);
static constexpr FieldUpdate make_builtin_update(void (*fptr)(FieldBuffer*), FieldBuffer* buffer);
static constexpr FieldUpdate make_meta_update(void (*fptr)());

/* template function declarations */
template<const auto& updates, std::size_t... indexes>
static void run_meta_update();

/* function declarations */
static void perror_exit(const char* why) DWMSTATUS_NORETURN;
static int  write_cmd_output(const char* cmd, FieldBuffer* field_buffer);
static void run_update(const FieldUpdate* field_update);
static void toggle_lang(FieldBuffer* field_buffer);
static void toggle_cpu_gov(FieldBuffer* field_buffer);
static void toggle_mic(FieldBuffer* field_buffer);
static void terminator();
static void setup();
static void init_statusbar();
static void update_screen();
static void handle_received(const std::uint32_t id);
static void cleanup_and_exit(const int) DWMSTATUS_NORETURN;
static void run();

/* global variables */
static std::array<FieldBuffer, R_SIZE> field_buffers = {};
static bool running = true;
#ifndef NO_X11
static Display* dpy = nullptr;
static int screen;
static Window root;
#endif

/* constexpr function definitions */
constexpr FieldUpdate
make_shell_update(const char* command, FieldBuffer* field_buffer)
{
        FieldUpdate f;

        f.type                    = FieldUpdate::Type::Shell;
        f.args.shell.command      = command;
        f.args.shell.field_buffer = field_buffer;

        return f;
}

constexpr FieldUpdate
make_builtin_update(void (*fptr)(FieldBuffer*), FieldBuffer* field_buffer)
{
        FieldUpdate f;

        f.type                      = FieldUpdate::Type::Builtin;
        f.args.builtin.fptr         = fptr;
        f.args.builtin.field_buffer = field_buffer;

        return f;
}

constexpr FieldUpdate
make_meta_update(void (*fptr)())
{
        FieldUpdate f;

        f.type           = FieldUpdate::Type::Meta;
        f.args.meta.fptr = fptr;

        return f;
}

/* field configs */
static constexpr auto shell_updates = []()
{
        std::array arr = std::to_array<std::pair<const char*, FieldBuffer*>>({
            {   /* time */
                R"(date +%H:%M:%S)",        /* shell command */
                &field_buffers[R_TIME]      /* reference to root buffer */
            },
            {   /* sys load*/
                R"(uptime | grep -wo "average: .*," | cut --delimiter=' ' -f2 | head -c4)",
                &field_buffers[R_LOAD]
            },
            {   /* cpu temp*/
                R"(sensors | grep -F "Core 0" | awk '{print $3}' | cut -c2-5)",
                &field_buffers[R_TEMP]
            },
            {   /* volume */
                R"(amixer sget Master | tail -n1 | get-from-to '[' ']' '--amixer')",
                &field_buffers[R_VOL]
            },
            {   /* memory usage */
                R"(xss-get-mem)",
                &field_buffers[R_MEM]
            },
            {   /* date */
                R"(date "+%d.%m.%Y")",
                &field_buffers[R_DATE]
            },
            {   /* weather */
                R"(curl wttr.in/Bucharest?format=1 2>/dev/null | get-from '+')",
                &field_buffers[R_WTH]
            }
        });

        std::array<FieldUpdate, arr.size()> field_updates;
        for(std::size_t i = 0; i < field_updates.size(); ++i)
        {
                field_updates[i] = make_shell_update(arr[i].first, arr[i].second);
        }

        return field_updates;
}();

static constexpr auto builtin_updates = []()
{
        std::array arr = std::to_array<std::pair<void(*)(FieldBuffer*), FieldBuffer*>>({
           /* pointer to function   reference to root buffer */
            { &toggle_lang,         &field_buffers[R_LANG] },
            { &toggle_cpu_gov,      &field_buffers[R_GOV]  },
            { &toggle_mic,          &field_buffers[R_MIC]  }
        });

        std::array<FieldUpdate, arr.size()> field_updates;
        for(std::size_t i = 0; i < field_updates.size(); ++i)
        {
                field_updates[i] = make_builtin_update(arr[i].first, arr[i].second);
        }

        return field_updates;
}();

static constexpr auto meta_updates = []()
{
        std::array arr = std::to_array<void (*)()>({
           /* pointer to function */
            &run_meta_update<shell_updates, 0, 1, 2, 4>,
            &terminator
        });

        std::array<FieldUpdate, arr.size()> field_updates;
        for(std::size_t i = 0; i < field_updates.size(); ++i)
        {
                field_updates[i] = make_meta_update(arr[i]);
        }

        return field_updates;
}();

static constexpr auto real_time_updates = std::to_array<const FieldUpdate*>({
        &meta_updates[1],    /* 0 */
        &shell_updates[3],   /* 1 */
        &shell_updates[6],   /* 2 */
        &builtin_updates[0], /* 3 */
        &builtin_updates[1], /* 4 */
        &builtin_updates[2], /* 5 */
        &meta_updates[0]     /* 6 */
});

/* template function definitions */
template<const auto& updates, std::size_t... indexes>
void
run_meta_update()
{
        (run_update(&updates[indexes]), ...);
}

/* function definitions */
void DWMSTATUS_NORETURN
perror_exit(const char* why)
{
        perror(why);
        exit(EXIT_FAILURE);
}

int
write_cmd_output(const char* cmd, FieldBuffer* field_buffer)
{
        FILE* pipe = popen(cmd, "r");
        if(pipe == nullptr)
        {
                perror_exit("popen");
        }

        char* buffer = field_buffer->data;
        buffer[0] = '\0';
        field_buffer->length = 0;

        if(fgets(buffer, BUFFER_MAX_SIZE + 1, pipe) != nullptr)
        {
                auto len = strlen(buffer);

                /* check for trailing newline and delete it */
                if(len > 0 && buffer[len - 1] == '\n')
                {
                        buffer[len - 1] = '\0';
                        --len;
                }

                field_buffer->length = len;
        }

        return pclose(pipe);
}

void
run_update(const FieldUpdate* field_update)
{
        switch(field_update->type)
        {
        case FieldUpdate::Type::Shell:
        {
                auto& args = field_update->args.shell;
                write_cmd_output(args.command, args.field_buffer);

                break;
        }
        case FieldUpdate::Type::Builtin:
        {
                auto& args = field_update->args.builtin;
                args.fptr(args.field_buffer);

                break;
        }
        case FieldUpdate::Type::Meta:
        {
                auto& args = field_update->args.meta;
                args.fptr();

                break;
        }
        default:
        {
                DWMSTATUS_UNREACHABLE;
        }
        }
}

void
toggle_lang(FieldBuffer* field_buffer)
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

        memcpy(field_buffer->data, ltable[idx].data(), 2);
        field_buffer->length = 2;
}

void
toggle_cpu_gov(FieldBuffer* field_buffer)
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

        memcpy(field_buffer->data, freq_table[idx].data(), 1);
        field_buffer->length = 1;
}

void
toggle_mic(FieldBuffer* field_buffer)
{
        static constexpr std::string_view mic_status_table[2] = {
            {"0"},
            {"1"}
        };

        static constexpr const char* command = "pactl set-source-mute @DEFAULT_SOURCE@ toggle";

        static std::size_t idx = 1;

        idx = !idx;
        std::system(command);

        memcpy(field_buffer->data, mic_status_table[idx].data(), 1);
        field_buffer->length = 1;
}

void
terminator()
{
        fmt::print(stderr, "handle_received(): Got id 0. Terminating...\n");
        running = false;
}

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
        for(const auto& u : shell_updates)   { run_update(&u); }
        for(const auto& u : builtin_updates) { run_update(&u); }
        update_screen();
}

void
update_screen()
{
        char buffer[ROOT_BUFFER_MAX_SIZE + 1];

        const auto res = std::apply(
            [&](auto&&... args)
            {
                    return fmt::format_to_n(
                               buffer,
                               ROOT_BUFFER_MAX_SIZE,
                               std::string_view(STATUS_FMT.data(), STATUS_FMT_LEN),
                               std::string_view(args.data, args.length)...
                           );
            },
            field_buffers
        );

        *res.out = '\0';

#ifndef NO_X11
        XStoreName(dpy, root, buffer);
        XFlush(dpy);
#else
        fmt::print("{}\n", buffer);
#endif
}

void
handle_received(const std::uint32_t id)
{
        if(id >= real_time_updates.size())
        {
                fmt::print(
                    stderr,
                    "handle_received(): Received id out of bounds: {}. Size is: {}.\n",
                    id,
                    real_time_updates.size()
                );

                return;
        }

        run_update(real_time_updates[id]);
        update_screen();
}

void DWMSTATUS_NORETURN
cleanup_and_exit(const int)
{
        unlink(SOCKET_PATH.data());
        _exit(EXIT_SUCCESS);
}

void
run()
{
        const int sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if(sock_fd < 0)
        {
                perror_exit("socket");
        }

        struct sockaddr_un name;
        memset(&name, 0, sizeof(name));
        name.sun_family = AF_UNIX;
        strncpy(name.sun_path, SOCKET_PATH.data(), sizeof(name.sun_path) - 1);

        int ret = bind(sock_fd, (const struct sockaddr*)&name, sizeof(name));
        if(ret < 0)
        {
                perror_exit("bind");
        }

        setup();
        init_statusbar();

        while(running)
        {
                std::uint32_t id;
                ret = read(sock_fd, &id, sizeof(id));
                if(ret < 0)
                {
                        unlink(SOCKET_PATH.data());
                        perror_exit("read");
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
        }

        close(sock_fd);
        unlink(SOCKET_PATH.data());
}

int
main()
{
        run();
}
