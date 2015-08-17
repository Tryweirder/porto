#include <algorithm>

#include "statistics.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "container.hpp"
#include "property.hpp"
#include "data.hpp"
#include "event.hpp"
#include "qdisc.hpp"
#include "client.hpp"
#include "task.hpp"
#include "kvalue.hpp"
#include "kv.pb.h"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"

void TContainerHolder::DestroyRoot(TScopedLock &holder_lock) {
    auto list = List(true);

    // we want children to be removed first
    std::reverse(std::begin(list), std::end(list));

    for (auto c: list) {
        TError error = Destroy(holder_lock, c);
        if (error)
            L_ERR() << "Can't destroy container " << c->GetName() << ": " << error << std::endl;
    }
}

TError TContainerHolder::ReserveDefaultClassId() {
    uint16_t id;
    TError error = IdMap.Get(id);
    if (error)
        return error;
    if (id != 2)
        return TError(EError::Unknown, "Unexpected default class id " + std::to_string(id));
    return TError::Success();
}

TError TContainerHolder::CreateRoot(TScopedLock &holder_lock) {
    TError error = TaskGetLastCap();
    if (error)
        return error;

    BootTime = GetBootTime();

    std::shared_ptr<TContainer> container;
    error = Create(holder_lock, ROOT_CONTAINER, TCred(0, 0), container);
    if (error)
        return error;

    if (container->GetId() != ROOT_CONTAINER_ID)
        return TError(EError::Unknown, "Unexpected root container id " + std::to_string(container->GetId()));

    error = ReserveDefaultClassId();
    if (error)
        return error;

    error = container->Prop->Set<bool>(P_ISOLATE, false);
    if (error)
        return error;

    error = container->Start(nullptr, true);
    if (error)
        return error;

    return TError::Success();
}

TError TContainerHolder::CreatePortoRoot(TScopedLock &holder_lock) {
    std::shared_ptr<TContainer> container;
    TError error = Create(holder_lock, PORTO_ROOT_CONTAINER, TCred(0, 0), container);
    if (error)
        return error;

    if (container->GetId() != PORTO_ROOT_CONTAINER_ID)
        return TError(EError::Unknown, "Unexpected /porto container id " + std::to_string(container->GetId()));

    error = container->Prop->Set<bool>(P_ISOLATE, false);
    if (error)
        return error;

    error = container->Start(nullptr, true);
    if (error)
        return error;

    ScheduleLogRotatation();

    Statistics->Created = 0;
    Statistics->RestoreFailed = 0;
    Statistics->RemoveDead = 0;
    Statistics->Rotated = 0;
    Statistics->Started = 0;

    return TError::Success();
}

bool TContainerHolder::ValidName(const std::string &name) const {
    if (name == ROOT_CONTAINER || name == PORTO_ROOT_CONTAINER)
        return true;

    if (name.length() == 0 || name.length() > 128)
        return false;

    for (std::string::size_type i = 0; i + 1 < name.length(); i++)
        if (name[i] == '/' && name[i + 1] == '/')
            return false;

    if (*name.begin() == '/')
        return false;

    if (*(name.end()--) == '/')
        return false;

    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_' || c == '/' || c == '-' || c == '@' || c == ':' || c == '.');
                   }) == name.end();
}

std::shared_ptr<TContainer> TContainerHolder::GetParent(const std::string &name) const {
    std::shared_ptr<TContainer> parent;

    if (name == ROOT_CONTAINER)
        return nullptr;
    else if (name == PORTO_ROOT_CONTAINER)
        return Containers.at(ROOT_CONTAINER);

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos) {
        return Containers.at(PORTO_ROOT_CONTAINER);
    } else {
        std::string parentName = name.substr(0, n);

        if (Containers.find(parentName) == Containers.end())
            return nullptr;

        return Containers.at(parentName);
    }
}

TError TContainerHolder::Create(TScopedLock &holder_lock, const std::string &name, const TCred &cred, std::shared_ptr<TContainer> &container) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers.find(name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, "container " + name + " already exists");

    if (Containers.size() + 1 > config().container().max_total())
        return TError(EError::ResourceNotAvailable, "number of created containers exceeds limit");

    auto parent = GetParent(name);
    if (!parent && name != ROOT_CONTAINER)
        return TError(EError::InvalidValue, "invalid parent container");

    if (parent && !parent->IsRoot() && !parent->IsPortoRoot()) {
        TError error = parent->CheckPermission(cred);
        if (error)
            return error;
    }

    uint16_t id;
    TError error = IdMap.Get(id);
    if (error)
        return error;

    auto c = std::make_shared<TContainer>(shared_from_this(), Storage, name, parent, id, Net);
    error = c->Create(cred);
    if (error)
        return error;

    Containers[name] = c;
    Statistics->Created++;

    if (parent)
        parent->AddChild(c);

    container = c;

    return TError::Success();
}

