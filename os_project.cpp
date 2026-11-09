#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <memory>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <limits>

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
#endif

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
        std::chrono::high_resolution_clock::time_point start_time;
        std::chrono::high_resolution_clock::time_point end_time;
        bool running;

    public:
        Timer() : running(false) {}

        void start() {
            start_time = std::chrono::high_resolution_clock::now();
            running = true;
        }

        void stop() {
            end_time = std::chrono::high_resolution_clock::now();
            running = false;
        }

        double elapsedMilliseconds() const {
            auto end = running ? std::chrono::high_resolution_clock::now() : end_time;
            return std::chrono::duration<double, std::milli>(end - start_time).count();
        }
    };

    class Logger {
    public:
        enum class Level { INFO, WARN, ERROR, TRACE, SUCCESS };

    private:
        std::mutex log_mutex;
        std::ofstream log_file;
        bool log_to_file;

    public:
        Logger() : log_to_file(false) {
            log_file.open("threadweaver_session_log.txt", std::ios::out | std::ios::app);
            if (log_file.is_open()) {
                log_to_file = true;
            }
        }

        ~Logger() {
            if (log_file.is_open()) log_file.close();
        }

        void log(Level level, const std::string& sender, const std::string& message) {
            std::lock_guard<std::mutex> lock(log_mutex);

            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::stringstream ss;
            ss << std::put_time(std::localtime(&time_t_now), "%H:%M:%S")
               << '.' << std::setfill('0') << std::setw(3) << ms.count();
            std::string timestamp = ss.str();

            std::string level_str;
            std::string color_code;

            switch (level) {
                case Level::INFO:    level_str = "[INFO]   "; color_code = Color::Cyan();    break;
                case Level::WARN:    level_str = "[WARN]   "; color_code = Color::Yellow();  break;
                case Level::ERROR:   level_str = "[ERROR]  "; color_code = Color::Red();     break;
                case Level::TRACE:   level_str = "[TRACE]  "; color_code = Color::Magenta(); break;
                case Level::SUCCESS: level_str = "[SUCCESS]"; color_code = Color::Green();   break;
            }

            std::cout << Color::Bold() << "[" << timestamp << "] " << color_code 
                      << level_str << Color::Reset() << " [" << Color::Yellow() 
                      << sender << Color::Reset() << "] " << message << std::endl;

            if (log_to_file && log_file.is_open()) {
                log_file << "[" << timestamp << "] " << level_str << " [" << sender << "] " << message << "\n";
                log_file.flush();
            }
        }
    };

    inline Logger g_Logger;

    namespace UI {
        inline void InitTerminal() {
            #ifdef _WIN32
                SetConsoleOutputCP(65001);
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD dwMode = 0;
                GetConsoleMode(hOut, &dwMode);
                SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
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
  |              INTERACTIVE CONCURRENCY & SYNCHRONIZATION STUDIO                   |
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
            std::cout << "  " << Color::BgCyan() << Color::White() << Color::Bold() << " LOCATION " 
                      << Color::Reset() << " " << Color::Cyan() << Color::Bold() << path << Color::Reset() << "\n\n";
        }

        inline void PrintHeader(const std::string& title) {
            int total_width = 83;
            int padding = (total_width - 4 - static_cast<int>(title.length())) / 2;
            std::string left_pad(std::max(0, padding), ' ');
            std::string right_pad(std::max(0, total_width - 4 - static_cast<int>(title.length()) - padding), ' ');

            std::cout << Color::Yellow() << Color::Bold()
                      << "+" << std::string(total_width - 2, '=') << "+\n"
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

        inline void PrintErrorCard(const std::string& err_msg) {
            std::cout << "\n  " << Color::BgRed() << Color::White() << Color::Bold()
                      << " [FAILSAFE GUARD ERROR] " << Color::Reset()
                      << " " << Color::Red() << Color::Bold() << err_msg << Color::Reset() << "\n";
        }

        inline void PrintWarningCard(const std::string& warn_msg) {
            std::cout << "\n  " << Color::BgYellow() << Color::White() << Color::Bold()
                      << " [FAILSAFE GUARD WARNING] " << Color::Reset()
                      << " " << Color::Yellow() << Color::Bold() << warn_msg << Color::Reset() << "\n";
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
                          << std::setw(3) << static_cast<int>(progress * 100.0) << "% " << Color::Reset() << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            std::cout << "\n";
        }

        inline int SafeReadInt(const std::string& prompt, int min_val, int max_val) {
            int val;
            while (true) {
                std::cout << prompt;
                if (std::cin >> val) {
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    if (val >= min_val && val <= max_val) {
                        return val;
                    }
                    PrintErrorCard("Value out of allowable bounds [" + std::to_string(min_val) + " to " + std::to_string(max_val) + "]! Try again.");
                } else {
                    std::cin.clear();
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    PrintErrorCard("Invalid non-numeric input detected! Please enter an integer.");
                }
            }
        }

        inline void Pause() {
            std::cout << "\n" << Color::Yellow() << "  Press ENTER to continue..." << Color::Reset();
            std::cin.get();
        }
    }
}

