#include <array>
#include <tuple>
#include <fmt/core.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#ifndef NO_X11
#include <X11/Xlib.h>
#endif

/* macros */
#define DWMSTATUS_NORETURN    __attribute__((__noreturn__))
#define DWMSTATUS_UNREACHABLE __builtin_unreachable()
#define SHELL                 "/bin/sh"
#define SHCMD(cmd)            {SHELL, "-c", cmd, nullptr}

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
static constexpr int BUFFER_MAX_SIZE         = 255;
static constexpr int ROOT_BUFFER_MAX_SIZE    = R_SIZE * BUFFER_MAX_SIZE;
static constexpr const char* SOCKET_PATH     = "/tmp/dwmstatus.socket";
static constexpr std::string_view STATUS_FMT = "[{} |{} |{} |{} |{} |{} |{} |{} |{} |{}]";

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

        constexpr FieldUpdate(const char* command, FieldBuffer* field_buffer);
        constexpr FieldUpdate(void (*fptr)(FieldBuffer*), FieldBuffer* field_buffer);
        constexpr FieldUpdate(void (*fptr)());

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
                ShellArgs   shell;
                BuiltinArgs builtin;
                MetaArgs    meta;
        } args;
};

constexpr FieldUpdate::FieldUpdate(const char* command, FieldBuffer* field_buffer)
{
        type                    = Type::Shell;
        args.shell.command      = command;
        args.shell.field_buffer = field_buffer;
}

constexpr FieldUpdate::FieldUpdate(void (*fptr)(FieldBuffer*), FieldBuffer* field_buffer)
{
        type                      = Type::Builtin;
        args.builtin.fptr         = fptr;
        args.builtin.field_buffer = field_buffer;
}

constexpr FieldUpdate::FieldUpdate(void (*fptr)())
{
        type           = Type::Meta;
        args.meta.fptr = fptr;
}

/* template function declarations */
template<const auto& updates, std::size_t... indexes>
static void run_meta_update();

/* function declarations */
static void die(const bool cond, const char* why);
static ssize_t read_all(const int fd, void* buffer, const size_t nbytes);
static void create_child(const char* cmd, const int pipe_fds[2]);
static int get_named_socket();
static void perror_exit(const char* why) DWMSTATUS_NORETURN;
static int read_cmd_output(const char* cmd, FieldBuffer* field_buffer);
static void run_update(const FieldUpdate* field_update);
static void toggle_lang(FieldBuffer* field_buffer);
static void toggle_cpu_gov(FieldBuffer* field_buffer);
static void toggle_mic(FieldBuffer* field_buffer);
static void terminator();
static void init_signals();
static void init_x();
static void init_statusbar();
static void update_screen();
static void handle_received(const std::uint32_t id);
static void cleanup_and_exit(const int sig) DWMSTATUS_NORETURN;
static void run();

/* global variables */
static std::array<FieldBuffer, R_SIZE> field_buffers = {};
static bool running = true;
#ifndef NO_X11
static Display* dpy = nullptr;
static int screen;
static Window root;
#endif

/* field configs */
static constexpr std::array shell_updates = std::to_array<FieldUpdate>({
        {       /* time */
                R"(date +%H:%M:%S)",        /* shell command */
                &field_buffers[R_TIME]      /* reference to root buffer */
        },
        {       /* sys load*/
                R"(uptime | grep -wo "average: .*," | cut --delimiter=' ' -f2 | head -c4)",
                &field_buffers[R_LOAD]
        },
        {       /* cpu temp*/
                R"(sensors | grep -F "Core 0" | awk '{print $3}' | cut -c2-5)",
                &field_buffers[R_TEMP]
        },
        {       /* volume */
                R"(amixer sget Master | tail -n1 | get-from-to '[' ']' '--amixer')",
                &field_buffers[R_VOL]
        },
        {       /* memory usage */
                R"(xss-get-mem)",
                &field_buffers[R_MEM]
        },
        {       /* date */
                R"(date "+%d.%m.%Y")",
                &field_buffers[R_DATE]
        },
        {       /* weather */
                R"(curl wttr.in/Bucharest?format=1 2>/dev/null | get-from '+')",
                &field_buffers[R_WTH]
        }
});

static constexpr std::array builtin_updates = std::to_array<FieldUpdate>({
       /* pointer to function   reference to root buffer */
        { &toggle_lang,         &field_buffers[R_LANG] },
        { &toggle_cpu_gov,      &field_buffers[R_GOV]  },
        { &toggle_mic,          &field_buffers[R_MIC]  }
});

