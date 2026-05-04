// ========================================================================
// hybrid.cpp
// ------------------------------------------------------------------------
// Description: Workload adaptive cpu scheduler. Looks at what's in the
//   ready queue at every scheduling decision and switches between FCFS,
//   SJF, and RR depending on the shape of the workload. The whole point
//   is that no single classical policy is best on every workload, so the
//   scheduler picks the right one for the current queue.
//
//   Rule (first match wins):
//     avg percentIO of arrived jobs >= 15   -> RR    (i/o heavy)
//     CoV of remaining bursts < 0.2         -> FCFS  (uniform)
//     CoV > 0.5                             -> SJF   (meaningful variance)
//     otherwise                             -> RR    (default to fair)
//
//   CoV = stddev / mean of remaining burst lengths over the running job
//   plus jobs in the ready queue that have already arrived. Including
//   the running job matters for the cold-start case where one long job
//   has the cpu and shorts are waiting.
//
//   Re-evaluates every tick. In sjf mode it preempts the running job
//   if a shorter eligible job is sitting in the ready queue (closes
//   the SRTF gap on s13 and s15).
//
//   Stabilization window: a mode change is only honored if at least
//   hybStabilization ticks have passed since the last change. This is
//   the proposal's "preventing rapid switching" countermeasure. Without
//   it the dispatcher oscillates between FCFS, SJF, and RR every few
//   ticks on medium-CoV workloads (s4, s6) and pays double the context
//   switches of pure RR while running an unstable mix of all three.
// ------------------------------------------------------------------------
// Author: Illiasse Mounjim
// Version: 2026.04.27
// ========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <iostream>
#include <string>
#include <vector>
#include "schedule.h"
#include "policy.h"
#include "hybrid.h"

using namespace local;

unsigned int const hybIoRange       = 100; // max i/o duration in ticks
int          const hybIoCutoff      = 15;  // avg percentIO above this -> RR mode
double       const hybCovLow        = 0.2; // CoV below this -> FCFS
double       const hybCovHigh       = 0.5; // CoV above this -> SJF
std::uint64_t const hybStabilization = 250; // min ticks between mode changes (kills oscillation on s4 and s6)

enum HybridMode { MODE_FCFS = 0, MODE_SJF = 1, MODE_RR = 2 };

// rejection-sample bias-free rand, same approach as the other policies
unsigned hybRand(unsigned range)
{
    for (unsigned x, r;;)
        if (x = rand(), r = x % range, x - r <= -range)
            return r;
}

// coefficient of variation of remaining burst lengths in the ready queue.
// jobs not yet arrived (arrival > t) are skipped because they cannot run.
double covRemaining(Schedule &q, std::uint64_t t)
{
    double sum = 0.0;
    int    n   = 0;
    for (Job j : q.schedule)
    {
        if (j.getArrival() <= (int)t)
        {
            sum += j.getLength();
            n++;
        }
    }
    if (n < 2) return 0.0;

    double mean = sum / n;
    double var  = 0.0;
    for (Job j : q.schedule)
    {
        if (j.getArrival() <= (int)t)
        {
            double d = j.getLength() - mean;
            var += d * d;
        }
    }
    var /= n;
    if (mean <= 0.0) return 0.0;
    return std::sqrt(var) / mean;
}

// average percentIO across the eligible ready jobs
double avgPercentIO(Schedule &q, std::uint64_t t)
{
    double sum = 0.0;
    int    n   = 0;
    for (Job j : q.schedule)
    {
        if (j.getArrival() <= (int)t)
        {
            sum += j.getPercentIO();
            n++;
        }
    }
    if (n == 0) return 0.0;
    return sum / n;
}

// look at what's eligible now and decide which classical policy to use next
HybridMode pickMode(Schedule &q, std::uint64_t t)
{
    if (q.schedule.empty()) return MODE_FCFS;

    double io  = avgPercentIO(q, t);
    double cov = covRemaining(q, t);

    if (io  >= hybIoCutoff) return MODE_RR;
    if (cov <  hybCovLow)   return MODE_FCFS;
    if (cov >  hybCovHigh)  return MODE_SJF;
    return MODE_RR;
}

