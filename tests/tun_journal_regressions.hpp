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
#include "include/global/Logger.hpp"
namespace TunJournalTests {
inline void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline int child(const QString& directory){TunStartup::Journal writer(directory);TunStartup::Record record;record.outcome=TunStartup::Outcome::Failed;return writer.Append(record,true)==TunStartup::WriteResult::Written?0:1;}
inline int evidenceChild(const QString& directory,bool failed){
 Logging::SetLevel(Logging::Level::Debug);Logging::Init(directory);
 if(failed){
  Logging::Write(Logging::Level::Error,"synthetic configure failure original detail");
  TunStartup::Journal writer(Logging::LogDir());TunStartup::Record record;record.attempt=1;record.rpcOK=true;
  record.diagnostic.available=true;record.diagnostic.tunStage=true;record.diagnostic.configureStage=true;record.diagnostic.version=2;
  record.diagnostic.causes.append({3,quint32(1167)});record.diagnostic.errorDetail="start inbound/tun[tun-in]: configure tun interface: set ipv4 address: device not connected";record.diagnostic.coreBuild="diagnostic=test-core";
  if(writer.Append(record,true)!=TunStartup::WriteResult::Written)return 1;
 }else Logging::Write(Logging::Level::Info,"synthetic next successful session");
 Logging::Shutdown();return 0;
}
inline void cleanRestartEvidence(){
 QTemporaryDir dir;check(dir.isValid(),"evidence directory");
 for(const auto& mode:QStringList{"failed","success"}){QProcess child;child.setProgram(QCoreApplication::applicationFilePath());child.setArguments({"--tun-evidence-child",dir.path(),mode});child.start();check(child.waitForFinished(10000)&&child.exitStatus()==QProcess::NormalExit&&child.exitCode()==0,"evidence clean child");}
 QFile ordinary(dir.path()+"/logs/throne.log");check(ordinary.open(QIODevice::ReadOnly),"ordinary log");check(!ordinary.readAll().contains("synthetic configure failure original detail"),"fixture did not reproduce ordinary log replacement");
 QFile journal(dir.path()+"/logs/tun-startup.jsonl");check(journal.open(QIODevice::ReadOnly),"retained evidence journal");auto value=QJsonDocument::fromJson(journal.readLine()).object();
 check(value["error_detail"].toString().contains("set ipv4 address")&&value["core_build"]=="diagnostic=test-core","clean restart lost original startup detail/core identity");
 auto causes=value["causes"].toArray();check(causes.size()==1&&causes.first().toObject()["step"]==3&&causes.first().toObject()["win32_code"]==1167,"clean restart lost typed step and Win32 code");
 check(value["v"]==2&&value["detail_truncated"]==false,"evidence version/truncation missing");
 std::cout<<"PASS failed-clean -> successful-clean real Logger/Journal retained detail, typed code and core build"<<std::endl;
}

inline void reviewPrivacy(){
 for(const auto& build:QStringList{"diagnostic=x;password=secret","diagnostic=x;token=abc","diagnostic=x;go=go1;go=go2","diagnostic=x;vcs=nothex","diagnostic=x;modified=yes"})check(TunStartup::SafeBuild(build)=="unavailable","unknown/duplicate/invalid build metadata accepted");
 for(const auto& path:QStringList{QStringLiteral("C:\\Users\\Alice Smith\\Documents\\profile"),QStringLiteral("\\\\server\\Alice Smith\\private profile")}){auto safe=TunStartup::SanitizeDetail("set ipv4 dns: "+path+" | device unavailable");check(safe.redacted&&!safe.text.contains("Smith")&&!safe.text.contains("profile"),"unquoted path with spaces leaked suffix");}
}
inline void detailPrivacy(){
 QTemporaryDir dir;check(dir.isValid(),"privacy directory");TunStartup::Journal writer(dir.path());TunStartup::Record record;
 record.event=TunStartup::Event::Terminal;record.diagnostic.version=2;record.diagnostic.causes={{5,std::nullopt},{0,quint32(99999)}};
 record.diagnostic.errorDetail=QStringLiteral("start inbound/tun[tun-in]: configure tun interface: set ipv4 dns: failed https://user:pw@private.example/sub?token=abc password=secret uuid=11111111-2222-3333-4444-555555555555 C:\\Private\\file 203.0.113.9 \"private-node\" {\"password\":\"jsonsecret\"}");
 record.diagnostic.coreBuild="https://private.example/token";check(writer.Append(record,true)==TunStartup::WriteResult::Written,"privacy append");
 QFile file(writer.Path());check(file.open(QIODevice::ReadOnly),"privacy read");auto encoded=file.readLine();file.close();auto value=QJsonDocument::fromJson(encoded).object();
 check(value["error_detail"].toString().contains("set ipv4 dns")&&value["detail_redacted"].toBool()&&value["core_build"]=="unavailable"&&value["event"]=="terminal","privacy metadata or stage lost");
 for(const auto& secret:QList<QByteArray>{"user:pw","private.example","token=abc","secret","11111111-2222","Private","203.0.113.9","private-node"})check(!encoded.contains(secret),"sensitive startup data persisted");
 auto causes=value["causes"].toArray();check(causes[0].toObject()["win32_code"].isNull()&&!causes[0].toObject()["code_available"].toBool()&&causes[1].toObject()["win32_code"]==99999,"missing-vs-real code collapsed");
 record.diagnostic.errorDetail=QString(12000,QChar(0x9519));record.diagnostic.coreBuild="diagnostic=test-core";
 check(writer.Append(record,true)==TunStartup::WriteResult::Written,"long detail append");check(file.open(QIODevice::ReadOnly),"long detail read");auto lines=file.readAll().split('\n');file.close();value=QJsonDocument::fromJson(lines[1]).object();
 check(value["detail_truncated"].toBool()&&value["error_detail"].toString().toUtf8().size()<=3072&&lines[1].size()<=TunStartup::MaxRecordBytes,"long detail cap/truncation");
 record.diagnostic.errorDetail=QString(3072,QChar('\\'));check(writer.Append(record,true)==TunStartup::WriteResult::Written,"max JSON escaped detail rejected");
 std::cout<<"PASS startup detail redaction, explicit missing codes, UTF8/JSON byte limits and event identity"<<std::endl;
}
inline void run(){
 reviewPrivacy();detailPrivacy();cleanRestartEvidence();
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
