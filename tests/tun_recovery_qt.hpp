#pragma once
#include "include/sys/TunStartupRecovery.hpp"
#include "include/sys/CoreRestartPolicy.hpp"
#include "include/global/Utils.hpp"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include "include/ui/widget/StartStopButton.hpp"
#include <future>
#include <stdexcept>
#include <iostream>
inline void runTunRecoveryQt(){
 Configs_sys::CoreRestartIntent intent;const auto token=intent.Expect(42);
 StartStopButton cancel;cancel.setState(StartStopButton::State::Connecting);
 if(!cancel.isEnabled())throw std::runtime_error("connecting button cannot cancel TUN recovery");
 QObject::connect(&cancel,&QAbstractButton::clicked,[&]{intent.Cancel();});
 QElapsedTimer clock;clock.start();int calls=0;
 QTimer stop;stop.setSingleShot(true);
 QObject::connect(&stop,&QTimer::timeout,[&]{cancel.click();});stop.start(25);
 auto worker=std::async(std::launch::async,[&]{return TunStartupRecovery::Run([&](int){++calls;return TunStartupRecovery::AttemptResult{true,true,true};},[&]{return intent.IsCurrent(token);},[&](int delay){return TunStartupRecovery::Wait(delay,[&]{return intent.IsCurrent(token);},[](unsigned long ms){return waitForBackgroundWork(ms);},[&]{return clock.elapsed();});},[&]{return clock.elapsed();},[](int,int){});});
 while(worker.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready&&clock.elapsed()<2000){QCoreApplication::processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}
 if(worker.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)throw std::runtime_error("TUN wait did not stop within deadline");
 auto outcome=worker.get();
 if(!outcome.cancelled||calls!=1||clock.elapsed()>500)throw std::runtime_error("real Qt stop allowed a second TUN Start");
 std::cout<<"PASS TUN real Qt stop during recovery wait="<<clock.elapsed()<<"ms; stale token did not restart\n";
}
