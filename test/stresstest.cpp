#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <map>
#include <algorithm>

#include "config.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "test.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
}

namespace test {

static const int retries = 10;

static std::vector<std::map<std::string, std::string>> vtasks =
{
    {
        { "command", "bash -ec 'sleep $N'" },
        { "env", "N=1" },
        { "stdout", "" },
        { "stderr", "" },
        { "exit_status", "0" },
        { "timeout", "5" },
    },
    {
        { "command", "bash -ec 'echo $A'" },
        { "env", "A=qwerty" },
        { "stdout", "qwerty\n" },
        { "stderr", "" },
        { "exit_status", "0" },
        { "timeout", "5" },
    },
    {
        { "parent", "meta" },
        { "name", "test" },

        { "command", "bash -ec 'echo $A && false'" },
        { "env", "A=qwerty" },
        { "stdout", "qwerty\n" },
        { "stderr", "" },
        { "exit_status", "256" },
        { "timeout", "5" },
    },
    {
        { "command", "bash -ec 'for i in $A; do sleep 1; echo $i >&2; done'" },
        { "env", "A=1 2 3" },
        { "stdout", "" },
        { "stderr", "1\n2\n3\n" },
        { "exit_status", "0" },
        { "timeout", "10" },
    }
};

static void Create(TPortoAPI &api, const std::string &name, const std::string &cwd) {
    std::vector<std::string> containers;

    Say() << "Create container: " << name << std::endl;

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) == containers.end());
    auto ret = api.Create(name);
    Expect(ret == EError::Success || ret == EError::ContainerAlreadyExists);
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) != containers.end());

    if (cwd.length()) {
        TFolder f(cwd);
        if (!f.Exists()) {
            TError error = f.Create(0755, true);
            ExpectEq(error, false);
        }
    }
}

static void SetProperty(TPortoAPI &api, std::string name, std::string type, std::string value) {
    std::string res_value;

    Say() << "SetProperty container: " << name << std::endl;

    ExpectApiSuccess(api.SetProperty(name, type, value));
    ExpectApiSuccess(api.GetProperty(name, type, res_value));
    ExpectEq(res_value, value);
}

static void Start(TPortoAPI &api, std::string name) {
    std::string pid;
    std::string ret;

    Say() << "Start container: " << name << std::endl;

    (void)api.Start(name);
    ExpectApiSuccess(api.GetData(name, "state", ret));
    Expect(ret == "dead" || ret == "running");
}

static void PauseResume(TPortoAPI &api, const std::string &name) {
    Say() << "PauseResume container: " << name << std::endl;

    std::string ret;
    int err = api.Pause(name);
    if (err) {
        ExpectApiSuccess(api.GetData(name, "state", ret));
        if (ret == "dead")
            return;
        else if (ret != "paused")
            throw "Can't pause, invalid state " + ret;
    }
    usleep(1000000);
    err = api.Resume(name);
    if (err) {
        ExpectApiSuccess(api.GetData(name, "state", ret));
        if (ret == "dead" || ret == "running")
            return;
        else
            throw "Can't resume, invalid state " + ret;
    }
}

static void WaitDead(TPortoAPI &api, std::string name, std::string timeout) {
    std::string pid;
    std::string ret;
    int t;

    Say() << "WaitDead container: " << name << std::endl;

    StringToInt(timeout, t);
    while (t--) {
        ExpectApiSuccess(api.GetData(name, "state", ret));
        Say() << "Poll " << name << ": "<< ret << std::endl;
        if (ret == "dead")
            return;
        PauseResume(api, name);
        usleep(1000000);
    }
    done++;
    throw std::string("Timeout");
}

static void CheckStdout(TPortoAPI &api, std::string name, std::string stream) {
    std::string ret;

    Say() << "CheckStdout container: " << name << std::endl;

    api.GetData(name, "stdout", ret);
    ExpectEq(ret, stream);
}

static void CheckStderr(TPortoAPI &api, std::string name, std::string stream) {
    std::string ret;

    Say() << "CheckStderr container: " << name << std::endl;

    api.GetData(name, "stderr", ret);
    ExpectEq(ret, stream);
}

static void CheckExit(TPortoAPI &api, std::string name, std::string stream) {
    std::string ret;
    Say() << "CheckExit container: " << name << std::endl;
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, stream);
}

