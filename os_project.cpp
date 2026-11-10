// ThreadWeaver - OS lab project
//
// Hands-on versions of the classic synchronization problems: producer-consumer,
// readers-writers and dining philosophers. You drive every step yourself from
// the menu.
//
// Producer-consumer and readers-writers are shared between processes. Open the
// program in two (or more) terminals and they all work on the same buffer and
// the same database at the same time. The state lives in a named shared memory
// block, and access is controlled with named OS semaphores and mutexes, so the
// textbook algorithms really run across separate processes.
//
// Build:  g++ -std=c++17 -pthread os_project.cpp -o os_project
//         (on older Linux systems you may also need -lrt)

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <future>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#ifdef ERROR
#undef ERROR
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Thin wrappers around the OS primitives we need for sharing between processes.
// Windows uses named kernel objects; everything else uses POSIX named
// semaphores and shm_open.
// ---------------------------------------------------------------------------
namespace ipc {

    enum class WaitResult { Acquired, Abandoned, TimedOut, Failed };

#ifdef _WIN32

    inline int32_t currentPid() { return static_cast<int32_t>(GetCurrentProcessId()); }

    // Creation time of a process. We store it next to the pid so that a pid
    // recycled by Windows isn't mistaken for a terminal that's still open.
    inline int64_t processStamp(int32_t pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!h) return 0;
        FILETIME created, exited, kernel, user;
        int64_t stamp = 0;
        if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
            stamp = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        }
        CloseHandle(h);
        return stamp;
    }

    inline bool isAlive(int32_t pid, int64_t stamp) {
        if (pid <= 0) return false;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!h) {
            // Access denied means the process exists, we just can't look at it.
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        CloseHandle(h);
        if (running && stamp != 0) {
            int64_t current = processStamp(pid);
            if (current != 0 && current != stamp) running = false;
        }
        return running;
    }

    inline std::string objectName(const std::string& base) { return "Local\\ThreadWeaver." + base; }

    class Semaphore {
    public:
        Semaphore() = default;
        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;
        ~Semaphore() { close(); }

        bool open(const std::string& base, long initial, long max_count) {
            close();
            handle_ = CreateSemaphoreA(nullptr, initial, max_count, objectName(base).c_str());
            return handle_ != nullptr;
        }

        WaitResult wait(int timeout_ms) {
            if (!handle_) return WaitResult::Failed;
            DWORD r = WaitForSingleObject(handle_, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
            if (r == WAIT_OBJECT_0) return WaitResult::Acquired;
            if (r == WAIT_TIMEOUT) return WaitResult::TimedOut;
            return WaitResult::Failed;
        }

        bool post(long n = 1) { return handle_ && ReleaseSemaphore(handle_, n, nullptr) != FALSE; }
        void close() { if (handle_) { CloseHandle(handle_); handle_ = nullptr; } }

        // Windows deletes named objects when the last handle closes.
        static void remove(const std::string&) {}

    private:
        HANDLE handle_ = nullptr;
    };

    class Mutex {
    public:
        Mutex() = default;
        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;
        ~Mutex() { close(); }

        bool open(const std::string& base) {
            close();
            handle_ = CreateMutexA(nullptr, FALSE, objectName(base).c_str());
            return handle_ != nullptr;
        }

        WaitResult wait(int timeout_ms) {
            if (!handle_) return WaitResult::Failed;
            DWORD r = WaitForSingleObject(handle_, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
            if (r == WAIT_OBJECT_0) return WaitResult::Acquired;
            if (r == WAIT_ABANDONED) return WaitResult::Abandoned;  // previous owner died holding it
            if (r == WAIT_TIMEOUT) return WaitResult::TimedOut;
            return WaitResult::Failed;
        }

        void release() { if (handle_) ReleaseMutex(handle_); }
        void close() { if (handle_) { CloseHandle(handle_); handle_ = nullptr; } }
        static void remove(const std::string&) {}

    private:
        HANDLE handle_ = nullptr;
    };

    class SharedMemory {
    public:
        SharedMemory() = default;
        SharedMemory(const SharedMemory&) = delete;
        SharedMemory& operator=(const SharedMemory&) = delete;
        ~SharedMemory() { close(); }

        bool open(const std::string& base, size_t size, bool& created) {
            close();
            mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(size), objectName(base).c_str());
            if (!mapping_) return false;
            created = GetLastError() != ERROR_ALREADY_EXISTS;
            view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size);
            if (!view_) { close(); return false; }
            return true;
        }

        void* data() const { return view_; }

        void close() {
            if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
            if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        }

        static void remove(const std::string&) {}

    private:
        HANDLE mapping_ = nullptr;
        void* view_ = nullptr;
    };

