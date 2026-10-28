// Game-thread CPU log and core placement (lane AJ). See cpu_monitor.h.
#include "cpu_monitor.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include "gx_thread.h"
#include "runtime_log.h"
#include <aurora/aurora.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

// aurora.cpp (lane AK): the renderer-lock waits and the frame worker's holds since the previous call, per game frame.
extern "C" size_t aurora_lock_stats_line(char* out, size_t size, uint64_t frames, double seconds);

namespace cpumon {
namespace {

std::atomic<uint64_t> g_slots[kSlotCount];
std::atomic<uint64_t> g_frames{0};
constexpr int kMaxCpus = 16;
std::atomic<uint32_t> g_onCpu[kMaxCpus];
std::atomic<pid_t> g_gameTid{0};

struct Config {
    bool log = false;
    int core = -1;          // -1: no pinning; -2: auto
    bool exclusive = true;
};

const Config& Cfg() {
    static const Config cfg = [] {
        Config c;
        const char* log = std::getenv("KARTFRAME_CPULOG");
        c.log = log != nullptr && log[0] == '1';
        if (const char* core = std::getenv("KARTFRAME_GAME_CORE"); core != nullptr && *core != '\0') {
            std::string v(core);
            if (const auto colon = v.find(':'); colon != std::string::npos) {
                c.exclusive = v.substr(colon + 1) != "shared";
                v = v.substr(0, colon);
            }
            if (v == "auto") c.core = -2;
            else if (v != "off" && !v.empty() && v.find_first_not_of("0123456789") == std::string::npos)
                c.core = std::atoi(v.c_str());
        }
        return c;
    }();
    return cfg;
}

bool ReadLine(const std::string& path, char* buf, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return false;
    const bool ok = std::fgets(buf, static_cast<int>(size), f) != nullptr;
    std::fclose(f);
    return ok;
}

long ReadLong(const std::string& path) {
    char buf[64];
    return ReadLine(path, buf, sizeof(buf)) ? std::strtol(buf, nullptr, 10) : -1;
}

struct TaskSample {
    uint64_t run = 0, wait = 0;
    int cpu = -1;
    std::string comm;
};

bool ReadTask(pid_t tid, TaskSample& out) {
    char buf[512];
    const std::string base = "/proc/self/task/" + std::to_string(tid);
    if (!ReadLine(base + "/schedstat", buf, sizeof(buf))) return false;
    unsigned long long run = 0, wait = 0;
    if (std::sscanf(buf, "%llu %llu", &run, &wait) != 2) return false;
    out.run = run;
    out.wait = wait;
    if (ReadLine(base + "/stat", buf, sizeof(buf))) {
        // comm may hold spaces; the fields after the last ')' start with state (field 3); processor is field 39.
        if (const char* close = std::strrchr(buf, ')')) {
            const char* open = std::strchr(buf, '(');
            if (open != nullptr && open < close) out.comm.assign(open + 1, close);
            int field = 2;
            for (const char* p = close + 1; *p != '\0'; ++p) {
                if (*p == ' ') {
                    ++field;
                    if (field == 39) {
                        out.cpu = std::atoi(p + 1);
                        break;
                    }
                }
            }
        }
    }
    for (char& c : out.comm) if (c == ' ') c = '_';
    return true;
}

std::vector<pid_t> ListTasks() {
    std::vector<pid_t> tids;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids.push_back(static_cast<pid_t>(std::atoi(e->d_name)));
        }
        closedir(dir);
    }
    return tids;
}

struct Zone {
    std::string temp, type;
};

std::vector<Zone> ListZones() {
    std::vector<Zone> zones;
    if (DIR* dir = opendir("/sys/class/thermal")) {
        while (dirent* e = readdir(dir)) {
            if (std::strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            const std::string base = std::string("/sys/class/thermal/") + e->d_name;
            char type[64] = "?";
            if (ReadLine(base + "/type", type, sizeof(type))) type[std::strcspn(type, "\n")] = '\0';
            zones.push_back({base + "/temp", type});
        }
        closedir(dir);
    }
    return zones;
}

int PickAutoCore(const cpu_set_t& allowed) {
    int best = -1;
    long bestCap = -1, bestFreq = -1;
    for (int c = 0; c < kMaxCpus; ++c) {
        if (!CPU_ISSET(c, &allowed)) continue;
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c);
        const long cap = ReadLong(base + "/cpu_capacity");
        const long freq = ReadLong(base + "/cpufreq/cpuinfo_max_freq");
        if (cap > bestCap || (cap == bestCap && freq > bestFreq)) {
            best = c;
            bestCap = cap;
            bestFreq = freq;
        }
    }
    return best;
}

