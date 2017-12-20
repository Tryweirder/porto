#include <algorithm>

#include "rpc.hpp"
#include "config.hpp"
#include "version.hpp"
#include "property.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "event.hpp"
#include "protobuf.hpp"
#include "helpers.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "portod.hpp"
#include "storage.hpp"

extern "C" {
#include <sys/stat.h>
}

static std::string RequestAsString(const rpc::TContainerRequest &req) {
    if (req.has_create())
        return std::string("create ") + req.create().name();
    else if (req.has_createweak())
        return std::string("create weak ") + req.createweak().name();
    else if (req.has_destroy())
        return "destroy " + req.destroy().name();
    else if (req.has_list())
        return "list containers";
    else if (req.has_getproperty())
        return "pget "  + req.getproperty().name() + " " + req.getproperty().property();
    else if (req.has_setproperty())
        return "pset " + req.setproperty().name() + " " +
                         req.setproperty().property() + " " +
                         req.setproperty().value();
    else if (req.has_getdata())
        return "dget " + req.getdata().name() + " " + req.getdata().data();
    else if (req.has_get()) {
        std::string ret = "get";

        for (int i = 0; i < req.get().name_size(); i++)
            ret += " " + req.get().name(i);

        if (req.get().name_size() && req.get().variable_size())
            ret += ",";

        for (int i = 0; i < req.get().variable_size(); i++)
            ret += " " + req.get().variable(i);

        return ret;
    } else if (req.has_start())
        return "start " + req.start().name();
    else if (req.has_stop())
        return "stop " + req.stop().name();
    else if (req.has_pause())
        return "pause " + req.pause().name();
    else if (req.has_resume())
        return "resume " + req.resume().name();
    else if (req.has_propertylist())
        return "list available properties";
    else if (req.has_datalist())
        return "list available data";
    else if (req.has_kill())
        return "kill " + req.kill().name() + " " + std::to_string(req.kill().sig());
    else if (req.has_version())
        return "get version";
    else if (req.has_wait()) {
        std::string ret = "wait";

        for (int i = 0; i < req.wait().name_size(); i++)
            ret += " " + req.wait().name(i);

        if (req.wait().has_timeout())
            ret += " timeout " + std::to_string(req.wait().timeout());

        return ret;
    } else if (req.has_createvolume()) {
        std::string ret = "create volume " + req.createvolume().path();
        for (auto p: req.createvolume().properties())
            ret += " " + p.name() + "=" + p.value();
        return ret;
    } else if (req.has_linkvolume())
        return "link volume " + req.linkvolume().path() + " to " +
                                    req.linkvolume().container();
    else if (req.has_unlinkvolume())
        return "unlink volume " + req.unlinkvolume().path() + " from " +
                                      req.unlinkvolume().container();
    else if (req.has_listvolumes())
        return "list volumes";
    else if (req.has_tunevolume()) {
        std::string ret = "tune volume " + req.tunevolume().path();
        for (auto p: req.tunevolume().properties())
            ret += " " + p.name() + "=" + p.value();
        return ret;
    } else if (req.has_convertpath())
        return "convert " + req.convertpath().path() +
            " from " + req.convertpath().source() +
            " to " + req.convertpath().destination();
    else if (req.has_attachprocess())
        return "attach " + std::to_string(req.attachprocess().pid()) +
            " (" + req.attachprocess().comm() + ") to " +
            req.attachprocess().name();
    else if (req.has_locateprocess())
        return "locate " + std::to_string(req.locateprocess().pid()) +
                " (" + req.locateprocess().comm() + ")";
    else
        return req.ShortDebugString();
}