#else

    inline int32_t currentPid() { return static_cast<int32_t>(getpid()); }
    inline int64_t processStamp(int32_t) { return 0; }

    inline bool isAlive(int32_t pid, int64_t) {
        if (pid <= 0) return false;
        return kill(pid, 0) == 0 || errno == EPERM;
    }

    inline std::string objectName(const std::string& base) { return "/threadweaver." + base; }

    class Semaphore {
    public:
        Semaphore() = default;
        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;
        ~Semaphore() { close(); }

        bool open(const std::string& base, long initial, long /*max_count*/) {
            close();
            sem_ = sem_open(objectName(base).c_str(), O_CREAT, 0600, static_cast<unsigned>(initial));
            if (sem_ == SEM_FAILED) { sem_ = nullptr; return false; }
            return true;
        }

        // sem_timedwait doesn't exist on macOS, so poll with a short sleep instead.
        WaitResult wait(int timeout_ms) {
            if (!sem_) return WaitResult::Failed;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            for (;;) {
                if (sem_trywait(sem_) == 0) return WaitResult::Acquired;
                if (errno != EAGAIN && errno != EINTR) return WaitResult::Failed;
                if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return WaitResult::TimedOut;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        bool post(long n = 1) {
            if (!sem_) return false;
            for (long i = 0; i < n; ++i) {
                if (sem_post(sem_) != 0) return false;
            }
            return true;
        }

        void close() { if (sem_) { sem_close(sem_); sem_ = nullptr; } }
        static void remove(const std::string& base) { sem_unlink(objectName(base).c_str()); }

    private:
        sem_t* sem_ = nullptr;
    };

    // POSIX has no named mutex, so use a binary semaphore. It can't tell us
    // when its owner dies; the session code covers that with owner pids.
    class Mutex {
    public:
        bool open(const std::string& base) { return sem_.open(base, 1, 1); }
        WaitResult wait(int timeout_ms) { return sem_.wait(timeout_ms); }
        void release() { sem_.post(); }
        void close() { sem_.close(); }
        static void remove(const std::string& base) { Semaphore::remove(base); }

    private:
        Semaphore sem_;
    };

    class SharedMemory {
    public:
        SharedMemory() = default;
        SharedMemory(const SharedMemory&) = delete;
        SharedMemory& operator=(const SharedMemory&) = delete;
        ~SharedMemory() { close(); }

        bool open(const std::string& base, size_t size, bool& created) {
            close();
            int fd = shm_open(objectName(base).c_str(), O_CREAT | O_RDWR, 0600);
            if (fd < 0) return false;

            struct stat st {};
            if (fstat(fd, &st) != 0) { ::close(fd); return false; }
            created = st.st_size == 0;
            if (created && ftruncate(fd, static_cast<off_t>(size)) != 0) { ::close(fd); return false; }
            if (!created && static_cast<size_t>(st.st_size) < size) { ::close(fd); return false; }

            void* view = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (view == MAP_FAILED) return false;
            view_ = view;
            size_ = size;
            return true;
        }

        void* data() const { return view_; }

        void close() {
            if (view_) { munmap(view_, size_); view_ = nullptr; }
        }

        static void remove(const std::string& base) { shm_unlink(objectName(base).c_str()); }

    private:
        void* view_ = nullptr;
        size_t size_ = 0;
    };

#endif
}

namespace Toolkit {

    namespace Color {
        inline bool enable_colors = true;

        inline std::string Reset()     { return enable_colors ? "\033[0m"  : ""; }
        inline std::string Bold()      { return enable_colors ? "\033[1m"  : ""; }
        inline std::string Dim()       { return enable_colors ? "\033[2m"  : ""; }
        inline std::string Red()       { return enable_colors ? "\033[31m" : ""; }
        inline std::string Green()     { return enable_colors ? "\033[32m" : ""; }
        inline std::string Yellow()    { return enable_colors ? "\033[33m" : ""; }
        inline std::string Blue()      { return enable_colors ? "\033[34m" : ""; }
        inline std::string Magenta()   { return enable_colors ? "\033[35m" : ""; }
        inline std::string Cyan()      { return enable_colors ? "\033[36m" : ""; }
        inline std::string White()     { return enable_colors ? "\033[37m" : ""; }
        inline std::string BgRed()     { return enable_colors ? "\033[41m" : ""; }
        inline std::string BgYellow()  { return enable_colors ? "\033[43m" : ""; }
        inline std::string BgCyan()    { return enable_colors ? "\033[46m" : ""; }
    }

    class Timer {
    private:
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        bool running = false;

    public:
        void start() {
            start_time = std::chrono::steady_clock::now();
            running = true;
        }

        void stop() {
            end_time = std::chrono::steady_clock::now();
            running = false;
        }

        double elapsedMilliseconds() const {
            auto end = running ? std::chrono::steady_clock::now() : end_time;
            return std::chrono::duration<double, std::milli>(end - start_time).count();
        }
    };

    class Logger {
    public:
        enum class Level { INFO, WARN, ERROR, OK };

    private:
        std::mutex log_mutex;
        std::ofstream log_file;
        ipc::Mutex file_lock;   // several terminals append to the same file
        bool file_lock_ok = false;
        bool echo = true;       // also print to the console
        std::string source = "--";

    public:
        Logger() {
            log_file.open("threadweaver_session_log.txt", std::ios::out | std::ios::app);
            file_lock_ok = file_lock.open("logfile");
        }

        void setSource(const std::string& name) {
            std::lock_guard<std::mutex> lock(log_mutex);
            source = name;
        }

        // Screens redraw themselves, so console output would just flicker past.
        // With echo off, messages only go to the log file.
        void setEcho(bool on) {
            std::lock_guard<std::mutex> lock(log_mutex);
            echo = on;
        }

        void log(Level level, const std::string& sender, const std::string& message) {
            std::lock_guard<std::mutex> lock(log_mutex);

            auto now = std::chrono::system_clock::now();
            std::time_t now_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            std::stringstream ss;
            ss << std::put_time(std::localtime(&now_t), "%H:%M:%S")
               << '.' << std::setfill('0') << std::setw(3) << ms.count();
            std::string timestamp = ss.str();

            std::string tag;
            std::string color;
            switch (level) {
                case Level::INFO:  tag = "INFO "; color = Color::Cyan();   break;
                case Level::WARN:  tag = "WARN "; color = Color::Yellow(); break;
                case Level::ERROR: tag = "ERROR"; color = Color::Red();    break;
                case Level::OK:    tag = "OK   "; color = Color::Green();  break;
            }

            if (echo) {
                std::cout << "  " << Color::Dim() << timestamp << Color::Reset() << " "
                          << color << Color::Bold() << tag << Color::Reset() << " "
                          << Color::Yellow() << sender << Color::Reset() << ": " << message << std::endl;
            }

            if (log_file.is_open()) {
                // If another terminal hangs on to the log lock we write anyway
                // rather than freeze the UI.
                bool locked = false;
                if (file_lock_ok) {
                    ipc::WaitResult r = file_lock.wait(300);
                    locked = (r == ipc::WaitResult::Acquired || r == ipc::WaitResult::Abandoned);
                }
                log_file << timestamp << " [" << source << "] " << tag << " " << sender << ": " << message << "\n";
                log_file.flush();
                if (locked) file_lock.release();
            }
        }
    };

    inline Logger g_Logger;

    namespace UI {

        // Thrown when stdin is closed (Ctrl+Z / Ctrl+D or a closed pipe) so we
        // can shut down cleanly instead of spinning on a dead stream.
        struct InputClosed : std::runtime_error {
            InputClosed() : std::runtime_error("input stream was closed") {}
        };

        inline void InitTerminal() {
            #ifdef _WIN32
                SetConsoleOutputCP(65001);
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD dwMode = 0;
                if (GetConsoleMode(hOut, &dwMode)) {
                    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                }
            #endif
        }

        inline void ClearScreen() {
            #ifdef _WIN32
                std::system("cls");
            #else
                std::cout << "\033[2J\033[1;1H";
            #endif
        }

        inline void TypeText(const std::string& text, int delay_ms = 18) {
            for (char c : text) {
                std::cout << c << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }

        inline void PrintBanner(bool animate = false) {
            const std::string banner = R"(
  +---------------------------------------------------------------------------------+
  |  _____ _   _ ____  _____    _    ____  __        _______    ___   _____ ____    |
  | |_   _| | | |  _ \| ____|  / \  |  _ \ \ \      / / ____|  / _ \ | ____|  _ \   |
  |   | | | |_| | |_) |  _|   / _ \ | | | | \ \ /\ / /|  _|   | | | ||  _| | |_) |  |
  |   | | |  _  |  _ <| |___ / ___ \| |_| |  \ V  V / | |___  | |_| || |___|  _ <   |
  |   |_| |_| |_|_| \_\_____/_/   \_\____/    \_/\_/  |_____|  \___/ |_____|_| \_\  |
  |                                                                                 |
  |                    OS LAB PROJECT - PROCESS SYNCHRONIZATION                     |
  +---------------------------------------------------------------------------------+
)";
            std::cout << Color::Cyan() << Color::Bold();
            if (animate) {
                std::stringstream ss(banner);
                std::string line;
                while (std::getline(ss, line)) {
                    std::cout << line << "\n" << std::flush;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            } else {
                std::cout << banner;
            }
            std::cout << Color::Reset() << "\n";
        }

        inline void LoadingDots(const std::string& label, int dots = 3, int delay_ms = 150) {
            std::cout << "  " << Color::Dim() << label << std::flush;
            for (int i = 0; i < dots; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                std::cout << "." << std::flush;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            std::cout << "\r" << std::string(label.length() + dots + 4, ' ') << "\r" << Color::Reset() << std::flush;
        }

        inline void BlinkAlert(const std::string& text, int blinks = 3) {
            std::string blank(text.length(), ' ');
            for (int i = 0; i < blinks; ++i) {
                std::cout << "\r" << Color::BgRed() << Color::White() << Color::Bold() << text << Color::Reset() << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
                std::cout << "\r" << blank << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }
            std::cout << "\r" << Color::BgRed() << Color::White() << Color::Bold() << text << Color::Reset() << "\n\n";
        }

        inline void PrintHeader(const std::string& title, bool animate = true) {
            int total_width = 83;
            int padding = (total_width - 4 - static_cast<int>(title.length())) / 2;
            std::string left_pad(std::max(0, padding), ' ');
            std::string right_pad(std::max(0, total_width - 4 - static_cast<int>(title.length()) - padding), ' ');

            std::cout << Color::Yellow() << Color::Bold() << "+";
            for (int drawn = 0; drawn < total_width - 2; drawn += 9) {
                std::cout << std::string(std::min(9, total_width - 2 - drawn), '=') << std::flush;
                if (animate) std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
            std::cout << "+\n"
                      << "| " << left_pad << Color::White() << title << Color::Yellow() << right_pad << " |\n"
                      << "+" << std::string(total_width - 2, '=') << "+\n"
                      << Color::Reset() << "\n";
        }

        inline bool animate_menu = false;

        inline void PrintMenuCard(const std::string& num, const std::string& title, const std::string& desc) {
            std::cout << "  " << Color::Cyan() << Color::Bold() << "[" << num << "]" << Color::Reset()
                      << " " << Color::Bold() << std::left << std::setw(32) << title << Color::Reset()
                      << Color::Dim() << desc << Color::Reset() << "\n" << std::flush;
            if (animate_menu) std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }

        inline void PrintOption(const std::string& num, const std::string& text) {
            std::cout << "  " << Color::Cyan() << "[" << num << "]" << Color::Reset() << " " << text << "\n";
        }

        // One-line result of the last action, shown under the screen's status.
        struct Status {
            enum class Kind { None, Ok, Info, Warn, Error };
            Kind kind = Kind::None;
            std::string text;

            static Status Ok(std::string t)    { return {Kind::Ok, std::move(t)}; }
            static Status Info(std::string t)  { return {Kind::Info, std::move(t)}; }
            static Status Warn(std::string t)  { return {Kind::Warn, std::move(t)}; }
            static Status Error(std::string t) { return {Kind::Error, std::move(t)}; }
        };

        inline void PrintStatus(const Status& s) {
            if (s.kind == Status::Kind::None || s.text.empty()) return;
            std::string color = Color::Cyan();
            if (s.kind == Status::Kind::Ok)    color = Color::Green();
            if (s.kind == Status::Kind::Warn)  color = Color::Yellow();
            if (s.kind == Status::Kind::Error) color = Color::Red();
            std::cout << "  " << color << Color::Bold() << "> " << Color::Reset() << color << s.text << Color::Reset() << "\n\n";
        }

        inline void PrintErrorCard(const std::string& msg) {
            std::cout << "\n  " << Color::BgRed() << Color::White() << Color::Bold() << " ERROR " << Color::Reset()
                      << " " << Color::Red() << msg << Color::Reset() << "\n";
        }

        inline void PrintWarningCard(const std::string& msg) {
            std::cout << "\n  " << Color::BgYellow() << Color::White() << Color::Bold() << " NOTE " << Color::Reset()
                      << " " << Color::Yellow() << msg << Color::Reset() << "\n";
        }

        inline void SpinnerAnimation(const std::string& task_label, int duration_ms = 400) {
            const char frames[] = {'|', '/', '-', '\\'};
            int steps = duration_ms / 50;
            for (int i = 0; i < steps; ++i) {
                std::cout << "\r  " << Color::Yellow() << Color::Bold() << frames[i % 4] << Color::Reset()
                          << " " << Color::Cyan() << task_label << "..." << Color::Reset() << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::cout << "\r" << std::string(task_label.length() + 20, ' ') << "\r" << std::flush;
        }

        // Runs `work` on a worker thread and keeps a spinner with a countdown
        // going on this thread until it finishes. Used for the semaphore waits,
        // which can block for a while when another terminal holds the resource.
        // `on_tick` runs about every 400 ms on this thread so the caller can
        // redraw the screen when another terminal changes something.
        template <typename Work, typename Tick>
        auto RunWithSpinner(const std::string& label, int timeout_ms, Work&& work, Tick&& on_tick) -> decltype(work()) {
            auto task = std::async(std::launch::async, std::forward<Work>(work));
            const char frames[] = {'|', '/', '-', '\\'};
            auto started = std::chrono::steady_clock::now();
            std::string line;

            for (int frame = 0;; ++frame) {
                bool done = task.wait_for(std::chrono::milliseconds(80)) == std::future_status::ready;
                if (done && frame >= 4) break;  // show a few frames even when it's instant
                if (done) std::this_thread::sleep_for(std::chrono::milliseconds(80));
                if (!done && frame % 5 == 0) on_tick();

                auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                long long left = std::max<long long>(0, (timeout_ms - waited + 999) / 1000);
                line = label + "  (" + std::to_string(left) + "s left)";
                std::cout << "\r  " << Color::Yellow() << Color::Bold() << frames[frame % 4] << Color::Reset()
                          << " " << Color::Cyan() << line << Color::Reset() << "   " << std::flush;
            }
            std::cout << "\r" << std::string(line.length() + 10, ' ') << "\r" << std::flush;
            return task.get();
        }

        template <typename Work>
        auto RunWithSpinner(const std::string& label, int timeout_ms, Work&& work) -> decltype(work()) {
            return RunWithSpinner(label, timeout_ms, std::forward<Work>(work), [] {});
        }

        inline void ProgressBarAnimation(const std::string& label, int duration_ms = 500) {
            int width = 30;
            int steps = 20;
            int delay = duration_ms / steps;

            for (int i = 0; i <= steps; ++i) {
                float progress = static_cast<float>(i) / steps;
                int pos = static_cast<int>(width * progress);

                std::cout << "\r  " << Color::Bold() << label << " [" << Color::Reset();
                for (int j = 0; j < width; ++j) {
                    if (j < pos) std::cout << Color::Green() << "=";
                    else if (j == pos) std::cout << Color::Yellow() << ">";
                    else std::cout << " ";
                }
                std::cout << Color::Reset() << Color::Bold() << "] "
                          << std::right << std::setw(3) << static_cast<int>(progress * 100.0) << std::left
                          << "% " << Color::Reset() << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            std::cout << "\n";
        }

        inline std::string ReadLine(const std::string& prompt) {
            std::cout << prompt << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) throw InputClosed();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        // Returns true and sets `out` if `line` is a whole number in range;
        // otherwise prints why it was rejected. Blank lines are rejected silently.
        inline bool ParseChoice(const std::string& line, int min_val, int max_val, int& out) {
            if (line.find_first_not_of(" \t") == std::string::npos) return false;

            std::istringstream in(line);
            long long val = 0;
            char extra = 0;
            if (!(in >> val) || (in >> extra)) {
                PrintErrorCard("Invalid input. Enter a whole number.");
                return false;
            }
            if (val < min_val || val > max_val) {
                PrintErrorCard("Enter a value from " + std::to_string(min_val) + " to " + std::to_string(max_val) + ".");
                return false;
            }
            out = static_cast<int>(val);
            return true;
        }

        inline int SafeReadInt(const std::string& prompt, int min_val, int max_val) {
            for (;;) {
                int val = 0;
                if (ParseChoice(ReadLine(prompt), min_val, max_val, val)) return val;
            }
        }

        // Like SafeReadInt, but while nothing is being typed it calls
        // `refresh` twice a second. If refresh redraws the screen (returns
        // true) the prompt and anything typed so far are printed again. This
        // keeps shared screens live while the user is deciding what to do.
        // Needs a real console; with redirected input it falls back to SafeReadInt.
        template <typename Refresh>
        inline int ReadChoiceLive(const std::string& prompt, int min_val, int max_val, Refresh&& refresh) {
#ifdef _WIN32
            DWORD mode = 0;
            if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
                std::string typed;
                std::cout << prompt << std::flush;
                auto last_check = std::chrono::steady_clock::now();

                for (;;) {
                    if (_kbhit()) {
                        int c = _getch();
                        if (c == 0 || c == 0xE0) { _getch(); continue; }        // arrow/function keys
                        if (c == 3 || (c == 26 && typed.empty())) throw InputClosed();  // Ctrl+C / Ctrl+Z
                        if (c == '\r' || c == '\n') {
                            std::cout << "\n";
                            int val = 0;
                            if (ParseChoice(typed, min_val, max_val, val)) return val;
                            typed.clear();
                            std::cout << prompt << std::flush;
                        } else if (c == '\b') {
                            if (!typed.empty()) { typed.pop_back(); std::cout << "\b \b" << std::flush; }
                        } else if (c >= 32 && c < 127 && typed.size() < 12) {
                            typed += static_cast<char>(c);
                            std::cout << static_cast<char>(c) << std::flush;
                        }
                        continue;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_check >= std::chrono::milliseconds(500)) {
                        last_check = now;
                        if (refresh()) std::cout << prompt << typed << std::flush;
                    }
                }
            }
#endif
            (void)refresh;
            return SafeReadInt(prompt, min_val, max_val);
        }

        inline void Pause() {
            ReadLine("\n" + Color::Dim() + "  Press Enter to continue." + Color::Reset());
        }
    }
}

// ---------------------------------------------------------------------------
// The shared session: one block of shared memory that every open terminal maps,
// plus the semaphores and mutexes that guard it.
//
// Lock order (always the same, which is what keeps this deadlock free):
//     read-count lock  ->  db write semaphore  ->  state lock
//     empty/full semaphore  ->  state lock
// The state lock is only ever held for a few lines and never while waiting on
// anything else or waiting for the user to type.
// ---------------------------------------------------------------------------
namespace Toolkit {

    constexpr int kMaxTerminals = 8;
    constexpr int kMaxBufferSize = 100;
    constexpr int kActivitySize = 8;
    constexpr uint32_t kMagic = 0x54575331;   // "TWS1"
    constexpr int kStateLockMs = 3000;
    constexpr int kReleaseLockMs = 10000;

    enum WaitingFor : int32_t { kNotWaiting = 0, kWaitProduce, kWaitConsume, kWaitRead, kWaitWrite };

    struct TerminalSlot {
        int32_t pid;        // 0 means the slot is free
        int32_t number;     // what we show to people: terminal 1, 2, ...
        int64_t stamp;
        int32_t reading;
        int32_t writing;
        int32_t waiting;    // one of WaitingFor
    };

    struct ActivityLine {
        int64_t when;
        int32_t terminal;
        char text[100];
    };

    struct SharedData {
        TerminalSlot terminals[kMaxTerminals];

        // producer-consumer
        int32_t capacity;
        int32_t items[kMaxBufferSize];
        int32_t item_from[kMaxBufferSize];   // terminal that produced each item
        int32_t front;
        int32_t rear;
        int32_t count;
        uint64_t produced;
        uint64_t consumed;

        // readers-writers
        int32_t db_value;
        int32_t read_count;       // guarded by the read-count lock, not the state lock
        int32_t writer_terminal;  // 0 when nobody is writing
        uint64_t reads;
        uint64_t writes;

        // last few things that happened, from every terminal
        uint32_t activity_next;
        uint32_t change_seq;      // bumped on every change so screens know when to redraw
        ActivityLine activity[kActivitySize];
    };

    struct SharedBlock {
        uint32_t magic;
        uint32_t layout_size;
        std::atomic<int32_t> state_owner;   // pid holding the state lock
        std::atomic<int32_t> rc_owner;      // pid holding the read-count lock
        SharedData data;
    };

    static_assert(std::atomic<int32_t>::is_always_lock_free, "need lock-free atomics for shared memory");

    struct OpResult {
        enum class Status { Ok, TimedOut, Failed };
        Status status = Status::Failed;
        int value = 0;
        std::string message;

        bool ok() const { return status == Status::Ok; }
        static OpResult Ok(int v = 0, std::string m = "") { return {Status::Ok, v, std::move(m)}; }
        static OpResult TimedOut(std::string m) { return {Status::TimedOut, 0, std::move(m)}; }
        static OpResult Failed(std::string m) { return {Status::Failed, 0, std::move(m)}; }
    };

    // Scoped hold on a cross-process lock. Records the owner's pid so that if a
    // terminal dies while holding it, the others can tell and take over.
    class CrossLock {
    public:
        CrossLock(ipc::Mutex& m, std::atomic<int32_t>& owner, int timeout_ms) : mutex_(m), owner_(owner) {
            int32_t me = ipc::currentPid();
            ipc::WaitResult r = mutex_.wait(timeout_ms);
            if (r == ipc::WaitResult::Acquired || r == ipc::WaitResult::Abandoned) {
                owner_.store(me);
                held_ = true;
                recovered_ = (r == ipc::WaitResult::Abandoned);
                return;
            }
#ifndef _WIN32
            // POSIX semaphores don't notice a dead owner. If the recorded owner
            // is gone, inherit its hold (the semaphore count is still 0, so our
            // release later puts it back to 1). The CAS stops two terminals from
            // both doing this.
            if (r == ipc::WaitResult::TimedOut) {
                int32_t holder = owner_.load();
                if (holder != 0 && holder != me && !ipc::isAlive(holder, 0) &&
                    owner_.compare_exchange_strong(holder, me)) {
                    held_ = true;
                    recovered_ = true;
                }
            }
#endif
        }

        ~CrossLock() {
            if (held_) {
                owner_.store(0);
                mutex_.release();
            }
        }

        CrossLock(const CrossLock&) = delete;
        CrossLock& operator=(const CrossLock&) = delete;

        bool held() const { return held_; }
        bool recovered() const { return recovered_; }

    private:
        ipc::Mutex& mutex_;
        std::atomic<int32_t>& owner_;
        bool held_ = false;
        bool recovered_ = false;
    };

    class SharedSession {
    public:
        struct Snapshot {
            bool ok = false;
            int capacity = 0, count = 0, front = 0;
            std::vector<int> items;
            uint64_t produced = 0, consumed = 0;
            int db_value = 0, active_readers = 0, writer_terminal = 0;
            uint64_t reads = 0, writes = 0;
            int waiting_producers = 0, waiting_consumers = 0, waiting_readers = 0, waiting_writers = 0;
            int terminals_online = 0;
            uint32_t version = 0;
            std::vector<std::string> activity;
        };

        ~SharedSession() { detach(); }

        bool attach(int capacity_hint, std::string& error) {
            if (block_) return true;
            pid_ = ipc::currentPid();
            stamp_ = ipc::processStamp(pid_);

            if (!openBlock(error, true)) return false;

            // Every failure below leaves the locked scope before closeAll(), so
            // the lock is released while the shared memory is still mapped.
            bool ok = false;
            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (!lock.held()) {
                    error = "shared state is locked by another terminal";
                } else if (block_->magic == kMagic && block_->layout_size != sizeof(SharedBlock)) {
                    error = "another terminal is running a different version; close it and try again";
                } else {
                    if (block_->magic != kMagic) initialise(capacity_hint);
                    ok = openSemaphoresLocked();
                    if (!ok) error = "could not create shared semaphores";
                }
            }
            if (!ok) { closeAll(); return false; }

            reapDeadTerminals();   // free any slots left behind by crashed terminals

            ok = false;
            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                SharedData& d = block_->data;

                my_index_ = -1;
                if (!lock.held()) {
                    error = "shared state is locked by another terminal";
                } else {
                    for (int i = 0; i < kMaxTerminals; ++i) {
                        if (d.terminals[i].pid == 0) { my_index_ = i; break; }
                    }
                    if (my_index_ < 0) error = "terminal limit reached (" + std::to_string(kMaxTerminals) + ")";
                }

                if (my_index_ >= 0) {
                    // Lowest number nobody else is using, so people see 1, 2, 3...
                    number_ = 1;
                    for (bool taken = true; taken; ) {
                        taken = false;
                        for (const auto& t : d.terminals) {
                            if (t.pid != 0 && t.number == number_) { taken = true; ++number_; break; }
                        }
                    }

                    d.terminals[my_index_] = TerminalSlot{pid_, number_, stamp_, 0, 0, kNotWaiting};
                    logActivityLocked("connected");
                    ok = true;
                }
            }
            if (!ok) { closeAll(); return false; }

            stop_reaper_ = false;
            reaper_ = std::thread(&SharedSession::reaperLoop, this);
            return true;
        }

        void detach() {
            if (!block_) return;
            stopReaper();
            releaseHeld();

            bool last = false;
            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (lock.held()) {
                    SharedData& d = block_->data;
                    d.terminals[my_index_] = TerminalSlot{};
                    logActivityLocked("disconnected");
                    last = true;
                    for (const auto& t : d.terminals) {
                        if (t.pid != 0) { last = false; break; }
                    }
                }
            }
            closeAll();
#ifndef _WIN32
            // POSIX objects stick around after everyone exits; tidy up.
            if (last) removeNames();
#else
            (void)last;
#endif
        }

        bool isAttached() const { return block_ != nullptr; }
        int terminalNumber() const { return number_; }
        bool isReading() const { return reading_; }
        bool isWriting() const { return writing_; }

        // Drops any read or write hold this terminal still has. Safe to call
        // any time; used on the way out and after unexpected errors.
        void releaseHeld() {
            if (writing_) endWrite(false, 0);
            if (reading_) endRead();
        }

        Snapshot snapshot() {
            Snapshot s;
            if (!block_) return s;
            CrossLock lock(state_lock_, block_->state_owner, 1000);
            if (!lock.held()) return s;

            const SharedData& d = block_->data;
            s.ok = true;
            s.version = d.change_seq;
            s.capacity = d.capacity;
            s.count = d.count;
            s.front = d.front;
            s.items.assign(d.items, d.items + d.capacity);
            s.produced = d.produced;
            s.consumed = d.consumed;
            s.db_value = d.db_value;
            s.writer_terminal = d.writer_terminal;
            s.reads = d.reads;
            s.writes = d.writes;

            for (const auto& t : d.terminals) {
                if (t.pid == 0) continue;
                s.terminals_online++;
                s.active_readers += t.reading;
                if (t.waiting == kWaitProduce) s.waiting_producers++;
                if (t.waiting == kWaitConsume) s.waiting_consumers++;
                if (t.waiting == kWaitRead)    s.waiting_readers++;
                if (t.waiting == kWaitWrite)   s.waiting_writers++;
            }

            uint32_t end = d.activity_next;
            uint32_t begin = end > static_cast<uint32_t>(kActivitySize) ? end - kActivitySize : 0;
            for (uint32_t i = begin; i < end; ++i) {
                const ActivityLine& line = d.activity[i % kActivitySize];
                std::time_t when = static_cast<std::time_t>(line.when);
                char clock[16] = "--:--:--";
                if (std::tm* tm = std::localtime(&when)) std::strftime(clock, sizeof(clock), "%H:%M:%S", tm);
                std::string text(line.text, strnlen(line.text, sizeof(line.text)));
                s.activity.push_back(std::string(clock) + "  T" + std::to_string(line.terminal) + "  " + text);
            }
            return s;
        }

        // ---- producer-consumer --------------------------------------------

        OpResult produce(int value, int timeout_ms) {
            if (!block_) return OpResult::Failed("Not connected to the shared session.");
            if (!setWaiting(kWaitProduce)) return busyResult();

            // Classic order: wait(empty) -> lock -> add item -> unlock -> signal(full)
            ipc::WaitResult r = empty_.wait(timeout_ms);
            OpResult result;
            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (!lock.held()) {
                    if (r == ipc::WaitResult::Acquired) empty_.post();   // don't lose the slot
                    return busyResult();
                }
                SharedData& d = block_->data;
                d.terminals[my_index_].waiting = kNotWaiting;

                if (r == ipc::WaitResult::TimedOut)
                    return OpResult::TimedOut("Timed out. The buffer is full; consume an item to free a slot.");
                if (r != ipc::WaitResult::Acquired)
                    return OpResult::Failed("Could not wait on the empty-slot semaphore.");

                if (d.count >= d.capacity) {
                    // The semaphore let us in but the buffer is full. That should
                    // never happen; keep the data safe and don't pass the bad token on.
                    logActivityLocked("buffer inconsistency detected on produce");
                    return OpResult::Failed("Buffer state is inconsistent. The item was not added.");
                }

                d.items[d.rear] = value;
                d.item_from[d.rear] = number_;
                d.rear = (d.rear + 1) % d.capacity;
                d.count++;
                d.produced++;
                logActivityLocked("produced " + std::to_string(value));
                result = OpResult::Ok(value, "You produced " + std::to_string(value) + ".");
            }
            full_.post();
            return result;
        }

        OpResult consume(int timeout_ms) {
            if (!block_) return OpResult::Failed("Not connected to the shared session.");
            if (!setWaiting(kWaitConsume)) return busyResult();

            // Classic order: wait(full) -> lock -> take item -> unlock -> signal(empty)
            ipc::WaitResult r = full_.wait(timeout_ms);
            OpResult result;
            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (!lock.held()) {
                    if (r == ipc::WaitResult::Acquired) full_.post();
                    return busyResult();
                }
                SharedData& d = block_->data;
                d.terminals[my_index_].waiting = kNotWaiting;

                if (r == ipc::WaitResult::TimedOut)
                    return OpResult::TimedOut("Timed out. The buffer is empty; produce an item first.");
                if (r != ipc::WaitResult::Acquired)
                    return OpResult::Failed("Could not wait on the full-slot semaphore.");

                if (d.count <= 0) {
                    logActivityLocked("buffer inconsistency detected on consume");
                    return OpResult::Failed("Buffer state is inconsistent. No item was consumed.");
                }

                int item = d.items[d.front];
                int from = d.item_from[d.front];
                d.front = (d.front + 1) % d.capacity;
                d.count--;
                d.consumed++;
                logActivityLocked("consumed " + std::to_string(item));
                std::string origin = (from == number_ || from == 0) ? "" : " (produced by Terminal " + std::to_string(from) + ")";
                result = OpResult::Ok(item, "You consumed " + std::to_string(item) + origin + ".");
            }
            empty_.post();
            return result;
        }

        // Only allowed while the buffer is empty and nobody is waiting on it,
        // because the empty-slot semaphore has to be topped up or drained to match.
        OpResult resizeBuffer(int new_capacity) {
            if (!block_) return OpResult::Failed("Not connected to the shared session.");
            new_capacity = std::max(1, std::min(kMaxBufferSize, new_capacity));

            CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
            if (!lock.held()) return busyResult();
            SharedData& d = block_->data;

            if (d.capacity == new_capacity) return OpResult::Ok(new_capacity, "Buffer size unchanged.");
            if (d.count != 0)
                return OpResult::Failed("Buffer must be empty to resize (" + std::to_string(d.count) + " item(s) remaining).");
            for (const auto& t : d.terminals) {
                if (t.pid != 0 && (t.waiting == kWaitProduce || t.waiting == kWaitConsume))
                    return OpResult::Failed("Terminal " + std::to_string(t.number) + " is waiting on the buffer. Try again shortly.");
            }

            int old_capacity = d.capacity;
            if (new_capacity > old_capacity) {
                if (!empty_.post(new_capacity - old_capacity))
                    return OpResult::Failed("Could not update the empty-slot semaphore.");
            } else {
                int needed = old_capacity - new_capacity, taken = 0;
                while (taken < needed && empty_.wait(0) == ipc::WaitResult::Acquired) ++taken;
                if (taken < needed) {
                    if (taken > 0) empty_.post(taken);
                    return OpResult::Failed("Could not resize the buffer safely. Size unchanged.");
                }
            }

            d.capacity = new_capacity;
            d.front = d.rear = 0;
            logActivityLocked("set buffer size to " + std::to_string(new_capacity));
            return OpResult::Ok(new_capacity, "Buffer size set to " + std::to_string(new_capacity) + ".");
        }

        // ---- readers-writers (readers get priority) ------------------------

        OpResult beginRead(int timeout_ms) {
            if (!block_) return OpResult::Failed("Not connected to the shared session.");
            if (reading_ || writing_) return OpResult::Failed("This terminal already holds a database lock.");
            if (!setWaiting(kWaitRead)) return busyResult();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            SharedData& d = block_->data;
            int value = 0;
            bool recovered = false;
            {
                CrossLock rc(rc_lock_, block_->rc_owner, timeout_ms);
                if (!rc.held()) {
                    clearWaiting();
                    return OpResult::TimedOut("Timed out waiting for read access.");
                }
                recovered = rc.recovered();

                // First reader in locks writers out for the whole group.
                bool first = d.read_count <= 0;
                if (first) {
                    d.read_count = 0;
                    ipc::WaitResult r = wrt_.wait(remainingMs(deadline));
                    if (r != ipc::WaitResult::Acquired) {
                        clearWaiting();
                        if (r == ipc::WaitResult::TimedOut)
                            return OpResult::TimedOut("Timed out. A writer is holding the database.");
                        return OpResult::Failed("Could not wait on the database semaphore.");
                    }
                }
                d.read_count++;

                CrossLock state(state_lock_, block_->state_owner, kStateLockMs);
                if (!state.held()) {
                    // Undo, otherwise writers would be shut out for good.
                    d.read_count--;
                    if (first) wrt_.post();
                    return busyResult();
                }
                TerminalSlot& me = d.terminals[my_index_];
                me.waiting = kNotWaiting;
                me.reading = 1;
                d.reads++;
                value = d.db_value;
                logActivityLocked("started reading (value " + std::to_string(value) + ")");
            }
            reading_ = true;
            return OpResult::Ok(value, recovered ? "Recovered a lock held by a closed terminal." : "");
        }

        OpResult endRead() {
            if (!block_ || !reading_) return OpResult::Failed("No read lock is held.");
            SharedData& d = block_->data;
            {
                CrossLock rc(rc_lock_, block_->rc_owner, kReleaseLockMs);
                if (!rc.held()) return OpResult::Failed("Could not release the read lock. Try again.");

                if (d.read_count > 0) {
                    d.read_count--;
                    if (d.read_count == 0) wrt_.post();   // last reader out lets writers in
                }

                CrossLock state(state_lock_, block_->state_owner, kStateLockMs);
                if (state.held()) {
                    d.terminals[my_index_].reading = 0;
                    logActivityLocked("finished reading");
                }
            }
            reading_ = false;
            return OpResult::Ok();
        }

        OpResult beginWrite(int timeout_ms) {
            if (!block_) return OpResult::Failed("Not connected to the shared session.");
            if (reading_ || writing_) return OpResult::Failed("This terminal already holds a database lock.");
            if (!setWaiting(kWaitWrite)) return busyResult();

            ipc::WaitResult r = wrt_.wait(timeout_ms);
            CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
            if (!lock.held()) {
                if (r == ipc::WaitResult::Acquired) wrt_.post();
                return busyResult();
            }
            SharedData& d = block_->data;
            d.terminals[my_index_].waiting = kNotWaiting;

            if (r == ipc::WaitResult::TimedOut)
                return OpResult::TimedOut("Timed out. The database is in use by readers or another writer.");
            if (r != ipc::WaitResult::Acquired)
                return OpResult::Failed("Could not wait on the database semaphore.");

            d.writer_terminal = number_;
            d.terminals[my_index_].writing = 1;
            logActivityLocked("started writing");
            writing_ = true;
            return OpResult::Ok(d.db_value);
        }

        OpResult endWrite(bool commit, int value) {
            if (!block_ || !writing_) return OpResult::Failed("No write lock is held.");
            OpResult result = OpResult::Ok(value);
            {
                CrossLock lock(state_lock_, block_->state_owner, kReleaseLockMs);
                if (lock.held()) {
                    SharedData& d = block_->data;
                    if (commit) {
                        d.db_value = value;
                        d.writes++;
                    }
                    d.writer_terminal = 0;
                    d.terminals[my_index_].writing = 0;
                    logActivityLocked(commit ? "wrote " + std::to_string(value) : "cancelled the write");
                } else if (commit) {
                    result = OpResult::Failed("Could not save the value. Shared state is locked.");
                }
            }
            // Always give the database back, even if saving failed.
            wrt_.post();
            writing_ = false;
            return result;
        }

    private:
        static int remainingMs(std::chrono::steady_clock::time_point deadline) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            return static_cast<int>(std::max<long long>(0, left));
        }

        static OpResult busyResult() {
            return OpResult::Failed("Shared state is busy. Try again.");
        }

        bool openBlock(std::string& error, bool allow_reset) {
            if (!state_lock_.open("state") || !rc_lock_.open("readcount")) {
                error = "could not create shared locks";
                closeAll();
                return false;
            }
            bool created = false;
            if (!memory_.open("shared", sizeof(SharedBlock), created)) {
                error = "could not open shared memory (an older version may still be running)";
                closeAll();
                return false;
            }
            block_ = static_cast<SharedBlock*>(memory_.data());

#ifndef _WIN32
            // POSIX objects outlive the processes that made them. If nobody in
            // the table is alive any more, it's left over from a crashed run.
            if (!created && allow_reset && block_->magic == kMagic) {
                bool anyone = false;
                for (const auto& t : block_->data.terminals) {
                    if (t.pid != 0 && t.pid != pid_ && ipc::isAlive(t.pid, t.stamp)) { anyone = true; break; }
                }
                if (!anyone) {
                    closeAll();
                    removeNames();
                    return openBlock(error, false);
                }
            }
#else
            (void)created;
            (void)allow_reset;
#endif
            return true;
        }

        void initialise(int capacity_hint) {
            block_->data = SharedData{};
            block_->data.capacity = std::max(1, std::min(kMaxBufferSize, capacity_hint));
            block_->data.db_value = 100;
            block_->layout_size = sizeof(SharedBlock);
            block_->magic = kMagic;
        }

        // Initial values only matter when the semaphore is being created. If
        // another terminal already made it, the OS keeps the current count.
        bool openSemaphoresLocked() {
            const SharedData& d = block_->data;
            bool db_free = d.read_count <= 0 && d.writer_terminal == 0;
            return empty_.open("slots.empty", d.capacity - d.count, kMaxBufferSize) &&
                   full_.open("slots.full", d.count, kMaxBufferSize) &&
                   wrt_.open("db.write", db_free ? 1 : 0, 1);
        }

        void closeAll() {
            empty_.close();
            full_.close();
            wrt_.close();
            memory_.close();
            rc_lock_.close();
            state_lock_.close();
            block_ = nullptr;
            number_ = 0;
            my_index_ = -1;
        }

        static void removeNames() {
            ipc::SharedMemory::remove("shared");
            ipc::Mutex::remove("state");
            ipc::Mutex::remove("readcount");
            ipc::Semaphore::remove("slots.empty");
            ipc::Semaphore::remove("slots.full");
            ipc::Semaphore::remove("db.write");
        }

        // Caller must hold the state lock.
        void logActivityLocked(const std::string& text) {
            SharedData& d = block_->data;
            ActivityLine& line = d.activity[d.activity_next % kActivitySize];
            line.when = static_cast<int64_t>(std::time(nullptr));
            line.terminal = number_;
            std::snprintf(line.text, sizeof(line.text), "%s", text.c_str());
            d.activity_next++;
            d.change_seq++;
        }

        bool setWaiting(int32_t what) {
            CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
            if (!lock.held()) return false;
            block_->data.terminals[my_index_].waiting = what;
            block_->data.change_seq++;
            return true;
        }

        void clearWaiting() { setWaiting(kNotWaiting); }

        // Background thread: once a second, look for terminals that were
        // closed or crashed and give back whatever they were holding.
        void reaperLoop() {
            std::unique_lock<std::mutex> lk(reaper_mutex_);
            while (!stop_reaper_) {
                reaper_cv_.wait_for(lk, std::chrono::seconds(1), [this] { return stop_reaper_; });
                if (stop_reaper_) break;
                lk.unlock();
                try {
                    reapDeadTerminals();
                } catch (...) {
                    // Never let the cleanup thread take the program down.
                }
                lk.lock();
            }
        }

        void stopReaper() {
            {
                std::lock_guard<std::mutex> lk(reaper_mutex_);
                stop_reaper_ = true;
            }
            reaper_cv_.notify_all();
            if (reaper_.joinable()) reaper_.join();
        }

        void reapDeadTerminals() {
            std::lock_guard<std::mutex> guard(reap_mutex_);
            if (!block_) return;
            SharedData& d = block_->data;

            struct Orphan { bool reading; bool writing; };
            std::vector<Orphan> orphans;
            {
                CrossLock lock(state_lock_, block_->state_owner, 500);
                if (!lock.held()) return;   // try again next tick
                for (auto& t : d.terminals) {
                    if (t.pid == 0 || t.pid == pid_ || ipc::isAlive(t.pid, t.stamp)) continue;
                    orphans.push_back({t.reading != 0, t.writing != 0});
                    if (t.writing && d.writer_terminal == t.number) d.writer_terminal = 0;
                    logActivityLocked("terminal " + std::to_string(t.number) + " disconnected; locks released");
                    t = TerminalSlot{};
                }
            }

            for (const auto& o : orphans) {
                if (o.writing) wrt_.post();
                if (o.reading) pending_reader_releases_++;
            }

            // A dead reader still counts towards read_count; take it off so the
            // last real reader (or nobody) lets writers back in.
            while (pending_reader_releases_ > 0) {
                CrossLock rc(rc_lock_, block_->rc_owner, 500);
                if (!rc.held()) break;   // retry on the next tick
                if (d.read_count > 0) {
                    d.read_count--;
                    if (d.read_count == 0) wrt_.post();
                }
                pending_reader_releases_--;
            }
        }

        ipc::SharedMemory memory_;
        ipc::Mutex state_lock_;
        ipc::Mutex rc_lock_;
        ipc::Semaphore empty_;
        ipc::Semaphore full_;
        ipc::Semaphore wrt_;
        SharedBlock* block_ = nullptr;

        int32_t pid_ = 0;
        int64_t stamp_ = 0;
        int number_ = 0;
        int my_index_ = -1;
        std::atomic<bool> reading_{false};
        std::atomic<bool> writing_{false};

        std::thread reaper_;
        std::mutex reaper_mutex_;
        std::condition_variable reaper_cv_;
        bool stop_reaper_ = false;
        std::mutex reap_mutex_;
        int pending_reader_releases_ = 0;
    };

    inline SharedSession g_Session;
    inline std::string g_SessionError;
}