TError TContainerHolder::Get(const std::string &name, std::shared_ptr<TContainer> &c) {
    if (Containers.find(name) == Containers.end())
        return TError(EError::ContainerDoesNotExist, "container " + name + " doesn't exist");

    c = Containers[name];
    return TError::Success();
}

TError TContainerHolder::GetLocked(TScopedLock &holder_lock,
                                   const std::shared_ptr<TClient> client,
                                   const std::string &name,
                                   const bool checkPerm,
                                   std::shared_ptr<TContainer> &c,
                                   TNestedScopedLock &l) {
    std::string absoluteName;

    // resolve name
    if (client) {
        std::shared_ptr<TContainer> clientContainer;
        TError error = client->GetContainer(clientContainer);
        if (error)
            return error;

        error = clientContainer->AbsoluteName(name, absoluteName, !checkPerm);
        if (error)
            return error;
    } else {
        absoluteName = name;
    }

    // get container
    TError error = Get(absoluteName, c);
    if (error)
        return error;

    // lock container
    l = TNestedScopedLock(*c, holder_lock);

    // make sure it's still alive
    if (!c->IsValid())
        return TError(EError::ContainerDoesNotExist, "container doesn't exist");

    // check permissions
    if (client && checkPerm) {
        error = c->CheckPermission(client->GetCred());
        if (error)
            return error;
    }

    return TError::Success();
}

TError TContainerHolder::Get(int pid, std::shared_ptr<TContainer> &c) {

    std::map<std::string, std::string> cgmap;
    TError error = GetTaskCgroups(pid, cgmap);
    if (error)
        return error;

    if (cgmap.find("freezer") == cgmap.end())
        return TError(EError::Unknown, "Can't determine freezer cgroup of client process");

    auto freezer = cgmap["freezer"];
    auto prefix = "/" + PORTO_ROOT_CGROUP + "/";
    std::string name;

    if (freezer.length() > prefix.length() && freezer.substr(0, prefix.length()) == prefix)
        name = freezer.substr(prefix.length());
    else
        name = ROOT_CONTAINER;

    return Get(name, c);
}

TError TContainerHolder::Destroy(TScopedLock &holder_lock, std::shared_ptr<TContainer> c) {
    if (c->GetState() != EContainerState::Stopped) {
        // we are destroying container and we don't need any exit status, so
        // forcefully kill all processes for destroy to be faster
        TError error = c->SendTreeSignal(holder_lock, SIGKILL);
        if (error)
            return error;

        error = c->StopTree(holder_lock);
        if (error)
            return error;
    }

    Unlink(holder_lock, c);

    return TError::Success();
}

void TContainerHolder::Unlink(TScopedLock &holder_lock, std::shared_ptr<TContainer> c) {
    for (auto name: c->GetChildren()) {
        std::shared_ptr<TContainer> child;
        if (Get(name, child))
            continue;

        Unlink(holder_lock, child);
    }

    c->Destroy(holder_lock);

    IdMap.Put(c->GetId());
    Containers.erase(c->GetName());
    Statistics->Created--;
}

std::vector<std::shared_ptr<TContainer> > TContainerHolder::List(bool all) const {
    std::vector<std::shared_ptr<TContainer> > ret;

    for (auto c : Containers) {
        PORTO_ASSERT(c.first == c.second->GetName());
        if (!all && c.second->IsPortoRoot())
            continue;
        ret.push_back(c.second);
    }

    return ret;
}

TError TContainerHolder::RestoreId(const kv::TNode &node, uint16_t &id) {
    std::string value = "";

    TError error = Storage->Get(node, P_RAW_ID, value);
    if (error) {
        // FIXME before v1.0 we didn't store id for meta or stopped containers;
        // don't try to recover, just assign new safe one
        error = IdMap.GetSince(config().container().max_total(), id);
        if (error)
            return error;
        L_WRN() << "Couldn't restore container id, using " << id << std::endl;
    } else {
        error = StringToUint16(value, id);
        if (error)
            return error;

        error = IdMap.GetAt(id);
        if (error) {
            // FIXME before v1.0 there was a possibility for two containers
            // to use the same id, allocate new one upon restore we see this

            error = IdMap.GetSince(config().container().max_total(), id);
            if (error)
                return error;
            L_WRN() << "Container ids clashed, using new " << id << std::endl;
        }
            return error;
    }

    return TError::Success();
}

