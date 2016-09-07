#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "device.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/mount.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <grp.h>
#include <linux/kdev_t.h>
#include <net/if.h>
#include <linux/capability.h>
}

using std::stringstream;
using std::string;
using std::vector;
using std::map;

// TTask
//

TTask::TTask(std::unique_ptr<TTaskEnv> &env) : Env(std::move(env)) {}

TTask::TTask(pid_t pid) : Pid(pid) {}

void TTask::ReportPid(pid_t pid) const {
    TError error = Env->Sock.SendPid(pid);
    if (error) {
        L_ERR() << error << std::endl;
        Abort(error);
    }
    Env->ReportStage++;
}

void TTask::Abort(const TError &error) const {
    TError error2;

    /*
     * stage0: RecvPid WPid
     * stage1: RecvPid VPid
     * stage2: RecvError
     */
    L() << "abort due to " << error << std::endl;

    for (int stage = Env->ReportStage; stage < 2; stage++) {
        error2 = Env->Sock.SendPid(getpid());
        if (error2)
            L_ERR() << error2 << std::endl;
    }

    error2 = Env->Sock.SendError(error);
    if (error2)
        L_ERR() << error2 << std::endl;

    _exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    SetProcessName("portod-spawn-c");
    TTask *task = static_cast<TTask*>(arg);
    task->StartChild();
    return EXIT_FAILURE;
}

