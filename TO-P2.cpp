#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <random>
#include <chrono>
#include <limits>
#include <condition_variable>

struct InstanceStats {
    int id;
    std::atomic<bool> active;
    std::atomic<int> partiesServed;
    std::atomic<int> totalTime; 

    std::mutex runMutex;
    std::chrono::steady_clock::time_point currentRunStart;
    int currentRunDuration; 

    InstanceStats(int id_)
        : id(id_), active(false), partiesServed(0), totalTime(0),
          currentRunStart(std::chrono::steady_clock::now()),
          currentRunDuration(0) {}
};

// player queue
int g_tanks = 0;
int g_heals = 0;
int g_dps   = 0;

int g_t1 = 1; 
int g_t2 = 15; 


std::mutex g_queueMutex;             
std::condition_variable g_cvPlayers; 

std::mutex g_coutMutex;              
std::atomic<bool> g_recruiting(false);   
std::atomic<bool> g_doneAll(false);     
std::atomic<int> g_bonusGenerations(10);

int readPositiveInt(const std::string &prompt) {
    int value;
    while (true) {
        std::cout << prompt;
        if (!(std::cin >> value) || value <= 0) {
            std::cout << "Invalid input. Please enter a positive whole number.\n";
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            return value;
        }
    }
}

char readYesNo(const std::string &prompt) {
    char c;
    while (true) {
        std::cout << prompt;
        if (!(std::cin >> c)) {
            std::cout << "Invalid input. Please enter y/Y or n/N.\n";
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        if (c == 'y' || c == 'Y' || c == 'n' || c == 'N') {
            return c;
        }
        std::cout << "Invalid input. Please enter y/Y or n/N.\n";
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
}


bool canFormPartyLocked() {
    return (g_tanks >= 1 && g_heals >= 1 && g_dps >= 3);
}

void dungeonWorker(InstanceStats *stats, unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> durationDist(g_t1, g_t2);

    while (true) {
        int runDuration = 0;

        {
            std::unique_lock<std::mutex> lock(g_queueMutex);

            g_cvPlayers.wait(lock, [] {
                return canFormPartyLocked() || !g_recruiting.load();
            });

            if (!canFormPartyLocked()) {
                break;
            }

            g_tanks -= 1;
            g_heals -= 1;
            g_dps   -= 3;

            runDuration = durationDist(rng);

            {
                std::lock_guard<std::mutex> lock(stats->runMutex);
                stats->currentRunStart    = std::chrono::steady_clock::now();
                stats->currentRunDuration = runDuration;
            }

            stats->active.store(true);
            stats->partiesServed.fetch_add(1);
            stats->totalTime.fetch_add(runDuration);

            {
                std::lock_guard<std::mutex> outLock(g_coutMutex);
                std::cout << "[Instance " << stats->id << "] starting run. "
                          << "Queue left -> Tanks: " << g_tanks
                          << ", Heals: " << g_heals
                          << ", DPS: " << g_dps << '\n';
            }
        } // release g_queueMutex

        {
            std::lock_guard<std::mutex> outLock(g_coutMutex);
            std::cout << "[Instance " << stats->id << "] ACTIVE for "
                      << runDuration << " seconds" << '\n';
        }

        std::this_thread::sleep_for(std::chrono::seconds(runDuration));

        stats->active.store(false);

        {
            std::lock_guard<std::mutex> outLock(g_coutMutex);
            std::cout << "[Instance " << stats->id << "] finished a run." << '\n';
        }
    }

    {
        std::lock_guard<std::mutex> outLock(g_coutMutex);
        // std::cout << "[Instance " << stats->id
        //           << "] shutting down - no more parties." << '\n';
    }
}

// Bonus random player generator
void playerGenerator() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> sleepDist(1, 3);   // seconds between arrivals
    std::uniform_int_distribution<int> roleDist(1, 3);    

    int generations = g_bonusGenerations.load();
    for (int i = 0; i < generations; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(sleepDist(rng)));

        int addT = 0, addH = 0, addD = 0;
        int choice = roleDist(rng);
        if (choice == 1) {
            addT = 1;
        } else if (choice == 2) {
            addH = 1; 
        } else {
            addD = 1; 
        }

        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_tanks += addT;
            g_heals += addH;
            g_dps   += addD;

            {
                std::lock_guard<std::mutex> outLock(g_coutMutex);
                std::cout << "[Generator] Added -> +" << addT << "T, +" << addH
                        << "H, +" << addD << "D. Now in queue: T=" << g_tanks
                        << ", H=" << g_heals << ", D=" << g_dps << '\n';
            }
        }

        g_cvPlayers.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_recruiting.store(false);
    }
    g_cvPlayers.notify_all();
}