namespace Toolkit {

    struct ProblemConfig {
        int buffer_capacity = 5;
        int num_philosophers = 5;
        int wait_seconds = 20;   // how long a blocked action waits before giving up
    };

    struct BenchmarkResult {
        std::string problem_name;
        std::string strategy_name;
        bool is_safe_implementation;
        uint64_t total_operations;
        double execution_time_ms;
        uint64_t wait_contention_count;
        bool data_integrity_passed;
        std::string notes;
    };

    class PerformanceAnalyzer {
    private:
        std::vector<BenchmarkResult> history;
        std::mutex history_mutex;

        // Quote a CSV field and double any quotes inside it.
        static std::string csvField(const std::string& s) {
            std::string out = "\"";
            for (char c : s) {
                if (c == '"') out += '"';
                out += c;
            }
            return out + "\"";
        }

    public:
        void recordResult(const BenchmarkResult& result) {
            std::lock_guard<std::mutex> lock(history_mutex);
            history.push_back(result);
        }

        void printTable(bool animate) {
            std::lock_guard<std::mutex> lock(history_mutex);

            if (history.empty()) {
                std::cout << "  " << Color::Dim() << "No activity recorded in this session." << Color::Reset() << "\n\n";
                return;
            }

            std::cout << Color::Bold() << std::left
                      << "  " << std::setw(22) << "Problem"
                      << std::setw(26) << "Method"
                      << std::setw(8) << "Safe"
                      << std::setw(12) << "Actions"
                      << std::setw(12) << "Blocked"
                      << "Integrity" << Color::Reset() << "\n";
            std::cout << "  " << std::string(89, '-') << "\n";

            for (const auto& res : history) {
                std::string safe = res.is_safe_implementation ? "Yes" : "No";
                std::string integrity = res.data_integrity_passed ? "Passed" : "Failed";
                std::cout << "  " << std::setw(22) << res.problem_name
                          << std::setw(26) << res.strategy_name
                          << (res.is_safe_implementation ? Color::Green() : Color::Red()) << std::setw(8) << safe << Color::Reset()
                          << std::setw(12) << res.total_operations
                          << std::setw(12) << res.wait_contention_count
                          << (res.data_integrity_passed ? Color::Green() : Color::Red()) << integrity << Color::Reset()
                          << "\n" << std::flush;
                if (animate) std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            std::cout << "\n";
        }

        UI::Status exportCSV(const std::string& filename = "benchmark_report.csv") {
            std::lock_guard<std::mutex> lock(history_mutex);
            if (history.empty()) return UI::Status::Warn("Nothing to export yet.");

            UI::ProgressBarAnimation("Exporting", 400);

            std::ofstream file(filename);
            if (!file.is_open()) {
                g_Logger.log(Logger::Level::ERROR, "report", "could not open " + filename);
                return UI::Status::Error("Could not write " + filename + ". Close it if it is open in another program.");
            }

            file << "Problem,Method,Safe,Actions,Blocked,Integrity,DurationMs,Notes\n";
            for (const auto& res : history) {
                file << csvField(res.problem_name) << ","
                     << csvField(res.strategy_name) << ","
                     << (res.is_safe_implementation ? "Yes" : "No") << ","
                     << res.total_operations << ","
                     << res.wait_contention_count << ","
                     << (res.data_integrity_passed ? "Passed" : "Failed") << ","
                     << static_cast<long long>(res.execution_time_ms) << ","
                     << csvField(res.notes) << "\n";
            }
            file.close();
            if (!file) return UI::Status::Error("An error occurred while writing " + filename + ".");

            g_Logger.log(Logger::Level::OK, "report", "exported " + filename);
            return UI::Status::Ok("Report exported to " + filename + ".");
        }
    };