static void Destroy(TPortoAPI &api, const std::string &name, const std::string &cwd) {
    std::vector<std::string> containers;

    Say() << "Destroy container: " << name << std::endl;

    ExpectApiSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) != containers.end());
    auto error = (EError)api.Destroy(name);
    // portod may be killed during invocation of destroy (so it might or might
    // not destroy the container), expect either success (if portod was
    // killed before it had time to remove container) or error (if portod
    // finished removal but didn't have time to send ack to the user)
    Expect(error == EError::Success || error == EError::ContainerDoesNotExist);
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) == containers.end());

    if (cwd.length()) {
        TFolder f(cwd);
        f.Remove(true);
    }
}

static void Tasks(int n, int iter) {
    tid = n;
    Say() << "Run task" << std::to_string(n) << std::endl;
    usleep(10000 * n);
    try {
        TPortoAPI api(config().rpc_sock().file().path(), retries);
        for (; iter; iter--) {
            if (iter % 10 == 0)
                Say() << std::to_string(iter) << " iterations left" << std::endl;
            for (unsigned int t = 0; t < vtasks.size(); t++) {
                std::string name = "stresstest" + std::to_string(n) + "_" + std::to_string(t);
                if (vtasks[t].find("name") != vtasks[t].end())
                    name = vtasks[t]["name"];

                std::string parent;
                if (vtasks[t].find("parent") != vtasks[t].end()) {
                    parent = vtasks[t]["parent"] + std::to_string(n) + "_" + std::to_string(t);
                    name = parent + "/" + name;
                }

                if (parent.length())
                    Create(api, parent, "");

                std::string cwd = "/tmp/stresstest/" + name;
                Create(api, name, cwd);
                SetProperty(api, name, "env", vtasks[t]["env"]);
                SetProperty(api, name, "command", vtasks[t]["command"]);
                SetProperty(api, name, "cwd", cwd);
                Start(api, name);
                WaitDead(api, name, vtasks[t]["timeout"]);
                CheckExit(api, name, vtasks[t]["exit_status"]);
                CheckStdout(api, name, vtasks[t]["stdout"]);
                CheckStderr(api, name, vtasks[t]["stderr"]);
                Destroy(api, name, cwd);

                if (parent.length())
                    Destroy(api, parent, "");
            }
        }
    } catch (std::string e) {
        Say() << "ERROR: Exception " << e << std::endl;
        Say() << "ERROR: Stop task" << std::to_string(n) << std::endl;
        abort();
    }
    Say() << "Stop task" << std::to_string(n) << std::endl;
}

static void StressKill() {
    TPortoAPI api(config().rpc_sock().file().path(), retries);
    std::cout << "Run kill" << std::endl;
    while (!done) {
        usleep(1000000);
        TFile f(config().slave_pid().path());
        int pid;
        std::vector<std::string> containers;
        if (api.List(containers) != 0)
            continue;
        if (f.AsInt(pid))
            Say(std::cerr) << "ERROR: Don't open " << config().slave_pid().path() << std::endl;
        if (kill(pid, SIGKILL)) {
            Say(std::cerr) << "ERROR: Don't send kill to " << pid << std::endl;
        } else {
            std::cout << "[-] Killed " << pid << std::endl;
        }
    }
}

int StressTest(int threads, int iter, bool killPorto) {
    int i;
    std::vector<std::thread> thrTasks;
    std::thread thrKill;

    if (threads < 0)
        threads = vtasks.size();

    try {
        (void)signal(SIGPIPE, SIG_IGN);

        config.Load();
        TPortoAPI api(config().rpc_sock().file().path(), retries);
        RestartDaemon(api);

        for (i = 1; i <= threads; i++)
            thrTasks.push_back(std::thread(Tasks, i, iter));
        if (killPorto)
            thrKill = std::thread(StressKill);
        for (auto& th : thrTasks)
            th.join();
        done++;
        if (killPorto)
            thrKill.join();

        TestDaemon(api);
    } catch (std::string e) {
        std::cerr << "ERROR: " << e << std::endl;
        abort();
    }

    std::cout << "Test completed!" << std::endl;

    return 0;
}
}