namespace Toolkit {

    class Semaphore {
    private:
        std::mutex mtx;
        std::condition_variable cv;
        int count;
        std::atomic<uint64_t> total_waits{0};

    public:
        explicit Semaphore(int initial_count = 0) : count(initial_count) {}

        bool try_wait() {
            std::lock_guard<std::mutex> lock(mtx);
            if (count > 0) {
                --count;
                return true;
            }
            total_waits++;
            return false;
        }

        void signal() {
            std::lock_guard<std::mutex> lock(mtx);
            ++count;
            cv.notify_one();
        }

        int getValue() {
            std::lock_guard<std::mutex> lock(mtx);
            return count;
        }

        uint64_t getTotalWaits() const { return total_waits.load(); }
    };

    class TrackedMutex {
    private:
        std::mutex mtx;
        std::atomic<uint64_t> lock_acquisitions{0};
        std::atomic<uint64_t> contention_count{0};

    public:
        void lock() {
            if (!mtx.try_lock()) {
                contention_count++;
                mtx.lock();
            }
            lock_acquisitions++;
        }

        bool try_lock() {
            if (mtx.try_lock()) {
                lock_acquisitions++;
                return true;
            }
            contention_count++;
            return false;
        }

        void unlock() { mtx.unlock(); }
        uint64_t getContentions() const { return contention_count.load(); }
    };
}

namespace Toolkit {

    struct ProblemConfig {
        int buffer_capacity = 5;
        int num_philosophers = 5;
        int philosopher_strategy = 1;
    };

    class ConfigManager {
    private:
        ProblemConfig config;

    public:
        ConfigManager() = default;

        ProblemConfig& getConfig() { return config; }
        const ProblemConfig& getConfig() const { return config; }

        void configureAll() {
            UI::PrintHeader("SYSTEM PARAMETERS CONFIGURATION");
            UI::PrintBreadcrumb("DASHBOARD > SYSTEM CONFIGURATION");

            std::cout << "  [Producer-Consumer Configuration]\n";
            config.buffer_capacity = UI::SafeReadInt("  Enter Buffer Capacity (1 to 100): ", 1, 100);

            std::cout << "\n  [Dining Philosophers Configuration]\n";
            config.num_philosophers = UI::SafeReadInt("  Enter Number of Philosophers (2 to 20): ", 2, 20);

            std::cout << "  Select Deadlock Avoidance Strategy:\n"
                      << "    1. Resource Hierarchy (Ordered Fork Pickup)\n"
                      << "    2. Room Semaphore (Limit Diners to N-1)\n";
            config.philosopher_strategy = UI::SafeReadInt("  Choice (1-2): ", 1, 2);

            UI::ProgressBarAnimation("Saving Configuration Settings", 400);
            std::cout << Color::Green() << "\n  [Configuration Settings Applied Successfully!]\n" << Color::Reset();
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

    public:
        void recordResult(const BenchmarkResult& result) {
            std::lock_guard<std::mutex> lock(history_mutex);
            benchmark_history.push_back(result);
        }

        void displaySummaryTable() {
            std::lock_guard<std::mutex> lock(history_mutex);
            UI::PrintHeader("PERFORMANCE & ANALYTICS DASHBOARD");
            UI::PrintBreadcrumb("DASHBOARD > PERFORMANCE REPORT");

            if (benchmark_history.empty()) {
                std::cout << Color::Yellow() << "  No benchmark history available yet. Interact with the modules first!\n" << Color::Reset();
                return;
            }

            std::cout << Color::Bold() << std::left
                      << std::setw(22) << "  Module Name"
                      << std::setw(25) << "Execution Strategy"
                      << std::setw(10) << "Safe?"
                      << std::setw(14) << "User Ops"
                      << std::setw(16) << "Contentions"
                      << std::setw(14) << "Integrity"
                      << "\n" << Color::Reset();

            std::cout << "  " << std::string(100, '-') << "\n";

            for (const auto& res : benchmark_history) {
                std::string safe_str = res.is_safe_implementation ? (Color::Green() + "YES" + Color::Reset()) 
                                                                  : (Color::Red() + "NO" + Color::Reset());
                std::string integrity_str = res.data_integrity_passed ? (Color::Green() + "PASSED" + Color::Reset()) 
                                                                        : (Color::Red() + "FAILED" + Color::Reset());

                std::cout << std::left
                          << "  " << std::setw(20) << res.problem_name
                          << std::setw(25) << res.strategy_name
                          << std::setw(19) << safe_str
                          << std::setw(14) << res.total_operations
                          << std::setw(16) << res.wait_contention_count
                          << std::setw(22) << integrity_str
                          << "\n";
            }
            std::cout << "  " << std::string(100, '-') << "\n";
        }

        void exportCSVReport(const std::string& filename = "benchmark_report.csv") {
            std::lock_guard<std::mutex> lock(history_mutex);
            UI::PrintHeader("EXPORT BENCHMARK CSV REPORT");
            UI::PrintBreadcrumb("DASHBOARD > EXPORT REPORT");

            UI::ProgressBarAnimation("Writing Session History to CSV", 500);

            std::ofstream file(filename);
            if (!file.is_open()) {
                g_Logger.log(Logger::Level::ERROR, "Analyzer", "Failed to open CSV file for writing.");
                UI::PrintErrorCard("Failed to create file: " + filename);
                return;
            }

            file << "Problem,Strategy,SafeImplementation,TotalOps,WaitContentions,IntegrityPassed,Notes\n";
            for (const auto& res : benchmark_history) {
                file << res.problem_name << ","
                     << res.strategy_name << ","
                     << (res.is_safe_implementation ? "True" : "False") << ","
                     << res.total_operations << ","
                     << res.wait_contention_count << ","
                     << (res.data_integrity_passed ? "Pass" : "Fail") << ","
                     << "\"" << res.notes << "\"\n";
            }
            file.close();
            std::cout << Color::Green() << "\n  [CSV Report Exported Successfully to " << filename << "!]\n" << Color::Reset();
            g_Logger.log(Logger::Level::SUCCESS, "Analyzer", "CSV report exported cleanly to " + filename);
        }
    };

