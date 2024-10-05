#include "galois/runtime/ThreadTimer.h"
#include "galois/runtime/Executor_OnEach.h"
#include "galois/runtime/Statistics.h"

#include <ctime>
#include <limits>
#include <iostream>
#include <sstream>

void galois::runtime::ThreadTimers::reportTimes(const char* category,
                                                const char* region) {

  uint64_t minTime = std::numeric_limits<uint64_t>::max();
  uint64_t maxTime = 0;
  uint64_t sumTime = 0;

  for (unsigned i = 0; i < timers_.size(); ++i) {
    auto ns = timers_.getRemote(i)->get_nsec();
    minTime = std::min(minTime, ns);
    maxTime = std::max(maxTime, ns);
    sumTime += ns;
  }
  auto avgTime = sumTime / timers_.size();
  
  std::stringstream ss;
  ss << "ThreadTimer : " << category << ", " << region << ", " << maxTime/1000000 << ", " << avgTime/1000000 << ", " << minTime/1000000 << std::endl;
  std::cout << ss.str();

  std::string timeCat = category + std::string("PerThreadTimes");
  std::string lagCat  = category + std::string("PerThreadLag");
/*
  on_each_gen(
      [&](auto tid, auto) {
        auto ns  = timers_.getLocal()->get_nsec();
        auto lag = ns - minTime;
        assert(lag > 0 && "negative time lag from min is impossible");

        reportStat_Tmax(region, timeCat.c_str(), ns / 1000000);
        reportStat_Tmax(region, lagCat.c_str(), lag / 1000000);
      },
      std::make_tuple());
      */
}
