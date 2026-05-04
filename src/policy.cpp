#include <iostream>
#include <string>
#include <vector>
#include "policy.h"
#include "schedule.h"

using namespace local;

int starvationThreshold = 500; // adjustable starvation threshold

namespace local { namespace policy { bool csvMode = false; } }

void policy::Trace::addEvent(policy::Event e)
{
    policy::Trace t = *this;
    t.trace.push_back(e);
    *this = t;
    return;
}

policy::Event policy::Trace::getEvent(int i)
{
    return this->trace.at(i);
}

policy::Event policy::Trace::getLastOccured(int id) // get last event tied to ID
{
    for(std::vector<policy::Event>::reverse_iterator it = this->trace.rbegin(); it < this->trace.rend(); it++)
    {
        if((*it).getID() == id)
        {
            return *it;
        }
    }

    exit(3);
}

policy::Event policy::Trace::getFirstOccured(int id) // get first event tied to id
{
    for(std::vector<policy::Event>::iterator it = this->trace.begin(); it < this->trace.end(); it++)
    {
        if((*it).getID() == id)
        {
            return *it;
        }
    }

    exit(3);
}

float policy::Trace::fairnessEvent(int id)
{ // spits out the unfairness metric based on what job's end we're looking for
    std::vector<policy::Event>::reverse_iterator idEndIt;
    std::vector<policy::Event>::iterator nextEndIt;

    for(idEndIt = this->trace.rbegin(); idEndIt != this->trace.rend(); idEndIt++)
    {
        if((*idEndIt).getID() == id)
        {
            nextEndIt = idEndIt.base();
            break;
        }
    }

    if(nextEndIt == this->trace.end())
    { // if idEndIt is the final event
        return 1;
    }

    for(; nextEndIt != this->trace.end(); nextEndIt++)
    { // find the ending after
        if((*nextEndIt).getID() != -1)
        {
            if((*nextEndIt) == (this->getLastOccured((*nextEndIt).getID())))
            {
                break;
            }
        }
    }

    return ((float) (*idEndIt).getEnd()) / ((float) (*nextEndIt).getEnd());
}

std::string toString(policy::Event e)
{
    std::string s = "(id - " + std::to_string(e.getID()) + " , start - " + std::to_string(e.getStart());
    s += ", end - " + std::to_string(e.getEnd()) + ")";
    return s;
}

std::string toString(policy::Trace trace)
{
    std::string s;
    for(policy::Event e : trace.trace)
    {
        s += toString(e) + "\n";
    }
    return s;
}

std::string toString(policy::Policy s)
{
    std::string str;
    str += s.name + "\n";
    str += toString(s.trace) + "\n";
    return str;
}