    inline PerformanceAnalyzer g_Analyzer;
}

namespace Toolkit {

    // Local buffer, used only by the unsynchronized demo.
    template <typename T>
    class CircularBuffer {
    private:
        std::vector<T> buffer;
        int capacity;
        int front = 0;
        int rear = 0;
        int count = 0;

    public:
        explicit CircularBuffer(int cap) : capacity(std::max(1, cap)) {
            buffer.resize(capacity);
        }

        bool isFull() const { return count == capacity; }
        bool isEmpty() const { return count == 0; }

        void push(const T& item) {
            buffer[rear] = item;
            rear = (rear + 1) % capacity;
            count++;
        }

        T pop() {
            T item = buffer[front];
            front = (front + 1) % capacity;
            count--;
            return item;
        }

        int size() const { return count; }
        int cap() const { return capacity; }
        bool filled(int i) const { return ((i - front + capacity) % capacity) < count; }
        const T& at(int i) const { return buffer[i]; }
    };

    namespace View {

        // Draws buffer slots, ten per row: [  42] for an item, [    ] for empty.
        template <typename Filled, typename ValueAt>
        inline void PrintSlots(int capacity, Filled filled, ValueAt value_at) {
            for (int i = 0; i < capacity; ++i) {
                std::cout << (i % 10 == 0 ? (i ? "\n  " : "  ") : " ");
                if (filled(i)) std::cout << Color::Green() << Color::Bold() << "[" << std::right << std::setw(4) << value_at(i) << std::left << "]" << Color::Reset();
                else           std::cout << Color::Dim() << "[    ]" << Color::Reset();
            }
            std::cout << "\n";
        }

