#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <econ/tui.hpp>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
namespace econ::tui {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void request_close(int) {
    interrupted = 1;
}
void output(const std::string &s) {
    std::size_t position = 0;
    while (position < s.size()) {
        auto n = ::write(STDOUT_FILENO, s.data() + position, s.size() - position);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw Error(3, "Cannot write to terminal");
        }
        if (n == 0)
            throw Error(3, "Terminal closed");
        position += static_cast<std::size_t>(n);
    }
}
class Terminal {
    termios original_{};
    struct sigaction old_int_{}, old_term_{};
    bool raw_ = false, screen_ = false, int_ = false, term_ = false;

  public:
    Terminal() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
            throw Error(2, "tui needs an interactive terminal on stdin and stdout; use inspect/explain "
                           "--json for pipes");
        if (auto value = std::getenv("TERM"); value && std::string(value) == "dumb")
            throw Error(2, "tui requires an ANSI-compatible terminal (TERM is dumb)");
        if (tcgetattr(STDIN_FILENO, &original_) != 0)
            throw Error(3, "Cannot read terminal settings");
        try {
            auto raw = original_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
            raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
                throw Error(3, "Cannot enter interactive terminal mode");
            raw_ = true;
            struct sigaction action{};
            action.sa_handler = request_close;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGINT, &action, &old_int_) != 0)
                throw Error(3, "Cannot install terminal signal handler");
            int_ = true;
            if (sigaction(SIGTERM, &action, &old_term_) != 0)
                throw Error(3, "Cannot install terminal signal handler");
            term_ = true;
            interrupted = 0;
            screen_ = true;
            output("\x1b[?1049h\x1b[?25l\x1b[2J");
        } catch (...) {
            restore();
            throw;
        }
    }
    ~Terminal() {
        restore();
    }
    void restore() noexcept {
        if (raw_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
            raw_ = false;
        }
        if (int_) {
            sigaction(SIGINT, &old_int_, nullptr);
            int_ = false;
        }
        if (term_) {
            sigaction(SIGTERM, &old_term_, nullptr);
            term_ = false;
        }
        if (screen_) {
            static constexpr char reset[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
            auto ignored = ::write(STDOUT_FILENO, reset, sizeof(reset) - 1);
            (void)ignored;
            screen_ = false;
        }
    }
    std::pair<int, int> size() const {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
            return {80, 24};
        return {std::clamp(static_cast<int>(ws.ws_col), 1, 400),
                std::clamp(static_cast<int>(ws.ws_row), 1, 120)};
    }
    std::string read_key() {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        auto ready = poll(&fd, 1, 250);
        if (ready < 0) {
            if (errno == EINTR)
                return "";
            throw Error(3, "Terminal input polling failed");
        }
        if (!ready)
            return "";
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return "CTRL_D";
        unsigned char c = 0;
        auto n = read(STDIN_FILENO, &c, 1);
        if (n == 0)
            return "CTRL_D";
        if (n < 0) {
            if (errno == EINTR)
                return "";
            throw Error(3, "Terminal input failed");
        }
        if (c == 3)
            return "CTRL_C";
        if (c == 4)
            return "CTRL_D";
        if (c == 10 || c == 13)
            return "ENTER";
        if (c == 9)
            return "TAB";
        if (c == 127 || c == 8)
            return "BACKSPACE";
        if (c != 27)
            return std::string(1, static_cast<char>(c));
        std::string sequence;
        for (int i = 0; i < 12; ++i) {
            fd.revents = 0;
            if (poll(&fd, 1, 35) <= 0 || !(fd.revents & POLLIN))
                break;
            char next = 0;
            if (read(STDIN_FILENO, &next, 1) != 1)
                break;
            sequence += next;
            if ((sequence.size() > 1 && next >= 'A' && next <= 'Z') || next == '~')
                break;
            if (sequence.size() == 1 && next != '[' && next != 'O')
                break;
        }
        if (sequence == "[A" || sequence == "OA")
            return "UP";
        if (sequence == "[B" || sequence == "OB")
            return "DOWN";
        if (sequence == "[C" || sequence == "OC")
            return "RIGHT";
        if (sequence == "[D" || sequence == "OD")
            return "LEFT";
        if (sequence == "[H" || sequence == "OH" || sequence == "[1~" || sequence == "[7~")
            return "HOME";
        if (sequence == "[F" || sequence == "OF" || sequence == "[4~" || sequence == "[8~")
            return "END";
        if (sequence == "[5~")
            return "PAGEUP";
        if (sequence == "[6~")
            return "PAGEDOWN";
        if (sequence == "[Z")
            return "BACKTAB";
        return "ESC";
    }
};
} // namespace
int launch(const Options &options) {
    Terminal terminal;
    auto [columns, rows] = terminal.size();
    Frame loading(columns - 1, rows);
    loading.put(1, 2, "TOTOCONOMY / Simulation inspector", Ink::Accent);
    loading.put(3, 2, "Indexing recorded history by day...");
    output(loading.ansi(options.color));
    Inspector inspector(options.runs_directory, options.run);
    bool running = true, dirty = true;
    auto last_size = terminal.size();
    while (running && !interrupted) {
        auto size = terminal.size();
        if (size != last_size) {
            dirty = true;
            last_size = size;
        }
        if (dirty) {
            output(inspector.render(size.first, size.second).ansi(options.color));
            dirty = false;
        }
        auto key = terminal.read_key();
        if (!key.empty()) {
            if (key == "ENTER" || key == "v" || key == "R") {
                Frame wait(size.first - 1, size.second);
                wait.put(1, 2, "Reading recorded artifacts...", Ink::Bold);
                output(wait.ansi(options.color));
            }
            running = inspector.key(key);
            dirty = true;
        }
    }
    return 0;
}
} // namespace econ::tui
