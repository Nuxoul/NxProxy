#include "include/sys/TunStartupRecovery.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include "tun_recovery_cases.hpp"
int main(){
 try{
  std::int64_t now=0;int calls=0;std::vector<int> waits;
  auto outcome=TunStartupRecovery::Run(
   [&](int timeout){if(timeout<=0||timeout>30000)throw std::runtime_error("invalid call budget");++calls;return TunStartupRecovery::AttemptResult{true,calls==1,calls==1};},
   []{return true;},[&](int ms){waits.push_back(ms);now+=ms;return true;},[&]{return now;},[](int,int){});
  if(calls!=2||outcome.attempts!=2||outcome.cancelled||waits!=std::vector<int>{1000})throw std::runtime_error("transient TUN failure was not recovered automatically");
  runTunRecoveryCases();
  std::cout<<"PASS transient TUN Start failure then successful fresh attempt\n";return 0;
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