void MonitorLoop(pid_t gameTid) {
    const Config& cfg = Cfg();
    const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    sched_getaffinity(0, sizeof(allowed), &allowed);
    int core = cfg.core == -2 ? PickAutoCore(allowed) : cfg.core;
    if (core >= 0 && (core >= kMaxCpus || !CPU_ISSET(core, &allowed))) {
        RT_LOG(RT_TAG_RUNTIME) << "game core: KARTFRAME_GAME_CORE=" << core << " is not an allowed core; not pinning"
                               << std::endl;
        core = -1;
    }
    cpu_set_t others = allowed;
    if (core >= 0) {
        CPU_CLR(core, &others);
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(core, &one);
        const bool ok = sched_setaffinity(gameTid, sizeof(one), &one) == 0;
        RT_LOG(RT_TAG_RUNTIME) << "game core: game thread (tid " << gameTid << ") pinned to core " << core
                               << (ok ? "" : " FAILED") << ", capacity "
                               << ReadLong("/sys/devices/system/cpu/cpu" + std::to_string(core) + "/cpu_capacity")
                               << (cfg.exclusive ? "; every other thread kept off it" : " (shared)") << std::endl;
        if (cfg.exclusive) sched_setaffinity(0, sizeof(others), &others);
    }
    const std::vector<Zone> zones = cfg.log ? ListZones() : std::vector<Zone>{};
    std::map<pid_t, TaskSample> last;
    uint64_t lastSlots[kSlotCount]{};
    uint64_t lastFrames = 0;
    auto lastTime = std::chrono::steady_clock::now();
    uint32_t moved = 0;
    for (uint32_t tick = 0;; ++tick) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::vector<pid_t> tids = ListTasks();
        if (core >= 0 && cfg.exclusive) {
            // Threads started since the last pass inherit their creator's mask; move any that may use the core.
            for (pid_t tid : tids) {
                if (tid == gameTid || tid == self) continue;
                cpu_set_t mask;
                if (sched_getaffinity(tid, sizeof(mask), &mask) != 0 || !CPU_ISSET(core, &mask)) continue;
                cpu_set_t next;
                CPU_AND(&next, &mask, &others);
                if (CPU_COUNT(&next) == 0) next = others;
                if (sched_setaffinity(tid, sizeof(next), &next) == 0) ++moved;
            }
            if (tick < 3 || tick % 60 == 0) {
                RT_LOG(RT_TAG_RUNTIME) << "game core: " << moved << " thread placements moved off core " << core
                                       << " so far" << std::endl;
            }
        }
        if (!cfg.log) continue;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
        const uint64_t frames = g_frames.load(std::memory_order_relaxed);
        const uint64_t df = frames - lastFrames;
        lastFrames = frames;
        const double perFrame = df != 0 ? 1.0 / static_cast<double>(df) : 0.0;
        std::map<pid_t, TaskSample> cur;
        for (pid_t tid : tids) {
            TaskSample s;
            if (ReadTask(tid, s)) cur[tid] = s;
        }
        const auto delta = [&](pid_t tid, uint64_t& run, uint64_t& wait) {
            const auto c = cur.find(tid);
            const auto l = last.find(tid);
            run = wait = 0;
            if (c == cur.end() || l == last.end()) return false;
            run = c->second.run - l->second.run;
            wait = c->second.wait - l->second.wait;
            return true;
        };
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(2);
        uint64_t run = 0, wait = 0;
        delta(gameTid, run, wait);
        uint64_t slots[kSlotCount];
        for (uint32_t i = 0; i < kSlotCount; ++i) {
            const uint64_t v = g_slots[i].load(std::memory_order_relaxed);
            slots[i] = v - lastSlots[i];
            lastSlots[i] = v;
        }
        const double wallMs = df != 0 ? dt * 1e3 * perFrame : 0.0;
        const double cpuMs = run / 1e6 * perFrame, runqMs = wait / 1e6 * perFrame, paceMs = slots[kPace] / 1e6 * perFrame;
        out << "cpulog: fps " << std::setprecision(1) << df / dt << std::setprecision(2) << " | game ms/frame: wall "
            << wallMs << " cpu " << cpuMs << " runq " << runqMs << " pace " << paceMs << " other_off "
            << std::max(0.0, wallMs - cpuMs - runqMs - paceMs) << " | hooks ms/frame: commit "
            << slots[kVrCommit] / 1e6 * perFrame << " update " << slots[kVrUpdate] / 1e6 * perFrame << " ui "
            << slots[kUiHook] / 1e6 * perFrame << " overlay " << slots[kOverlay] / 1e6 * perFrame << " events "
            << slots[kEvents] / 1e6 * perFrame << " gxsync " << slots[kGxSync] / 1e6 * perFrame << " | game on core";
        uint32_t onCpuTotal = 0, onCpu[kMaxCpus];
        for (int c = 0; c < kMaxCpus; ++c) onCpuTotal += onCpu[c] = g_onCpu[c].exchange(0, std::memory_order_relaxed);
        for (int c = 0; c < kMaxCpus; ++c) {
            if (onCpu[c] != 0) out << ' ' << c << ':' << (onCpu[c] * 100 + onCpuTotal / 2) / std::max(onCpuTotal, 1u) << '%';
        }
        // The labelled threads, then the busiest others: on-CPU/run-queue ms a second @ last core.
        out << " | threads ms/s run/wait@core:";
        const uint32_t gx = GxThread::NativeThreadId();
        const uint32_t worker = aurora_get_frame_worker_native_thread_id();
        const auto put = [&](const char* label, pid_t tid) {
            uint64_t r = 0, w = 0;
            if (!delta(tid, r, w)) return;
            out << ' ' << label << ' ' << static_cast<int>(r / 1e6 / dt) << '/' << static_cast<int>(w / 1e6 / dt) << '@'
                << cur[tid].cpu;
        };
        put("game", gameTid);
        if (gx > 1) put("gx", static_cast<pid_t>(gx));
        if (worker != 0) put("worker", static_cast<pid_t>(worker));
        std::vector<std::pair<uint64_t, pid_t>> busy;
        for (const auto& [tid, s] : cur) {
            if (tid == gameTid || tid == static_cast<pid_t>(gx) || tid == static_cast<pid_t>(worker)) continue;
            uint64_t r = 0, w = 0;
            if (delta(tid, r, w)) busy.push_back({r, tid});
        }
        std::sort(busy.rbegin(), busy.rend());
        for (size_t i = 0; i < busy.size() && i < 4; ++i) {
            const std::string label = cur[busy[i].second].comm + "#" + std::to_string(busy[i].second);
            put(label.c_str(), busy[i].second);
        }
        out << " | MHz";
        for (int c = 0; c < kMaxCpus; ++c) {
            if (!CPU_ISSET(c, &allowed)) continue;
            const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/";
            const long f = ReadLong(base + "scaling_cur_freq");
            const long cap = ReadLong(base + "scaling_max_freq");
            const long top = ReadLong(base + "cpuinfo_max_freq");
            out << ' ' << c << ':' << f / 1000;
            if (cap > 0 && top > 0 && cap < top) out << "(cap " << cap / 1000 << ')';
        }
        long hot = -1000000;
        const Zone* hotZone = nullptr;
        for (const Zone& z : zones) {
            const long t = ReadLong(z.temp);
            if (t > hot) {
                hot = t;
                hotZone = &z;
            }
        }
        if (hotZone != nullptr) out << " | hot " << std::setprecision(1) << hot / 1000.0 << " C " << hotZone->type;
        {
            // Appended last so the fields before keep their places for the existing readers.
            char lockLine[640];
            if (aurora_lock_stats_line(lockLine, sizeof(lockLine), df, dt) != 0) out << lockLine;
        }
        RT_LOG(RT_TAG_RUNTIME) << out.str() << std::endl;
        last.swap(cur);
    }
}

} // namespace