static std::string ResponseAsString(const rpc::TContainerResponse &resp) {
    std::string ret;

    if (resp.error()) {
        ret = fmt::format("Error: {}:{}({})", resp.error(),
                          rpc::EError_Name(resp.error()), resp.errormsg());
    } else if (resp.has_list()) {
        for (int i = 0; i < resp.list().name_size(); i++)
            ret += resp.list().name(i) + " ";
    } else if (resp.has_propertylist()) {
        for (int i = 0; i < resp.propertylist().list_size(); i++)
            ret += resp.propertylist().list(i).name()
                + " (" + resp.propertylist().list(i).desc() + ")";
    } else if (resp.has_datalist()) {
        for (int i = 0; i < resp.datalist().list_size(); i++)
            ret += resp.datalist().list(i).name()
                + " (" + resp.datalist().list(i).desc() + ")";
    } else if (resp.has_volumelist()) {
        for (auto v: resp.volumelist().volumes())
            ret += v.path() + " ";
    } else if (resp.has_getproperty()) {
        ret = resp.getproperty().value();
    } else if (resp.has_getdata()) {
        ret = resp.getdata().value();
    } else if (resp.has_get()) {
        for (int i = 0; i < resp.get().list_size(); i++) {
            auto entry = resp.get().list(i);

            if (ret.length())
                ret += "; ";
            ret += entry.name() + ":";

            for (int j = 0; j < entry.keyval_size(); j++) {
                auto &val = entry.keyval(j);
                ret += " " + val.variable() + "=";
                if (val.has_error())
                    ret += fmt::format("{}:{}({})", val.error(),
                                       rpc::EError_Name(val.error()),
                                       val.errormsg());
                else if (val.has_value())
                    ret += val.value();
            }
        }
    } else if (resp.has_version()) {
        ret = resp.version().tag() + " #" + resp.version().revision();
    } else if (resp.has_wait()) {
        if (resp.wait().name().empty())
            ret = "Wait timeout";
        else
            ret = "Wait " + resp.wait().name();
    } else if (resp.has_convertpath())
        ret = resp.convertpath().path();
    else
        ret = "Ok";

    return ret;
}

/* not logged in normal mode */
static bool SilentRequest(const rpc::TContainerRequest &req) {
    return
        req.has_list() ||
        req.has_getproperty() ||
        req.has_getdata() ||
        req.has_get() ||
        req.has_propertylist() ||
        req.has_datalist() ||
        req.has_version() ||
        req.has_wait() ||
        req.has_listvolumeproperties() ||
        req.has_listvolumes() ||
        req.has_listlayers() ||
        req.has_convertpath() ||
        req.has_getlayerprivate() ||
        req.has_liststorage() ||
        req.has_locateprocess();
}

static bool ValidRequest(const rpc::TContainerRequest &req) {
    return
        req.has_create() +
        req.has_createweak() +
        req.has_destroy() +
        req.has_list() +
        req.has_getproperty() +
        req.has_setproperty() +
        req.has_getdata() +
        req.has_get() +
        req.has_start() +
        req.has_stop() +
        req.has_pause() +
        req.has_resume() +
        req.has_propertylist() +
        req.has_datalist() +
        req.has_kill() +
        req.has_version() +
        req.has_wait() +
        req.has_listvolumeproperties() +
        req.has_createvolume() +
        req.has_linkvolume() +
        req.has_unlinkvolume() +
        req.has_listvolumes() +
        req.has_tunevolume() +
        req.has_importlayer() +
        req.has_exportlayer() +
        req.has_removelayer() +
        req.has_listlayers() +
        req.has_convertpath() +
        req.has_attachprocess() +
        req.has_getlayerprivate() +
        req.has_setlayerprivate() +
        req.has_liststorage() +
        req.has_removestorage() +
        req.has_importstorage() +
        req.has_exportstorage() +
        req.has_locateprocess() == 1;
}

static TError CheckPortoWriteAccess() {
    if (CL->AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");
    return OK;
}

static noinline TError CreateContainer(std::string reqName, bool weak) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    std::string name;
    error = CL->ResolveName(reqName, name);
    if (error)
        return error;

    std::shared_ptr<TContainer> ct;
    error = TContainer::Create(name, ct);
    if (error)
        return error;

    if (!error && weak) {
        ct->IsWeak = true;
        ct->SetProp(EProperty::WEAK);

        error = ct->Save();
        if (!error)
            CL->WeakContainers.emplace_back(ct);
    }

    return error;
}

noinline TError DestroyContainer(const rpc::TContainerDestroyRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Destroy();
}

static noinline TError StartContainer(const rpc::TContainerStartRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Start();
}

noinline TError StopContainer(const rpc::TContainerStopRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    uint64_t timeout_ms = req.has_timeout_ms() ?
        req.timeout_ms() : config().container().stop_timeout_ms();
    return ct->Stop(timeout_ms);
}