        inline void PrintLabel(const std::string& label, const std::string& value) {
            std::cout << "  " << Color::Dim() << std::left << std::setw(12) << label << Color::Reset() << value << "\n";
        }

        inline void PrintActivity(const SharedSession::Snapshot& s) {
            std::cout << "\n  " << Color::Bold() << "Activity" << Color::Reset() << "\n";
            if (s.activity.empty()) {
                std::cout << "  " << Color::Dim() << "No activity yet." << Color::Reset() << "\n";
                return;
            }
            size_t start = s.activity.size() > 4 ? s.activity.size() - 4 : 0;
            for (size_t i = start; i < s.activity.size(); ++i) {
                std::cout << "  " << Color::Dim() << s.activity[i] << Color::Reset() << "\n";
            }
        }

        inline void PrintActions(const std::vector<std::pair<std::string, std::string>>& actions) {
            std::cout << "\n ";
            for (const auto& a : actions) {
                std::cout << " " << Color::Cyan() << Color::Bold() << "[" << a.first << "]" << Color::Reset() << " " << a.second << "  ";
            }
            std::cout << "\n\n";
        }

        inline UI::Status ToStatus(const OpResult& r, uint64_t& timeouts, uint64_t& failures) {
            if (r.ok()) return UI::Status::Ok(r.message);
            if (r.status == OpResult::Status::TimedOut) {
                timeouts++;
                return UI::Status::Warn(r.message);
            }
            failures++;
            return UI::Status::Error(r.message);
        }