void statusMonitor(const std::vector<InstanceStats*> &instances) {
    using namespace std::chrono_literals;
    while (!g_doneAll.load()) {
        {
            std::lock_guard<std::mutex> outLock(g_coutMutex);
            std::cout << "\n=== CURRENT INSTANCE STATUS ===\n";
            for (auto *inst : instances) { 
                bool isActive   = inst->active.load();
                int parties     = inst->partiesServed.load();
                int totalTime   = inst->totalTime.load();

                int elapsedThisRun = 0;
                int runDuration    = 0;

                if (isActive) {
                    auto now = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lock(inst->runMutex);
                    runDuration = inst->currentRunDuration;

                    if (runDuration > 0) {
                        auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                            now - inst->currentRunStart
                        ).count();
                        if (diff < 0) diff = 0;
                        if (diff > runDuration) diff = runDuration;
                        elapsedThisRun = static_cast<int>(diff);
                    }
                }

                if (isActive) {
                    
                    std::cout << "Instance " << inst->id << ": "
                            << "active"
                            << " | parties served: " << parties
                            << " | time: " << elapsedThisRun << "/" << runDuration << "s\n";
                } else {
                    
                    std::cout << "Instance " << inst->id << ": "
                            << "empty"
                            << " | parties served: " << parties
                            << " | time: 0/0s" << "\n";
                            // << " (total: " << totalTime << "s)\n";
                }
            }

            std::cout << "================================\n" << std::endl;
        }
        std::this_thread::sleep_for(1s);
    }
}

int main() {
    int n;  
    int t;  
    int h;  
    int d;  

    n = readPositiveInt("Enter maximum number of concurrent instances (n): ");
    t = readPositiveInt("Enter number of tank players in the queue (t): ");
    h = readPositiveInt("Enter number of healer players in the queue (h): ");
    d = readPositiveInt("Enter number of DPS players in the queue (d): ");

    g_t1 = readPositiveInt("Enter minimum dungeon clear time in seconds (t1): ");

    while (true) {
        g_t2 = readPositiveInt("Enter maximum dungeon clear time in seconds (t2, <= 15): ");
        if (g_t2 <= 15) break;
        std::cout << "t2 must be less than or equal to 15. Please try again.\n";
    }

    if (g_t1 > g_t2) {
        std::swap(g_t1, g_t2);
    }

    g_tanks = t;
    g_heals = h;
    g_dps   = d;

    char enableBonus = readYesNo("Enable bonus random player generator thread? (y/n): ");

    bool useBonus = (enableBonus == 'y' || enableBonus == 'Y');
    g_recruiting.store(useBonus);

    if (useBonus) {
        int userGenerations = readPositiveInt("How many additional random players to add? ");
        g_bonusGenerations.store(userGenerations);
    }

    std::vector<InstanceStats*> instances;
    instances.reserve(n);
    for (int i = 0; i < n; ++i) {
        instances.push_back(new InstanceStats(i + 1));
    }

    std::vector<std::thread> workers;
    workers.reserve(n);
    for (int i = 0; i < n; ++i) {
        unsigned int seed = static_cast<unsigned int>(std::random_device{}()) + i;
        workers.emplace_back(dungeonWorker, instances[i], seed);
    }

    // Start random player generator
    std::thread generatorThread;
    if (useBonus) {
        generatorThread = std::thread(playerGenerator);
    } else {
        
        g_cvPlayers.notify_all();
    }

    std::thread monitorThread(statusMonitor, std::cref(instances));

    for (auto &th : workers) {
        if (th.joinable()) th.join();
    }


    if (useBonus && generatorThread.joinable()) {
        generatorThread.join();
    }

    g_doneAll.store(true);
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    int totalParties = 0;
    int totalTime    = 0;

    std::cout << "\n===== FINAL SUMMARY =====\n";
    for (const auto *inst : instances) {
        int p = inst->partiesServed.load();
        int tm = inst->totalTime.load();
        totalParties += p;
        totalTime    += tm;
        std::cout << "Instance " << inst->id
                  << " served " << p << " parties, total time served: "
                  << tm << " seconds" << '\n';
    }

    std::cout << "--------------------------\n";
    std::cout << "Total parties served by all instances: " << totalParties << '\n';
    std::cout << "Total time served by all instances: " << totalTime << " seconds" << '\n';
    std::cout << "Remaining in queue -> Tanks: " << g_tanks
              << ", Heals: " << g_heals
              << ", DPS: " << g_dps << '\n';
    std::cout << "==========================\n";

    for (auto *inst : instances) {
        delete inst;
    }

    return 0;
}
