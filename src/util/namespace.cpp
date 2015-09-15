#include "namespace.hpp"
#include "util/file.hpp"

using std::pair;
using std::string;

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}

/* returns true if two processes are in one pid namespace */
bool InPidNamespace(pid_t pid1, pid_t pid2) {
    struct stat st1, st2;

    if (stat(("/proc/" + std::to_string(pid1) + "/ns/pid").c_str(), &st1) ||
        stat(("/proc/" + std::to_string(pid2) + "/ns/pid").c_str(), &st2))
        return false;

    return st1.st_ino == st2.st_ino;
}

TError TNamespaceFd::Open(TPath path) {
    Close();
    Fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (Fd < 0)
        return TError(EError::Unknown, errno, "Cannot open " + path.ToString());
    return TError::Success();
}

TError TNamespaceFd::Open(pid_t pid, std::string type) {
    return Open("/proc/" + std::to_string(pid) + "/" + type);
}

void TNamespaceFd::Close() {
    if (Fd >= 0) {
        close(Fd);
        Fd = -1;
    }
}

TError TNamespaceFd::SetNs(int type) const {
    if (Fd >= 0 && setns(Fd, type))
        return TError(EError::Unknown, errno, "Cannot set namespace");
    return TError::Success();
}

TError TNamespaceFd::Chdir() const {
    if (Fd >= 0 && fchdir(Fd))
        return TError(EError::Unknown, errno, "Cannot change cwd");
    return TError::Success();
}

TError TNamespaceFd::Chroot() const {
    if (Fd >= 0 && (fchdir(Fd) || chroot(".")))
        return TError(EError::Unknown, errno, "Cannot change root");
    return TError::Success();
}

bool TNamespaceFd::operator==(const TNamespaceFd &other) const {
    struct stat a, b;

    if (Fd < 0 || other.Fd < 0 || fstat(Fd, &a) || fstat(other.Fd, &b))
        return false;

    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool TNamespaceFd::operator!=(const TNamespaceFd &other) const {
    return !(*this == other);
}

TError TNamespaceSnapshot::Open(pid_t pid) {
    TError error;

    error = Ipc.Open(pid, "ns/ipc");
    if (error)
        return error;
    error = Uts.Open(pid, "ns/uts");
    if (error)
        return error;
    error = Net.Open(pid, "ns/net");
    if (error)
        return error;
    error = Pid.Open(pid, "ns/pid");
    if (error)
        return error;
    error = Mnt.Open(pid, "ns/mnt");
    if (error)
        return error;
    error = Root.Open(pid, "root");
    if (error)
        return error;
    error = Cwd.Open(pid, "cwd");
    if (error)
        return error;
    return TError::Success();
}

TError TNamespaceSnapshot::Enter() const {
    TError error;

    error = Ipc.SetNs(CLONE_NEWIPC);
    if (error)
        return error;
    error = Uts.SetNs(CLONE_NEWUTS);
    if (error)
        return error;
    error = Net.SetNs(CLONE_NEWNET);
    if (error)
        return error;
    error = Pid.SetNs(CLONE_NEWPID);
    if (error)
        return error;
    error = Mnt.SetNs(CLONE_NEWNS);
    if (error)
        return error;
    error = Root.Chroot();
    if (error)
        return error;
    error = Cwd.Chdir();
    if (error)
        return error;
    return TError::Success();
}
