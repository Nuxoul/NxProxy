// Standalone real-method SQLite regression/benchmark. The runner extracts unchanged
// ProfilesRepo methods; only unrelated application/profile types are replaced by this shim.
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QList>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <SQLiteCpp.h>
#include <sqlite3.h>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <iostream>
#include <stdexcept>
namespace Configs {
constexpr int BUSY_TIMEOUT_MS=5000;
std::atomic<int> serializations=0;
void NotifyError(const std::string&,const std::exception&) {}
class Database {
    SQLite::Database connection;
    std::string path;
public:
    int commits=0;
    Database(const std::string& p):connection(p,SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE,5000),path(p) {
        connection.exec("PRAGMA journal_mode=WAL");connection.exec("PRAGMA synchronous=NORMAL");
        sqlite3_commit_hook(connection.getHandle(),[](void* p){++*static_cast<int*>(p);return 0;},&commits);
    }
    const std::string& Path() const{return path;}
    void exec(const std::string& sql){connection.exec(sql);}
    template<class... Args> void exec(const std::string& sql,Args... args) {auto s=query(sql,args...);s->exec();}
    template<class... Args> auto query(const std::string& sql,Args... args) {
        auto s=std::make_unique<SQLite::Statement>(connection,sql);int i=1;(s->bind(i++,args),...);return s;
    }
};
struct Outbound {
    QString name="initial";
    QJsonObject ExportToJson(){++serializations;return {{"name",name},{"server","127.0.0.1"},{"payload",QString(2048,'x')}};}
};
struct Profile {
    int id=-1,gid=1,latency=10; qint64 latency_at=100,traffic_downlink=11,traffic_uplink=22;
    QString type="vless",name="initial",dl_speed="old-dl",ul_speed="old-ul",test_country="ZZ",ip_out="old-ip";
    std::shared_ptr<Outbound> outbound=std::make_shared<Outbound>();
};
#include "o1_records.inc"
class ProfilesRepo {
    Database& db;SQLite::Database testResultsDb;mutable std::mutex mutex;
    mutable std::map<int,std::weak_ptr<Profile>> identityMap;
    void createTables() const;bool profilesColumnExists(const char*) const;
    void saveToDatabase(const Profile*,int) const;
public:
    explicit ProfilesRepo(Database&);
    bool Save(const std::shared_ptr<Profile>&);
    TestResultSaveOutcome SaveTestResults(const std::vector<TestResult>&);
    int commits=0;
    void instrument(){sqlite3_commit_hook(testResultsDb.getHandle(),[](void* p){++*static_cast<int*>(p);return 0;},&commits);}
};
#include "o1_methods.inc"
}
using namespace Configs;
void require(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
std::string value(Database& db,int id,const std::string& col){auto s=db.query("SELECT "+col+" FROM profiles WHERE id=?",id);require(s->executeStep(),"missing row");return s->getColumn(0).getText();}
void regression(const std::string& path){
 Database db(path);db.exec("CREATE TABLE groups(id INTEGER PRIMARY KEY)");db.exec("INSERT INTO groups VALUES(1),(2)");ProfilesRepo repo(db);
 auto a=std::make_shared<Profile>(),b=std::make_shared<Profile>();a->id=1;b->id=2;repo.Save(a);repo.Save(b);
 const auto config=value(db,1,"outbound_json");
 auto r=repo.SaveTestResults({{1,TestResult::Kind::Url,42,123,QString("illegal"),{},QString("illegal"),QString("illegal")}});
 require(r.success&&r.appliedIDs==QList<int>{1},"URL applied IDs");
 require(a->latency==42&&a->latency_at==123&&a->dl_speed=="old-dl"&&a->test_country=="ZZ","URL independence/cache");
 r=repo.SaveTestResults({{1,TestResult::Kind::Ip,999,999,QString("illegal"),{},QString("US"),QString("1.2.3.4")}});
 require(r.success&&value(db,1,"latency")=="42"&&value(db,1,"dl_speed")=="old-dl"&&a->ip_out=="1.2.3.4","IP independence");
 r=repo.SaveTestResults({{1,TestResult::Kind::Speed,{},0,QString("new-dl")}});
 require(r.success&&value(db,1,"ul_speed")=="old-ul"&&value(db,1,"latency_at")=="123"&&value(db,1,"ip_out")=="1.2.3.4","DL-only independence");
 r=repo.SaveTestResults({{1,TestResult::Kind::Health,55,124,{},QString("illegal"),QString("illegal")}});
 require(r.success&&value(db,1,"ul_speed")=="old-ul"&&value(db,1,"test_country")=="US","health independence");
 require(value(db,1,"outbound_json")==config&&value(db,1,"name")=="initial"&&value(db,1,"gid")=="1"&&value(db,1,"type")=="vless"&&value(db,1,"traffic_dl")=="11"&&value(db,1,"traffic_up")=="22","non-test fields touched");
 db.exec("CREATE TRIGGER reject_result BEFORE UPDATE OF latency ON profiles WHEN NEW.id=2 BEGIN SELECT RAISE(ABORT,'injected'); END");
 r=repo.SaveTestResults({{1,TestResult::Kind::Url,-1,200},{2,TestResult::Kind::Url,-1,200}});
 require(!r.success&&r.appliedIDs.isEmpty()&&a->latency==55&&b->latency==10&&value(db,1,"latency")=="55","rollback/cache/failure cleanup gate");
 db.exec("DROP TRIGGER reject_result");db.exec("DELETE FROM profiles WHERE id=2");
 r=repo.SaveTestResults({{2,TestResult::Kind::Url,-1,201},{1,TestResult::Kind::Url,60,201}});
 require(r.success&&r.appliedIDs==QList<int>{1}&&b->latency==10,"deleted ID applied or resurrected");
 auto count=db.query("SELECT count(*) FROM profiles WHERE id=2");count->executeStep();require(count->getColumn(0).getInt()==0,"deleted row resurrected");count.reset();
 std::atomic<bool> start=false;
 std::thread writer([&]{Database other(path);while(!start.load())std::this_thread::yield();for(int i=0;i<100;++i)other.exec("UPDATE profiles SET name='subscription',gid=2,type='trojan',outbound_json='subscription-json',traffic_dl=333,traffic_up=444 WHERE id=1");});
 start=true;for(int i=0;i<100;++i)require(repo.SaveTestResults({{1,TestResult::Kind::Url,70+i,300+i}}).success,"concurrent update failed");writer.join();
 require(value(db,1,"latency")=="169"&&value(db,1,"name")=="subscription"&&value(db,1,"outbound_json")=="subscription-json"&&value(db,1,"gid")=="2"&&value(db,1,"type")=="trojan"&&value(db,1,"traffic_dl")=="333"&&value(db,1,"traffic_up")=="444","concurrent subscription/result lost");
 require(repo.SaveTestResults({}).success,"empty batch");
 std::cout<<"PASS field independence, committed cache, trigger rollback, failure cleanup gate, deleted-ID skip, concurrent subscription change\n";
}
void benchmark(const std::string& path,int n,int repeat){
 Database db(path);db.exec("CREATE TABLE groups(id INTEGER PRIMARY KEY)");db.exec("INSERT INTO groups VALUES(1)");ProfilesRepo repo(db);repo.instrument();
 std::vector<std::shared_ptr<Profile>> nodes;std::vector<TestResult> results;std::mt19937 random(0x01337);
 for(int i=1;i<=n;++i){auto p=std::make_shared<Profile>();p->id=i;repo.Save(p);nodes.push_back(p);results.emplace_back(i,TestResult::Kind::Url,1+random()%900,1700000000);}
 serializations=0;db.commits=0;QElapsedTimer timer;timer.start();
 for(int i=0;i<n;++i){nodes[i]->latency=*results[i].latency;nodes[i]->latency_at=results[i].measuredAt;repo.Save(nodes[i]);}
 const auto oldNs=timer.nsecsElapsed();const int oldSerial=serializations.load(),oldTx=db.commits;
 // Both measured paths change latency from the same initial state.
 db.exec("UPDATE profiles SET latency=10,latency_at=100");
 serializations=0;repo.commits=0;timer.restart();auto outcome=repo.SaveTestResults(results);const auto newNs=timer.nsecsElapsed();
 require(outcome.success&&outcome.appliedIDs.size()==n,"benchmark batch");require(serializations==0&&repo.commits==1&&oldSerial==n&&oldTx==n,"benchmark work counts");
 std::cout<<n<<','<<repeat<<",Save,"<<oldSerial<<','<<oldTx<<','<<oldNs/1e6<<'\n';
 std::cout<<n<<','<<repeat<<",SaveTestResults,"<<serializations<<','<<repo.commits<<','<<newNs/1e6<<'\n';
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);QTemporaryDir tmp;try{require(tmp.isValid(),"temp dir");regression((tmp.path()+"/regression.db").toStdString());std::cout<<"nodes,repeat,method,outbound_serializations,transactions,ms\n";for(int n:{100,1000})for(int r=1;r<=3;++r)benchmark((tmp.path()+QString("/%1-%2.db").arg(n).arg(r)).toStdString(),n,r);return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