static constexpr std::array meta_updates = std::to_array<FieldUpdate>({
     /* pointer to function */
        &run_meta_update<shell_updates, 0, 1, 2, 4>,
        &terminator
});

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
void
die(const bool cond, const char* why)
{
        if(cond)
                perror_exit(why);
}

ssize_t
read_all(const int fd, void* buffer, const size_t nbytes)
{
        char* buf = (char*)buffer;
        size_t read_so_far = 0;

        while(read_so_far < nbytes)
        {
                const ssize_t rc = read(fd, buf + read_so_far, nbytes - read_so_far);
                if(rc < 0)
                        return rc;

                if(rc == 0)
                        break;

                read_so_far += rc;
        }

        return read_so_far;
}

void
create_child(const char* cmd, const int pipe_fds[2])
{
        const pid_t child_pid = fork();
        die(child_pid < 0, "fork");

        if(child_pid == 0)
        {
                int rc = close(pipe_fds[0]);
                if(rc < 0)
                        exit(EXIT_FAILURE);

                rc = dup2(pipe_fds[1], STDOUT_FILENO);
                if(rc < 0)
                        exit(EXIT_FAILURE);

                const char* new_argv[] = SHCMD(cmd);
                execv(new_argv[0], (char**)new_argv);
                exit(EXIT_FAILURE);
        }
}

int
get_named_socket()
{
        const int sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        die(sock_fd < 0, "socket");

        struct sockaddr_un name;
        memset(&name, 0, sizeof(name));
        name.sun_family = AF_UNIX;
        strncpy(name.sun_path, SOCKET_PATH, sizeof(name.sun_path) - 1);

        const int rc = bind(sock_fd, (const struct sockaddr*)&name, sizeof(name));
        die(rc < 0, "bind");

        return sock_fd;
}

void DWMSTATUS_NORETURN
perror_exit(const char* why)
{
        perror(why);
        exit(EXIT_FAILURE);
}

int
read_cmd_output(const char* cmd, FieldBuffer* field_buffer)
{
        int rc;

        int pipe_fds[2];
        rc = pipe(pipe_fds);
        die(rc < 0, "pipe");

        create_child(cmd, pipe_fds);
        rc = close(pipe_fds[1]);
        die(rc < 0, "close");

        auto& [len, buf] = *field_buffer;

        buf[len = 0] = '\0';
        const ssize_t b_read = read_all(pipe_fds[0], buf, BUFFER_MAX_SIZE);
        die(b_read < 0, "read");

        buf[len = b_read] = '\0';
        if(len > 0 && buf[len - 1] == '\n')
                buf[--len] = '\0';

        rc = close(pipe_fds[0]);
        if(rc < 0)
        {
                wait(nullptr);
                return rc;
        }

        rc = wait(nullptr);
        if(rc < 0)
                return rc;

        return 0;
}

void
run_update(const FieldUpdate* field_update)
{
        switch(field_update->type)
        {
        case FieldUpdate::Type::Shell:
        {
                auto& args = field_update->args.shell;
                const int rc = read_cmd_output(args.command, args.field_buffer);
                die(rc < 0, "read_cmd_output");

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
init_signals()
{
        int rc;

        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = &cleanup_and_exit;
        rc = sigemptyset(&act.sa_mask);
        die(rc < 0, "sigemptyset");
        act.sa_flags = 0;

        for(const int sig : {SIGTERM, SIGINT, SIGHUP})
        {
                struct sigaction old;
                rc = sigaction(sig, nullptr, &old);
                die(rc < 0, "sigaction");

                if(old.sa_handler != SIG_IGN)
                {
                        rc = sigaction(sig, &act, nullptr);
                        die(rc < 0, "sigaction");
                }
        }
}

void
init_x()
{
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
                               STATUS_FMT,
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
cleanup_and_exit(const int sig)
{
        unlink(SOCKET_PATH);
        _exit(EXIT_SUCCESS);
}

void
run()
{
        const int sock_fd = get_named_socket();

        init_signals();
        init_x();
        init_statusbar();

        while(running)
        {
                std::uint32_t id;

                const auto rc = read(sock_fd, &id, sizeof(id));
                if(rc < 0)
                {
                        unlink(SOCKET_PATH);
                        perror_exit("read");
                }

                if(rc != sizeof(id))
                {
                        fmt::print(
                            stderr,
                            "read(): Received {} out of {} bytes needed for table index\n",
                            rc,
                            sizeof(id)
                        );
                }
                else
                {
                        handle_received(id);
                }
        }

        close(sock_fd);
        unlink(SOCKET_PATH);
}

int
main()
{
        run();
}
