#pragma once
#include <QApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QPointer>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include "include/global/Utils.hpp"
inline void runBackgroundRegressions() {
    auto check=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
    auto wait=[&](auto pred,int deadline){QElapsedTimer t;t.start();while(!pred()&&t.elapsed()<deadline){QCoreApplication::processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}check(pred(),"background wait deadline");};
    QSemaphore gate;std::atomic<int> entered{0},done{0};QMutex idsMutex;QSet<Qt::HANDLE> workers;
    for(int i=0;i<64;++i)check(runOnBackgroundTask([&]{
        {QMutexLocker l(&idsMutex);workers.insert(QThread::currentThreadId());}
        entered.fetch_add(1);gate.acquire();done.fetch_add(1);
    }),"pool rejected under capacity");
    wait([&]{return entered.load()==4;},2000);
    check(backgroundShortTaskCount()==64,"accepted short task count");
    check(!runOnBackgroundTask([]{}),"unbounded queue accepted task65");
    gate.release(64);wait([&]{return backgroundShortTaskCount()==0;},5000);
    check(done.load()==64&&workers.size()<=4,"pool worker count or drain incorrect");
    std::atomic<int> blockers{0};QSemaphore block;
    for(int i=0;i<4;++i)check(runOnBackgroundTask([&]{blockers.fetch_add(1);block.acquire();}),"owner-test blocker admission");
    wait([&]{return blockers.load()==4;},2000);
    auto* owner=new QObject;std::atomic<bool> called{false};check(runOnBackgroundTask([&]{called=true;},owner),"owner guarded admission");delete owner;
    block.release(4);wait([&]{return backgroundShortTaskCount()==0;},5000);check(!called.load(),"expired task owner executed");
    check(runOnBackgroundTask([]{throw std::runtime_error("intentional background exception");}),"exception task admission");
    wait([&]{return backgroundShortTaskCount()==0;},2000);
    std::atomic<bool> asleep{false},woke{false};runOnNewThread([&]{asleep=true;woke=!waitForBackgroundWork(600000);});
    wait([&]{return asleep.load();},2000);QElapsedTimer stop;stop.start();requestBackgroundStop();
    check(finishBackgroundWork(3000),"long worker did not finish on shutdown");
    check(woke.load()&&stop.elapsed()<3000,"shutdown waited for full ten-minute sleep");
    check(!runOnBackgroundTask([]{}),"shutdown accepted new short work");
    std::cout<<"PASS O3 bounded64 admission / <=4 reusable workers / destroyed owner / exception / wakeable stop="<<stop.elapsed()<<"ms\n";
}