// hold the current mode unless hybStabilization ticks have passed since
// the last change. without this the dispatcher flips on consecutive
// ticks and ends up running an unstable mix of three policies (see s4
// and s6 forensics, the proposal predicted exactly this failure mode).
HybridMode pickModeStable(Schedule &q, std::uint64_t t,
                          HybridMode current, std::uint64_t &lastChange)
{
    HybridMode candidate = pickMode(q, t);
    if (candidate == current)                    return current;     // nothing to debate
    if (t < lastChange + hybStabilization)       return current;     // inside the hold window
    lastChange = t;
    return candidate;
}

// return the index of the next job to run given the chosen mode.
// returns -1 if no job is eligible at time t.
int pickIndex(Schedule &q, std::uint64_t t, HybridMode mode)
{
    int pick = -1;

    if (mode == MODE_SJF)
    {
        int minLen = INT_MAX;
        for (int i = 0; i < (int)q.schedule.size(); i++)
        {
            if (q.schedule[i].getArrival() <= (int)t &&
                q.schedule[i].getLength()  <  minLen)
            {
                minLen = q.schedule[i].getLength();
                pick   = i;
            }
        }
    }
    else // FCFS or RR both pick the head of the arrival-ordered queue
    {
        int minArr = INT_MAX;
        for (int i = 0; i < (int)q.schedule.size(); i++)
        {
            if (q.schedule[i].getArrival() <= (int)t &&
                q.schedule[i].getArrival()  <  minArr)
            {
                minArr = q.schedule[i].getArrival();
                pick   = i;
            }
        }
    }
    return pick;
}

// pull the next job out of the ready queue under the active mode.
// also closes any open idle gap in the trace.
Job hybridNextJob(Schedule &readyQueue, std::uint64_t currTime, bool &noRunning,
                  std::uint64_t breakStart, policy::Trace &trace, HybridMode mode)
{
    int idx = pickIndex(readyQueue, currTime, mode);
    if (idx >= 0)
    {
        Job next = readyQueue.schedule[idx];
        readyQueue.schedule.erase(readyQueue.schedule.begin() + idx);
        if (noRunning){
            trace.addEvent(policy::Event(breakStart, currTime, -1));
            breakStart = 0;
        }
        noRunning = false;
        // initialize start now so a same-tick i/o block doesn't read garbage
        next.setStarted(true);
        next.setStart(currTime);
        return next;
    }
    noRunning = true;
    return Job(-1, 0, 0, 0, 1);
}

