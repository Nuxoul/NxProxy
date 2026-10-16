#pragma once
#include "include/sys/TunStartupRecovery.hpp"
#include <stdexcept>
#include <vector>
#include <iostream>
inline void runTunRecoveryCases(){
 auto check=[](bool ok,const char*text){if(!ok)throw std::runtime_error(text);};
 using Result=TunStartupRecovery::AttemptResult;
 for(const auto first:{Result{true,false,false},Result{true,true,false},Result{false,true,true}}){
  int calls=0;auto out=TunStartupRecovery::Run([&](int){++calls;return first;},[]{return true;},[](int){throw std::runtime_error("nonretryable call waited");return false;},[]{return std::int64_t{0};},[](int,int){});
  check(calls==1&&!out.exhausted,"normal/permanent/transport failure was retried");
 }
 {int calls=0;std::int64_t clock=0;std::vector<int> waits;
 auto out=TunStartupRecovery::Run([&](int){++calls;return Result{true,true,true};},[]{return true;},[&](int ms){waits.push_back(ms);clock+=ms;return true;},[&]{return clock;},[](int,int){});
 check(calls==3&&out.exhausted&&waits==std::vector<int>{1000,2000},"retry budget or backoff not bounded");}
 {int calls=0;bool current=true;
 auto out=TunStartupRecovery::Run([&](int){++calls;return Result{true,true,true};},[&]{return current;},[&](int){current=false;return false;},[]{return std::int64_t{0};},[](int,int){});
 check(calls==1&&out.cancelled,"stop during wait revived configuration");}
 {int calls=0;bool current=true;
 auto out=TunStartupRecovery::Run([&](int){++calls;current=false;return Result{true,false,false};},[&]{return current;},[](int){return true;},[]{return std::int64_t{0};},[](int,int){});
 check(calls==1&&out.cancelled,"stale success was published");}
 {int calls=0;std::int64_t clock=0;std::vector<int> timeouts;
 auto out=TunStartupRecovery::Run([&](int ms){++calls;timeouts.push_back(ms);clock+=std::min(28000,ms);return Result{true,true,true};},[]{return true;},[&](int ms){clock+=ms;return true;},[&]{return clock;},[](int,int){});
 check(calls==3&&out.exhausted&&timeouts==std::vector<int>{30000,30000,1000},"total RPC deadline not carried to remaining call");}
 {int calls=0;auto out=TunStartupRecovery::Run([&](int){++calls;return Result{};},[]{return false;},[](int){return true;},[]{return std::int64_t{0};},[](int,int){});
 check(calls==0&&out.cancelled,"already stopped intent started");}
 {int calls=0;std::int64_t clock=0;
 auto out=TunStartupRecovery::Run([&](int){++calls;clock=59500;return Result{true,true,true};},[]{return true;},[](int){throw std::runtime_error("wait exceeded total deadline");return true;},[&]{return clock;},[](int,int){});
 check(calls==1&&out.exhausted,"deadline did not stop retry wait");}
 std::cout<<"PASS TUN recovery normal/permanent/transport fallback, 3-attempt budget, stop/stale/deadline\n";
}