noinline TError PauseContainer(const rpc::TContainerPauseRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    return ct->Pause();
}

noinline TError ResumeContainer(const rpc::TContainerResumeRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Resume();
}

noinline TError ListContainers(const rpc::TContainerListRequest &req,
                               rpc::TContainerResponse &rsp) {
    std::string mask = req.has_mask() ? req.mask() : "***";
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        std::string name;
        if (ct->IsRoot() || CL->ComposeName(ct->Name, name) ||
                !StringMatch(name, mask))
            continue;
        rsp.mutable_list()->add_name(name);
    }
    return OK;
}

noinline TError GetContainerProperty(const rpc::TContainerGetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.property());
            if (error)
                return error;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.property());

        error = ct->GetProperty(req.property(), value);
        if (!error)
            rsp.mutable_getproperty()->set_value(value);
    }
    return error;
}

noinline TError SetContainerProperty(const rpc::TContainerSetPropertyRequest &req) {
    std::string property = req.property();
    std::string value = req.value();

    /* legacy kludge */
    if (property.find('.') != std::string::npos) {
        if (property == "cpu.smart") {
            if (value == "0") {
                property = P_CPU_POLICY;
                value = "normal";
            } else {
                property = P_CPU_POLICY;
                value = "rt";
            }
        } else if (property == "memory.limit_in_bytes") {
            property = P_MEM_LIMIT;
        } else if (property == "memory.low_limit_in_bytes") {
            property = P_MEM_GUARANTEE;
        } else if (property == "memory.recharge_on_pgfault") {
            property = P_RECHARGE_ON_PGFAULT;
            value = value == "0" ? "false" : "true";
        }
    }

    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    return ct->SetProperty(property, value);
}

noinline TError GetContainerData(const rpc::TContainerGetDataRequest &req,
                                 rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.data());
            if (error)
                return error;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.data());

        error = ct->GetProperty(req.data(), value);
        if (!error)
            rsp.mutable_getdata()->set_value(value);
    }
    return error;
}

static void FillGetResponse(const rpc::TContainerGetRequest &req,
                            rpc::TContainerGetResponse &rsp,
                            std::string &name) {
    std::shared_ptr<TContainer> ct;

    auto lock = LockContainers();
    TError containerError = CL->ResolveContainer(name, ct);
    lock.unlock();

    auto entry = rsp.add_list();
    entry->set_name(name);
    for (int j = 0; j < req.variable_size(); j++) {
        auto var = req.variable(j);

        auto keyval = entry->add_keyval();
        std::string value;

        TError error = containerError;
        if (!error && req.has_real() && req.real())
            error = ct->HasProperty(var);
        if (!error)
            error = ct->GetProperty(var, value);

        keyval->set_variable(var);
        if (error) {
            keyval->set_error(error.Error);
            keyval->set_errormsg(error.Message());
        } else {
            keyval->set_value(value);
        }
    }
}

noinline TError GetContainerCombined(const rpc::TContainerGetRequest &req,
                                     rpc::TContainerResponse &rsp) {
    bool try_lock = req.has_nonblock() && req.nonblock();
    auto get = rsp.mutable_get();
    std::list <std::string> masks, names;

    for (int i = 0; i < req.name_size(); i++) {
        auto name = req.name(i);
        if (name.find_first_of("*?") == std::string::npos)
            names.push_back(name);
        else
            masks.push_back(name);
    }

    if (!masks.empty()) {
        auto lock = LockContainers();
        for (auto &it: Containers) {
            auto &ct = it.second;
            std::string name;
            if (ct->IsRoot() || CL->ComposeName(ct->Name, name))
                continue;
            if (masks.empty())
                names.push_back(name);
            for (auto &mask: masks) {
                if (StringMatch(name, mask)) {
                    names.push_back(name);
                    break;
                }
            }
        }
    }

    /* Lock all containers for read. TODO: lock only common ancestor */

    auto lock = LockContainers();
    TError error = RootContainer->LockRead(lock, try_lock);
    lock.unlock();
    if (error)
        return error;

    if (req.has_sync() && req.sync())
        TContainer::SyncPropertiesAll();

    for (auto &name: names)
        FillGetResponse(req, *get, name);

    RootContainer->Unlock();

    return OK;
}