std::map<std::string, std::shared_ptr<TKeyValueNode>> TContainerHolder::SortNodes(const std::vector<std::shared_ptr<TKeyValueNode>> &nodes) {
    // FIXME since v1.0 we use container id as kvalue node name and because
    // we need to create containers in particular order we create this
    // name-sorted map
    std::map<std::string, std::shared_ptr<TKeyValueNode>> name2node;

    for (auto node : nodes) {
        kv::TNode n;
        TError error = node->Load(n);
        if (error) {
            L_ERR() << "Can't load key-value node " << node->GetPath() << ": " << error << std::endl;
            node->Remove();
            Statistics->RestoreFailed++;
            continue;
        }

        std::string name;
        if (TKeyValueStorage::Get(n, P_RAW_NAME, name))
            name = TKeyValueStorage::FromPath(node->GetName());

        name2node[name] = node;
    }

    return name2node;
}

bool TContainerHolder::RestoreFromStorage() {
    std::vector<std::shared_ptr<TKeyValueNode>> nodes;

    auto holder_lock = ScopedLock();

    TError error = Storage->ListNodes(nodes);
    if (error) {
        L_ERR() << "Can't list key-value nodes: " << error << std::endl;
        return false;
    }

    auto name2node = SortNodes(nodes);
    bool restored = false;
    for (auto &pair : name2node) {
        auto node = pair.second;
        auto name = pair.first;

        L_ACT() << "Found " << name << " container in kvs" << std::endl;

        kv::TNode n;
        error = node->Load(n);
        if (error)
            continue;

        restored = true;
        error = Restore(holder_lock, name, n);
        if (error) {
            L_ERR() << "Can't restore " << name << ": " << error << std::endl;
            Statistics->RestoreFailed++;
            node->Remove();
            continue;
        }

        // FIXME since v1.0 we need to cleanup kvalue nodes with old naming
        if (TKeyValueStorage::Get(n, P_RAW_NAME, name))
            node->Remove();
    }

    if (restored) {
        for (auto &c: Containers) {
            if (c.second->IsLostAndRestored()) {
                ScheduleCgroupSync();
                break;
            }
        }
    }

    return restored;
}