    inline PerformanceAnalyzer g_Analyzer;
}

namespace Toolkit {

    template <typename T>
    class CircularBuffer {
    private:
        std::vector<T> buffer;
        int capacity;
        int front;
        int rear;
        int count;

    public:
        explicit CircularBuffer(int cap) : capacity(cap), front(0), rear(0), count(0) {
            buffer.resize(cap);
        }

        bool isFull() const { return count == capacity; }
        bool isEmpty() const { return count == 0; }
        int getCount() const { return count; }
        int getCapacity() const { return capacity; }

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
            ss << "[";
            for (int i = 0; i < capacity; ++i) {
                bool filled = false;
                if (count > 0) {
                    if (front < rear) {
                        filled = (i >= front && i < rear);
                    } else {
                        filled = (i >= front || i < rear);
                    }
                }
                if (filled) {
                    ss << Color::Green() << Color::Bold() << "[ITEM:" << buffer[i] << "]" << Color::Reset();
                } else {
                    ss << Color::Cyan() << "[ EMPTY ]" << Color::Reset();
                }
                if (i < capacity - 1) ss << " ";
            }
            ss << "] (Count: " << count << "/" << capacity << ")";
            return ss.str();
        }
    };

    class ProducerConsumerEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("MANUAL PRODUCER-CONSUMER CONTROL PANEL");
            UI::PrintBreadcrumb("DASHBOARD > PRODUCER-CONSUMER ENGINE");

            CircularBuffer<int> buffer(config.buffer_capacity);
            TrackedMutex buffer_mutex;
            Semaphore empty_sem(config.buffer_capacity);
            Semaphore full_sem(0);

            bool exit_module = false;
            uint64_t total_user_ops = 0;
            uint64_t blocked_attempts = 0;

            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "CURRENT BUFFER STATE: " << Color::Reset() << buffer.renderVisualState() << "\n";
                std::cout << "  " << Color::Dim() << "Empty Slots (empty_sem) = " << empty_sem.getValue()
                          << " | Filled Slots (full_sem) = " << full_sem.getValue() << Color::Reset() << "\n\n";

                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Produce Item (Enter Custom Value & Producer ID)\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Consume Item (Enter Consumer ID)\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Finish Session & Record Metrics\n\n";

                int option = UI::SafeReadInt("  Select Action (0-2): ", 0, 2);

