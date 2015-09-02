#pragma once

#include <string>
#include <mutex>

#include "common.hpp"
#include "epoll.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

class TContainer;
class TContainerHolder;
class TContainerWaiter;
class TEpollLoop;

namespace rpc {
    class TContainerRequest;
}

enum class EClientState {
    ReadingLength,
    ReadingData,
};

class TClient : public TEpollSource {
public:
    TClient(std::shared_ptr<TEpollLoop> loop, int fd);
    ~TClient();

    int GetFd() const;
    pid_t GetPid() const;
    const TCred& GetCred() const;
    const std::string& GetComm() const;

    void BeginRequest();
    size_t GetRequestTime();

    TError Identify(TContainerHolder &holder, bool full = true);
    std::string GetContainerName() const;
    TError GetContainer(std::shared_ptr<TContainer> &container) const;

    friend std::ostream& operator<<(std::ostream& stream, TClient& client);

    std::shared_ptr<TContainerWaiter> Waiter;
    bool Readonly();

    bool ReadRequest(rpc::TContainerRequest &req, bool &hangup);
    bool ReadInterrupted();

private:
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    size_t RequestStartMs;

    TError LoadGroups();
    TError IdentifyContainer(TContainerHolder &holder);
    std::weak_ptr<TContainer> Container;

    bool FullLog = true;

    EClientState State;
    uint64_t Length;
    uint64_t Pos;
    TScopedMem Request;

    void SetState(EClientState state);
};