        inline std::string Plural(int n, const std::string& word) {
            return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
        }

        // Header, connection line and a hint when this is the only terminal.
        inline void PrintSharedTop(const std::string& title, bool first, const SharedSession::Snapshot& snap) {
            UI::ClearScreen();
            UI::PrintHeader(title, first);
            std::cout << "  " << Color::Green() << "Terminal " << g_Session.terminalNumber() << Color::Reset();
            if (snap.ok) std::cout << Color::Dim() << "   " << Plural(snap.terminals_online, "terminal") << " connected" << Color::Reset();
            std::cout << "\n";
            if (snap.ok && snap.terminals_online == 1) {
                std::cout << "  " << Color::Dim() << "Run the program in another terminal to work on the same data." << Color::Reset() << "\n";
            }
            std::cout << "\n";
        }

        // Spinner tick callback: redraws with `draw` whenever the shared state
        // has changed since it was last shown.
        template <typename Draw>
        inline auto LiveRedraw(uint32_t shown_version, Draw draw) {
            return [shown_version, draw]() mutable {
                auto s = g_Session.snapshot();
                if (!s.ok || s.version == shown_version) return;
                shown_version = s.version;
                draw(s);
            };
        }

        // Same idea for the idle prompt: returns true when it redrew, so the
        // prompt can be printed again underneath.
        template <typename Draw>
        inline auto IdleRedraw(uint32_t shown_version, Draw draw) {
            return [shown_version, draw]() mutable {
                auto s = g_Session.snapshot();
                if (!s.ok || s.version == shown_version) return false;
                shown_version = s.version;
                draw(s);
                return true;
            };
        }

        inline bool RequireSession() {
            if (g_Session.isAttached()) return true;
            UI::ClearScreen();
            UI::PrintHeader("SHARED SESSION UNAVAILABLE");
            UI::PrintErrorCard("Shared mode is not available: " +
                               (g_SessionError.empty() ? std::string("unknown error") : g_SessionError) + ".");
            UI::Pause();
            return false;
        }
    }

    class ProducerConsumerEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            if (!View::RequireSession()) return;

            const int timeout_ms = config.wait_seconds * 1000;
            uint64_t done = 0, timeouts = 0, failures = 0;
            UI::Status status;
            bool first = true;
            Timer timer;
            timer.start();

            // Draws the whole screen. Also used while waiting, so changes made
            // in other terminals show up live during the countdown.
            auto draw = [&](const SharedSession::Snapshot& snap, bool animate, const UI::Status& st, bool waiting) {
                View::PrintSharedTop("PRODUCER-CONSUMER", animate, snap);
                if (snap.ok) {
                    std::cout << "  " << Color::Bold() << "Buffer" << Color::Reset() << "  " << snap.count << " of " << snap.capacity << " slots in use\n";
                    View::PrintSlots(snap.capacity,
                                     [&](int i) { return ((i - snap.front + snap.capacity) % snap.capacity) < snap.count; },
                                     [&](int i) { return snap.items[i]; });
                    std::cout << "\n";
                    View::PrintLabel("Produced", std::to_string(snap.produced));
                    View::PrintLabel("Consumed", std::to_string(snap.consumed));
                    View::PrintLabel("Waiting", View::Plural(snap.waiting_producers, "producer") + ", " +
                                                View::Plural(snap.waiting_consumers, "consumer"));
                    View::PrintActivity(snap);
                } else {
                    UI::PrintWarningCard("Shared state is temporarily unavailable.");
                }
                std::cout << "\n";
                UI::PrintStatus(st);
                if (!waiting) View::PrintActions({{"1", "Produce"}, {"2", "Consume"}, {"3", "Refresh"}, {"0", "Back"}});
            };

            for (bool running = true; running; ) {
                auto snap = g_Session.snapshot();
                draw(snap, first, status, false);
                first = false;

                int option = UI::ReadChoiceLive("  Select: ", 0, 3,
                    View::IdleRedraw(snap.version, [&](const SharedSession::Snapshot& s) { draw(s, false, status, false); }));
                status = {};

                if (option == 1) {
                    int value = UI::SafeReadInt("  Item value (1-9999): ", 1, 9999);
                    OpResult r = UI::RunWithSpinner("Waiting for a free slot", timeout_ms,
                        [&] { return g_Session.produce(value, timeout_ms); },
                        View::LiveRedraw(snap.version, [&](const SharedSession::Snapshot& s) {
                            draw(s, false, UI::Status::Info("Producing " + std::to_string(value) + ". Waiting for a free slot."), true);
                        }));
                    if (r.ok()) done++;
                    status = View::ToStatus(r, timeouts, failures);
                } else if (option == 2) {
                    OpResult r = UI::RunWithSpinner("Waiting for an item", timeout_ms,
                        [&] { return g_Session.consume(timeout_ms); },
                        View::LiveRedraw(snap.version, [&](const SharedSession::Snapshot& s) {
                            draw(s, false, UI::Status::Info("Consuming. Waiting for an item."), true);
                        }));
                    if (r.ok()) done++;
                    status = View::ToStatus(r, timeouts, failures);
                } else if (option == 0) {
                    running = false;
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Producer-Consumer", "Shared semaphores", true, done,
                                     timer.elapsedMilliseconds(), timeouts, failures == 0,
                                     "Buffer shared across terminals with named semaphores."});
        }