TError TTask::ChildApplyCapabilities() {
    uint64_t effective, permitted, inheritable;

    if (!Env->Cred.IsRootUser())
        return TError::Success();

    effective = permitted = -1;
    inheritable = Env->Caps;

    TError error = SetCap(effective, permitted, inheritable);
    if (error)
        return error;

    for (int i = 0; i <= LastCapability; i++) {
        if (!(Env->Caps & (1ULL << i)) && i != CAP_SETPCAP) {
            TError error = DropBoundedCap(i);
            if (error)
                return error;
        }
    }

    if (!(Env->Caps & (1ULL << CAP_SETPCAP))) {
        TError error = DropBoundedCap(CAP_SETPCAP);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildDropPriveleges() {
    if (setgid(Env->Cred.Gid) < 0)
        return TError(EError::Unknown, errno, "setgid()");

    if (setgroups(Env->Cred.Groups.size(), Env->Cred.Groups.data()) < 0)
        return TError(EError::Unknown, errno, "setgroups()");

    if (setuid(Env->Cred.Uid) < 0)
        return TError(EError::Unknown, errno, "setuid()");

    return TError::Success();
}

TError TTask::ChildExec() {

    /* set environment for wordexp */
    TError error = Env->Env.Apply();

    auto envp = Env->Env.Envp();
    auto fd = Env->PortoInitFd.GetFd();

    if (Env->Command.empty()) {
        const char *args[] = {
            "portoinit",
            "--container",
            Env->Container.c_str(),
            NULL,
        };
        SetDieOnParentExit(0);
        CloseFds(-1, {fd});
        fexecve(Env->PortoInitFd.GetFd(), (char *const *)args, envp);
        return TError(EError::InvalidValue, errno, "fexecve(" +
                      std::to_string(Env->PortoInitFd.GetFd()) +  ", portoinit)");
    }

    wordexp_t result;

    int ret = wordexp(Env->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        return TError(EError::Unknown, EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        return TError(EError::Unknown, EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        return TError(EError::Unknown, EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        return TError(EError::Unknown, EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        return TError(EError::Unknown, EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    if (Verbose) {
        L() << "command=" << Env->Command << std::endl;
        for (unsigned i = 0; result.we_wordv[i]; i++)
            L() << "argv[" << i << "]=" << result.we_wordv[i] << std::endl;
        for (unsigned i = 0; envp[i]; i++)
            L() << "environ[" << i << "]=" << envp[i] << std::endl;
    }
    SetDieOnParentExit(0);
    fd = Env->Sock.GetFd();
    CloseFds(-1, {0, 1, 2, fd});
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, envp);

    return TError(EError::InvalidValue, errno, string("execvpe(") + result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ")");
}

TError TTask::ChildBindDns() {
    vector<string> files = { "/etc/hosts", "/etc/resolv.conf" };

    for (auto &file : files) {
        TPath path = Env->Root / file;
        TError error;

        error = path.CreateAll(0600);
        if (!error)
            error = path.BindRemount(file, MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildBindDirectores() {
    for (auto &bindMap : Env->BindMap) {
        TPath src, dest;

        if (bindMap.Source.IsAbsolute())
            src = bindMap.Source;
        else
            src = Env->ParentCwd / bindMap.Source;

        if (bindMap.Dest.IsAbsolute())
            dest = Env->Root / bindMap.Dest;
        else
            dest = Env->Root / Env->Cwd / bindMap.Dest;

        if (!StringStartsWith(dest.RealPath().ToString(), Env->Root.ToString()))
            return TError(EError::InvalidValue, "Container bind mount "
                          + src.ToString() + " resolves to root "
                          + dest.RealPath().ToString()
                          + " (" + Env->Root.ToString() + ")");

        TError error;
        if (src.IsDirectoryFollow())
            error = dest.MkdirAll(0755);
        else
            error = dest.CreateAll(0600);
        if (error)
            return error;

        // Drop nosuid,noexec,nodev
        error = dest.BindRemount(src, bindMap.Rdonly ? MS_RDONLY : 0);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildRemountRootRo() {
    if (!Env->RootRdOnly || Env->LoopDev >= 0)
        return TError::Success();

    // remount everything except binds to ro
    std::vector<std::shared_ptr<TMount>> snapshot;
    TError error = TMount::Snapshot(snapshot);
    if (error)
        return error;

    for (auto mnt : snapshot) {
        TPath path = Env->Root.InnerPath(mnt->GetMountpoint());
        if (path.IsEmpty())
            continue;

        bool skip = false;
        for (auto &bindMap : Env->BindMap) {
            TPath dest = bindMap.Dest;

            if (dest.NormalPath() == path.NormalPath()) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;

        error = mnt->GetMountpoint().Remount(MS_REMOUNT | MS_BIND | MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountRun() {
    TPath run = Env->Root / "run";
    std::vector<std::string> run_paths, subdirs;
    std::vector<struct stat> run_paths_stat;
    TError error;

    if (run.Exists()) {
        error = run.ListSubdirs(subdirs);
        if (error)
            return error;
    }

    /* We want to recreate /run dir tree with up to RUN_SUBDIR_LIMIT nodes */
    if (subdirs.size() >= RUN_SUBDIR_LIMIT)
        return TError(EError::Unknown, "Too many subdirectories in /run!");

    run_paths.reserve(RUN_SUBDIR_LIMIT);
    for (const auto &i: subdirs) {

        /* Skip creating special directories, we'll do it later */
        if (i == "shm" || i == "lock")
            continue;

        run_paths.push_back(i);
    }

    for (auto i = run_paths.begin(); i != run_paths.end(); ++i) {
        auto current = *i;
        TPath current_path = run / current;

        error = current_path.ListSubdirs(subdirs);
        if (error)
            return error;

        if (subdirs.size() + run_paths.size() >= RUN_SUBDIR_LIMIT)
            return TError(EError::Unknown, "Too many subdirectories in /run!");

        for (auto dir : subdirs)
            run_paths.push_back(current + "/" + dir);
    }

    for (const auto &i: run_paths) {
        struct stat current_stat;
        TPath current_path = run / i;

        error = current_path.StatStrict(current_stat);
        if (error)
            return error;

        run_paths_stat.push_back(current_stat);
    }

    error = run.MkdirAll(0755);
    if (!error)
        error = run.Mount("tmpfs", "tmpfs",
                MS_NOSUID | MS_NODEV | MS_STRICTATIME,
                { "mode=755", "size=32m"});
    if (error)
        return error;

    for (unsigned int i = 0; i < run_paths.size(); i++) {
        TPath current = run / run_paths[i];
        auto &current_stat = run_paths_stat[i];
        mode_t mode = current_stat.st_mode;

        /* forbid other-writable directory without sticky bit */
        if ((mode & 01002) == 02) {
            L() << "Other writable without sticky: " << current << std::endl;
            mode &= ~02;
        }

        error = current.Mkdir(mode);
        if (error)
            return error;

        error = current.Chown(current_stat.st_uid, current_stat.st_gid);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountRootFs() {
    TError error;

    if (Env->Root.IsRoot())
        return TError::Success();

    if (Env->LoopDev >= 0)
        error = Env->Root.Mount("/dev/loop" + std::to_string(Env->LoopDev),
                                "ext4", Env->RootRdOnly ? MS_RDONLY : 0, {});
    else
        error = Env->Root.Bind(Env->Root);
    if (error)
        return error;

    struct {
        std::string target;
        std::string type;
        unsigned long flags;
        std::vector<std::string> opts;
    } mounts[] = {
        { "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME,
            { "mode=755", "size=32m" } },
        { "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
            { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5" }},
        { "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, {}},
        { "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {}},
    };

    for (auto &m : mounts) {
        TPath target = Env->Root + m.target;
        error = target.MkdirAll(0755);
        if (!error)
            error = target.Mount(m.type, m.type, m.flags, m.opts);
        if (error)
            return error;
    }

    error = ChildMountRun();
    if (error)
        return error;

    struct {
        const std::string path;
        mode_t mode;
    } dirs[] = {
        { "/run/lock",  01777 },
        { "/run/shm",   01777 },
    };

    for (auto &d : dirs) {
        error = (Env->Root + d.path).Mkdir(d.mode);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        mode_t mode;
        dev_t dev;
    } nodes[] = {
        { "/dev/null",    0666 | S_IFCHR, MKDEV(1, 3) },
        { "/dev/zero",    0666 | S_IFCHR, MKDEV(1, 5) },
        { "/dev/full",    0666 | S_IFCHR, MKDEV(1, 7) },
        { "/dev/random",  0666 | S_IFCHR, MKDEV(1, 8) },
        { "/dev/urandom", 0666 | S_IFCHR, MKDEV(1, 9) },
        { "/dev/tty",     0666 | S_IFCHR, MKDEV(5, 0) },
        { "/dev/console", 0600 | S_IFCHR, MKDEV(1, 3) }, /* to /dev/null */
    };

    for (auto &n : nodes) {
        error = (Env->Root + n.path).Mknod(n.mode, n.dev);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        const std::string target;
    } symlinks[] = {
        { "/dev/ptmx", "pts/ptmx" },
        { "/dev/fd", "/proc/self/fd" },
        { "/dev/stdin", "/proc/self/fd/0" },
        { "/dev/stdout", "/proc/self/fd/1" },
        { "/dev/stderr", "/proc/self/fd/2" },
        { "/dev/shm", "../run/shm" },
    };

    for (auto &s : symlinks) {
        error = (Env->Root + s.path).Symlink(s.target);
        if (error)
            return error;
    }

    for (auto &dev: Env->Devices) {
        error = dev.Makedev(Env->Root);
        if (error)
            return error;
    }

    std::vector<std::string> proc_ro = {
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
    };

    if (!Env->Cred.IsRootUser())
        proc_ro.push_back("/proc/sys");

    for (auto &p : proc_ro) {
        TPath path = Env->Root + p;
        error = path.BindRemount(path, MS_RDONLY);
        if (error)
            return error;
    }

    TPath proc_kcore = Env->Root + "/proc/kcore";
    error = proc_kcore.BindRemount(Env->Root + "/dev/null", MS_RDONLY);
    if (error)
        return error;

    if (Env->BindDns) {
        error = ChildBindDns();
        if (error)
            return error;
    } else if (Env->ResolvConf.length()) {
        TPath resolvconf = Env->Root + "/etc/resolv.conf";
        if (!resolvconf.IsRegularStrict()) {
            if (!resolvconf.Exists())
                error = resolvconf.Mkfile(0644);
            else
                error = TError(EError::InvalidState, "non-regular file");
        }
        if (!error)
            error = resolvconf.WriteAll(Env->ResolvConf);
        if (error)
            return TError(error, "cannot write /etc/resolv.conf");
    }

    return TError::Success();
}

TError TTask::ChildIsolateFs() {

    if (Env->Root.IsRoot())
        return TError::Success();

    TError error = Env->Root.PivotRoot();
    if (error) {
        L_WRN() << "Can't pivot root, roll back to chroot: " << error << std::endl;

        error = Env->Root.Chroot();
        if (error)
            return error;
    }

    // Allow suid binaries and device nodes at container root.
    error = TPath("/").Remount(MS_REMOUNT | MS_BIND |
                               (Env->RootRdOnly ? MS_RDONLY : 0));
    if (error) {
        L_ERR() << "Can't remount / as suid and dev:" << error << std::endl;
        return error;
    }

    TPath newRoot("/");
    return newRoot.Chdir();
}

TError TTask::ChildApplyLimits() {
    for (auto pair : Env->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                          "setrlimit(" + std::to_string(pair.first) +
                          ", " + std::to_string(pair.second.rlim_cur) +
                          ":" + std::to_string(pair.second.rlim_max) + ")");
    }

    return TError::Success();
}

TError TTask::ChildSetHostname() {
    if (Env->Hostname == "")
        return TError::Success();

    if (Env->SetEtcHostname) {
        TPath path("/etc/hostname");
        if (path.Exists()) {
            TError error = path.WriteAll(Env->Hostname + "\n");
            if (error)
                return error;
        }
    }

    return SetHostName(Env->Hostname);
}

TError TTask::ConfigureChild() {
    TError error;

    /* Die together with waiter */
    if (Env->TripleFork)
        SetDieOnParentExit(SIGKILL);

    error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (Env->NewMountNs) {
        // Remount to slave to receive propogations from parent namespace
        error = TPath("/").Remount(MS_SLAVE | MS_REC);
        if (error)
            return error;
    }

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TPath tmpProc("/proc");
        error = tmpProc.UmountAll();
        if (error)
            return error;
        error = tmpProc.Mount("proc", "proc",
                              MS_NOEXEC | MS_NOSUID | MS_NODEV, {});
        if (error)
            return error;
    }

    error = ChildMountRootFs();
    if (error)
        return error;

    error = ChildBindDirectores();
    if (error)
        return error;

    error = ChildRemountRootRo();
    if (error)
        return error;

    error = ChildIsolateFs();
    if (error)
        return error;

    error = ChildSetHostname();
    if (error)
        return error;

    error = Env->Cwd.Chdir();
    if (error)
        return error;

    if (Env->NewMountNs) {
        // Make all shared: subcontainers will get propgation from us
        error = TPath("/").Remount(MS_SHARED | MS_REC);
        if (error)
            return error;
    }

    error = ChildApplyCapabilities();
    if (error)
        return error;

    if (Env->QuadroFork) {
        Pid = fork();
        if (Pid < 0)
            return TError(EError::Unknown, errno, "fork()");

        if (Pid) {
            auto fd = Env->PortoInitFd.GetFd();
            auto pid_ = std::to_string(Pid);
            const char * argv[] = {
                "portoinit",
                "--container",
                Env->Container.c_str(),
                "--wait",
                pid_.c_str(),
                NULL,
            };
            auto envp = Env->Env.Envp();

            CloseFds(-1, { fd } );
            fexecve(fd, (char *const *)argv, envp);
            return TError(EError::Unknown, errno, "fexecve()");
        } else {
            Pid = getpid();

            Env->MasterSock2.Close();

            error = Env->Sock2.SendPid(Pid);
            if (error)
                return error;
            error = Env->Sock2.RecvZero();
            if (error)
                return error;
            /* Parent forwards VPid */
            Env->ReportStage++;

            Env->Sock2.Close();

            if (setsid() < 0)
                return TError(EError::Unknown, errno, "setsid()");
        }
    }

    error = ChildDropPriveleges();
    if (error)
        return error;

    error = Env->Stdin.OpenInChild(Env->Cred);
    if (error)
        return error;

    error = Env->Stdout.OpenInChild(Env->Cred);
    if (error)
        return error;

    error = Env->Stderr.OpenInChild(Env->Cred);
    if (error)
        return error;

    return TError::Success();
}

TError TTask::WaitAutoconf() {
    if (Env->Autoconf.empty())
        return TError::Success();

    SetProcessName("portod-autoconf");

    auto sock = std::make_shared<TNl>();
    TError error = sock->Connect();
    if (error)
        return error;

    for (auto &name: Env->Autoconf) {
        TNlLink link(sock, name);

        error = link.Load();
        if (error)
            return error;

        error = link.WaitAddress(config().network().autoconf_timeout_s());
        if (error)
            return error;
    }

    return TError::Success();
}

void TTask::StartChild() {
    TError error;

    /* WPid reported by parent */
    Env->ReportStage++;

    /* Wait for report WPid in parent */
    error = Env->Sock.RecvZero();
    if (error)
        Abort(error);

    /* Report VPid in pid namespace we're enter */
    if (!Env->Isolate)
        ReportPid(getpid());
    else if (!Env->QuadroFork)
        Env->ReportStage++;

    /* Apply configuration */
    error = ConfigureChild();
    if (error)
        Abort(error);

    /* Wait for Wakeup */
    error = Env->Sock.RecvZero();
    if (error)
        Abort(error);

    /* Reset signals before exec, signal block already lifted */
    ResetIgnoredSignals();

    error = WaitAutoconf();
    if (error)
        Abort(error);

    error = ChildExec();
    Abort(error);
}

TError TTask::Start() {
    TError error;

    Pid = VPid = WPid = 0;
    ExitStatus = 0;

    error = TUnixSocket::SocketPair(Env->MasterSock, Env->Sock);
    if (error)
        return error;


    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    pid_t forkPid = ForkFromThread();
    if (forkPid < 0) {
        Env->Sock.Close();
        TError error(EError::Unknown, errno, "fork()");
        L() << "Can't spawn child: " << error << std::endl;
        return error;
    } else if (forkPid == 0) {
        TError error;

        /* Switch from signafd back to normal signal delivery */
        ResetBlockedSignals();

        SetDieOnParentExit(SIGKILL);

        SetProcessName("portod-spawn-p");

        char stack[8192];

        (void)setsid();

        // move to target cgroups
        for (auto &cg : Env->Cgroups) {
            error = cg.Attach(getpid());
            if (error)
                Abort(error);
        }

        /* Default stdin/stdio/stderr are in host mount namespace */
        error = Env->Stdin.OpenOnHost(Env->Cred);
        if (error)
            Abort(error);

        error = Env->Stdout.OpenOnHost(Env->Cred);
        if (error)
            Abort(error);

        error = Env->Stderr.OpenOnHost(Env->Cred);
        if (error)
            Abort(error);

        /* Enter parent namespaces */
        error = Env->ParentNs.Enter();
        if (error)
            Abort(error);

        if (Env->TripleFork) {
            /*
             * Enter into pid-namespace. fork() hangs in libc if child pid
             * collide with parent pid outside. vfork() has no such problem.
             */
            forkPid = vfork();
            if (forkPid < 0)
                Abort(TError(EError::Unknown, errno, "fork()"));

            if (forkPid)
                _exit(EXIT_SUCCESS);
        }

        if (Env->QuadroFork) {
            error = TUnixSocket::SocketPair(Env->MasterSock2, Env->Sock2);
            if (error)
                Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (Env->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (Env->NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        /* Create UTS namspace if hostname is changed or isolate=true */
        if (Env->Isolate || Env->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);

        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            Abort(error);
        }

        /* Report WPid in host pid namespace */
        if (Env->TripleFork)
            ReportPid(GetTid());
        else
            ReportPid(clonePid);

        /* Report VPid in parent pid namespace for new pid-ns */
        if (Env->Isolate && !Env->QuadroFork)
            ReportPid(clonePid);

        /* WPid reported, wakeup child */
        error = Env->MasterSock.SendZero();
        if (error)
            Abort(error);

        /* ChildCallback() reports VPid here if !Env->Isolate */
        if (!Env->Isolate && !Env->QuadroFork)
            Env->ReportStage++;

        /*
         * QuadroFork waiter receives application VPid from init
         * task and forwards it into host.
         */
        if (Env->QuadroFork) {
            pid_t appPid, appVPid;

            /* close other side before reading */
            Env->Sock2.Close();

            error = Env->MasterSock2.RecvPid(appPid, appVPid);
            if (error)
                Abort(error);
            /* Forward VPid */
            ReportPid(appPid);
            error = Env->MasterSock2.SendZero();
            if (error)
                Abort(error);

            Env->MasterSock2.Close();
        }

        if (Env->TripleFork) {
            auto fd = Env->PortoInitFd.GetFd();
            auto pid = std::to_string(clonePid);
            const char * argv[] = {
                "portoinit",
                "--container",
                Env->Container.c_str(),
                "--wait",
                pid.c_str(),
                NULL,
            };
            auto envp = Env->Env.Envp();

            CloseFds(-1, { fd } );
            fexecve(fd, (char *const *)argv, envp);
            kill(clonePid, SIGKILL);
            _exit(EXIT_FAILURE);
        }

        _exit(EXIT_SUCCESS);
    }

    Env->Sock.Close();

    error = Env->MasterSock.SetRecvTimeout(config().container().start_timeout_ms());
    if (error)
        goto kill_all;

    error = Env->MasterSock.RecvPid(WPid, VPid);
    if (error)
        goto kill_all;

    error = Env->MasterSock.RecvPid(Pid, VPid);
    if (error)
        goto kill_all;

    int status;
    if (waitpid(forkPid, &status, 0) < 0) {
        error = TError(EError::Unknown, errno, "wait for middle task failed");
        goto kill_all;
    }
    forkPid = 0;

    /* Task was alive, even if it already died we'll get zombie */
    error = Env->MasterSock.SendZero();
    if (error)
        L() << "Task wakeup error: " << error << std::endl;

    /* Prefer reported error if any */
    error = Env->MasterSock.RecvError();
    if (error)
        goto kill_all;

    if (!error && status) {
        error = TError(EError::Unknown, "Start failed, status " + std::to_string(status));
        goto kill_all;
    }

    ClearEnv();
    State = Started;
    return TError::Success();

kill_all:
    L_ACT() << "Kill partialy constructed container: " << error << std::endl;
    for (auto &cg : Env->Cgroups)
        (void)cg.KillAll(SIGKILL);
    if (forkPid) {
        (void)kill(forkPid, SIGKILL);
        (void)waitpid(forkPid, nullptr, 0);
    }
    Pid = VPid = WPid = 0;
    ExitStatus = -1;
    return error;
}

pid_t TTask::GetPid() const {
    return Pid;
}

pid_t TTask::GetWPid() const {
    return WPid;
}

std::vector<int> TTask::GetPids() const {
    return {Pid, VPid, WPid};
}

pid_t TTask::GetPidFor(pid_t pid) const {
    if (InPidNamespace(pid, getpid()))
        return Pid;
    if (WPid != Pid && InPidNamespace(pid, WPid))
        return VPid;
    if (InPidNamespace(pid, Pid))
        return 1;
    return 0;
}

bool TTask::IsRunning() const {
    return State == Started;
}

int TTask::GetExitStatus() const {
    return ExitStatus;
}

void TTask::Exit(int status) {
    ExitStatus = status;
    State = Stopped;
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        throw "Tried to kill invalid process!";

    L_ACT() << "kill " << signal << " " << Pid << std::endl;

    int ret = kill(Pid, signal);
    if (ret != 0)
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");

    return TError::Success();
}

bool TTask::IsZombie() const {
    std::vector<std::string> lines;
    TError err = TPath("/proc/" + std::to_string(Pid) + "/status").ReadLines(lines);
    if (err)
        return false;

    for (auto &l : lines)
        if (l.compare(0, 7, "State:\t") == 0)
            return l.substr(7, 1) == "Z";

    return false;
}

bool TTask::HasCorrectParent() {
    pid_t ppid;
    TError error = GetTaskParent(WPid, ppid);
    if (error) {
        L() << "Can't get ppid of restored task: " << error << std::endl;
        return false;
    }

    if (ppid != getppid()) {
        L() << "Invalid ppid of restored task: " << ppid << " != " << getppid() << std::endl;
        return false;
    }

    return true;
}

void TTask::Restore(std::vector<int> pids) {
    ExitStatus = 0;
    Pid = pids[0];
    VPid = pids[1];
    WPid = pids[2];
    State = Started;
}

void TTask::ClearEnv() {
    Env = nullptr;
}

TTask::~TTask() {
}