noinline TError ListProperty(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_propertylist();
    for (auto elem : ContainerProperties) {
        if (elem.second->IsReadOnly || !elem.second->IsSupported || elem.second->IsHidden)
            continue;
        auto entry = list->add_list();
        entry->set_name(elem.first);
        entry->set_desc(elem.second->Desc.c_str());
    }
    return OK;
}

noinline TError ListData(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_datalist();
    for (auto elem : ContainerProperties) {
        if (!elem.second->IsReadOnly || !elem.second->IsSupported || elem.second->IsHidden)
            continue;
        auto entry = list->add_list();
        entry->set_name(elem.first);
        entry->set_desc(elem.second->Desc.c_str());
    }
    return OK;
}

noinline TError Kill(const rpc::TContainerKillRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (error)
        return error;
    error = CL->CanControl(*ct);
    if (error)
        return error;
    return ct->Kill(req.sig());
}

noinline TError Version(rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(PORTO_VERSION);
    ver->set_revision(PORTO_REVISION);

    return OK;
}

noinline TError Wait(const rpc::TContainerWaitRequest &req,
                     rpc::TContainerResponse &rsp,
                     std::shared_ptr<TClient> &client) {
    auto lock = LockContainers();
    bool queueWait = !req.has_timeout() || req.timeout() != 0;

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    auto waiter = std::make_shared<TContainerWaiter>(client);

    for (int i = 0; i < req.name_size(); i++) {
        std::string name = req.name(i);
        std::string abs_name;

        if (name.find_first_of("*?") != std::string::npos) {
            waiter->Wildcards.push_back(name);
            continue;
        }

        std::shared_ptr<TContainer> ct;
        TError error = client->ResolveContainer(name, ct);
        if (error) {
            rsp.mutable_wait()->set_name(name);
            return error;
        }

        /* Explicit wait notifies non-running and hollow meta immediately */
        if (ct->State != EContainerState::Running &&
                ct->State != EContainerState::Starting &&
                (ct->State != EContainerState::Meta ||
                 !ct->RunningChildren)) {
            rsp.mutable_wait()->set_name(name);
            return OK;
        }

        if (queueWait)
            ct->AddWaiter(waiter);
    }

    if (!waiter->Wildcards.empty()) {
        for (auto &it: Containers) {
            auto &ct = it.second;
            if (ct->IsRoot())
                continue;

            /* Wildcard notifies immediately only dead and hollow meta */
            if (ct->State != EContainerState::Dead &&
                    (ct->State != EContainerState::Meta ||
                     ct->RunningChildren))
                continue;

            std::string name;
            if (!client->ComposeName(ct->Name, name) &&
                    waiter->MatchWildcard(name)) {
                rsp.mutable_wait()->set_name(name);
                return OK;
            }
        }

        if (queueWait)
            TContainerWaiter::AddWildcard(waiter);
    }

    if (!queueWait) {
        rsp.mutable_wait()->set_name("");
        return OK;
    }

    client->Waiter = waiter;

    if (req.has_timeout()) {
        TEvent e(EEventType::WaitTimeout, nullptr);
        e.WaitTimeout.Waiter = waiter;
        EventQueue->Add(req.timeout(), e);
    }

    return TError::Queued();
}

noinline TError ConvertPath(const rpc::TConvertPathRequest &req,
                            rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> src, dst;
    TError error;

    auto lock = LockContainers();
    error = CL->ResolveContainer(
            (req.has_source() && req.source().length()) ?
            req.source() : SELF_CONTAINER, src);
    if (error)
        return error;
    error = CL->ResolveContainer(
            (req.has_destination() && req.destination().length()) ?
            req.destination() : SELF_CONTAINER, dst);
    if (error)
        return error;

    if (src == dst) {
        rsp.mutable_convertpath()->set_path(req.path());
        return OK;
    }

    TPath srcRoot;
    if (src->State == EContainerState::Stopped) {
        for (auto ct = src; ct; ct = ct->Parent)
            srcRoot = ct->Root / srcRoot;
    } else
        srcRoot = src->RootPath;
    srcRoot = srcRoot.NormalPath();

    TPath dstRoot;
    if (dst->State == EContainerState::Stopped) {
        for (auto ct = dst; ct; ct = ct->Parent)
            dstRoot = ct->Root / dstRoot;
    } else
        dstRoot = dst->RootPath;
    dstRoot = dstRoot.NormalPath();

    TPath path(srcRoot / req.path());
    path = dstRoot.InnerPath(path);

    if (path.IsEmpty())
        return TError(EError::InvalidValue, "Path is unreachable");
    rsp.mutable_convertpath()->set_path(path.ToString());
    return OK;
}

