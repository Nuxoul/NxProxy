#pragma once
#include <stdexcept>
#include <iostream>
#include <QTemporaryDir>
#include <QProcess>
#include <atomic>
#include <thread>
#include <vector>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#if __has_include("include/sys/TunStartupJournal.hpp")
#include "include/sys/TunStartupJournal.hpp"
namespace TunJournalTests {
inline void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline int child(const QString& directory){TunStartup::Journal writer(directory);TunStartup::Record record;record.outcome=TunStartup::Outcome::Failed;return writer.Append(record,true)==TunStartup::WriteResult::Written?0:1;}
inline void run(){
 QTemporaryDir dir;check(dir.isValid(),"journal temporary directory");
 TunStartup::Journal writer(dir.path());TunStartup::Record record;record.outcome=TunStartup::Outcome::Failed;record.diagnostic={true,true,true,183,1168,1};
 check(writer.Append(record,false)==TunStartup::WriteResult::Disabled&&!QFile::exists(writer.Path()),"logging Off wrote journal");
 check(writer.Append(record,true)==TunStartup::WriteResult::Written,"journal first append");
 QFile file(writer.Path());check(file.open(QIODevice::ReadOnly),"journal open");auto first=file.readAll();file.close();check(first.contains("183")&&!first.contains("password")&&first.size()<=TunStartup::MaxRecordBytes,"journal bounds/privacy");
 {QFile partial(writer.Path());check(partial.open(QIODevice::Append),"partial open");partial.write("{partial");}
 for(int i=0;i<2;++i){QProcess childProcess;childProcess.setProgram(QCoreApplication::applicationFilePath());childProcess.setArguments({"--tun-journal-child",dir.path()});childProcess.start();check(childProcess.waitForFinished(10000)&&childProcess.exitCode()==0,"journal clean-relaunch child");}
 check(file.open(QIODevice::ReadOnly),"retained open");auto retained=file.readAll();file.close();check(retained.startsWith(first)&&retained.contains("{partial\n{"),"clean restart truncated history or joined partial tail");
 int valid=0;for(auto line:retained.split('\n')){if(QJsonDocument::fromJson(line).isObject())++valid;}check(valid==3,"partial tail invalidated valid prior records");
 for(int i=0;i<1200;++i){record.attempt=i%3+1;check(writer.Append(record,true)==TunStartup::WriteResult::Written,"journal rotation append");}
 check(QFileInfo(writer.Path()).size()<=TunStartup::MaxFileBytes&&QFileInfo(writer.Path()+".1").size()<=TunStartup::MaxFileBytes&&QFile::exists(writer.Path()+".1"),"journal two-file cap/rotation");
 {QLockFile lock(writer.Path()+".lock");check(lock.tryLock(0),"journal contention setup");QElapsedTimer timer;timer.start();check(writer.Append(record,true)==TunStartup::WriteResult::Unavailable&&timer.elapsed()<100,"journal lock blocked startup");}
 QTemporaryDir blocked;QFile nonDirectory(blocked.path()+"/file");check(nonDirectory.open(QIODevice::WriteOnly),"unwritable seed");nonDirectory.close();TunStartup::Journal unavailable(nonDirectory.fileName());check(unavailable.Append(record,true)==TunStartup::WriteResult::Unavailable,"unwritable journal returned written");
 std::atomic<int> failures=0;std::vector<std::thread> threads;for(int i=0;i<4;++i)threads.emplace_back([&]{TunStartup::Journal peer(dir.path());for(int j=0;j<30;++j){auto result=peer.Append(record,true);if(result==TunStartup::WriteResult::Disabled)++failures;}});for(auto& thread:threads)thread.join();check(failures==0,"concurrent journal policy changed");
 std::cout<<"PASS journal clean-process retention, partial tail, two-file rotation, Off, nonblocking contention and unavailable path"<<std::endl;
}
}
#else
namespace TunJournalTests {inline int child(const QString&){return 1;}inline void run(){throw std::runtime_error("bounded cross-restart TUN journal is missing");}}
#endif
