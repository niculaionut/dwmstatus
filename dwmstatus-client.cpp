#include <fmt/core.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static constexpr std::string_view SOCKET_PATH = "/tmp/dwmstatus.socket";

int main(const int argc, const char* argv[])
{
        if(argc != 2)
        {
                fmt::print(stderr, "Usage: dwmstatus-client <id-of-update-command>");
                exit(EXIT_FAILURE);
        }

        const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if(server_fd < 0)
        {
                perror("socket");
                exit(EXIT_FAILURE);
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));

        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

        int ret = connect(server_fd, (const struct sockaddr*)&addr, sizeof(addr));
        if(ret < 0)
        {
                perror("connect");
                exit(EXIT_FAILURE);
        }

        std::uint32_t num;
        try
        {
                num = std::stoul(argv[1]);
        }
        catch(std::invalid_argument&)
        {
                fmt::print(stderr, "Failed to convert '{}' to std::uint32_t\n", argv[1]);
                exit(EXIT_FAILURE);
        }

        ret = write(server_fd, &num, sizeof(num));
        if(ret < 0)
        {
                perror("write");
                exit(EXIT_FAILURE);
        }

        close(server_fd);
}
