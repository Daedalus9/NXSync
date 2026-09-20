"""Execute the actual added dmnt gate code against a deterministic fake OS/SD."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parents[1]
helpers = []
for version in ("1.8.0", "1.11.2"):
    patch = (ROOT / f"patches/atmosphere/{version}/dmnt-cheat-api.patch").read_text()
    start = patch.index("+        constexpr const char *NxsyncGateEnabledPath")
    end = patch.index("         class FrozenAddressMapEntry", start)
    helpers.append("\n".join(line[1:] for line in patch[start:end].splitlines() if line.startswith("+")))
if helpers[0] != helpers[1]:
    raise RuntimeError("The two supported dmnt gates diverged; test each separately")

shim = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <map>
#include <string>
using u64=std::uint64_t; using u32=std::uint32_t; using s64=std::int64_t; using s32=std::int32_t;
struct Result { int code=0; Result(int c=0):code(c){} u32 GetValue()const{return code;} };
Result ResultSuccess(){return {};}
#define R_SUCCEEDED(r) ((r).code==0)
#define R_FAILED(r) ((r).code!=0)
#define R_TRY(r) do {auto value=(r); if(R_FAILED(value)) return value;}while(0)
#define R_SUCCEED() return Result{}
#define AMS_UNUSED(r) (void)(r)
template<class F> struct Scope { F f; ~Scope(){f();} };
struct ScopeTag{};
template<class F> Scope<F> operator+(ScopeTag,F f){return {f};}
#define JOIN2(a,b) a##b
#define JOIN(a,b) JOIN2(a,b)
#define ON_SCOPE_EXIT auto JOIN(scope,__COUNTER__)=ScopeTag{}+[&]()
struct TimeSpan {
    s64 value;
    static constexpr TimeSpan FromMilliSeconds(s64 n){return {n};}
    static constexpr TimeSpan FromSeconds(s64 n){return {n*1000};}
    static constexpr TimeSpan FromMinutes(s64 n){return {n*60000};}
    bool operator>=(TimeSpan other)const{return value>=other.value;}
};
s64 now=0;
int scenario=0;
bool sawGrant=false;
int processState=0, startCalls=0, closeCalls=0;
bool debugAttached=false, userCodeRan=false, failAttach=false, failStart=false;
bool closeFromHome(){
    // Kernel rejects termination of Created / CreatedAttached processes.
    if(processState==0||processState==1)return false;
    processState=5;
    return true;
}
std::map<std::string,std::string> sd;
const std::string base="sdmc:/config/NXSync/";
const std::string lease="version=2\nsequence=42\nprocess_id=42\ntitle_id=0100000000000001\nprofile_uid=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
void step();
namespace os {
struct ProcessId{u64 value;}; using NativeHandle=int;
struct Tick {s64 value; Tick operator-(Tick b)const{return {value-b.value};} TimeSpan ToTimeSpan()const{return {value};}};
Tick GetSystemTick(){return {now};}
void SleepThread(TimeSpan interval){now+=interval.value;step();}
}
namespace ncm {struct ProgramId{u64 value;};}
namespace util {using std::snprintf; template<class... A> int SNPrintf(char* p,size_t n,const char* f,A... a){return std::snprintf(p,n,f,a...);}}
namespace fs {
struct FileHandle{std::string name;}; enum DirectoryEntryType{File};
constexpr int OpenMode_Read=1,OpenMode_Write=2,CreateOption_None=0;
enum class WriteOption{Flush};
struct ResultTargetLocked{static bool Includes(Result r){return r.code==2;}};
struct ResultPathNotFound{static bool Includes(Result r){return r.code==1;}};
Result GetEntryType(DirectoryEntryType*,const char* p){return sd.count(p)?0:1;}
Result DeleteFile(const char* p){return sd.erase(p)?0:1;}
Result RenameFile(const char* a,const char* b){if(!sd.count(a))return 1;sd[b]=sd[a];sd.erase(a);return 0;}
Result CreateFile(const char* p,size_t n,int){sd[p]=std::string(n,0);return 0;}
Result OpenFile(FileHandle* f,const char* p,int){if(!sd.count(p))return 1;f->name=p;return 0;}
void CloseFile(FileHandle){}
Result WriteFile(FileHandle f,s64,const char* p,size_t n,WriteOption){sd[f.name]=std::string(p,n);return 0;}
Result GetFileSize(s64* n,FileHandle f){*n=sd[f.name].size();return 0;}
Result ReadFile(FileHandle f,s64,void* p,size_t n){std::memcpy(p,sd[f.name].data(),n);return 0;}
}
namespace svc {
using Handle=int; constexpr Handle InvalidHandle=-1;
constexpr int ProcessInfoType_ProcessState=0, ProcessState_Terminating=5, ProcessState_Terminated=6;
Result GetProcessInfo(s64* out,int,int){
    if(scenario==13&&now>=2000)return 1;
    *out=processState;return 0;
}
Result DebugActiveProcess(Handle* out,u64){
    if(failAttach)return 1;
    assert(!debugAttached&&processState==0); debugAttached=true;processState=1;*out=9;return 0;
}
Result TerminateDebugProcess(Handle handle){assert(handle==9&&debugAttached);return closeFromHome()?0:1;}
}
namespace os {void CloseNativeHandle(int handle){
    assert(handle==9&&debugAttached);++closeCalls;debugAttached=false;
    if(processState==4||processState==7){processState=2;userCodeRan=true;}
    else if(processState==1)processState=0;
}}
namespace pm::dmnt {Result StartProcess(os::ProcessId){
    assert(debugAttached&&processState==1);++startCalls;
    if(failStart)return 1;
    processState=4;return 0;
}}
void step(){
    if(sd.count(base+"launch.restore-grant")){
        assert(sd.count(base+"launch.restore-guard")); // persisted BEFORE grant
        sawGrant=true;
    }
    assert(debugAttached&&!userCodeRan);
    if(scenario==1&&now>=30000)sd[base+"launch.restore-claim"]=lease;
    if(scenario>=2&&scenario<=5&&now>=29000)sd[base+"launch.restore-claim"]=lease;
    if(scenario==2&&now>=35000){
        sd.erase(base+"launch.restore-guard");
        sd[base+"launch.decision"]="version=2\nsequence=42\naction=allow\nmessage=ok\n";
    }
    if(scenario==4&&sawGrant)sd[base+"launch.decision"]="version=2\nsequence=42\naction=abort\nmessage=failed\n";
    if(scenario==5&&sawGrant)sd[base+"launch.decision"]="version=2\nsequence=42\naction=allow\nmessage=unsafe\n";
    if(scenario==6&&now>=2000)assert(closeFromHome());
    if(scenario==7)sd[base+"launch.decision"]="version=2\nsequence=420\naction=allow\nmessage=stale\n";
    if((scenario==8||scenario==9||scenario==14||scenario==15)&&now>=1000)
        sd[base+"launch.phase"]="version=2\nsequence=42\nphase=choice\n";
    if((scenario==9||scenario==14||scenario==15)&&now>=2000)
        sd[base+"launch.phase"]="version=2\nsequence=42\nphase=download\n";
    if(scenario==9&&now>=3000) // Backward transitions cannot reset the download timer.
        sd[base+"launch.phase"]="version=2\nsequence=42\nphase=choice\n";
    if(scenario==10&&now>=30000)
        sd[base+"launch.phase"]="version=2\nsequence=42\nphase=download\n";
    if(scenario==11)sd[base+"launch.phase"]="version=2\nsequence=420\nphase=choice\n";
    if(scenario==12){
        if(now>=1000)sd[base+"launch.restore-claim"]=lease;
        if(now>=2000){assert(closeFromHome());processState=6;}
    }
    if(scenario==14&&now>=301000)sd[base+"launch.restore-claim"]=lease;
    if(scenario==14&&now>=310000){
        sd.erase(base+"launch.restore-guard");
        sd[base+"launch.decision"]="version=2\nsequence=42\naction=allow\nmessage=ok\n";
    }
    if(scenario==15&&now>=302000)sd[base+"launch.restore-claim"]=lease;
    if(scenario==16)sd[base+"launch.phase"]="version=2\nsequence=42\nphase=choice\nphase=download\n";
    if(scenario==17)sd[base+"launch.phase"]="version=2\nsequence=42\nphase=download";

}
'''
checks = r'''
int main(){
    // Reproduce the original kernel limitation, then exercise the real hold + gate.
    assert(!closeFromHome());
    for(scenario=0;scenario<=17;++scenario){
        sd.clear();now=0;sawGrant=false;sd[base+"launch-gate.enabled"]="yes";
        processState=0;debugAttached=false;userCodeRan=false;startCalls=closeCalls=0;
        bool allowed;
        {
            NxsyncLaunchHold hold;
            assert(R_SUCCEEDED(hold.Begin({42}))&&hold.Started());
            assert(processState==4&&debugAttached&&!userCodeRan);
            allowed=NxsyncRunLaunchGate({42},{0x0100000000000001},7);
            assert(!userCodeRan); // Never execute game code during a check or restore.
            if(!allowed)hold.Cancel();
        }
        assert(startCalls==1&&closeCalls==1);
        assert(userCodeRan==allowed);
        assert(!sd.count(base+"launch.request")&&!sd.count(base+"launch.restore-grant"));
        assert(!sd.count(base+"launch.phase"));
        if(scenario==0||scenario==1||scenario==7||scenario==10||scenario==11||scenario==16||scenario==17)
            assert(allowed&&now==30000&&!sawGrant);
        if(scenario==2)assert(allowed&&now==35000&&sawGrant);
        if(scenario==3)assert(!allowed&&now==1829000&&sawGrant&&sd.count(base+"launch.restore-guard"));
        if(scenario==4||scenario==5)assert(!allowed&&sawGrant&&sd.count(base+"launch.restore-guard"));
        if(scenario==6||scenario==13)assert(!allowed&&!sawGrant&&now==2000);
        if(scenario==8)assert(allowed&&now==121000&&!sawGrant);
        if(scenario==9||scenario==15)assert(allowed&&now==302000&&!sawGrant);
        if(scenario==12)assert(!allowed&&now==2000&&sawGrant&&sd.count(base+"launch.restore-guard"));
        if(scenario==14)assert(allowed&&now==310000&&sawGrant);
    }
    processState=0;startCalls=closeCalls=0;userCodeRan=false;
    int transferred;
    {
        NxsyncLaunchHold hold;
        assert(R_SUCCEEDED(hold.Begin({42})));
        transferred=hold.TakeHandle();
        assert(hold.Started()); // The cheat manager must not start a second time.
    }
    assert(debugAttached&&!userCodeRan&&closeCalls==0&&startCalls==1);
    os::CloseNativeHandle(transferred);assert(userCodeRan&&closeCalls==1);
    for(int failure=0;failure!=2;++failure){
        processState=0;startCalls=closeCalls=0;userCodeRan=false;
        failAttach=failure==0;failStart=failure==1;
        {NxsyncLaunchHold hold;assert(R_FAILED(hold.Begin({42}))&&!hold.Started());}
        assert(!debugAttached&&!userCodeRan&&processState==0);
    }
    failAttach=failStart=false;
    sd.clear();sd[base+"launch.restore-guard"]=lease;
    assert(!NxsyncRunLaunchGate({50},{0x0100000000000001},7)); // disabled gate still protects save
    assert(NxsyncRunLaunchGate({50},{0x0100000000000002},7)); // recovery via a different title
    sd.clear();sd[base+"launch.restore-guard.bak"]=lease;
    assert(!NxsyncRunLaunchGate({50},{0x0100000000000001},7));
    sd.clear();sd[base+"launch.decision"]="version=2\nsequence=42\naction=allow\nmessage=ok\naction=abort\n";
    assert(!NxsyncReadDecision(42,"allow"));
}
'''
with tempfile.TemporaryDirectory(prefix="nxsync-gate-test-") as directory:
    root = Path(directory)
    (root / "gate.cpp").write_text(shim + helpers[0] + checks)
    subprocess.run([sys.argv[1], "-std=c++17", "-O2", "-UNDEBUG", str(root / "gate.cpp"), "-o", str(root / "gate")], check=True)
    subprocess.run([str(root / "gate")], check=True)
