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

            std::cout << "  " << Color::Dim() << timestamp << Color::Reset() << " "
                      << color << Color::Bold() << tag << Color::Reset() << " "
                      << Color::Yellow() << sender << Color::Reset() << ": " << message << std::endl;

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

        inline void PrintBreadcrumb(const std::string& path) {
            std::cout << "  " << Color::Dim() << path << Color::Reset() << "\n\n";
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

        inline void PrintHeader(const std::string& title) {
            int total_width = 83;
            int padding = (total_width - 4 - static_cast<int>(title.length())) / 2;
            std::string left_pad(std::max(0, padding), ' ');
            std::string right_pad(std::max(0, total_width - 4 - static_cast<int>(title.length()) - padding), ' ');

            std::cout << Color::Yellow() << Color::Bold() << "+";
            for (int drawn = 0; drawn < total_width - 2; drawn += 9) {
                std::cout << std::string(std::min(9, total_width - 2 - drawn), '=') << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
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
        template <typename Work>
        auto RunWithSpinner(const std::string& label, int timeout_ms, Work&& work) -> decltype(work()) {
            auto task = std::async(std::launch::async, std::forward<Work>(work));
            const char frames[] = {'|', '/', '-', '\\'};
            auto started = std::chrono::steady_clock::now();
            std::string line;

            for (int frame = 0;; ++frame) {
                bool done = task.wait_for(std::chrono::milliseconds(80)) == std::future_status::ready;
                if (done && frame >= 4) break;  // show a few frames even when it's instant
                if (done) std::this_thread::sleep_for(std::chrono::milliseconds(80));

                auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                long long left = std::max<long long>(0, (timeout_ms - waited + 999) / 1000);
                line = label + "  (" + std::to_string(left) + "s left)";
                std::cout << "\r  " << Color::Yellow() << Color::Bold() << frames[frame % 4] << Color::Reset()
                          << " " << Color::Cyan() << line << Color::Reset() << "   " << std::flush;
            }
            std::cout << "\r" << std::string(line.length() + 10, ' ') << "\r" << std::flush;
            return task.get();
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

        inline int SafeReadInt(const std::string& prompt, int min_val, int max_val) {
            for (;;) {
                std::string line = ReadLine(prompt);
                if (line.find_first_not_of(" \t") == std::string::npos) continue;  // just ENTER, ask again

                std::istringstream in(line);
                long long val = 0;
                char extra = 0;
                if (!(in >> val) || (in >> extra)) {
                    PrintErrorCard("Please type a whole number.");
                    continue;
                }
                if (val < min_val || val > max_val) {
                    PrintErrorCard("Pick something between " + std::to_string(min_val) + " and " + std::to_string(max_val) + ".");
                    continue;
                }
                return static_cast<int>(val);
            }
        }

        inline void Pause() {
            ReadLine("\n" + Color::Yellow() + "  Press ENTER to go back..." + Color::Reset());
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
            std::vector<std::string> activity;
        };

        ~SharedSession() { detach(); }

        bool attach(int capacity_hint, std::string& error) {
            if (block_) return true;
            pid_ = ipc::currentPid();
            stamp_ = ipc::processStamp(pid_);

            if (!openBlock(error, true)) return false;

            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (!lock.held()) {
                    error = "the shared state is locked by another terminal and didn't free up";
                    closeAll();
                    return false;
                }
                if (block_->magic != kMagic) {
                    initialise(capacity_hint);
                } else if (block_->layout_size != sizeof(SharedBlock)) {
                    error = "another terminal is running a different build of this program, close it first";
                    closeAll();
                    return false;
                }
                if (!openSemaphoresLocked()) {
                    error = "couldn't create the shared semaphores";
                    closeAll();
                    return false;
                }
            }

            reapDeadTerminals();   // free any slots left behind by crashed terminals

            {
                CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
                if (!lock.held()) {
                    error = "the shared state is locked by another terminal and didn't free up";
                    closeAll();
                    return false;
                }
                SharedData& d = block_->data;

                my_index_ = -1;
                for (int i = 0; i < kMaxTerminals; ++i) {
                    if (d.terminals[i].pid == 0) { my_index_ = i; break; }
                }
                if (my_index_ < 0) {
                    error = "all " + std::to_string(kMaxTerminals) + " terminal slots are in use";
                    closeAll();
                    return false;
                }

                // Lowest number nobody else is using, so people see 1, 2, 3...
                number_ = 1;
                for (bool taken = true; taken; ) {
                    taken = false;
                    for (const auto& t : d.terminals) {
                        if (t.pid != 0 && t.number == number_) { taken = true; ++number_; break; }
                    }
                }

                d.terminals[my_index_] = TerminalSlot{pid_, number_, stamp_, 0, 0, kNotWaiting};
                logActivityLocked("joined");
            }

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
                    logActivityLocked("left");
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
            if (writing_) endWrite(0, false, 0);
            if (reading_) endRead(0);
        }

        Snapshot snapshot() {
            Snapshot s;
            if (!block_) return s;
            CrossLock lock(state_lock_, block_->state_owner, 1000);
            if (!lock.held()) return s;

            const SharedData& d = block_->data;
            s.ok = true;
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

        OpResult produce(int producer_id, int value, int timeout_ms) {
            if (!block_) return OpResult::Failed("not connected to the shared session");
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
                    return OpResult::TimedOut("The buffer stayed full the whole time. Consume something first (here or in another terminal).");
                if (r != ipc::WaitResult::Acquired)
                    return OpResult::Failed("Waiting on the empty-slot semaphore failed.");

                if (d.count >= d.capacity) {
                    // The semaphore let us in but the buffer is full. That should
                    // never happen; keep the data safe and don't pass the bad token on.
                    logActivityLocked("buffer/semaphore mismatch on produce, item dropped");
                    return OpResult::Failed("The buffer and its semaphore disagree, so the item was not added.");
                }

                d.items[d.rear] = value;
                d.rear = (d.rear + 1) % d.capacity;
                d.count++;
                d.produced++;
                logActivityLocked("producer " + std::to_string(producer_id) + " added " + std::to_string(value));
                result = OpResult::Ok(value, "Added " + std::to_string(value) + " to the buffer (" +
                                             std::to_string(d.count) + "/" + std::to_string(d.capacity) + ").");
            }
            full_.post();
            return result;
        }

        OpResult consume(int consumer_id, int timeout_ms) {
            if (!block_) return OpResult::Failed("not connected to the shared session");
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
                    return OpResult::TimedOut("The buffer stayed empty the whole time. Produce something first (here or in another terminal).");
                if (r != ipc::WaitResult::Acquired)
                    return OpResult::Failed("Waiting on the full-slot semaphore failed.");

                if (d.count <= 0) {
                    logActivityLocked("buffer/semaphore mismatch on consume, nothing taken");
                    return OpResult::Failed("The buffer and its semaphore disagree, so nothing was taken.");
                }

                int item = d.items[d.front];
                d.front = (d.front + 1) % d.capacity;
                d.count--;
                d.consumed++;
                logActivityLocked("consumer " + std::to_string(consumer_id) + " took " + std::to_string(item));
                result = OpResult::Ok(item, "Took " + std::to_string(item) + " from the buffer (" +
                                            std::to_string(d.count) + "/" + std::to_string(d.capacity) + " left).");
            }
            empty_.post();
            return result;
        }

        // Only allowed while the buffer is empty and nobody is waiting on it,
        // because the empty-slot semaphore has to be topped up or drained to match.
        OpResult resizeBuffer(int new_capacity) {
            if (!block_) return OpResult::Failed("not connected to the shared session");
            new_capacity = std::max(1, std::min(kMaxBufferSize, new_capacity));

            CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
            if (!lock.held()) return busyResult();
            SharedData& d = block_->data;

            if (d.capacity == new_capacity) return OpResult::Ok(new_capacity, "The buffer already has that size.");
            if (d.count != 0)
                return OpResult::Failed("The shared buffer still has " + std::to_string(d.count) + " item(s). Empty it before resizing.");
            for (const auto& t : d.terminals) {
                if (t.pid != 0 && (t.waiting == kWaitProduce || t.waiting == kWaitConsume))
                    return OpResult::Failed("Terminal " + std::to_string(t.number) + " is waiting on the buffer right now. Try again in a moment.");
            }

            int old_capacity = d.capacity;
            if (new_capacity > old_capacity) {
                if (!empty_.post(new_capacity - old_capacity))
                    return OpResult::Failed("Couldn't adjust the empty-slot semaphore.");
            } else {
                int needed = old_capacity - new_capacity, taken = 0;
                while (taken < needed && empty_.wait(0) == ipc::WaitResult::Acquired) ++taken;
                if (taken < needed) {
                    if (taken > 0) empty_.post(taken);
                    return OpResult::Failed("Couldn't shrink the empty-slot semaphore safely, size left as it was.");
                }
            }

            d.capacity = new_capacity;
            d.front = d.rear = 0;
            logActivityLocked("resized the buffer to " + std::to_string(new_capacity));
            return OpResult::Ok(new_capacity, "Shared buffer resized to " + std::to_string(new_capacity) + " slots.");
        }

        // ---- readers-writers (readers get priority) ------------------------

        OpResult beginRead(int reader_id, int timeout_ms) {
            if (!block_) return OpResult::Failed("not connected to the shared session");
            if (reading_ || writing_) return OpResult::Failed("This terminal is already using the database.");
            if (!setWaiting(kWaitRead)) return busyResult();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            SharedData& d = block_->data;
            int value = 0;
            bool recovered = false;
            {
                CrossLock rc(rc_lock_, block_->rc_owner, timeout_ms);
                if (!rc.held()) {
                    clearWaiting();
                    return OpResult::TimedOut("Another reader is queued behind a writer. Gave up waiting for the read-count lock.");
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
                            return OpResult::TimedOut("A writer kept the database locked the whole time.");
                        return OpResult::Failed("Waiting on the database semaphore failed.");
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
                logActivityLocked("reader " + std::to_string(reader_id) + " started reading (" + std::to_string(value) + ")");
            }
            reading_ = true;
            return OpResult::Ok(value, recovered ? "Recovered a lock left behind by a closed terminal." : "");
        }

        OpResult endRead(int reader_id) {
            if (!block_ || !reading_) return OpResult::Failed("This terminal isn't reading.");
            SharedData& d = block_->data;
            {
                CrossLock rc(rc_lock_, block_->rc_owner, kReleaseLockMs);
                if (!rc.held()) return OpResult::Failed("Couldn't get the read-count lock to finish reading. Try again.");

                if (d.read_count > 0) {
                    d.read_count--;
                    if (d.read_count == 0) wrt_.post();   // last reader out lets writers in
                }

                CrossLock state(state_lock_, block_->state_owner, kStateLockMs);
                if (state.held()) {
                    d.terminals[my_index_].reading = 0;
                    logActivityLocked("reader " + std::to_string(reader_id) + " finished reading");
                }
            }
            reading_ = false;
            return OpResult::Ok();
        }

        OpResult beginWrite(int writer_id, int timeout_ms) {
            if (!block_) return OpResult::Failed("not connected to the shared session");
            if (reading_ || writing_) return OpResult::Failed("This terminal is already using the database.");
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
                return OpResult::TimedOut("Readers or another writer kept the database busy the whole time.");
            if (r != ipc::WaitResult::Acquired)
                return OpResult::Failed("Waiting on the database semaphore failed.");

            d.writer_terminal = number_;
            d.terminals[my_index_].writing = 1;
            logActivityLocked("writer " + std::to_string(writer_id) + " locked the database");
            writing_ = true;
            return OpResult::Ok(d.db_value);
        }

        OpResult endWrite(int writer_id, bool commit, int value) {
            if (!block_ || !writing_) return OpResult::Failed("This terminal isn't writing.");
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
                    logActivityLocked(commit ? "writer " + std::to_string(writer_id) + " wrote " + std::to_string(value)
                                             : "writer " + std::to_string(writer_id) + " left without writing");
                } else if (commit) {
                    result = OpResult::Failed("Couldn't save the value because the shared state stayed locked.");
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
            return OpResult::Failed("The shared state is busy (another terminal may have frozen). Try again.");
        }

        bool openBlock(std::string& error, bool allow_reset) {
            if (!state_lock_.open("state") || !rc_lock_.open("readcount")) {
                error = "couldn't create the shared locks";
                closeAll();
                return false;
            }
            bool created = false;
            if (!memory_.open("shared", sizeof(SharedBlock), created)) {
                error = "couldn't map the shared memory (an older build may still be running)";
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
        }

        bool setWaiting(int32_t what) {
            CrossLock lock(state_lock_, block_->state_owner, kStateLockMs);
            if (!lock.held()) return false;
            block_->data.terminals[my_index_].waiting = what;
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
                    logActivityLocked("terminal " + std::to_string(t.number) + " went away, released its locks");
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
        int philosopher_strategy = 1;
        int wait_seconds = 20;   // how long a blocked producer/consumer/reader/writer waits
    };

    class ConfigManager {
    private:
        ProblemConfig config;

    public:
        ProblemConfig& getConfig() { return config; }
        const ProblemConfig& getConfig() const { return config; }

        void configureAll() {
            UI::PrintHeader("SETTINGS");
            UI::PrintBreadcrumb("Main menu > Settings");

            std::cout << "  Producer-consumer\n";
            int capacity = UI::SafeReadInt("  Buffer size (1-100): ", 1, kMaxBufferSize);

            std::cout << "\n  Waiting\n";
            config.wait_seconds = UI::SafeReadInt("  How long to wait for a busy resource, in seconds (1-120): ", 1, 120);

            std::cout << "\n  Dining philosophers\n";
            config.num_philosophers = UI::SafeReadInt("  Number of philosophers (2-20): ", 2, 20);

            std::cout << "  Deadlock avoidance strategy:\n"
                      << "    1. Resource hierarchy (pick up the lower-numbered fork first)\n"
                      << "    2. Room semaphore (at most N-1 at the table)\n";
            config.philosopher_strategy = UI::SafeReadInt("  Choice (1-2): ", 1, 2);

            UI::ProgressBarAnimation("Saving", 400);

            config.buffer_capacity = capacity;
            if (g_Session.isAttached()) {
                OpResult r = g_Session.resizeBuffer(capacity);
                if (!r.ok()) {
                    UI::PrintWarningCard(r.message + " Your other settings were saved.");
                    return;
                }
            }

            std::cout << Color::Green() << "\n  ";
            UI::TypeText("Settings saved.", 12);
            std::cout << "\n" << Color::Reset();
        }
    };
}

namespace Toolkit {

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
        std::vector<BenchmarkResult> benchmark_history;
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
            benchmark_history.push_back(result);
        }

        void displaySummaryTable() {
            std::lock_guard<std::mutex> lock(history_mutex);
            UI::PrintHeader("SESSION REPORT");
            UI::PrintBreadcrumb("Main menu > Report");

            if (benchmark_history.empty()) {
                std::cout << Color::Yellow() << "  Nothing recorded yet. Try one of the problems first.\n" << Color::Reset();
                return;
            }

            std::cout << Color::Bold() << std::left
                      << std::setw(22) << "  Problem"
                      << std::setw(25) << "Approach"
                      << std::setw(10) << "Safe"
                      << std::setw(14) << "Actions"
                      << std::setw(16) << "Blocked"
                      << std::setw(14) << "Integrity"
                      << "\n" << Color::Reset();

            std::cout << "  " << std::string(100, '-') << "\n";

            for (const auto& res : benchmark_history) {
                std::string safe_str = res.is_safe_implementation ? (Color::Green() + "yes" + Color::Reset())
                                                                  : (Color::Red() + "no" + Color::Reset());
                std::string integrity_str = res.data_integrity_passed ? (Color::Green() + "ok" + Color::Reset())
                                                                      : (Color::Red() + "broken" + Color::Reset());

                std::cout << std::left
                          << "  " << std::setw(20) << res.problem_name
                          << std::setw(25) << res.strategy_name
                          << std::setw(10 + static_cast<int>(safe_str.size()) - (res.is_safe_implementation ? 3 : 2)) << safe_str
                          << std::setw(14) << res.total_operations
                          << std::setw(16) << res.wait_contention_count
                          << integrity_str
                          << "\n" << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            std::cout << "  " << std::string(100, '-') << "\n";
        }

        void exportCSVReport(const std::string& filename = "benchmark_report.csv") {
            std::lock_guard<std::mutex> lock(history_mutex);
            UI::PrintHeader("EXPORT CSV");
            UI::PrintBreadcrumb("Main menu > Export CSV");

            UI::ProgressBarAnimation("Writing " + filename, 500);

            std::ofstream file(filename);
            if (!file.is_open()) {
                g_Logger.log(Logger::Level::ERROR, "report", "couldn't open " + filename + " for writing");
                UI::PrintErrorCard("Couldn't create " + filename + ". Is it open in another program?");
                return;
            }

            file << "Problem,Approach,Safe,Actions,Blocked,Integrity,TimeMs,Notes\n";
            for (const auto& res : benchmark_history) {
                file << csvField(res.problem_name) << ","
                     << csvField(res.strategy_name) << ","
                     << (res.is_safe_implementation ? "yes" : "no") << ","
                     << res.total_operations << ","
                     << res.wait_contention_count << ","
                     << (res.data_integrity_passed ? "ok" : "broken") << ","
                     << static_cast<long long>(res.execution_time_ms) << ","
                     << csvField(res.notes) << "\n";
            }
            file.close();
            if (!file) {
                UI::PrintErrorCard("Something went wrong while writing " + filename + ".");
                return;
            }

            std::cout << Color::Green() << "\n  ";
            UI::TypeText("Saved to " + filename + ".", 12);
            std::cout << "\n" << Color::Reset();
            g_Logger.log(Logger::Level::OK, "report", "exported " + filename);
        }
    };

    inline PerformanceAnalyzer g_Analyzer;
}

namespace Toolkit {

    // Local buffer, only used by the unsynchronized demo.
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

        std::string renderVisualState() const {
            std::stringstream ss;
            for (int i = 0; i < capacity; ++i) {
                bool filled = ((i - front + capacity) % capacity) < count;
                if (filled) ss << Color::Green() << Color::Bold() << "[" << std::right << std::setw(4) << buffer[i] << std::left << "]" << Color::Reset();
                else        ss << Color::Dim() << "[    ]" << Color::Reset();
                ss << " ";
            }
            ss << " " << count << "/" << capacity;
            return ss.str();
        }
    };

    namespace View {

        inline void PrintSharedBuffer(const SharedSession::Snapshot& s) {
            std::cout << "  " << Color::Bold() << "Shared buffer" << Color::Reset()
                      << "  " << s.count << " of " << s.capacity << " slots used\n";
            for (int i = 0; i < s.capacity; ++i) {
                if (i % 10 == 0) std::cout << (i ? "\n    " : "    ");
                bool filled = ((i - s.front + s.capacity) % s.capacity) < s.count;
                if (filled) std::cout << Color::Green() << Color::Bold() << "[" << std::right << std::setw(4) << s.items[i] << std::left << "]" << Color::Reset() << " ";
                else        std::cout << Color::Dim() << "[    ]" << Color::Reset() << " ";
            }
            std::cout << "\n  " << Color::Dim()
                      << "produced " << s.produced << ", consumed " << s.consumed
                      << "  |  waiting: " << s.waiting_producers << " producer(s), " << s.waiting_consumers << " consumer(s)"
                      << "  |  " << s.terminals_online << " terminal(s) online"
                      << Color::Reset() << "\n";
        }

        inline void PrintDatabase(const SharedSession::Snapshot& s) {
            std::cout << "  " << Color::Bold() << "Shared database" << Color::Reset()
                      << "  value = " << Color::Green() << Color::Bold() << s.db_value << Color::Reset()
                      << "  |  reading now: " << s.active_readers
                      << "  |  writer: " << (s.writer_terminal ? "terminal " + std::to_string(s.writer_terminal) : std::string("none")) << "\n";
            std::cout << "  " << Color::Dim()
                      << "reads " << s.reads << ", writes " << s.writes
                      << "  |  waiting: " << s.waiting_readers << " reader(s), " << s.waiting_writers << " writer(s)"
                      << "  |  " << s.terminals_online << " terminal(s) online"
                      << Color::Reset() << "\n";
        }

        inline void PrintActivity(const SharedSession::Snapshot& s) {
            std::cout << "\n  " << Color::Bold() << "Recent activity" << Color::Reset() << Color::Dim() << "  (all terminals)" << Color::Reset() << "\n";
            if (s.activity.empty()) {
                std::cout << "    " << Color::Dim() << "nothing yet" << Color::Reset() << "\n";
                return;
            }
            size_t start = s.activity.size() > 5 ? s.activity.size() - 5 : 0;
            for (size_t i = start; i < s.activity.size(); ++i) {
                std::cout << "    " << Color::Dim() << s.activity[i] << Color::Reset() << "\n";
            }
        }

        inline void ReportResult(const OpResult& r, const std::string& who, uint64_t& timeouts, uint64_t& failures) {
            if (r.ok()) {
                if (!r.message.empty()) g_Logger.log(Logger::Level::OK, who, r.message);
            } else if (r.status == OpResult::Status::TimedOut) {
                timeouts++;
                UI::PrintWarningCard(r.message);
                g_Logger.log(Logger::Level::WARN, who, "gave up waiting");
            } else {
                failures++;
                UI::PrintErrorCard(r.message);
                g_Logger.log(Logger::Level::ERROR, who, r.message);
            }
        }

        inline bool RequireSession() {
            if (g_Session.isAttached()) return true;
            UI::PrintErrorCard("The shared session isn't available: " +
                               (g_SessionError.empty() ? std::string("unknown reason") : g_SessionError));
            return false;
        }
    }

    class ProducerConsumerEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("PRODUCER-CONSUMER (SHARED)");
            UI::PrintBreadcrumb("Main menu > Producer-consumer");
            if (!View::RequireSession()) return;

            std::cout << "  " << Color::Dim() << "Open this program in another terminal to produce and consume on the same buffer.\n"
                      << "  Producers wait on 'empty', consumers wait on 'full', and a mutex guards the buffer itself."
                      << Color::Reset() << "\n";

            const int timeout_ms = config.wait_seconds * 1000;
            uint64_t done = 0, timeouts = 0, failures = 0;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                auto snap = g_Session.snapshot();
                std::cout << "\n";
                if (snap.ok) {
                    View::PrintSharedBuffer(snap);
                    View::PrintActivity(snap);
                } else {
                    UI::PrintWarningCard("Couldn't read the shared state just now.");
                }
                std::cout << "\n";
                UI::PrintOption("1", "Produce an item");
                UI::PrintOption("2", "Consume an item");
                UI::PrintOption("3", "Refresh");
                UI::PrintOption("0", "Back to the main menu");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-3): ", 0, 3);

                if (option == 1) {
                    int producer_id = UI::SafeReadInt("  Producer id (1-100): ", 1, 100);
                    int value = UI::SafeReadInt("  Value to add (1-9999): ", 1, 9999);
                    std::string who = "producer " + std::to_string(producer_id);

                    OpResult r = UI::RunWithSpinner(who + " waiting for an empty slot", timeout_ms,
                        [&] { return g_Session.produce(producer_id, value, timeout_ms); });
                    if (r.ok()) done++;
                    View::ReportResult(r, who, timeouts, failures);
                } else if (option == 2) {
                    int consumer_id = UI::SafeReadInt("  Consumer id (1-100): ", 1, 100);
                    std::string who = "consumer " + std::to_string(consumer_id);

                    OpResult r = UI::RunWithSpinner(who + " waiting for an item", timeout_ms,
                        [&] { return g_Session.consume(consumer_id, timeout_ms); });
                    if (r.ok()) done++;
                    View::ReportResult(r, who, timeouts, failures);
                } else if (option == 3) {
                    UI::LoadingDots("Refreshing", 3, 80);
                } else {
                    running = false;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Producer-Consumer";
            res.strategy_name = "Shared semaphores";
            res.is_safe_implementation = true;
            res.total_operations = done;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = timeouts;
            res.data_integrity_passed = failures == 0;
            res.notes = "Buffer shared with other terminals through named semaphores.";
            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("PRODUCER-CONSUMER WITHOUT SYNCHRONIZATION");
            UI::PrintBreadcrumb("Main menu > Unsafe demos > Producer-consumer");

            CircularBuffer<int> buffer(config.buffer_capacity);
            uint64_t violations = 0;
            uint64_t ops = 0;

            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                std::cout << "\n  " << Color::Bold() << "Buffer (no locks): " << Color::Reset() << buffer.renderVisualState() << "\n\n";
                UI::PrintOption("1", "Produce without checking for space");
                UI::PrintOption("2", "Consume without checking for items");
                UI::PrintOption("0", "Back");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-2): ", 0, 2);

                if (option == 1) {
                    int val = UI::SafeReadInt("  Value: ", 1, 9999);
                    ops++;
                    if (buffer.isFull()) {
                        violations++;
                        UI::PrintErrorCard("Overflow. Without the 'empty' semaphore nothing stopped the producer from writing past a full buffer.");
                        g_Logger.log(Logger::Level::ERROR, "producer", "buffer overflow");
                    } else {
                        buffer.push(val);
                        g_Logger.log(Logger::Level::WARN, "producer", "added " + std::to_string(val) + " without any locking");
                    }
                } else if (option == 2) {
                    ops++;
                    if (buffer.isEmpty()) {
                        violations++;
                        UI::PrintErrorCard("Underflow. Without the 'full' semaphore the consumer read from an empty buffer.");
                        g_Logger.log(Logger::Level::ERROR, "consumer", "buffer underflow");
                    } else {
                        int item = buffer.pop();
                        g_Logger.log(Logger::Level::WARN, "consumer", "took " + std::to_string(item) + " without any locking");
                    }
                } else {
                    running = false;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Producer-Consumer";
            res.strategy_name = "No synchronization";
            res.is_safe_implementation = false;
            res.total_operations = ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = violations;
            res.data_integrity_passed = (violations == 0);
            res.notes = "Overflow and underflow triggered by hand.";
            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    struct LocalDatabase {
        int value = 100;
        int active_readers = 0;
        int active_writers = 0;
    };

    class ReadersWritersEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("READERS-WRITERS (SHARED)");
            UI::PrintBreadcrumb("Main menu > Readers-writers");
            if (!View::RequireSession()) return;

            std::cout << "  " << Color::Dim() << "Open this program in another terminal to share the database. Several readers can\n"
                      << "  read at once; a writer needs it to itself. Readers have priority, so a steady\n"
                      << "  stream of readers can keep a writer waiting."
                      << Color::Reset() << "\n";

            const int timeout_ms = config.wait_seconds * 1000;
            uint64_t done = 0, timeouts = 0, failures = 0;
            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                auto snap = g_Session.snapshot();
                std::cout << "\n";
                if (snap.ok) {
                    View::PrintDatabase(snap);
                    View::PrintActivity(snap);
                } else {
                    UI::PrintWarningCard("Couldn't read the shared state just now.");
                }
                std::cout << "\n";
                UI::PrintOption("1", "Start reading");
                UI::PrintOption("2", "Start writing");
                UI::PrintOption("3", "Refresh");
                UI::PrintOption("0", "Back to the main menu");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-3): ", 0, 3);

                if (option == 1) {
                    int reader_id = UI::SafeReadInt("  Reader id (1-100): ", 1, 100);
                    std::string who = "reader " + std::to_string(reader_id);

                    OpResult r = UI::RunWithSpinner(who + " waiting to read", timeout_ms,
                        [&] { return g_Session.beginRead(reader_id, timeout_ms); });
                    View::ReportResult(r, who, timeouts, failures);
                    if (!r.ok()) continue;

                    done++;
                    g_Logger.log(Logger::Level::OK, who, "read value " + std::to_string(r.value));
                    std::cout << "\n  " << Color::Green() << "You're reading. " << Color::Reset()
                              << "Other terminals can read as well, but writers will wait until you finish.\n";
                    UI::ReadLine("  Press ENTER when you're done reading...");

                    OpResult end = g_Session.endRead(reader_id);
                    while (!end.ok()) {
                        UI::PrintErrorCard(end.message);
                        UI::ReadLine("  Press ENTER to try again...");
                        end = g_Session.endRead(reader_id);
                    }
                    g_Logger.log(Logger::Level::INFO, who, "finished reading");
                } else if (option == 2) {
                    int writer_id = UI::SafeReadInt("  Writer id (1-100): ", 1, 100);
                    std::string who = "writer " + std::to_string(writer_id);

                    OpResult r = UI::RunWithSpinner(who + " waiting for the database", timeout_ms,
                        [&] { return g_Session.beginWrite(writer_id, timeout_ms); });
                    View::ReportResult(r, who, timeouts, failures);
                    if (!r.ok()) continue;

                    std::cout << "\n  " << Color::Green() << "The database is yours. " << Color::Reset()
                              << "Current value is " << r.value << ". Everyone else waits until you're done.\n";
                    int value = UI::SafeReadInt("  New value (1-9999, or 0 to cancel): ", 0, 9999);

                    OpResult end = g_Session.endWrite(writer_id, value != 0, value);
                    if (!end.ok()) {
                        failures++;
                        UI::PrintErrorCard(end.message);
                    } else if (value != 0) {
                        done++;
                        g_Logger.log(Logger::Level::OK, who, "wrote " + std::to_string(value));
                    } else {
                        g_Logger.log(Logger::Level::INFO, who, "cancelled, nothing written");
                    }
                } else if (option == 3) {
                    UI::LoadingDots("Refreshing", 3, 80);
                } else {
                    running = false;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Readers-Writers";
            res.strategy_name = "Shared, reader priority";
            res.is_safe_implementation = true;
            res.total_operations = done;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = timeouts;
            res.data_integrity_passed = failures == 0;
            res.notes = "Database shared with other terminals; read count lock plus write semaphore.";
            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveMode() {
            UI::ClearScreen();
            UI::PrintHeader("READERS-WRITERS WITHOUT SYNCHRONIZATION");
            UI::PrintBreadcrumb("Main menu > Unsafe demos > Readers-writers");

            LocalDatabase db;
            uint64_t conflicts = 0;
            uint64_t ops = 0;

            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                std::cout << "\n  " << Color::Bold() << "Database (no locks): " << Color::Reset()
                          << "value " << db.value << ", readers " << db.active_readers << ", writers " << db.active_writers << "\n\n";

                UI::PrintOption("1", "Read without a lock");
                UI::PrintOption("2", "Write without a lock");
                UI::PrintOption("3", "Read while a write is half done");
                UI::PrintOption("0", "Back");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-3): ", 0, 3);

                if (option == 1) {
                    ops++;
                    g_Logger.log(Logger::Level::INFO, "reader", "read " + std::to_string(db.value));
                } else if (option == 2) {
                    int val = UI::SafeReadInt("  Value to write: ", 1, 9999);
                    ops++;
                    db.value = val;
                    g_Logger.log(Logger::Level::WARN, "writer", "wrote " + std::to_string(val) + " with nothing stopping readers");
                } else if (option == 3) {
                    ops += 2;
                    conflicts++;
                    db.active_writers++;
                    db.active_readers++;
                    UI::PrintErrorCard("Dirty read. The reader saw the value while a writer was still changing it.");
                    g_Logger.log(Logger::Level::ERROR, "reader", "dirty read of " + std::to_string(db.value) + " during a write");
                    db.active_writers--;
                    db.active_readers--;
                } else {
                    running = false;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Readers-Writers";
            res.strategy_name = "No synchronization";
            res.is_safe_implementation = false;
            res.total_operations = ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = conflicts;
            res.data_integrity_passed = (conflicts == 0);
            res.notes = "Dirty reads triggered by hand.";
            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    class DiningPhilosophersEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("DINING PHILOSOPHERS");
            UI::PrintBreadcrumb("Main menu > Dining philosophers");

            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            std::vector<std::string> phil_state(N, "thinking");

            uint64_t total_meals = 0;
            uint64_t blocked_fork_pickups = 0;

            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                std::cout << "\n  " << Color::Bold() << "Table (" << N << " philosophers)" << Color::Reset() << "\n";
                for (int i = 0; i < N; ++i) {
                    std::string st_color = (phil_state[i] == "eating") ? Color::Green() : Color::Yellow();
                    int left_fork = i;
                    int right_fork = (i + 1) % N;

                    std::cout << "    Philosopher " << std::right << std::setw(2) << i << std::left << ": "
                              << st_color << std::setw(10) << phil_state[i] << Color::Reset()
                              << " fork " << left_fork << ": " << (fork_taken[left_fork] ? Color::Red() + "taken" : Color::Green() + "free ") << Color::Reset()
                              << "   fork " << right_fork << ": " << (fork_taken[right_fork] ? Color::Red() + "taken" : Color::Green() + "free ") << Color::Reset() << "\n";
                }
                std::cout << "\n";

                UI::PrintOption("1", "Pick up left fork");
                UI::PrintOption("2", "Pick up right fork and eat");
                UI::PrintOption("3", "Put both forks down and think");
                UI::PrintOption("0", "Back to the main menu");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-3): ", 0, 3);

                if (option == 0) {
                    running = false;
                    continue;
                }

                int p_id = UI::SafeReadInt("  Which philosopher (0-" + std::to_string(N - 1) + "): ", 0, N - 1);
                int left_fork = p_id;
                int right_fork = (p_id + 1) % N;
                std::string who = "philosopher " + std::to_string(p_id);

                if (option == 1) {
                    UI::SpinnerAnimation(who + " reaching for fork " + std::to_string(left_fork));

                    if (!fork_taken[left_fork]) {
                        fork_taken[left_fork] = true;
                        g_Logger.log(Logger::Level::INFO, who, "picked up left fork " + std::to_string(left_fork));
                    } else {
                        blocked_fork_pickups++;
                        UI::PrintErrorCard("Fork " + std::to_string(left_fork) + " is already being used by a neighbour.");
                        g_Logger.log(Logger::Level::WARN, who, "left fork " + std::to_string(left_fork) + " is taken");
                    }
                } else if (option == 2) {
                    UI::SpinnerAnimation(who + " reaching for fork " + std::to_string(right_fork));

                    if (fork_taken[left_fork] && !fork_taken[right_fork]) {
                        fork_taken[right_fork] = true;
                        phil_state[p_id] = "eating";
                        total_meals++;
                        g_Logger.log(Logger::Level::OK, who, "picked up right fork " + std::to_string(right_fork) + " and is eating");
                    } else if (!fork_taken[left_fork]) {
                        blocked_fork_pickups++;
                        UI::PrintWarningCard("Philosopher " + std::to_string(p_id) + " has to pick up the left fork first.");
                        g_Logger.log(Logger::Level::WARN, who, "tried the right fork before the left one");
                    } else {
                        blocked_fork_pickups++;
                        UI::PrintErrorCard("Fork " + std::to_string(right_fork) + " is taken.");
                        g_Logger.log(Logger::Level::WARN, who, "right fork " + std::to_string(right_fork) + " is taken");
                    }
                } else if (option == 3) {
                    fork_taken[left_fork] = false;
                    fork_taken[right_fork] = false;
                    phil_state[p_id] = "thinking";
                    g_Logger.log(Logger::Level::INFO, who, "put the forks down and is thinking");
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Dining Philosophers";
            res.strategy_name = "Manual fork control";
            res.is_safe_implementation = true;
            res.total_operations = total_meals;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = blocked_fork_pickups;
            res.data_integrity_passed = true;
            res.notes = "Every pickup and release was done by hand.";
            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveDeadlockDemo(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("DINING PHILOSOPHERS DEADLOCK");
            UI::PrintBreadcrumb("Main menu > Unsafe demos > Dining philosophers");

            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            int left_forks_held = 0;

            Timer timer;
            timer.start();

            for (bool running = true; running; ) {
                std::cout << "\n  " << Color::Bold() << "Left forks held: " << Color::Reset()
                          << Color::Red() << Color::Bold() << left_forks_held << "/" << N << Color::Reset() << "\n\n";

                UI::PrintOption("1", "Make a philosopher pick up only the left fork");
                UI::PrintOption("2", "Everyone grabs their left fork at once");
                UI::PrintOption("0", "Back");
                std::cout << "\n";

                int option = UI::SafeReadInt("  Choose (0-2): ", 0, 2);

                if (option == 1) {
                    int p_id = UI::SafeReadInt("  Which philosopher (0-" + std::to_string(N - 1) + "): ", 0, N - 1);

                    if (!fork_taken[p_id]) {
                        fork_taken[p_id] = true;
                        left_forks_held++;
                        g_Logger.log(Logger::Level::WARN, "philosopher " + std::to_string(p_id), "holding left fork " + std::to_string(p_id));
                    } else {
                        UI::PrintWarningCard("Fork " + std::to_string(p_id) + " is already held.");
                    }

                    if (left_forks_held == N) {
                        std::cout << "\n";
                        UI::BlinkAlert("  DEADLOCK: all " + std::to_string(N) + " philosophers hold a left fork and wait for a right one  ");
                    }
                } else if (option == 2) {
                    UI::SpinnerAnimation("Everyone reaching for their left fork");
                    for (int i = 0; i < N; ++i) {
                        fork_taken[i] = true;
                        g_Logger.log(Logger::Level::WARN, "philosopher " + std::to_string(i),
                                     "holding fork " + std::to_string(i) + ", waiting for fork " + std::to_string((i + 1) % N));
                    }
                    left_forks_held = N;
                    std::cout << "\n";
                    UI::BlinkAlert("  DEADLOCK: circular wait, nobody can eat  ");
                } else {
                    running = false;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Dining Philosophers";
            res.strategy_name = "No ordering (deadlock)";
            res.is_safe_implementation = false;
            res.total_operations = left_forks_held;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = left_forks_held;
            res.data_integrity_passed = left_forks_held < N;
            res.notes = "Circular wait built by hand.";
            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    class MainMenu {
    private:
        ConfigManager config_mgr;

        // Runs one screen and makes sure an unexpected error drops us back at
        // the menu instead of killing the program while holding a shared lock.
        template <typename Screen>
        void runScreen(Screen&& screen) {
            try {
                screen();
            } catch (const UI::InputClosed&) {
                throw;
            } catch (const std::exception& e) {
                g_Session.releaseHeld();
                UI::PrintErrorCard(std::string("Something went wrong: ") + e.what());
            }
            UI::Pause();
        }

        void printSessionLine() {
            if (!g_Session.isAttached()) {
                std::cout << "  " << Color::Yellow() << "Shared mode is off: " << g_SessionError << Color::Reset() << "\n";
                return;
            }
            auto snap = g_Session.snapshot();
            std::cout << "  " << Color::Green() << "Terminal " << g_Session.terminalNumber() << Color::Reset();
            if (snap.ok) {
                std::cout << Color::Dim() << "  |  " << snap.terminals_online << " terminal(s) connected"
                          << "  |  buffer " << snap.count << "/" << snap.capacity
                          << "  |  db value " << snap.db_value << Color::Reset();
            }
            std::cout << "\n\n";
        }

    public:
        void run() {
            UI::ClearScreen();
            std::cout << "\n";
            UI::LoadingDots("Connecting to the shared session");

            if (!g_Session.attach(config_mgr.getConfig().buffer_capacity, g_SessionError)) {
                g_Logger.log(Logger::Level::WARN, "session", "shared mode unavailable: " + g_SessionError);
            } else {
                g_Logger.setSource("T" + std::to_string(g_Session.terminalNumber()));
            }

            bool first_draw = true;
            for (bool running = true; running; ) {
                UI::ClearScreen();
                UI::PrintBanner(first_draw);
                UI::animate_menu = first_draw;
                first_draw = false;

                printSessionLine();
                std::cout << Color::Bold() << "  MAIN MENU\n" << Color::Reset();
                std::cout << "  " << std::string(79, '-') << "\n";

                UI::PrintMenuCard("1", "Producer-consumer", "Shared bounded buffer, works across terminals");
                UI::PrintMenuCard("2", "Readers-writers", "Shared database, works across terminals");
                UI::PrintMenuCard("3", "Dining philosophers", "Pick up and put down forks by hand");
                UI::PrintMenuCard("4", "Unsafe demos", "See what breaks without synchronization");
                UI::PrintMenuCard("5", "Session report", "What you did this session");
                UI::PrintMenuCard("6", "Settings", "Buffer size, wait time, philosophers");
                UI::PrintMenuCard("7", "Export CSV", "Save the session report to a file");
                UI::PrintMenuCard("8", "Colors", std::string("currently ") + (Color::enable_colors ? "on" : "off"));
                UI::PrintMenuCard("0", "Quit", "");

                std::cout << "  " << std::string(79, '-') << "\n\n";
                UI::animate_menu = false;

                int choice = UI::SafeReadInt("  Choose (0-8): ", 0, 8);

                switch (choice) {
                    case 1:
                        runScreen([&] { ProducerConsumerEngine::runInteractiveMode(config_mgr.getConfig()); });
                        break;
                    case 2:
                        runScreen([&] { ReadersWritersEngine::runInteractiveMode(config_mgr.getConfig()); });
                        break;
                    case 3:
                        runScreen([&] { DiningPhilosophersEngine::runInteractiveMode(config_mgr.getConfig()); });
                        break;
                    case 4:
                        runScreen([&] {
                            UI::ClearScreen();
                            UI::PrintHeader("UNSAFE DEMOS");
                            UI::PrintBreadcrumb("Main menu > Unsafe demos");
                            std::cout << "  These run locally in this terminal only.\n\n"
                                      << "  1. Producer-consumer (overflow and underflow)\n"
                                      << "  2. Readers-writers (dirty read)\n"
                                      << "  3. Dining philosophers (circular wait deadlock)\n\n";
                            int naive_choice = UI::SafeReadInt("  Choose (1-3): ", 1, 3);
                            if (naive_choice == 1) ProducerConsumerEngine::runNaiveInteractiveMode(config_mgr.getConfig());
                            else if (naive_choice == 2) ReadersWritersEngine::runNaiveInteractiveMode();
                            else DiningPhilosophersEngine::runNaiveInteractiveDeadlockDemo(config_mgr.getConfig());
                        });
                        break;
                    case 5:
                        runScreen([&] { UI::ClearScreen(); g_Analyzer.displaySummaryTable(); });
                        break;
                    case 6:
                        runScreen([&] { UI::ClearScreen(); config_mgr.configureAll(); });
                        break;
                    case 7:
                        runScreen([&] { UI::ClearScreen(); g_Analyzer.exportCSVReport(); });
                        break;
                    case 8:
                        Color::enable_colors = !Color::enable_colors;
                        std::cout << "\n";
                        UI::LoadingDots("Switching colors");
                        std::cout << Color::Green() << "  Colors are now " << (Color::enable_colors ? "on" : "off") << ".\n" << Color::Reset();
                        UI::Pause();
                        break;
                    case 0:
                        running = false;
                        break;
                }
            }
        }
    };
}

int main() {
    using namespace Toolkit;
    UI::InitTerminal();

    int exit_code = 0;
    try {
        MainMenu menu;
        menu.run();
    } catch (const UI::InputClosed&) {
        std::cout << "\n  Input closed, shutting down.\n";
    } catch (const std::exception& e) {
        UI::PrintErrorCard(std::string("Unexpected error: ") + e.what());
        exit_code = 1;
    } catch (...) {
        UI::PrintErrorCard("Unexpected error.");
        exit_code = 1;
    }

    // Give back anything we still hold so the other terminals aren't stuck.
    std::cout << "\n";
    UI::LoadingDots("Releasing locks and shutting down");
    g_Session.detach();

    std::cout << Color::Cyan() << "  ";
    UI::TypeText("Bye!", 25);
    std::cout << "\n" << Color::Reset();
    return exit_code;
}