noinline TError ListVolumeProperties(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_volumepropertylist();
    for (auto &prop: VolumeProperties) {
        auto p = list->add_properties();
        p->set_name(prop.Name);
        p->set_desc(prop.Desc);
    }

    return OK;
}

noinline static void
FillVolumeDescription(rpc::TVolumeDescription *desc, const TVolume &volume) {

    desc->set_path(CL->ComposePath(volume.Path).ToString());

    TStringMap props;
    TTuple links;
    volume.DumpConfig(props, links);

    for (auto &kv: props) {
        auto p = desc->add_properties();
        p->set_name(kv.first);
        p->set_value(kv.second);
    }

    for (auto &name: links)
        desc->add_containers(name);
}

noinline TError CreateVolume(const rpc::TVolumeCreateRequest &req,
                             rpc::TContainerResponse &rsp) {
    TStringMap cfg;

    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    if (req.has_path() && !req.path().empty())
        cfg[V_PATH] = req.path();

    if (!cfg.count(V_PLACE) && CL->DefaultPlace() != PORTO_PLACE)
        cfg[V_PLACE] = CL->DefaultPlace().ToString();

    std::shared_ptr<TVolume> volume;
    TError error = TVolume::Create(cfg, volume);
    if (error)
        return error;

    FillVolumeDescription(rsp.mutable_volume(), *volume);
    return OK;
}

noinline TError TuneVolume(const rpc::TVolumeTuneRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStringMap cfg;
    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;

    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;

    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot tune volume " + volume->Path.ToString());

    return volume->Tune(cfg);
}

noinline TError LinkVolume(const rpc::TVolumeLinkRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.has_container() ?
                                req.container() : SELF_CONTAINER, ct, true);
    if (error)
        return error;

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;
    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot link volume" + volume->Path.ToString());

    return volume->LinkContainer(*ct);
}

noinline TError UnlinkVolume(const rpc::TVolumeUnlinkRequest &req) {
    bool strict = req.has_strict() && req.strict();
    std::shared_ptr<TContainer> ct;
    TError error;

    if (!req.has_container() || req.container() != "***") {
        error = CL->WriteContainer(req.has_container() ? req.container() :
                                    SELF_CONTAINER, ct, true);
        if (error)
            return error;
    }

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;
    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot unlink volume " + volume->Path.ToString());

    if (ct) {
        error = volume->UnlinkContainer(*ct, strict);
        if (error)
            return error;
    }

    CL->ReleaseContainer();

    if (volume->State == EVolumeState::Unlinked || !ct) {
        error = volume->Destroy(strict);
        if (error && strict && ct)
            (void)volume->LinkContainer(*ct);
    }

    return error;
}

noinline TError ListVolumes(const rpc::TVolumeListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TError error;

    if (req.has_path() && !req.path().empty()) {
        std::shared_ptr<TVolume> volume;

        error = TVolume::Find(CL->ResolvePath(req.path()), volume);
        if (error)
            return error;

        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, *volume);
        return OK;
    }

    auto volumes_lock = LockVolumes();
    std::list<std::shared_ptr<TVolume>> list;
    for (auto &it : Volumes) {
        auto volume = it.second;

        if (req.has_container() &&
                std::find(volume->Containers.begin(), volume->Containers.end(),
                    req.container()) == volume->Containers.end())
            continue;

        if (!CL->ComposePath(volume->Path).IsEmpty())
            list.push_back(volume);
    }
    volumes_lock.unlock();

    for (auto &volume: list) {
        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, *volume);
    }

    return OK;
}