        static void runNaiveInteractiveMode(const ProblemConfig& config) {
            CircularBuffer<int> buffer(config.buffer_capacity);
            uint64_t violations = 0, ops = 0;
            UI::Status status = UI::Status::Info("No semaphores or locks are used here. Try producing into a full buffer.");
            bool first = true;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintHeader("PRODUCER-CONSUMER (UNSYNCHRONIZED)", first);
                first = false;

                std::cout << "  " << Color::Bold() << "Buffer" << Color::Reset() << "  " << buffer.size() << " of " << buffer.cap() << " slots in use\n";
                View::PrintSlots(buffer.cap(), [&](int i) { return buffer.filled(i); }, [&](int i) { return buffer.at(i); });
                std::cout << "\n";
                View::PrintLabel("Errors", std::to_string(violations));
                std::cout << "\n";
                UI::PrintStatus(status);
                View::PrintActions({{"1", "Produce"}, {"2", "Consume"}, {"0", "Back"}});

                int option = UI::SafeReadInt("  Select: ", 0, 2);

                if (option == 1) {
                    int val = UI::SafeReadInt("  Item value (1-9999): ", 1, 9999);
                    ops++;
                    if (buffer.isFull()) {
                        violations++;
                        status = UI::Status::Error("Buffer overflow. Nothing prevented writing to a full buffer.");
                        g_Logger.log(Logger::Level::ERROR, "producer", "buffer overflow");
                    } else {
                        buffer.push(val);
                        status = UI::Status::Warn("Produced " + std::to_string(val) + " without synchronization.");
                    }
                } else if (option == 2) {
                    ops++;
                    if (buffer.isEmpty()) {
                        violations++;
                        status = UI::Status::Error("Buffer underflow. Nothing prevented reading from an empty buffer.");
                        g_Logger.log(Logger::Level::ERROR, "consumer", "buffer underflow");
                    } else {
                        int item = buffer.pop();
                        status = UI::Status::Warn("Consumed " + std::to_string(item) + " without synchronization.");
                    }
                } else {
                    running = false;
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Producer-Consumer", "Unsynchronized", false, ops,
                                     timer.elapsedMilliseconds(), violations, violations == 0,
                                     "Overflow and underflow demonstration."});
        }
    };

    class ReadersWritersEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            if (!View::RequireSession()) return;

            const int timeout_ms = config.wait_seconds * 1000;
            uint64_t done = 0, timeouts = 0, failures = 0;
            UI::Status status;
            bool first = true;
            Timer timer;
            timer.start();

            // Draws the whole screen. Also used while waiting, so changes made
            // in other terminals show up live during the countdown.
            auto draw = [&](const SharedSession::Snapshot& snap, bool animate, const UI::Status& st, bool waiting) {
                View::PrintSharedTop("READERS-WRITERS", animate, snap);
                if (snap.ok) {
                    std::cout << "  " << Color::Bold() << "Database" << Color::Reset() << "\n";
                    View::PrintLabel("Value", Color::Green() + Color::Bold() + std::to_string(snap.db_value) + Color::Reset());
                    View::PrintLabel("Readers", std::to_string(snap.active_readers) + " active");
                    View::PrintLabel("Writer", snap.writer_terminal ? "Terminal " + std::to_string(snap.writer_terminal) : std::string("None"));
                    View::PrintLabel("Waiting", View::Plural(snap.waiting_readers, "reader") + ", " +
                                                View::Plural(snap.waiting_writers, "writer"));
                    View::PrintActivity(snap);
                } else {
                    UI::PrintWarningCard("Shared state is temporarily unavailable.");
                }
                std::cout << "\n";
                UI::PrintStatus(st);
                if (!waiting) View::PrintActions({{"1", "Read"}, {"2", "Write"}, {"3", "Refresh"}, {"0", "Back"}});
            };

            for (bool running = true; running; ) {
                auto snap = g_Session.snapshot();
                draw(snap, first, status, false);
                first = false;

                int option = UI::ReadChoiceLive("  Select: ", 0, 3,
                    View::IdleRedraw(snap.version, [&](const SharedSession::Snapshot& s) { draw(s, false, status, false); }));
                status = {};

                if (option == 1) {
                    OpResult r = UI::RunWithSpinner("Waiting for read access", timeout_ms,
                        [&] { return g_Session.beginRead(timeout_ms); },
                        View::LiveRedraw(snap.version, [&](const SharedSession::Snapshot& s) {
                            draw(s, false, UI::Status::Info("Requesting read access."), true);
                        }));
                    if (!r.ok()) {
                        status = View::ToStatus(r, timeouts, failures);
                        continue;
                    }
                    done++;
                    std::cout << "\n  " << Color::Green() << Color::Bold() << "Read lock acquired." << Color::Reset()
                              << " Value: " << Color::Bold() << r.value << Color::Reset() << "\n"
                              << "  " << Color::Dim() << "Other readers can join. Writers are blocked until you release." << Color::Reset() << "\n\n";
                    UI::ReadLine("  Press Enter to release the lock.");

                    OpResult end = g_Session.endRead();
                    while (!end.ok()) {
                        UI::PrintErrorCard(end.message);
                        UI::ReadLine("  Press Enter to retry.");
                        end = g_Session.endRead();
                    }
                    status = UI::Status::Ok("Read lock released.");
                } else if (option == 2) {
                    OpResult r = UI::RunWithSpinner("Waiting for write access", timeout_ms,
                        [&] { return g_Session.beginWrite(timeout_ms); },
                        View::LiveRedraw(snap.version, [&](const SharedSession::Snapshot& s) {
                            draw(s, false, UI::Status::Info("Requesting write access."), true);
                        }));
                    if (!r.ok()) {
                        status = View::ToStatus(r, timeouts, failures);
                        continue;
                    }
                    std::cout << "\n  " << Color::Green() << Color::Bold() << "Write lock acquired." << Color::Reset()
                              << " Current value: " << Color::Bold() << r.value << Color::Reset() << "\n"
                              << "  " << Color::Dim() << "All other readers and writers are blocked until you finish." << Color::Reset() << "\n\n";
                    int value = UI::SafeReadInt("  New value (1-9999, 0 to cancel): ", 0, 9999);

                    OpResult end = g_Session.endWrite(value != 0, value);
                    if (!end.ok()) {
                        failures++;
                        status = UI::Status::Error(end.message);
                    } else if (value != 0) {
                        done++;
                        status = UI::Status::Ok("Value updated to " + std::to_string(value) + ".");
                    } else {
                        status = UI::Status::Info("Write cancelled. No changes made.");
                    }
                } else if (option == 0) {
                    running = false;
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Readers-Writers", "Shared, reader priority", true, done,
                                     timer.elapsedMilliseconds(), timeouts, failures == 0,
                                     "Database shared across terminals with a read-count lock and write semaphore."});
        }

        static void runNaiveInteractiveMode() {
            int value = 100;
            uint64_t conflicts = 0, ops = 0;
            UI::Status status = UI::Status::Info("No locks are used here. Option 3 shows a read during a write.");
            bool first = true;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintHeader("READERS-WRITERS (UNSYNCHRONIZED)", first);
                first = false;

                std::cout << "  " << Color::Bold() << "Database" << Color::Reset() << "\n";
                View::PrintLabel("Value", std::to_string(value));
                View::PrintLabel("Errors", std::to_string(conflicts));
                std::cout << "\n";
                UI::PrintStatus(status);
                View::PrintActions({{"1", "Read"}, {"2", "Write"}, {"3", "Read during write"}, {"0", "Back"}});

                int option = UI::SafeReadInt("  Select: ", 0, 3);

                if (option == 1) {
                    ops++;
                    status = UI::Status::Info("Read value " + std::to_string(value) + ".");
                } else if (option == 2) {
                    int val = UI::SafeReadInt("  New value (1-9999): ", 1, 9999);
                    ops++;
                    value = val;
                    status = UI::Status::Warn("Wrote " + std::to_string(val) + " with no lock. Readers were not blocked.");
                } else if (option == 3) {
                    ops += 2;
                    conflicts++;
                    status = UI::Status::Error("Dirty read. The value was read while a write was in progress.");
                    g_Logger.log(Logger::Level::ERROR, "reader", "dirty read of " + std::to_string(value));
                } else {
                    running = false;
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Readers-Writers", "Unsynchronized", false, ops,
                                     timer.elapsedMilliseconds(), conflicts, conflicts == 0,
                                     "Dirty read demonstration."});
        }
    };

    class DiningPhilosophersEngine {
    private:
        static void printTable(int n, const std::vector<bool>& fork_taken, const std::vector<std::string>& state) {
            std::cout << Color::Bold() << "  " << std::left << std::setw(14) << "Philosopher" << std::setw(12) << "State"
                      << std::setw(14) << "Left fork" << "Right fork" << Color::Reset() << "\n";
            for (int i = 0; i < n; ++i) {
                int l = i, r = (i + 1) % n;
                auto fork = [&](int f) {
                    std::string text = std::to_string(f) + (fork_taken[f] ? " in use" : " free");
                    return (fork_taken[f] ? Color::Red() : Color::Green()) + text + Color::Reset() +
                           std::string(std::max<int>(1, 14 - static_cast<int>(text.size())), ' ');
                };
                std::string st_color = state.empty() ? Color::Reset() : (state[i] == "Eating" ? Color::Green() : Color::Yellow());
                std::cout << "  " << std::setw(14) << i
                          << st_color << std::setw(12) << (state.empty() ? "Waiting" : state[i]) << Color::Reset()
                          << fork(l) << fork(r) << "\n";
            }
        }

    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            std::vector<std::string> phil_state(N, "Thinking");
            uint64_t meals = 0, blocked = 0;
            UI::Status status = UI::Status::Info("Pick up the left fork, then the right fork to start eating.");
            bool first = true;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintHeader("DINING PHILOSOPHERS", first);
                first = false;

                printTable(N, fork_taken, phil_state);
                std::cout << "\n";
                UI::PrintStatus(status);
                View::PrintActions({{"1", "Take left fork"}, {"2", "Take right fork"}, {"3", "Release forks"}, {"0", "Back"}});

                int option = UI::SafeReadInt("  Select: ", 0, 3);
                if (option == 0) { running = false; continue; }

                int p = UI::SafeReadInt("  Philosopher (0-" + std::to_string(N - 1) + "): ", 0, N - 1);
                int left = p, right = (p + 1) % N;
                std::string name = "Philosopher " + std::to_string(p);

                if (option == 1) {
                    UI::SpinnerAnimation("Taking fork " + std::to_string(left));
                    if (!fork_taken[left]) {
                        fork_taken[left] = true;
                        status = UI::Status::Ok(name + " took fork " + std::to_string(left) + ".");
                    } else {
                        blocked++;
                        status = UI::Status::Error("Fork " + std::to_string(left) + " is in use.");
                    }
                } else if (option == 2) {
                    UI::SpinnerAnimation("Taking fork " + std::to_string(right));
                    if (!fork_taken[left]) {
                        blocked++;
                        status = UI::Status::Warn(name + " must take the left fork (" + std::to_string(left) + ") first.");
                    } else if (fork_taken[right]) {
                        blocked++;
                        status = UI::Status::Error("Fork " + std::to_string(right) + " is in use.");
                    } else {
                        fork_taken[right] = true;
                        phil_state[p] = "Eating";
                        meals++;
                        status = UI::Status::Ok(name + " is eating.");
                    }
                } else if (option == 3) {
                    fork_taken[left] = false;
                    fork_taken[right] = false;
                    phil_state[p] = "Thinking";
                    status = UI::Status::Ok(name + " released both forks.");
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Dining Philosophers", "Manual fork control", true, meals,
                                     timer.elapsedMilliseconds(), blocked, true,
                                     "All fork actions performed manually."});
        }

        static void runNaiveInteractiveDeadlockDemo(const ProblemConfig& config) {
            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            int held = 0;
            UI::Status status = UI::Status::Info("Each philosopher takes only the left fork. When all do, no one can eat.");
            bool first = true;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintHeader("DINING PHILOSOPHERS (DEADLOCK)", first);
                first = false;

                printTable(N, fork_taken, {});
                std::cout << "\n";
                View::PrintLabel("Forks held", std::to_string(held) + " of " + std::to_string(N));
                std::cout << "\n";
                UI::PrintStatus(status);
                View::PrintActions({{"1", "Take left fork"}, {"2", "All take left fork"}, {"0", "Back"}});

                int option = UI::SafeReadInt("  Select: ", 0, 2);

                if (option == 1) {
                    int p = UI::SafeReadInt("  Philosopher (0-" + std::to_string(N - 1) + "): ", 0, N - 1);
                    if (fork_taken[p]) {
                        status = UI::Status::Warn("Fork " + std::to_string(p) + " is already held.");
                    } else {
                        fork_taken[p] = true;
                        held++;
                        status = UI::Status::Warn("Philosopher " + std::to_string(p) + " holds fork " + std::to_string(p) +
                                                  " and waits for fork " + std::to_string((p + 1) % N) + ".");
                    }
                } else if (option == 2) {
                    UI::SpinnerAnimation("All philosophers taking their left fork");
                    std::fill(fork_taken.begin(), fork_taken.end(), true);
                    held = N;
                } else {
                    running = false;
                    continue;
                }

                if (held == N) {
                    std::cout << "\n";
                    UI::BlinkAlert("  DEADLOCK: every philosopher is waiting for a fork held by a neighbor  ");
                    status = UI::Status::Error("Deadlock. Circular wait: each philosopher holds one fork and waits for the next.");
                    g_Logger.log(Logger::Level::ERROR, "philosophers", "deadlock reached");
                }
            }

            timer.stop();
            g_Analyzer.recordResult({"Dining Philosophers", "Unordered (deadlock)", false, static_cast<uint64_t>(held),
                                     timer.elapsedMilliseconds(), static_cast<uint64_t>(held), held < N,
                                     "Circular wait demonstration."});
        }
    };
}