TError TContainerHolder::Restore(TScopedLock &holder_lock, const std::string &name,
                                 const kv::TNode &node) {
    if (name == ROOT_CONTAINER || name == PORTO_ROOT_CONTAINER)
        return TError::Success();

    L_ACT() << "Restore container " << name << " (" << node.ShortDebugString() << ")" << std::endl;

    auto parent = GetParent(name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    uint16_t id = 0;
    TError error = RestoreId(node, id);
    if (error)
        return error;

    if (!id)
        return TError(EError::Unknown, "Couldn't restore container id");

    auto c = std::make_shared<TContainer>(shared_from_this(), Storage, name, parent, id, Net);
    error = c->Restore(holder_lock, node);
    if (error) {
        L_ERR() << "Can't restore container " << name << ": " << error << std::endl;
        return error;
    }

    Containers[name] = c;
    Statistics->Created++;
    return TError::Success();
}

void TContainerHolder::ScheduleLogRotatation() {
    TEvent e(EEventType::RotateLogs);
    Queue->Add(config().daemon().rotate_logs_timeout_s() * 1000, e);
}

void TContainerHolder::ScheduleCgroupSync() {
    TEvent e(EEventType::CgroupSync);
    Queue->Add(5000, e);
}

bool TContainerHolder::DeliverEvent(const TEvent &event) {
    if (config().log().verbose())
        L_EVT() << "Deliver event " << event.GetMsg() << std::endl;

    bool delivered = false;

    auto holder_lock = ScopedLock();

    switch (event.Type) {
    case EEventType::OOM:
    {
        std::shared_ptr<TContainer> target = event.Container.lock();
        if (target) {
            // check whether container can die due to OOM under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->MayReceiveOom(event.OOM.Fd)) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->MayReceiveOom(event.OOM.Fd)) {
                    // we don't want any concurrent stop/start/pause/etc and
                    // don't care whether parent acquired or not
                    target->AcquireForced();
                    target->DeliverEvent(holder_lock, event);
                    target->Release();
                    delivered = true;
                }
            }
        }
        break;
    }
    case EEventType::Respawn:
    {
        std::shared_ptr<TContainer> target = event.Container.lock();
        if (target) {
            // check whether container can respawn under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->MayRespawn()) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->MayRespawn() && !target->IsAcquired()) {
                    target->DeliverEvent(holder_lock, event);
                    delivered = true;
                }
            }
        }
        break;
    }
    case EEventType::Exit:
    {
        auto list = List();
        for (auto &target : list) {
            // check whether container can exit under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->MayExit(event.Exit.Pid)) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->MayExit(event.Exit.Pid)) {
                    // we don't want any concurrent stop/start/pause/etc and
                    // don't care whether parent acquired or not
                    target->AcquireForced();
                    target->DeliverEvent(holder_lock, event);
                    target->Release();
                    break;
                }
            }
        }
        AckExitStatus(event.Exit.Pid);
        delivered = true;
        break;
    }
    case EEventType::CgroupSync:
    {
        bool rearm = false;
        auto list = List();
        for (auto &target : list) {
            // don't lock container here, LostAndRestored is never changed
            // after startup
            if (target->IsLostAndRestored())
                rearm = true;

            if (target->IsAcquired())
                continue;

            TNestedScopedLock lock(*target, holder_lock);
            if (target->IsValid() && target->IsLostAndRestored()) {
                if (target->IsAcquired())
                    continue;
                target->SyncStateWithCgroup(holder_lock);
            }
        }
        if (rearm)
            ScheduleCgroupSync();
        delivered = true;
        break;
    }
    case EEventType::WaitTimeout:
    {
        auto w = event.WaitTimeout.Waiter.lock();
        if (w)
            w->Signal(nullptr);
        delivered = true;
        break;
    }
    case EEventType::RotateLogs:
    {
        { /* gc */
            std::vector<std::string> remove;
            for (auto target : List())
                // don't lock container here, we don't care if we race, we
                // make real check under lock later
                if (target->CanRemoveDead())
                    remove.push_back(target->GetName());

            for (auto name : remove) {
                std::shared_ptr<TContainer> container;
                TError error = Get(name, container);
                if (error)
                    continue;

                TNestedScopedLock lock(*container, holder_lock, std::try_to_lock);
                if (!lock.IsLocked() ||
                    !container->IsValid() ||
                    !container->CanRemoveDead())
                    continue;

                TScopedAcquire acquire(container);
                if (!acquire.IsAcquired())
                    continue;

                L_ACT() << "Remove old dead " << name << std::endl;

                error = Destroy(holder_lock, container);
                if (error)
                    L_ERR() << "Can't destroy " << name << ": " << error << std::endl;
                else
                    Statistics->RemoveDead++;
            }
        }

        { /* clean old journals */
            TFolder folder(config().journal_dir().path());
            std::vector<std::string> list;

            TError err = folder.Items(EFileType::Regular, list);
            if (err)
                L_WRN() << "Can't list container journals" << std::endl;
            else {
                for (auto &filename : list) {
                    uint64_t seconds;
                    TPath p(folder.GetPath() + "/" + filename);

                    if (!p.SecondsSinceMtime(seconds) && seconds > config().keep_journals())
                        p.Unlink();
                }
            }
        }

        auto list = List();
        for (auto &target : list) {
            if (target->IsAcquired())
                continue;

            TNestedScopedLock lock(*target, holder_lock);
            if (target->IsValid() && !target->IsAcquired())
                target->DeliverEvent(holder_lock, event);
        }

        ScheduleLogRotatation();
        Statistics->Rotated++;
        delivered = true;
        break;
    }
    case EEventType::UpdateNetwork:
    {
        L() << "Refresh containers tc classes" << std::endl;

        auto list = List(true);
        for (auto &target : list) {
            TNestedScopedLock lock(*target, holder_lock);
            if (!target->IsValid())
                continue;

            TError error = target->UpdateNetwork();
            if (error)
                L_WRN() << "Can't update " << target->GetName() << " network: " << error << std::endl;
        }
        delivered = true;
        break;
    }
    default:
        L_ERR() << "Unknown event " << event.GetMsg() << std::endl;
    }

    if (!delivered)
        L() << "Couldn't deliver " << event.GetMsg() << std::endl;

    return delivered;
}