noinline TError ImportLayer(const rpc::TLayerImportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());

    if (req.has_private_value())
        layer.Private = req.private_value();

    layer.Owner = CL->Cred;

    return layer.ImportArchive(CL->ResolvePath(req.tarball()),
                               req.has_compress() ? req.compress() : "",
                               req.merge());
}

noinline TError GetLayerPrivate(const rpc::TLayerGetPrivateRequest &req,
                                rpc::TContainerResponse &rsp) {
    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    TError error = CL->CanControlPlace(layer.Place);
    if (error)
        return error;
    error = layer.Load();
    if (!error)
        rsp.mutable_layer_private()->set_private_value(layer.Private);
    return error;
}

noinline TError SetLayerPrivate(const rpc::TLayerSetPrivateRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    error = CL->CanControlPlace(layer.Place);
    if (error)
        return error;
    error = layer.Load();
    if (error)
        return error;
    error = CL->CanControl(layer.Owner);
    if (error)
        return TError(error, "Cannot set layer private " + layer.Name);
    return layer.SetPrivate(req.private_value());
}

noinline TError ExportLayer(const rpc::TLayerExportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    if (req.has_layer()) {
        TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                       PORTO_LAYERS, req.layer());

        error = CL->CanControlPlace(layer.Place);
        if (error)
            return error;

        error = layer.Load();
        if (error)
            return error;

        return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                                   req.has_compress() ? req.compress() : "");
    }

    auto volume = TVolume::Find(CL->ResolvePath(req.volume()));
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    TStorage layer(volume->Place, PORTO_VOLUMES, volume->Id);
    layer.Owner = volume->VolumeOwner;
    error = volume->GetUpperLayer(layer.Path);
    if (error)
        return error;

    return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                               req.has_compress() ? req.compress() : "");
}

noinline TError RemoveLayer(const rpc::TLayerRemoveRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    return layer.Remove();
}

noinline TError ListLayers(const rpc::TLayerListRequest &req,
                           rpc::TContainerResponse &rsp) {
    TPath place = req.has_place() ? req.place() : CL->DefaultPlace();
    TError error = CL->CanControlPlace(place);
    if (error)
        return error;

    std::list<TStorage> layers;
    error = TStorage::List(place, PORTO_LAYERS, layers);
    if (error)
        return error;

    auto list = rsp.mutable_layers();
    for (auto &layer: layers) {
        if (req.has_mask() && !StringMatch(layer.Name, req.mask()))
            continue;
        list->add_layer(layer.Name);
        (void)layer.Load();
        auto desc = list->add_layers();
        desc->set_name(layer.Name);
        desc->set_owner_user(layer.Owner.User());
        desc->set_owner_group(layer.Owner.Group());
        desc->set_private_value(layer.Private);
        desc->set_last_usage(layer.LastUsage());
    }

    return error;
}

noinline TError AttachProcess(const rpc::TAttachProcessRequest &req) {
    std::shared_ptr<TContainer> oldCt, newCt;
    pid_t pid = req.pid();
    std::string comm;
    TError error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    error = TranslatePid(pid, CL->Pid, pid);
    if (error)
        return error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    comm = GetTaskName(pid);

    /* sanity check and protection against races */
    if (req.comm().size() && req.comm() != comm)
        return TError(EError::InvalidValue, "wrong task comm for pid");

    error = TContainer::FindTaskContainer(pid, oldCt);
    if (error)
        return error;

    if (oldCt == CL->ClientContainer)
        error = CL->WriteContainer(SELF_CONTAINER, oldCt, true);
    else
        error = CL->WriteContainer(ROOT_PORTO_NAMESPACE + oldCt->Name, oldCt);
    if (error)
        return error;

    if (pid == oldCt->Task.Pid || pid == oldCt->WaitTask.Pid ||
            pid == oldCt->SeizeTask.Pid)
        return TError(EError::Busy, "cannot move main process");

    error = CL->WriteContainer(req.name(), newCt);
    if (error)
        return error;

    if (!newCt->IsChildOf(*oldCt))
        return TError(EError::Permission, "new container must be child of current");

    if (newCt->State != EContainerState::Running &&
            newCt->State != EContainerState::Meta)
        return TError(EError::InvalidState, "new container is not running");

    for (auto ct = newCt; ct && ct != oldCt; ct = ct->Parent)
        if (ct->Isolate)
            return TError(EError::InvalidState, "new container must be not isolated from current");

    L_ACT("Attach process {} ({}) from {} to {}", pid, comm,
          oldCt->Name, newCt->Name);

    for (auto hy: Hierarchies) {
        auto cg = newCt->GetCgroup(*hy);
        error = cg.Attach(pid);
        if (error)
            goto undo;
    }

    return OK;

undo:
    for (auto hy: Hierarchies) {
        auto cg = oldCt->GetCgroup(*hy);
        (void)cg.Attach(pid);
    }
    return error;
}

