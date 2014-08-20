#include <iostream>
#include <iomanip>
#include <sstream>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <grp.h>
#include <pwd.h>
}

using namespace std;

#define Expect(ret) _ExpectFailure(ret, true, __LINE__, __func__)
#define ExpectSuccess(ret) _ExpectFailure(ret, 0, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, __LINE__, __func__)
static void _ExpectFailure(int ret, int exp, int line, const char *func) {
    if (ret != exp)
        throw string("Got " + to_string(ret) + ", but expected " + to_string(exp) + " at " + func + ":" + to_string(line));
}

static void WaitExit(TPortoAPI &api, const string &pid, const string &name) {
    cerr << "Waiting for " << pid << " to exit..." << endl;

    int times = 100;
    int p = stoi(pid);

    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        /* we need to reap our child process which happens in the
         * exit_status handler, so execute it periodically */
        string ret;
        (void)api.GetData(name, "state", ret);

        kill(p, 0);
    } while (errno != ESRCH);

    if (times <= 0)
        throw string("Waited too long for task to exit");
}

static string GetCwd(const string &pid) {
    string lnk;
    TFile f("/proc/" + pid + "/cwd");
    TError error(f.ReadLink(lnk));
    if (error)
        return error.GetMsg();
    return lnk;
}

static string GetNamespace(const string &pid, const string &ns) {
    string link;
    TFile m("/proc/" + pid + "/ns/" + ns);
    (void)m.ReadLink(link);
    return link;
}

static map<string, string> GetCgroups(const string &pid) {
    TFile f("/proc/" + pid + "/cgroup");
    vector<string> lines;
    map<string, string> cgmap;
    (void)f.AsLines(lines);

    vector<string> tokens;
    for (auto l : lines) {
        tokens.clear();
        (void)SplitString(l, ':', tokens);
        cgmap[tokens[1]] = tokens[2];
    }

    return cgmap;
}

static void GetUidGid(const string &pid, int &uid, int &gid) {
    vector<string> st;
    TFile f("/proc/" + pid + "/status");
    (void)f.AsLines(st);

    string name;
    string stuid = st[7];
    stringstream ssuid(stuid);

    int euid, suid, fsuid;
    ssuid >> name;
    ssuid >> uid;
    ssuid >> euid;
    ssuid >> suid;
    ssuid >> fsuid;

    if (name != "Uid:" || uid != euid || euid != suid || suid != fsuid)
        uid = -2;

    string stgid = st[8];
    stringstream ssgid(stgid);

    int egid, sgid, fsgid;
    ssgid >> name;
    ssgid >> gid;
    ssgid >> egid;
    ssgid >> sgid;
    ssgid >> fsgid;

    if (name != "Gid:" || gid != egid || egid != sgid || sgid != fsgid)
        gid = -2;
}

static int UserUid(const string &user) {
    struct passwd *p = getpwnam(user.c_str());
    if (!p)
        return -1;
    else
        return p->pw_uid;
}

static int GroupGid(const string &group) {
    struct group *g = getgrnam(group.c_str());
    if (!g)
        return -1;
    else
        return g->gr_gid;
}

static string GetEnv(const string &pid) {
    string env;
    TFile f("/proc/" + pid + "/environ");
    (void)f.AsString(env);

    return env;
}

static void ShouldHaveOnlyRoot(TPortoAPI &api) {
    std::vector<std::string> containers;

    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 1);
    Expect(containers[0] == string("/"));
}

static void ShouldHaveValidProperties(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetProperty(name, "command", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetProperty(name, "low_limit", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "user", v));
    Expect(v == string("nobody"));
    ExpectSuccess(api.GetProperty(name, "group", v));
    Expect(v == string("nogroup"));
    ExpectSuccess(api.GetProperty(name, "env", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetProperty(name, "memory_guarantee", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory_limit", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "cpu_policy", v));
    Expect(v == string("normal"));
    ExpectSuccess(api.GetProperty(name, "cpu_priority", v));
    Expect(v == string("50"));
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == string("stopped"));
    ExpectFailure(api.GetData(name, "exit_status", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", v));
    Expect(v == string("-1"));
    ExpectFailure(api.GetData(name, "root_pid", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "stdout", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "stderr", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "cpu_usage", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "memory_usage", v), EError::InvalidState);
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    cerr << "Create container A" << endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Try to create existing container A" << endl;
    ExpectFailure(api.Create("a"), EError::ContainerAlreadyExists);
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Create container B" << endl;
    ExpectSuccess(api.Create("b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    cerr << "Remove container A" << endl;
    ExpectSuccess(api.Destroy("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("b"));

    cerr << "Remove container B" << endl;
    ExpectSuccess(api.Destroy("b"));

    cerr << "Try to execute operations on invalid container" << endl;
    ExpectFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    string value;
    ExpectFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.GetData("a", "root_pid", value), EError::ContainerDoesNotExist);

    ShouldHaveOnlyRoot(api);
}

static void TestEmpty(TPortoAPI &api) {
    cerr << "Make sure we can't start empty container" << endl;
    ExpectSuccess(api.Create("b"));
    ExpectFailure(api.Start("b"), EError::InvalidValue);
    ExpectSuccess(api.Destroy("b"));
}

static bool TaskRunning(TPortoAPI &api, const string &pid, const string &name) {
    int p = stoi(pid);

    string ret;
    (void)api.GetData(name, "state", ret);

    return kill(p, 0) == 0;
}

static void TestExitStatus(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Check exit status of 'false'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("256"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    cerr << "Check exit status of 'true'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    cerr << "Check exit status of invalid command" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("2"));

    cerr << "Check exit status of invalid directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("-2"));

    cerr << "Check exit status when killed by signal" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("9"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));
}

static void TestStreams(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Make sure stdout works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.Stop(name));

    cerr << "Make sure stderr works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string("err\n"));
    ExpectSuccess(api.Stop(name));
}

static void TestLongRunning(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Spawn long running task" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid, name) == true);

    cerr << "Check that task namespaces are correct" << endl;
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    Expect(GetNamespace("self", "mnt") != GetNamespace(pid, "mnt"));
    Expect(GetNamespace("self", "ipc") == GetNamespace(pid, "ipc"));
    Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));
    Expect(GetNamespace("self", "user") == GetNamespace(pid, "user"));
    Expect(GetNamespace("self", "uts") != GetNamespace(pid, "uts"));

    cerr << "Check that task cgroups are correct" << endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        Expect(name.second == "/");
    }

    cgmap = GetCgroups(pid);
    Expect(cgmap.size() == 3);
    for (auto kv : cgmap) {
        Expect(kv.second == "/porto/" + name);
    }

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid, name) == false);
}

static void TestIsolation(TPortoAPI &api, const string &name) {
    string ret;
    string pid;

    cerr << "Make sure PID isolation works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);

    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("1\n"));
    ExpectSuccess(api.Stop(name));
}