void policy::Policy::printTraceAnalysis()
{
    // in csv mode we still emit the trace events (python needs them for
    // gantt charts) but skip the human-readable schedule dump
    std::string str = toString(*this);
    std::cout << str << std::endl;

    if (!policy::csvMode) this->trace.s.printSchedule();

    //tests past here
    int avgTurn = 0;        // average turnabout time
    int maxTurn = 0;        // maximum turnabout time
    int avgResp = 0;        // average response time
    int maxResp = 0;        // maximum response time
    int starvation = 0;     // amount of startvation
    int contextSwitches = 0;// context switch overhead
    float avgFairness = 0;  // fairness

    int contextSwitchTime = 10;
    contextSwitches = this->trace.trace.size() - 1; // number of times context siwtched

    int turn;
    int resp;
    int starved = 0;
    float unfairness;

    // extra metric accumulators added for the final paper
    long long sumWaiting   = 0;   // turnaround minus burst, summed
    long long sumBurst     = 0;   // total cpu work across all jobs
    double    jainNum      = 0.0; // numerator for jain's index
    double    jainDen      = 0.0; // denominator for jain's index
    int       lastEnd      = 0;   // for throughput and cpu util

    for(Job j : this->trace.s.schedule)
    {
        resp = this->trace.getFirstOccured(j.getID()).getStart() - j.getArrival();
        // response time is by definition >= 0. clamp here so trace events
        // with stale start fields can't push it negative and corrupt the avg
        if (resp < 0) resp = 0;
        turn = this->trace.getLastOccured(j.getID()).getEnd() - j.getArrival(); // end - arrival, get end from trace
        unfairness = this->trace.fairnessEvent(j.getID());

        if(maxResp < resp)
        {
            maxResp = resp;
        }

        if(maxTurn < turn)
        {
            maxTurn = turn;
        }

        if(this->trace.getFirstOccured(j.getID()).getStart() - j.getArrival() >= starvationThreshold)
        {
            starved = 1;
        }

        if (!policy::csvMode)
            std::cout << "Job " << j.getID() << " - Response Time: " << resp << ", Turnabout Time: " << turn << ", Unfairness: " << unfairness << ", Starved: " << starved <<std::endl;

        avgResp += resp; //add together the response times
        avgTurn += turn; //add together all turnabouts
        avgFairness += unfairness; //add together all

        // jain's fairness index uses x_i = 1/turnaround as the per-job
        // share. when all turnarounds are equal, jain returns 1.0;
        // skewed distributions push it toward 1/n.
        if (turn > 0) {
            double x = 1.0 / (double)turn;
            jainNum += x;
            jainDen += x * x;
        }

        // waiting time = turnaround - actual cpu work for this job
        int burst = turn - resp; // approximation: cpu time = turnaround - waiting-before-start
        // more accurate: sum of (event.end - event.start) where event.id matches
        int actualBurst = 0;
        for (policy::Event e : this->trace.trace)
            if (e.getID() == j.getID())
                actualBurst += e.getEnd() - e.getStart();
        sumWaiting += turn - actualBurst;
        sumBurst   += actualBurst;

        int endT = this->trace.getLastOccured(j.getID()).getEnd();
        if (endT > lastEnd) lastEnd = endT;

        if(starved)
        {
            starvation++;
        }

        starved = 0;
    }

    // size() returns size_t (unsigned), and avgResp can be negative for
    // policies that record start before arrival in some edge case; the
    // implicit conversion to unsigned wraps and prints garbage. cast to
    // int so the signed division stays signed.
    int n = (int)this->trace.s.schedule.size();
    avgTurn = avgTurn / n;
    avgResp = avgResp / n;
    avgFairness = avgFairness / n;

    // derived metrics for the paper
    double waitingAvg = (double)sumWaiting / (double)n;
    double jainIndex  = (jainDen > 0.0) ? (jainNum * jainNum) / ((double)n * jainDen) : 1.0;
    double throughput = (lastEnd > 0) ? (double)n / (double)lastEnd : 0.0;
    double cpuUtil    = (lastEnd > 0) ? (double)sumBurst / (double)lastEnd : 0.0;

    if (policy::csvMode)
    {
        // METRICS,policy,turn_avg,turn_max,resp_avg,resp_max,fairness,
        //   starvation,ctx_overhead,jobs,waiting_avg,jain,throughput,cpu_util
        std::cout << "METRICS,"
                  << this->name << ","
                  << avgTurn << "," << maxTurn << ","
                  << avgResp << "," << maxResp << ","
                  << avgFairness << ","
                  << starvation << ","
                  << contextSwitches * contextSwitchTime << ","
                  << this->trace.s.schedule.size() << ","
                  << waitingAvg << ","
                  << jainIndex << ","
                  << throughput << ","
                  << cpuUtil << std::endl;
    }
    else
    {
        std::cout << std::endl;
        std::cout << "Turnabout time - " << "(Average: " << avgTurn << ", Maximum: " << maxTurn << ")" << std::endl;
        std::cout << "Response time - " << "(Average: " << avgResp << ", Maximum: " << maxResp << ")" << std::endl;
        std::cout << "Fairness: " << avgFairness << std::endl;
        std::cout << "Instances of Starvation: " << starvation << std::endl;
        std::cout << "Overhead from Context Switches: " << contextSwitches * contextSwitchTime << std::endl;
    }

    return;
}
