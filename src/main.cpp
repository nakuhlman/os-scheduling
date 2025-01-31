#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData
{
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process *> ready_queue;
    bool all_terminated;
    int num_terminated;
    double throughput_first_half;
    int first_half_count;
    double throughput_second_half;
    int second_half_count;
    int num_processes;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);
void SJFSort(std::list<Process*> ready_queue);
void PPSort(std::list<Process*> ready_queue);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process *> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;
    //  custom
    shared_data->num_terminated = 0;
    shared_data->throughput_first_half = 0.0;
    shared_data->first_half_count = 0;
    shared_data->throughput_second_half = 0.0;
    shared_data->second_half_count = 0;


    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    shared_data->num_processes = processes.size();

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    //   - Determine if all processes are in the terminated state
    while (!(shared_data->all_terminated))
    {
        if(shared_data->num_terminated >= processes.size()) {
            std::lock_guard<std::mutex>(shared_data->mutex);
            shared_data->all_terminated = true;
        }

        // Clear output from previous iteration
        clearOutput(num_lines);

        // Do the following:
        //   - Get current time
        uint64_t curTime = currentTime();
        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time) 
        for(int i = 0; i < processes.size(); i++) {
            Process* p = processes.at(i);
            if(p->getState() == Process::State::NotStarted && p->getStartTime() <= curTime - start) {
                // accessing shared data, lock mutex
                std::lock_guard<std::mutex>(shared_data->mutex);
                // if so put that process in the ready queue
                shared_data->ready_queue.push_back(p);
                // set the process state to Ready
                p->setState(Process::State::Ready, curTime);
            }
            //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
            if (p->getState() == Process::State::IO && curTime - p->getLastStateTime() >= p->getBurstTime(p->getCurrentBurst())) {
                p->nextBurst();
                p->setState(Process::State::Ready, curTime);
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(p);
                }
            }

            //   - *Check if any running process need to be interrupted (RR time slice expires) or newly ready process has higher priority)
            switch(shared_data->algorithm) {
                case RR:
                    // Round Robin Interrupt
                    if(p->getBurstTime(p->getCurrentBurst()) >= shared_data->time_slice) {
                        std::lock_guard<std::mutex>(shared_data->mutex);
                        p->interrupt();
                    }
                    break;
                case PP:
                    // Priority Scheduling
                    std::lock_guard<std::mutex>(shared_data->mutex);
                    for(const auto& Process : shared_data->ready_queue) {
                        // If a ready process has a higher priority then the current process..
                        if(Process->getPriority() > p->getPriority()) {
                            // signal an interrupt
                            p->interrupt();
                        }
                    }
                    break;
            }
        }
        
        // accessing shared data, lock mutex
        std::lock_guard<std::mutex>(shared_data->mutex);
        //   - *Sort the ready queue (if needed - based on scheduling algorithm)
        switch(shared_data->algorithm) {

            case SJF:
            // call SJFSort method
            SJFSort(shared_data->ready_queue);
            break;

            case PP:
            // call PPSort method
            PPSort(shared_data->ready_queue);
            break;

            default:
            break;
        }


        
        //   NOTE: * = accesses shared data (ready queue), so be sure to use proper synchronization

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000);
    }

    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    //  - CPU utilization (done)
    //  - Throughput
    //     - Average for first 50% of processes finished
    //     - Average for second 50% of processes finished
    //     - Overall average
    //  - Average turnaround time (done)
    //  - Average waiting time (done)

    double total_idle_probability = 1.0;
    double average_turnaround_time = 0.0;
    double average_waiting_time = 0.0;

    int num_processes = processes.size();
    for(int i = 0; i < num_processes; i++) {
        Process* p = processes.at(i);
        total_idle_probability *= (1.0 - (p->getCpuTime()/p->getTurnaroundTime()));
        average_turnaround_time += p->getTurnaroundTime();
        average_waiting_time += p->getWaitTime();
    }

    double cpu_util = (1.0 - total_idle_probability) * 100.0;
    double throughput_first_half = shared_data->throughput_first_half / shared_data->first_half_count;
    double throughput_second_half = shared_data->throughput_second_half / shared_data->second_half_count;
    double throughput_overall_average = (shared_data->throughput_first_half + shared_data->throughput_second_half) / (shared_data->first_half_count + shared_data->second_half_count);
    average_turnaround_time /= (double)num_processes;
    average_waiting_time /= (double)num_processes;

    printf("CPU Utilization: %.1f%%\n", cpu_util);
    printf("Throughput:\n");
    printf(" - Average for first 50%% of processes: %.3f\n", throughput_first_half);
    printf(" - Average for second 50%% of processes: %.3f\n", throughput_second_half);
    printf(" - Overall Average: %.3f\n", throughput_overall_average);

    printf("Average Turnaround Time: %.3f\n", average_turnaround_time);
    printf("Average Waiting Time: %.3f\n", average_waiting_time);

    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    while (!shared_data->all_terminated)
    {
        //   - *Get process at front of ready queue
        Process *p = NULL;
        {
            std::lock_guard<std::mutex>(shared_data->mutex);
            if (shared_data->ready_queue.size() > 0)
            {
                p = shared_data->ready_queue.front();
                shared_data->ready_queue.pop_front();
            }
        }
        //   - Simulate the processes running until one of the following:
        //     - CPU burst time has elapsed
        //     - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
        if (p != NULL)
        {
            p->setCpuCore(core_id);
            p->setState(Process::State::Running, currentTime());
            p->setBurstStartTime(currentTime());
            bool running = true;
            while (running)
            {
                p->updateProcess(currentTime());
                // currently running on cpu
                if (p->isInterrupted() || p->getBurstTime(p->getCurrentBurst()) <= 0)
                {
                    running = false;
                }
            }
            p->updateProcess(currentTime());
            //  - Place the process back in the appropriate queue
            //     - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
            //     - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
            //     - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)

            p->setCpuCore(-1);
            if (p->getRemainingTime() <= 0.0 || p->getCurrentBurst() >= p->getNumBursts())
            { // Terminated
                p->setState(Process::State::Terminated, currentTime());
                std::lock_guard<std::mutex>(shared_data->mutex);
                shared_data->num_terminated += 1;
                // when a process finishes, add its turnaround time to the first half or second half turnaround time counters
                if(((double)shared_data->num_terminated / shared_data->num_processes) <= 0.5) {
                    shared_data->throughput_first_half += p->getTurnaroundTime();
                    shared_data->first_half_count += 1;
                } else {
                    shared_data->throughput_second_half += p->getTurnaroundTime();
                    shared_data->second_half_count += 1;
                }
            }
            else if (p->isInterrupted())
            { // Interrupt
                p->interruptHandled();
                p->setState(Process::State::Ready, currentTime());
                std::lock_guard<std::mutex>(shared_data->mutex);
                shared_data->ready_queue.push_back(p);
            }
            else
            { // IO
                p->nextBurst();
                p->setState(Process::State::IO, currentTime());
                p->setBurstStartTime(currentTime());
            }
            p->interruptHandled();
            //  - Wait context switching time
            usleep(shared_data->context_switch);
        }
    }
    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
}

int printProcessOutput(std::vector<Process *> &processes, std::mutex &mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n",
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time,
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
    case Process::State::NotStarted:
        str = "not started";
        break;
    case Process::State::Ready:
        str = "ready";
        break;
    case Process::State::Running:
        str = "running";
        break;
    case Process::State::IO:
        str = "i/o";
        break;
    case Process::State::Terminated:
        str = "terminated";
        break;
    default:
        str = "unknown";
        break;
    }
    return str;
}

// Sorts the ready queue using Shortest Job First, SjfComparator
void SJFSort(std::list<Process*> ready_queue) {
   ready_queue.sort(SjfComparator());
}

// Sorts the ready queue using PPSort, PPComparator
void PPSort(std::list<Process*> ready_queue) {
   ready_queue.sort(PpComparator());
}