noinline TError LocateProcess(const rpc::TLocateProcessRequest &req,
                              rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    pid_t pid = req.pid();
    std::string name;

    if (pid <= 0 || TranslatePid(pid, CL->Pid, pid))
        return TError(EError::InvalidValue, "wrong pid");

    if (req.comm().size() && req.comm() != GetTaskName(pid))
        return TError(EError::InvalidValue, "wrong comm");

    if (TContainer::FindTaskContainer(pid, ct))
        return TError(EError::InvalidValue, "task not found");

    if (CL->ComposeName(ct->Name, name)) {
        if (CL->ClientContainer == ct)
            name = SELF_CONTAINER;
        else
            return TError(EError::Permission, "container is unreachable");
    }

    rsp.mutable_locateprocess()->set_name(name);

    return OK;
}

noinline TError ListStorage(const rpc::TStorageListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TPath place = req.has_place() ? req.place() : CL->DefaultPlace();
    TError error = CL->CanControlPlace(place);
    if (error)
        return error;

    std::list<TStorage> storages;
    error = TStorage::List(place, PORTO_STORAGE, storages);
    if (error)
        return error;

    auto list = rsp.mutable_storagelist();
    for (auto &storage: storages) {
        if (req.has_mask() && !StringMatch(storage.Name, req.mask()))
            continue;
        if (storage.Load())
            continue;
        auto desc = list->add_storages();
        desc->set_name(storage.Name);
        desc->set_owner_user(storage.Owner.User());
        desc->set_owner_group(storage.Owner.Group());
        desc->set_private_value(storage.Private);
        desc->set_last_usage(storage.LastUsage());
    }

    return error;
}

noinline TError RemoveStorage(const rpc::TStorageRemoveRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());
    return storage.Remove();
}

noinline TError ImportStorage(const rpc::TStorageImportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());
    storage.Owner = CL->Cred;
    if (req.has_private_value())
        storage.Private = req.private_value();

    return storage.ImportArchive(CL->ResolvePath(req.tarball()),
                                 req.has_compress() ? req.compress() : "");
}

noinline TError ExportStorage(const rpc::TStorageExportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());

    error = CL->CanControlPlace(storage.Place);
    if (error)
        return error;

    error = storage.Load();
    if (error)
        return error;

    return storage.ExportArchive(CL->ResolvePath(req.tarball()),
                                 req.has_compress() ? req.compress() : "");
}