static void TestEnvironment(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Check default environment" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string env = GetEnv(pid);
    static const char empty_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "HOME=/home/nobody\0"
        "USER=nobody\0";

    Expect(memcmp(empty_env, env.data(), sizeof(empty_env)) == 0);
    ExpectSuccess(api.Stop(name));

    cerr << "Check user-defined environment" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "env", "a=b;c=d;"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    env = GetEnv(pid);
    static const char ab_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "a=b\0"
        "c=d\0"
        "HOME=/home/nobody\0"
        "USER=nobody\0";

    Expect(memcmp(ab_env, env.data(), sizeof(ab_env)) == 0);
    ExpectSuccess(api.Stop(name));
}

static void TestUserGroup(TPortoAPI &api, const string &name) {
    int uid, gid;
    string pid;

    cerr << "Check default user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("nobody"));
    Expect(gid == GroupGid("nogroup"));
    ExpectSuccess(api.Stop(name));

    cerr << "Check custom user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "user", "daemon"));
    ExpectSuccess(api.SetProperty(name, "group", "bin"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("daemon"));
    Expect(gid == GroupGid("bin"));
    ExpectSuccess(api.Stop(name));
}

static void TestCwd(TPortoAPI &api, const string &name) {
    string pid;
    string cwd;
    string portod_pid, portod_cwd;

    TFile portod(PID_FILE);
    (void)portod.AsString(portod_pid);
    portod_cwd = GetCwd(portod_pid);

    cerr << "Check default working directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    cwd = GetCwd(pid);

    Expect(cwd == portod_cwd);
    Expect(access(portod_cwd.c_str(), F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    Expect(access(portod_cwd.c_str(), F_OK) == 0);

    cerr << "Check user defined working directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/tmp"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    Expect(access("/tmp/stdout", F_OK) == 0);
    Expect(access("/tmp/stderr", F_OK) == 0);

    cwd = GetCwd(pid);

    Expect(cwd == "/tmp");
    Expect(access("/tmp", F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    Expect(access("/tmp", F_OK) == 0);
}

/*
static void TestRoot(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Check filesystem isolation" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "pwd"));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    ExpectSuccess(api.SetProperty(name, "root", "/root/chroot"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string cwd = GetCwd(pid);
    WaitExit(api, pid, name);

    Expect(cwd == "/root/chroot");

    string v;
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == string("/\n"));

    // TODO: check /proc/<PID>/root
}
*/

static void TestStateMachine(TPortoAPI &api, const string &name) {
    string pid;
    string v;

    cerr << "Check container state machine" << endl;

    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "running");

    ExpectFailure(api.Start(name), EError::InvalidValue);

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid, name);
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "dead");

    ExpectFailure(api.Start(name), EError::InvalidValue);

    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    // TODO: add pause/resume

    ExpectSuccess(api.Destroy(name));
}

int Selftest() {
    TPortoAPI api;

    try {
        TestHolder(api);
        TestEmpty(api);
        TestStateMachine(api, "a");

        ExpectSuccess(api.Create("a"));
        TestExitStatus(api, "a");
        TestStreams(api, "a");
        TestLongRunning(api, "a");
        TestIsolation(api, "a");
        TestEnvironment(api, "a");
        TestUserGroup(api, "a");
        TestCwd(api, "a");
        //TestRoot(api, "a");
        ExpectSuccess(api.Destroy("a"));
        // TODO: check cgroups permissions
    } catch (string e) {
        cerr << "EXCEPTION: " << e << endl;
        return 1;
    }

    return 0;
}
