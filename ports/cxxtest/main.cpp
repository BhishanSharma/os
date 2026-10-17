// cxxtest: checks the mlibc + libstdc++ toolchain on Terminal OS.
// Each line prints "ok" or "FAILED"; the exit code is the number of failures.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int failures;

static void check(const char *what, bool ok) {
    std::cout << (ok ? "ok      " : "FAILED  ") << what << std::endl;
    if (!ok) failures++;
}

thread_local int per_thread = 7;
static int constructed;
struct Global {
    Global() { constructed = 42; }
};
static Global global;

int main(int argc, char **argv) {
    std::cout << "cxxtest: mlibc + libstdc++ on Terminal OS" << std::endl;
    check("static constructors ran", constructed == 42);
    check("argv", argc >= 1 && argv[0] && argv[argc] == nullptr);

    std::string s = "hello";
    s += ", world";
    check("std::string", s == "hello, world" && s.size() == 12);

    std::vector<int> v{5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    check("std::vector + std::sort", v == std::vector<int>{1, 3, 5, 9});

    std::map<std::string, int> m{{"b", 2}, {"a", 1}};
    check("std::map", m.begin()->first == "a" && m["b"] == 2);

    std::ostringstream os;
    os << 3.25 << ' ' << 0x1F << ' ' << std::hex << 255;
    check("iostream formatting (floats)", os.str() == "3.25 31 ff");

    bool caught = false;
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        caught = std::strcmp(e.what(), "boom") == 0;
    }
    check("exceptions", caught);

    auto p = std::make_unique<std::vector<std::string>>(1000, "x");
    check("new/delete (1000 strings)", p->size() == 1000 && (*p)[999] == "x");

    per_thread = 1;
    std::atomic<int> sum{0};
    int seen_in_thread = -1;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back([&sum, &seen_in_thread, i] {
            if (i == 0) seen_in_thread = per_thread;   // a fresh copy: 7
            for (int k = 0; k < 1000; k++) sum++;
        });
    for (auto &t : threads) t.join();
    check("std::thread x4 + atomics", sum == 4000);
    check("thread_local", per_thread == 1 && seen_in_thread == 7);

    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(mu);
        ready = true;
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return ready; });
    }
    waker.join();
    check("mutex + condition_variable", ready);

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check("steady_clock + sleep_for (100 ms)", ms >= 95 && ms < 400);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    check("system_clock (after 2024)", now > 1704067200);

    {
        std::ofstream out("CXXTEST.TXT");
        out << "line one\nline two\n";
    }
    std::ifstream in("CXXTEST.TXT");
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    check("fstream write + read", l1 == "line one" && l2 == "line two");
    struct stat st;
    check("stat", stat("CXXTEST.TXT", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 18);
    unlink("CXXTEST.TXT");

    int entries = 0;
    if (DIR *d = opendir("/")) {
        while (readdir(d)) entries++;
        closedir(d);
    }
    check("opendir/readdir", entries > 0);

    void *a = mmap(nullptr, 64 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool mm = a != MAP_FAILED;
    if (mm) {
        std::memset(a, 0xAB, 64 * 4096);
        mm = munmap((char *)a + 16 * 4096, 16 * 4096) == 0 &&   // a hole in the middle
             ((unsigned char *)a)[63 * 4096] == 0xAB && munmap(a, 64 * 4096) == 0;
    }
    check("mmap + partial munmap", mm);

    struct utsname u;
    check("uname", uname(&u) == 0 && std::strcmp(u.machine, "x86_64") == 0);
    check("hardware_concurrency", std::thread::hardware_concurrency() >= 1);
    std::printf("cpus: %u, system: %s %s\n", std::thread::hardware_concurrency(), u.sysname, u.release);

    std::cout << (failures ? "cxxtest: FAILED " : "cxxtest: all passed") << (failures ? std::to_string(failures) : "")
              << std::endl;
    return failures;
}