void HandleRpcRequest(const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client) {
    rpc::TContainerResponse rsp;
    std::string str;

    client->StartRequest();

    bool silent = !Verbose && SilentRequest(req);
    if (!silent)
        L_REQ("{} from {}", RequestAsString(req), client->Id);

    if (Debug)
        L_REQ("{} from {}", req.ShortDebugString(), client->Id);

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (!ValidRequest(req)) {
            L_ERR("Invalid request {} from {}", req.ShortDebugString(), client->Id);
            error = TError(EError::InvalidMethod, "invalid request");
        } else if (req.has_create())
            error = CreateContainer(req.create().name(), false);
        else if (req.has_createweak())
            error = CreateContainer(req.createweak().name(), true);
        else if (req.has_destroy())
            error = DestroyContainer(req.destroy());
        else if (req.has_list())
            error = ListContainers(req.list(), rsp);
        else if (req.has_getproperty())
            error = GetContainerProperty(req.getproperty(), rsp);
        else if (req.has_setproperty())
            error = SetContainerProperty(req.setproperty());
        else if (req.has_getdata())
            error = GetContainerData(req.getdata(), rsp);
        else if (req.has_get())
            error = GetContainerCombined(req.get(), rsp);
        else if (req.has_start())
            error = StartContainer(req.start());
        else if (req.has_stop())
            error = StopContainer(req.stop());
        else if (req.has_pause())
            error = PauseContainer(req.pause());
        else if (req.has_resume())
            error = ResumeContainer(req.resume());
        else if (req.has_propertylist())
            error = ListProperty(rsp);
        else if (req.has_datalist())
            error = ListData(rsp);
        else if (req.has_kill())
            error = Kill(req.kill());
        else if (req.has_version())
            error = Version(rsp);
        else if (req.has_wait())
            error = Wait(req.wait(), rsp, client);
        else if (req.has_listvolumeproperties())
            error = ListVolumeProperties(rsp);
        else if (req.has_createvolume())
            error = CreateVolume(req.createvolume(), rsp);
        else if (req.has_linkvolume())
            error = LinkVolume(req.linkvolume());
        else if (req.has_unlinkvolume())
            error = UnlinkVolume(req.unlinkvolume());
        else if (req.has_listvolumes())
            error = ListVolumes(req.listvolumes(), rsp);
        else if (req.has_tunevolume())
            error = TuneVolume(req.tunevolume());
        else if (req.has_importlayer())
            error = ImportLayer(req.importlayer());
        else if (req.has_exportlayer())
            error = ExportLayer(req.exportlayer());
        else if (req.has_removelayer())
            error = RemoveLayer(req.removelayer());
        else if (req.has_listlayers())
            error = ListLayers(req.listlayers(), rsp);
        else if (req.has_convertpath())
            error = ConvertPath(req.convertpath(), rsp);
        else if (req.has_attachprocess())
            error = AttachProcess(req.attachprocess());
        else if (req.has_getlayerprivate())
            error = GetLayerPrivate(req.getlayerprivate(), rsp);
        else if (req.has_setlayerprivate())
            error = SetLayerPrivate(req.setlayerprivate());
        else if (req.has_liststorage())
            error = ListStorage(req.liststorage(), rsp);
        else if (req.has_removestorage())
            error = RemoveStorage(req.removestorage());
        else if (req.has_importstorage())
            error = ImportStorage(req.importstorage());
        else if (req.has_exportstorage())
            error = ExportStorage(req.exportstorage());
        else if (req.has_locateprocess())
            error = LocateProcess(req.locateprocess(), rsp);
        else
            error = TError(EError::InvalidMethod, "invalid RPC method");
    } catch (std::bad_alloc exc) {
        rsp.Clear();
        error = TError("memory allocation failure");
    } catch (std::string exc) {
        rsp.Clear();
        error = TError(EError::Unknown, exc);
    } catch (const std::exception &exc) {
        rsp.Clear();
        error = TError(EError::Unknown, exc.what());
    } catch (...) {
        rsp.Clear();
        error = TError("unknown error");
    }

    client->FinishRequest();

    if (error != EError::Queued) {
        rsp.set_error(error.Error);
        rsp.set_errormsg(error.Message());

        /* log failed or slow silent requests */
        if (silent && (error || client->RequestTimeMs >= 1000)) {
            L_REQ("{} from {}", RequestAsString(req), client->Id);
            silent = false;
        }

        if (!silent)
            L_RSP("{} to {} (request took {} ms)",
                  ResponseAsString(rsp), client->Id, client->RequestTimeMs);

        if (Debug)
            L_RSP("{} to {}", rsp.ShortDebugString(), client->Id);

        error = client->QueueResponse(rsp);
        if (error)
            L_WRN("Cannot send response for {} : {}", client->Id, error);
    }
}

void SendWaitResponse(TClient &client, const std::string &name) {
    rpc::TContainerResponse rsp;

    rsp.set_error(EError::Success);
    rsp.mutable_wait()->set_name(name);

    if (!name.empty() || Verbose)
        L_RSP("{} to {} (request took {} ms)", ResponseAsString(rsp),
                client.Id, client.RequestTimeMs);

    if (Debug)
        L_RSP("{} to {}", rsp.ShortDebugString(), client.Id);

    TError error = client.QueueResponse(rsp);
    if (error)
        L_WRN("Cannot send response for {} : {}", client.Id, error);
}