                if (option == 1) {
                    int producer_id = UI::SafeReadInt("  Enter Producer ID (1 to 100): ", 1, 100);
                    int item_val = UI::SafeReadInt("  Enter Integer Item Value (1 to 9999): ", 1, 9999);

                    UI::SpinnerAnimation("Producer " + std::to_string(producer_id) + " requesting empty buffer slot");

                    if (empty_sem.try_wait()) {
                        buffer_mutex.lock();
                        buffer.push(item_val);
                        total_user_ops++;
                        g_Logger.log(Logger::Level::SUCCESS, "Producer_" + std::to_string(producer_id), "Pushed item " + std::to_string(item_val) + " into circular buffer.");
                        buffer_mutex.unlock();
                        full_sem.signal();
                    } else {
                        blocked_attempts++;
                        UI::PrintErrorCard("BUFFER OVERFLOW REJECTED! Buffer is at max capacity (" + std::to_string(config.buffer_capacity) + "/" + std::to_string(config.buffer_capacity) + "). Must consume an item first.");
                        g_Logger.log(Logger::Level::WARN, "Producer_" + std::to_string(producer_id), "BLOCKED! Buffer full. Action rejected by semaphore guard.");
                    }
                } else if (option == 2) {
                    int consumer_id = UI::SafeReadInt("  Enter Consumer ID (1 to 100): ", 1, 100);

                    UI::SpinnerAnimation("Consumer " + std::to_string(consumer_id) + " requesting filled buffer item");

                    if (full_sem.try_wait()) {
                        buffer_mutex.lock();
                        int item = buffer.pop();
                        total_user_ops++;
                        g_Logger.log(Logger::Level::SUCCESS, "Consumer_" + std::to_string(consumer_id), "Popped item " + std::to_string(item) + " from circular buffer.");
                        buffer_mutex.unlock();
                        empty_sem.signal();
                    } else {
                        blocked_attempts++;
                        UI::PrintErrorCard("BUFFER UNDERFLOW REJECTED! Buffer is empty (0/" + std::to_string(config.buffer_capacity) + "). Must produce an item first.");
                        g_Logger.log(Logger::Level::WARN, "Consumer_" + std::to_string(consumer_id), "BLOCKED! Buffer empty. Action rejected by semaphore guard.");
                    }
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Producer-Consumer";
            res.strategy_name = "Manual Semaphores";
            res.is_safe_implementation = true;
            res.total_operations = total_user_ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = blocked_attempts;
            res.data_integrity_passed = true;
            res.notes = "User manually controlled all semaphore produce/consume calls.";

            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("NAIVE PRODUCER-CONSUMER (MANUAL RACE DEMO)");
            UI::PrintBreadcrumb("DASHBOARD > SANDBOX > NAIVE PRODUCER-CONSUMER");

            CircularBuffer<int> buffer(config.buffer_capacity);
            bool exit_module = false;
            uint64_t violations = 0;
            uint64_t ops = 0;

            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "UNPROTECTED BUFFER STATE: " << Color::Reset() << buffer.renderVisualState() << "\n\n";
                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Force Produce Without Semaphore/Mutex Lock\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Force Consume Without Semaphore/Mutex Lock\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Exit Naive Demo\n\n";

                int option = UI::SafeReadInt("  Select Action (0-2): ", 0, 2);

                if (option == 1) {
                    int val = UI::SafeReadInt("  Enter Item Value: ", 1, 9999);
                    ops++;
                    if (buffer.isFull()) {
                        violations++;
                        UI::PrintErrorCard("RACE CONDITION EXPLOITED! Buffer overflow occurred due to missing semaphore boundary checks!");
                        g_Logger.log(Logger::Level::ERROR, "NaiveProducer", "RACE CONDITION VIOLATION! Buffer Overflowed!");
                    } else {
                        buffer.push(val);
                        g_Logger.log(Logger::Level::WARN, "NaiveProducer", "Unsynchronized item forced into buffer.");
                    }
                } else if (option == 2) {
                    ops++;
                    if (buffer.isEmpty()) {
                        violations++;
                        UI::PrintErrorCard("RACE CONDITION EXPLOITED! Buffer underflow occurred due to popping from empty queue!");
                        g_Logger.log(Logger::Level::ERROR, "NaiveConsumer", "RACE CONDITION VIOLATION! Buffer Underflowed!");
                    } else {
                        int item = buffer.pop();
                        g_Logger.log(Logger::Level::WARN, "NaiveConsumer", "Popped item " + std::to_string(item) + " without locking.");
                    }
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Producer-Consumer";
            res.strategy_name = "Naive (Unsafe User Control)";
            res.is_safe_implementation = false;
            res.total_operations = ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = violations;
            res.data_integrity_passed = (violations == 0);
            res.notes = "User manually triggered out-of-bound overflow/underflow errors.";

            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    struct SharedDatabase {
        int value = 100;
        int active_readers = 0;
        int active_writers = 0;
        std::mutex db_mutex;
    };

    class ReadersWritersEngine {
    public:
        static void runInteractiveMode() {
            UI::ClearScreen();
            UI::PrintHeader("MANUAL READERS-WRITERS CONTROL PANEL");
            UI::PrintBreadcrumb("DASHBOARD > READERS-WRITERS ENGINE");

            SharedDatabase db;
            int read_count = 0;
            TrackedMutex read_count_mutex;
            TrackedMutex write_resource_mutex;

            bool exit_module = false;
            uint64_t total_ops = 0;
            uint64_t contentions = 0;

            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "DATABASE STATE: " << Color::Reset()
                          << "Shared Value = " << Color::Green() << Color::Bold() << db.value << Color::Reset()
                          << " | Active Readers = " << db.active_readers
                          << " | Active Writers = " << db.active_writers << "\n\n";

                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Execute Reader Access (Enter Reader ID)\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Execute Writer Access (Enter Writer ID & Custom Value)\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Finish Session & Record Metrics\n\n";

                int option = UI::SafeReadInt("  Select Action (0-2): ", 0, 2);

                if (option == 1) {
                    int r_id = UI::SafeReadInt("  Enter Reader ID (1 to 100): ", 1, 100);

                    UI::SpinnerAnimation("Reader " + std::to_string(r_id) + " acquiring shared read lock");

                    read_count_mutex.lock();
                    read_count++;
                    if (read_count == 1) {
                        write_resource_mutex.lock();
                    }
                    read_count_mutex.unlock();

                    {
                        std::lock_guard<std::mutex> lock(db.db_mutex);
                        db.active_readers++;
                        total_ops++;
                        g_Logger.log(Logger::Level::INFO, "Reader_" + std::to_string(r_id), "Read DB Value = " + std::to_string(db.value) + " (Active Readers: " + std::to_string(db.active_readers) + ")");
                        db.active_readers--;
                    }

                    read_count_mutex.lock();
                    read_count--;
                    if (read_count == 0) {
                        write_resource_mutex.unlock();
                    }
                    read_count_mutex.unlock();
                } else if (option == 2) {
                    int w_id = UI::SafeReadInt("  Enter Writer ID (1 to 100): ", 1, 100);
                    int new_val = UI::SafeReadInt("  Enter New Value to Write (1 to 9999): ", 1, 9999);

                    UI::SpinnerAnimation("Writer " + std::to_string(w_id) + " acquiring exclusive write lock");

                    if (write_resource_mutex.try_lock()) {
                        std::lock_guard<std::mutex> lock(db.db_mutex);
                        db.active_writers++;
                        db.value = new_val;
                        total_ops++;
                        g_Logger.log(Logger::Level::SUCCESS, "Writer_" + std::to_string(w_id), "Wrote New DB Value = " + std::to_string(db.value));
                        db.active_writers--;
                        write_resource_mutex.unlock();
                    } else {
                        contentions++;
                        UI::PrintWarningCard("WRITER LOCK BLOCKED! Exclusive Database Lock is held by active readers or writers.");
                        g_Logger.log(Logger::Level::WARN, "Writer_" + std::to_string(w_id), "BLOCKED! Writer Lock is currently held.");
                    }
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Readers-Writers";
            res.strategy_name = "Manual Reader Preference";
            res.is_safe_implementation = true;
            res.total_operations = total_ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = contentions;
            res.data_integrity_passed = true;
            res.notes = "User explicitly controlled all reader and writer lock requests.";

            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveMode() {
            UI::ClearScreen();
            UI::PrintHeader("NAIVE READERS-WRITERS (MANUAL DIRTY READ DEMO)");
            UI::PrintBreadcrumb("DASHBOARD > SANDBOX > NAIVE READERS-WRITERS");

            SharedDatabase db;
            bool exit_module = false;
            uint64_t conflicts = 0;
            uint64_t ops = 0;

            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "UNLOCKED DATABASE STATE: " << Color::Reset()
                          << "Value = " << db.value << " | Readers = " << db.active_readers << " | Writers = " << db.active_writers << "\n\n";

                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Force Read Without Mutex Lock\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Force Write Without Mutex Lock\n"
                          << "  " << Color::Cyan() << "[3]" << Color::Reset() << " Force Simultaneous Dirty Read Collision\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Exit Naive Demo\n\n";

                int option = UI::SafeReadInt("  Select Action (0-3): ", 0, 3);

                if (option == 1) {
                    ops++;
                    g_Logger.log(Logger::Level::INFO, "NaiveReader", "Read DB Value = " + std::to_string(db.value));
                } else if (option == 2) {
                    int val = UI::SafeReadInt("  Enter Value to Write: ", 1, 9999);
                    ops++;
                    db.value = val;
                    g_Logger.log(Logger::Level::WARN, "NaiveWriter", "Overwrote DB Value to " + std::to_string(val) + " with no synchronization.");
                } else if (option == 3) {
                    ops += 2;
                    conflicts++;
                    db.active_writers++;
                    db.active_readers++;
                    UI::PrintErrorCard("DIRTY READ CONFLICT DETECTED! Unsynchronized Reader read value while Writer was modifying memory!");
                    g_Logger.log(Logger::Level::ERROR, "CollisionDemo", "DIRTY READ CONFLICT! Reader read value (" + std::to_string(db.value) + ") during active Write!");
                    db.active_writers--;
                    db.active_readers--;
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Readers-Writers";
            res.strategy_name = "Naive (No Synchronization)";
            res.is_safe_implementation = false;
            res.total_operations = ops;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = conflicts;
            res.data_integrity_passed = (conflicts == 0);
            res.notes = "User manually triggered unsynchronized dirty reads.";

            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    class DiningPhilosophersEngine {
    public:
        static void runInteractiveMode(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("MANUAL DINING PHILOSOPHERS TABLE");
            UI::PrintBreadcrumb("DASHBOARD > DINING PHILOSOPHERS ENGINE");

            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            std::vector<std::string> phil_state(N, "THINKING");

            bool exit_module = false;
            uint64_t total_meals = 0;
            uint64_t blocked_fork_pickups = 0;

            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "DINING TABLE STATUS (Total Philosophers = " << N << "):" << Color::Reset() << "\n";
                for (int i = 0; i < N; ++i) {
                    std::string st_color = (phil_state[i] == "EATING") ? Color::Green() : Color::Yellow();
                    int left_fork = i;
                    int right_fork = (i + 1) % N;

                    std::cout << "    Philosopher " << std::setw(2) << i << ": " << st_color << std::setw(10) << std::left << phil_state[i] << Color::Reset()
                              << " | Fork " << left_fork << ": " << (fork_taken[left_fork] ? Color::Red() + "BUSY" : Color::Green() + "FREE") << Color::Reset()
                              << " | Fork " << right_fork << ": " << (fork_taken[right_fork] ? Color::Red() + "BUSY" : Color::Green() + "FREE") << Color::Reset() << "\n";
                }
                std::cout << "\n";

                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Select Philosopher to Pick Up Left Fork\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Select Philosopher to Pick Up Right Fork & Start Eating\n"
                          << "  " << Color::Cyan() << "[3]" << Color::Reset() << " Select Philosopher to Release Both Forks & Return to Thinking\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Finish Session & Record Metrics\n\n";

                int option = UI::SafeReadInt("  Select Action (0-3): ", 0, 3);

                if (option >= 1 && option <= 3) {
                    int p_id = UI::SafeReadInt("  Enter Philosopher Index (0 to " + std::to_string(N - 1) + "): ", 0, N - 1);

                    int left_fork = p_id;
                    int right_fork = (p_id + 1) % N;

                    if (option == 1) {
                        UI::SpinnerAnimation("Philosopher " + std::to_string(p_id) + " reaching for Left Fork " + std::to_string(left_fork));

                        if (!fork_taken[left_fork]) {
                            fork_taken[left_fork] = true;
                            g_Logger.log(Logger::Level::INFO, "Philosopher_" + std::to_string(p_id), "Picked up Left Fork " + std::to_string(left_fork));
                        } else {
                            blocked_fork_pickups++;
                            UI::PrintErrorCard("FORK ACQUISITION CONFLICT! Left Fork " + std::to_string(left_fork) + " is already in use by neighbor!");
                            g_Logger.log(Logger::Level::WARN, "Philosopher_" + std::to_string(p_id), "BLOCKED! Left Fork " + std::to_string(left_fork) + " is busy.");
                        }
                    } else if (option == 2) {
                        UI::SpinnerAnimation("Philosopher " + std::to_string(p_id) + " reaching for Right Fork " + std::to_string(right_fork));

                        if (fork_taken[left_fork] && !fork_taken[right_fork]) {
                            fork_taken[right_fork] = true;
                            phil_state[p_id] = "EATING";
                            total_meals++;
                            g_Logger.log(Logger::Level::SUCCESS, "Philosopher_" + std::to_string(p_id), "Picked up Right Fork " + std::to_string(right_fork) + " and started EATING!");
                        } else if (!fork_taken[left_fork]) {
                            blocked_fork_pickups++;
                            UI::PrintWarningCard("ORDERING VIOLATION! Philosopher " + std::to_string(p_id) + " must pick up Left Fork before Right Fork!");
                            g_Logger.log(Logger::Level::WARN, "Philosopher_" + std::to_string(p_id), "FAILSAFE REJECTED: Left fork not held.");
                        } else {
                            blocked_fork_pickups++;
                            UI::PrintErrorCard("FORK ACQUISITION CONFLICT! Right Fork " + std::to_string(right_fork) + " is busy!");
                            g_Logger.log(Logger::Level::WARN, "Philosopher_" + std::to_string(p_id), "BLOCKED! Right Fork " + std::to_string(right_fork) + " is busy.");
                        }
                    } else if (option == 3) {
                        fork_taken[left_fork] = false;
                        fork_taken[right_fork] = false;
                        phil_state[p_id] = "THINKING";
                        g_Logger.log(Logger::Level::INFO, "Philosopher_" + std::to_string(p_id), "Released forks and returned to THINKING.");
                    }
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Dining Philosophers";
            res.strategy_name = "Manual Resource Control";
            res.is_safe_implementation = true;
            res.total_operations = total_meals;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = blocked_fork_pickups;
            res.data_integrity_passed = true;
            res.notes = "User manually controlled all fork pickups and releases.";

            g_Analyzer.recordResult(res);
        }

        static void runNaiveInteractiveDeadlockDemo(const ProblemConfig& config) {
            UI::ClearScreen();
            UI::PrintHeader("NAIVE DINING PHILOSOPHERS (MANUAL DEADLOCK DEMO)");
            UI::PrintBreadcrumb("DASHBOARD > SANDBOX > NAIVE DINING PHILOSOPHERS");

            int N = config.num_philosophers;
            std::vector<bool> fork_taken(N, false);
            int left_forks_held = 0;

            bool exit_module = false;
            Timer timer;
            timer.start();

            while (!exit_module) {
                std::cout << "\n  " << Color::Bold() << "TABLE STATE: " << Color::Reset()
                          << "Left Forks Held = " << Color::Red() << Color::Bold() << left_forks_held << "/" << N << Color::Reset() << "\n\n";

                std::cout << "  " << Color::Cyan() << "[1]" << Color::Reset() << " Force Philosopher to Pick Up Left Fork Only\n"
                          << "  " << Color::Cyan() << "[2]" << Color::Reset() << " Automatically Trigger 100% Circular Wait Deadlock\n"
                          << "  " << Color::Cyan() << "[0]" << Color::Reset() << " Reset & Exit Demo\n\n";

                int option = UI::SafeReadInt("  Select Action (0-2): ", 0, 2);

                if (option == 1) {
                    int p_id = UI::SafeReadInt("  Enter Philosopher Index (0 to " + std::to_string(N - 1) + "): ", 0, N - 1);

                    if (!fork_taken[p_id]) {
                        fork_taken[p_id] = true;
                        left_forks_held++;
                        g_Logger.log(Logger::Level::WARN, "Philosopher_" + std::to_string(p_id), "Picked up Left Fork " + std::to_string(p_id) + " and holding it.");
                    } else {
                        UI::PrintWarningCard("Fork " + std::to_string(p_id) + " is already held!");
                    }

                    if (left_forks_held == N) {
                        std::cout << "\n" << Color::BgRed() << Color::White() << Color::Bold()
                                  << "  SYSTEM DEADLOCK MANUALLY ACHIEVED! ALL " << N << " PHILOSOPHERS ARE HOLDING LEFT FORKS & WAITING FOR RIGHT FORKS!  "
                                  << Color::Reset() << "\n\n";
                    }
                } else if (option == 2) {
                    UI::SpinnerAnimation("Constructing circular wait dependency chain");
                    for (int i = 0; i < N; ++i) {
                        fork_taken[i] = true;
                        g_Logger.log(Logger::Level::WARN, "Philosopher_" + std::to_string(i), "Holding Left Fork " + std::to_string(i) + ". Waiting for Right Fork " + std::to_string((i + 1) % N) + "...");
                    }
                    left_forks_held = N;
                    std::cout << "\n" << Color::BgRed() << Color::White() << Color::Bold()
                              << "  CIRCULAR WAIT DEADLOCK COMPLETE! NO PHILOSOPHER CAN EVER EAT!  "
                              << Color::Reset() << "\n\n";
                } else if (option == 0) {
                    exit_module = true;
                }
            }

            timer.stop();

            BenchmarkResult res;
            res.problem_name = "Dining Philosophers";
            res.strategy_name = "Naive (Manual Deadlock)";
            res.is_safe_implementation = false;
            res.total_operations = left_forks_held;
            res.execution_time_ms = timer.elapsedMilliseconds();
            res.wait_contention_count = left_forks_held;
            res.data_integrity_passed = false;
            res.notes = "User manually constructed a 100% circular wait deadlock state.";

            g_Analyzer.recordResult(res);
        }
    };
}

namespace Toolkit {

    class MainMenu {
    private:
        ConfigManager config_mgr;

    public:
        void run() {
            UI::InitTerminal();
            bool exit_program = false;
            bool first_draw = true;

            while (!exit_program) {
                UI::ClearScreen();
                UI::PrintBanner(first_draw);
                UI::animate_menu = first_draw;
                first_draw = false;

                std::cout << Color::Bold() << "  MAIN CONTROL DASHBOARD (100% USER-INPUT INTERACTIVE)\n" << Color::Reset();
                std::cout << "  " << std::string(79, '-') << "\n";

                UI::PrintMenuCard("1", "Producer-Consumer Engine", "Manual step-by-step produce/consume control");
                UI::PrintMenuCard("2", "Readers-Writers Engine", "Manual database read/write input panel");
                UI::PrintMenuCard("3", "Dining Philosophers Engine", "Manual dining table fork picker");
                UI::PrintMenuCard("4", "Naive Demonstration Sandbox", "Manually trigger deadlocks & race condition errors");
                UI::PrintMenuCard("5", "Performance Analytics Report", "Display summary table of user-driven actions");
                UI::PrintMenuCard("6", "System Configuration", "Manually enter buffer capacities & philosopher counts");
                UI::PrintMenuCard("7", "Export CSV Report", "Save action history to file");
                UI::PrintMenuCard("8", "Toggle ANSI Colors", std::string("Currently: ") + (Color::enable_colors ? "[ENABLED]" : "[DISABLED]"));
                UI::PrintMenuCard("0", "Exit Studio", "Terminate application");

                std::cout << "  " << std::string(79, '-') << "\n\n";
                UI::animate_menu = false;

                int choice = UI::SafeReadInt("  Select Option (0-8): ", 0, 8);

                switch (choice) {
                    case 1:
                        ProducerConsumerEngine::runInteractiveMode(config_mgr.getConfig());
                        UI::Pause();
                        break;
                    case 2:
                        ReadersWritersEngine::runInteractiveMode();
                        UI::Pause();
                        break;
                    case 3:
                        DiningPhilosophersEngine::runInteractiveMode(config_mgr.getConfig());
                        UI::Pause();
                        break;
                    case 4:
                        UI::ClearScreen();
                        UI::PrintHeader("NAIVE UNPROTECTED DEMO SANDBOX");
                        UI::PrintBreadcrumb("DASHBOARD > DEMO SANDBOX");
                        std::cout << "  1. Naive Producer-Consumer (Manual Overflow/Underflow)\n"
                                  << "  2. Naive Readers-Writers (Manual Dirty Read Collision)\n"
                                  << "  3. Naive Dining Philosophers (Manual Circular Wait Deadlock)\n\n";
                        {
                            int naive_choice = UI::SafeReadInt("  Choice (1-3): ", 1, 3);
                            if (naive_choice == 1) ProducerConsumerEngine::runNaiveInteractiveMode(config_mgr.getConfig());
                            else if (naive_choice == 2) ReadersWritersEngine::runNaiveInteractiveMode();
                            else if (naive_choice == 3) DiningPhilosophersEngine::runNaiveInteractiveDeadlockDemo(config_mgr.getConfig());
                        }
                        UI::Pause();
                        break;
                    case 5:
                        UI::ClearScreen();
                        g_Analyzer.displaySummaryTable();
                        UI::Pause();
                        break;
                    case 6:
                        UI::ClearScreen();
                        config_mgr.configureAll();
                        UI::Pause();
                        break;
                    case 7:
                        UI::ClearScreen();
                        g_Analyzer.exportCSVReport();
                        UI::Pause();
                        break;
                    case 8:
                        Color::enable_colors = !Color::enable_colors;
                        std::cout << Color::Green() << "\n  [ANSI Color Preference Updated]\n" << Color::Reset();
                        UI::Pause();
                        break;
                    case 0:
                        exit_program = true;
                        std::cout << Color::Cyan() << "\n  ";
                        UI::TypeText("Exiting ThreadWeaver Studio. Goodbye!", 25);
                        std::cout << "\n" << Color::Reset();
                        break;
                }
            }
        }
    };
}

int main() {
    Toolkit::MainMenu menu;
    menu.run();
    return 0;
}

