#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
    }
    is_interrupted = false;
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    remain_time = 0;
    last_state_time = current_time;
    last_updated_time = current_time;
    for (i = 0; i < num_bursts; i += 2)
    {
        remain_time += burst_times[i];
    }
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready)
    {
        launch_time = current_time;
    }

    if (state == State::Ready && new_state == State::Running)
    {
        wait_time += current_time - last_state_time;
    }

    state = new_state;
    last_state_time = current_time;
    last_updated_time = current_time;
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // set turn time
    turn_time = current_time - launch_time;

    // update burst and cpu times
    if(state == State::Running){
        uint64_t delta_time = current_time - last_updated_time;
        cpu_time += delta_time;
        burst_times[current_burst] -= delta_time;
        if(burst_times[current_burst] < 0) burst_times[current_burst] = 0;
    }

    // recalculate remain time
    int new_remain_time = 0;
    for (uint8_t i = 0; i < num_bursts; i += 2)
    {
        new_remain_time += burst_times[i];
    }
    remain_time = new_remain_time;

    last_updated_time = current_time;
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    burst_times[burst_idx] = new_time;
}

uint16_t Process::getCurrentBurst() const
{
    return current_burst;
}

void Process::nextBurst()
{
    current_burst += 1;
}

uint16_t Process::getNumBursts() 
{
    return num_bursts;
} 

uint64_t Process::getLastStateTime() 
{
    return last_state_time;
}

uint32_t Process::getBurstTime(int burst_idx) const 
{
    return burst_times[burst_idx];
}

// Comparator methods: used in std::list sort() method
// No comparator needed for FCFS or RR (ready queue never sorted)

// SJF - comparator for sorting read queue based on shortest remaining CPU time
bool SjfComparator::operator()(const Process *p1, const Process *p2)
{
    // Return true if p1's current burst is less than or equal to p2's current burst
    return (p1->getBurstTime(p1->getCurrentBurst()) <= p2->getBurstTime(p2->getCurrentBurst()));
}

// PP - comparator for sorting read queue based on priority
bool PpComparator::operator()(const Process *p1, const Process *p2)
{
    if(p1->getPriority() > p2->getPriority()) {
        return true;
    } else {
        return false;
    }
}
