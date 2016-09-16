#pragma once

#include <memory>
#include <string>
#include <mutex>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/locks.hpp"
#include "util/namespace.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

class TNetworkDevice {
public:
    std::string Name;
    std::string Type;
    int Index;
    bool Managed;
    bool Prepared;
    bool Missing;
};

class TNetwork : public std::enable_shared_from_this<TNetwork>,
                 public TNonCopyable,
                 public TLockable {
    std::shared_ptr<TNl> Nl;
    struct nl_sock *GetSock() const { return Nl->GetSock(); }

    unsigned IfaceName = 0;

public:
    std::vector<TNetworkDevice> Devices;

    TError PrepareDevice(TNetworkDevice &dev);
    TError RefreshDevices();
    TError RefreshClasses(bool force);

    bool NewManagedDevices = false;

    int DeviceIndex(const std::string &name) {
        for (auto dev: Devices)
            if (dev.Name == name)
                return dev.Index;
        return 0;
    }

    TNlAddr NatBaseV4;
    TNlAddr NatBaseV6;
    TIdMap NatBitmap;

    TNetwork();
    ~TNetwork();
    TError Connect();
    TError ConnectNetns(TNamespaceFd &netns);
    TError ConnectNew(TNamespaceFd &netns);
    std::shared_ptr<TNl> GetNl() { return Nl; };

    TError Destroy();

    TError GetTrafficCounters(int minor, ETclassStat stat,
                              std::map<std::string, uint64_t> &result);

    TError GetInterfaceCounters(ETclassStat stat,
                                std::map<std::string, uint64_t> &result);

    TError UpdateTrafficClasses(int parent, int minor,
            std::map<std::string, uint64_t> &Prio,
            std::map<std::string, uint64_t> &Rate,
            std::map<std::string, uint64_t> &Ceil);
    TError RemoveTrafficClasses(int minor);

    TError AddTrafficClass(int ifIndex, uint32_t parent, uint32_t handle,
                           uint64_t prio, uint64_t rate, uint64_t ceil);
    TError DelTrafficClass(int ifIndex, uint32_t handle);

    TError GetGateAddress(std::vector<TNlAddr> addrs,
                          TNlAddr &gate4, TNlAddr &gate6, int &mtu);
    TError AddAnnounce(const TNlAddr &addr, std::string master);
    TError DelAnnounce(const TNlAddr &addr);

    TError GetNatAddress(std::vector <TNlAddr> &addrs);
    TError PutNatAddress(const std::vector <TNlAddr> &addrs);

    std::string GetIfaceName(const std::string &prefix);
    std::string MatchIface(const std::string &pattern);

    static void AddNetwork(ino_t inode, std::shared_ptr<TNetwork> &net);
    static std::shared_ptr<TNetwork> GetNetwork(ino_t inode);

    static void InitializeUnmanagedDevices();

    static void RefreshNetworks();
};


struct THostNetCfg {
    std::string Dev;
};

struct TMacVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Type;
    std::string Hw;
    int Mtu;
};

struct TIpVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Mode;
    int Mtu;
};

struct TIpVec {
    std::string Iface;
    TNlAddr Addr;
};

struct TGwVec {
    std::string Iface;
    TNlAddr Addr;
};

struct TVethNetCfg {
    std::string Bridge;
    std::string Name;
    std::string Hw;
    std::string Peer;
    int Mtu;
};

struct TL3NetCfg {
    std::string Name;
    std::string Master;
    int Mtu;
    std::vector<TNlAddr> Addrs;
    bool Nat;
};

class TContainerHolder;
class TContainer;

struct TNetCfg {
    std::shared_ptr<TContainerHolder> Holder;
    std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TNetwork> ParentNet;
    std::shared_ptr<TNetwork> Net;
    unsigned Id;
    unsigned ParentId;
    TCred OwnerCred;
    bool NewNetNs;
    bool Inherited;
    bool Host;
    bool NetUp;
    bool SaveIp;
    std::string Hostname;
    std::vector<THostNetCfg> HostIface;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TIpVlanNetCfg> IpVlan;
    std::vector<TVethNetCfg> Veth;
    std::vector<TL3NetCfg> L3lan;
    std::string NetNsName;
    std::string NetCtName;
    std::vector<TGwVec> GwVec;
    std::vector<TIpVec> IpVec;
    std::vector<std::string> Autoconf;

    TNamespaceFd NetNs;

    void Reset();
    TError ParseNet(std::vector<std::string> lines);
    TError ParseIp(std::vector<std::string> lines);
    TError FormatIp(std::vector<std::string> &lines);
    TError ParseGw(std::vector<std::string> lines);
    std::string GenerateHw(const std::string &name);
    TError ConfigureVeth(TVethNetCfg &veth);
    TError ConfigureL3(TL3NetCfg &l3);
    TError ConfigureInterfaces();
    TError PrepareNetwork();
    TError DestroyNetwork();
};

extern std::shared_ptr<TNetwork> HostNetwork;