bool Timing() noexcept { return Cfg().log; }

void Add(Slot slot, uint64_t ns) noexcept { g_slots[slot].fetch_add(ns, std::memory_order_relaxed); }

uint64_t NowNs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void GameFrame() noexcept {
    const Config& cfg = Cfg();
    if (!cfg.log && cfg.core == -1) return;
    if (g_gameTid.load(std::memory_order_relaxed) == 0) {
        const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
        g_gameTid.store(tid, std::memory_order_relaxed);
        try {
            std::thread(MonitorLoop, tid).detach();
            RT_LOG(RT_TAG_RUNTIME) << "cpulog: monitor started (KARTFRAME_CPULOG=" << (cfg.log ? 1 : 0)
                                   << ", KARTFRAME_GAME_CORE " << (cfg.core == -1 ? "off" : cfg.core == -2 ? "auto" : std::to_string(cfg.core))
                                   << (cfg.core != -1 && !cfg.exclusive ? ":shared" : "") << ")" << std::endl;
        } catch (...) {
        }
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (cfg.log) {
        const int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < kMaxCpus) g_onCpu[cpu].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace cpumon

#else

namespace cpumon {
bool Timing() noexcept { return false; }
void Add(Slot, uint64_t) noexcept {}
uint64_t NowNs() noexcept { return 0; }
void GameFrame() noexcept {}
} // namespace cpumon

#endif