namespace Toolkit {

    class MainMenu {
    private:
        ProblemConfig config;

        // Runs one screen. An unexpected error returns to the menu instead of
        // ending the program while a shared lock is held.
        template <typename Screen>
        void runScreen(Screen&& screen) {
            try {
                screen();
            } catch (const UI::InputClosed&) {
                throw;
            } catch (const std::exception& e) {
                g_Session.releaseHeld();
                UI::PrintErrorCard(std::string("Unexpected error: ") + e.what());
                UI::Pause();
            }
        }

        void unsafeDemos() {
            UI::ClearScreen();
            UI::PrintHeader("UNSAFE DEMOS");
            std::cout << "  " << Color::Dim() << "These run in this terminal only and show what fails without synchronization." << Color::Reset() << "\n\n";
            UI::PrintMenuCard("1", "Producer-consumer", "Overflow and underflow");
            UI::PrintMenuCard("2", "Readers-writers", "Dirty read");
            UI::PrintMenuCard("3", "Dining philosophers", "Deadlock");
            UI::PrintMenuCard("0", "Back", "");
            std::cout << "\n";

            int choice = UI::SafeReadInt("  Select: ", 0, 3);
            if (choice == 1) ProducerConsumerEngine::runNaiveInteractiveMode(config);
            else if (choice == 2) ReadersWritersEngine::runNaiveInteractiveMode();
            else if (choice == 3) DiningPhilosophersEngine::runNaiveInteractiveDeadlockDemo(config);
        }

        void report() {
            UI::Status status;
            bool first = true;
            for (;;) {
                UI::ClearScreen();
                UI::PrintHeader("SESSION REPORT", first);
                g_Analyzer.printTable(first);
                first = false;
                UI::PrintStatus(status);
                View::PrintActions({{"1", "Export to CSV"}, {"0", "Back"}});

                if (UI::SafeReadInt("  Select: ", 0, 1) == 0) return;
                status = g_Analyzer.exportCSV();
            }
        }

        void settings() {
            UI::Status status;
            bool first = true;
            for (;;) {
                UI::ClearScreen();
                UI::PrintHeader("SETTINGS", first);
                first = false;

                UI::PrintMenuCard("1", "Buffer size", std::to_string(config.buffer_capacity) + " slots");
                UI::PrintMenuCard("2", "Wait timeout", std::to_string(config.wait_seconds) + " seconds");
                UI::PrintMenuCard("3", "Philosophers", std::to_string(config.num_philosophers));
                UI::PrintMenuCard("4", "Colors", Color::enable_colors ? "On" : "Off");
                UI::PrintMenuCard("0", "Back", "");
                std::cout << "\n";
                UI::PrintStatus(status);

                int choice = UI::SafeReadInt("  Select: ", 0, 4);
                if (choice == 0) return;

                if (choice == 1) {
                    int size = UI::SafeReadInt("  Buffer size (1-100): ", 1, kMaxBufferSize);
                    if (g_Session.isAttached()) {
                        OpResult r = g_Session.resizeBuffer(size);
                        if (!r.ok()) { status = UI::Status::Error(r.message); continue; }
                    }
                    config.buffer_capacity = size;
                    status = UI::Status::Ok("Buffer size set to " + std::to_string(size) + ".");
                } else if (choice == 2) {
                    config.wait_seconds = UI::SafeReadInt("  Wait timeout in seconds (1-120): ", 1, 120);
                    status = UI::Status::Ok("Wait timeout set to " + std::to_string(config.wait_seconds) + " seconds.");
                } else if (choice == 3) {
                    config.num_philosophers = UI::SafeReadInt("  Philosophers (2-20): ", 2, 20);
                    status = UI::Status::Ok("Philosophers set to " + std::to_string(config.num_philosophers) + ".");
                } else {
                    Color::enable_colors = !Color::enable_colors;
                    status = UI::Status::Ok(std::string("Colors turned ") + (Color::enable_colors ? "on." : "off."));
                }
            }
        }

        void printSessionLine() {
            if (!g_Session.isAttached()) {
                std::cout << "  " << Color::Yellow() << "Offline: " << g_SessionError << Color::Reset() << "\n\n";
                return;
            }
            auto snap = g_Session.snapshot();
            std::cout << "  " << Color::Green() << "Terminal " << g_Session.terminalNumber() << Color::Reset();
            if (snap.ok) std::cout << Color::Dim() << "   " << View::Plural(snap.terminals_online, "terminal") << " connected" << Color::Reset();
            std::cout << "\n\n";
        }

    public:
        void run() {
            UI::ClearScreen();
            std::cout << "\n";
            UI::LoadingDots("Connecting");

            if (g_Session.attach(config.buffer_capacity, g_SessionError)) {
                g_Logger.setSource("T" + std::to_string(g_Session.terminalNumber()));
                // Another terminal may have created the buffer with a different size.
                auto snap = g_Session.snapshot();
                if (snap.ok) config.buffer_capacity = snap.capacity;
            } else {
                g_Logger.log(Logger::Level::WARN, "session", "shared mode unavailable: " + g_SessionError);
            }

            bool first_draw = true;
            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintBanner(first_draw);
                UI::animate_menu = first_draw;
                first_draw = false;

                printSessionLine();
                UI::PrintMenuCard("1", "Producer-consumer", "Shared buffer");
                UI::PrintMenuCard("2", "Readers-writers", "Shared database");
                UI::PrintMenuCard("3", "Dining philosophers", "Fork allocation");
                UI::PrintMenuCard("4", "Unsafe demos", "Behavior without synchronization");
                UI::PrintMenuCard("5", "Report", "Session summary and CSV export");
                UI::PrintMenuCard("6", "Settings", "");
                UI::PrintMenuCard("0", "Exit", "");
                std::cout << "\n";
                UI::animate_menu = false;

                int choice = UI::SafeReadInt("  Select: ", 0, 6);

                switch (choice) {
                    case 1: runScreen([&] { ProducerConsumerEngine::runInteractiveMode(config); }); break;
                    case 2: runScreen([&] { ReadersWritersEngine::runInteractiveMode(config); }); break;
                    case 3: runScreen([&] { DiningPhilosophersEngine::runInteractiveMode(config); }); break;
                    case 4: runScreen([&] { unsafeDemos(); }); break;
                    case 5: runScreen([&] { report(); }); break;
                    case 6: runScreen([&] { settings(); }); break;
                    default: running = false; break;
                }
            }
        }
    };
}

int main() {
    using namespace Toolkit;
    UI::InitTerminal();
    g_Logger.setEcho(false);   // screens redraw; messages go to the log file

    int exit_code = 0;
    try {
        MainMenu menu;
        menu.run();
    } catch (const UI::InputClosed&) {
        std::cout << "\n  Input closed.\n";
    } catch (const std::exception& e) {
        UI::PrintErrorCard(std::string("Unexpected error: ") + e.what());
        exit_code = 1;
    } catch (...) {
        UI::PrintErrorCard("Unexpected error.");
        exit_code = 1;
    }

    // Release anything still held so other terminals are not blocked.
    std::cout << "\n";
    UI::LoadingDots("Disconnecting");
    g_Session.detach();

    std::cout << Color::Cyan() << "  ";
    UI::TypeText("Session ended.", 20);
    std::cout << "\n" << Color::Reset();
    return exit_code;
}