// main loop, one tick per iteration. structure mirrors rr.cpp so the only
// real new thing is the mode recomputation at every scheduling decision.
policy::Trace hybridRunJobs(Schedule s, int quantum)
{
    Schedule readyQueue   = Schedule(s);
    Schedule blockedQueue = Schedule();
    policy::Trace trace   = policy::Trace();

    trace.s = s;

    std::sort(readyQueue.schedule.begin(), readyQueue.schedule.end(),
        [](Job a, Job b) { return a.getArrival() < b.getArrival(); });

    HybridMode    mode           = pickMode(readyQueue, 0);
    std::uint64_t lastModeChange = 0;                                   // ticks since the last accepted mode change

    Job  running    = readyQueue.schedule.front();
    readyQueue.schedule.erase(readyQueue.schedule.begin());
    bool noRunning  = false;
    std::uint64_t currTime   = 0;
    std::uint64_t breakStart = 0;
    int           slice      = 0;

    while (!readyQueue.schedule.empty() || !blockedQueue.schedule.empty()
           || (!noRunning && running.getLength() > 0))
    {
        if (currTime == UINT64_MAX)
        {
            std::cout << "Ran too long, terminating" << std::endl;
            exit(2);
        }

        if (!running.getStarted() && !noRunning)
        {
            running.setStarted(true);
            running.setStart(currTime);
        }

        if (!noRunning)
        {
            running.decrementLength();
            slice++;
        }

        // re-check mode every tick. include the running job so the
        // ratio of long-vs-short reflects what's actually on cpu, not
        // just what's waiting. without this, a long job alone at t=0
        // hides the variance until it finishes.
        Schedule combined = readyQueue;
        if (!noRunning) combined.schedule.push_back(running);
        mode = pickModeStable(combined, currTime, mode, lastModeChange);

        // sjf-mode preemption. if a shorter eligible job is sitting in
        // the ready queue, swap to it. this is what closes the SRTF gap
        // on s13 and s15 where a long job arrived alone at t=0
        if (!noRunning && mode == MODE_SJF && running.getLength() > 0)
        {
            int bestIdx = -1;
            int bestLen = running.getLength();
            for (int i = 0; i < (int)readyQueue.schedule.size(); i++)
            {
                if (readyQueue.schedule[i].getArrival() <= (int)currTime &&
                    readyQueue.schedule[i].getLength() < bestLen)
                {
                    bestLen = readyQueue.schedule[i].getLength();
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0)
            {
                trace.addEvent(policy::Event(running.getStart(), currTime,
                                             running.getID()));
                running.setStarted(false);
                readyQueue.schedule.push_back(running);

                Job next = readyQueue.schedule[bestIdx];
                readyQueue.schedule.erase(readyQueue.schedule.begin() + bestIdx);
                next.setStarted(true);
                next.setStart(currTime);
                running = next;
                slice = 0;
            }
        }

        // job finished
        if (running.getLength() <= 0)
        {
            if (!noRunning)
            {
                running.setStatus(1);
                trace.addEvent(policy::Event(running.getStart(), currTime,
                                             running.getID()));
                breakStart = 0;
            }
            mode    = pickModeStable(readyQueue, currTime, mode, lastModeChange);
            running = hybridNextJob(readyQueue, currTime, noRunning,
                                    breakStart, trace, mode);
            if (noRunning && breakStart == 0) breakStart = currTime;
            slice = 0;
        }
        // quantum used up, only matters in RR mode
        else if (mode == MODE_RR && slice >= quantum && !noRunning)
        {
            trace.addEvent(policy::Event(running.getStart(), currTime,
                                         running.getID()));
            running.setStarted(false);
            readyQueue.schedule.push_back(running);

            mode    = pickModeStable(readyQueue, currTime, mode, lastModeChange);
            running = hybridNextJob(readyQueue, currTime, noRunning,
                                    breakStart, trace, mode);
            if (noRunning && breakStart == 0) breakStart = currTime;
            slice = 0;
        }

        // i/o block
        if (hybRand(100) < (unsigned)running.getPercentIO() && !noRunning)
        {
            trace.addEvent(policy::Event(running.getStart(), currTime,
                                         running.getID()));
            running.setStarted(false);
            running.setIOEnd((int)(hybRand(hybIoRange) + currTime));
            blockedQueue.schedule.push_back(running);

            mode    = pickModeStable(readyQueue, currTime, mode, lastModeChange);
            running = hybridNextJob(readyQueue, currTime, noRunning,
                                    breakStart, trace, mode);
            if (noRunning && breakStart == 0) breakStart = currTime;
            slice = 0;
        }

        // unblock i/o-finished jobs back to ready
        for (auto it = blockedQueue.schedule.begin();
             it != blockedQueue.schedule.end(); )
        {
            if (currTime == (std::uint64_t)(*it).getIOEnd())
            {
                readyQueue.schedule.push_back(*it);
                it = blockedQueue.schedule.erase(it);
            }
            else
                it++;
        }

        currTime++;

        if (currTime % 500 == 0)
        {
            if (currTime % 100000 == 0)
                std::cout << "... t=" << currTime
                          << " remaining="
                          << readyQueue.schedule.size()
                             + blockedQueue.schedule.size()
                          << "\r";
        }
    }

    return trace;
}

policy::Policy policy::Hybrid::evaluate(Schedule s)
{
    return policy::Policy("HYBRID", hybridRunJobs(s, quantum), quantum);
}
