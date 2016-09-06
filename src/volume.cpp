#include <memory>
#include <sstream>
#include <algorithm>

#include "volume.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/quota.hpp"
#include "util/sha256.hpp"
#include "config.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <linux/falloc.h>
}


/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Clear() {
    return Volume->GetPath().ClearDirectory();
}

TError TVolumeBackend::Save(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Restore(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Resize(uint64_t space_limit, uint64_t inode_limit) {
    return TError(EError::NotSupported, "not implemented");
}

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:

    TError Configure(std::shared_ptr<TValueMap> Config) override {

        if (Volume->HaveQuota())
            return TError(EError::NotSupported, "Plain backend have no support of quota");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();

        TError error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        return Volume->GetPath().BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TPath path = Volume->GetPath();
        TError error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;
        return error;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeTmpfsBackend - tmpfs */

class TVolumeTmpfsBackend : public TVolumeBackend {
public:

    TError Configure(std::shared_ptr<TValueMap> Config) override {

        if (!Volume->HaveQuota())
            return TError(EError::NotSupported, "tmpfs backend requires space_limit");

        if (!Volume->IsAutoStorage())
            return TError(EError::NotSupported, "tmpfs backed doesn't support storage");

        return TError::Success();
    }

    TError Build() override {
        uint64_t spaceLimit, inodeLimit;
        Volume->GetQuota(spaceLimit, inodeLimit);
        return Volume->GetPath().Mount("porto:" + Volume->GetId(), "tmpfs",
                Volume->GetMountFlags(),
                { "size=" + std::to_string(spaceLimit),
                  "uid=" + std::to_string(Volume->GetCred().Uid),
                  "gid=" + std::to_string(Volume->GetCred().Gid),
                  "mode=" + StringFormat("%#o", Volume->GetPermissions())
                });
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return Volume->GetPath().Mount("porto:" + Volume->GetId(), "tmpfs",
                Volume->GetMountFlags() | MS_REMOUNT,
                { "size=" + std::to_string(space_limit),
                  "uid=" + std::to_string(Volume->GetCred().Uid),
                  "gid=" + std::to_string(Volume->GetCred().Gid),
                  "mode=" + StringFormat("%#o", Volume->GetPermissions())
                });
    }

    TError Destroy() override {
        TPath path = Volume->GetPath();
        TError error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;
        return error;
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeQuotaBackend - project quota */

class TVolumeQuotaBackend : public TVolumeBackend {
public:

    static bool Supported() {
        static bool supported = false, tested = false;

        if (!config().volumes().enable_quota())
            return false;

        if (!tested) {
            TProjectQuota quota(config().volumes().default_place() + "/" + config().volumes().volume_dir());
            supported = quota.Supported();
            if (supported)
                L_SYS() << "Project quota is supported: " << quota.Path << std::endl;
            else
                L_SYS() << "Project quota not supported: " << quota.Path << std::endl;
            tested = true;
        }

        return supported;
    }

    TError Configure(std::shared_ptr<TValueMap> Config) override {

        if (Volume->IsAutoPath())
            return TError(EError::NotSupported, "Quota backend requires path");

        if (!Volume->HaveQuota())
            return TError(EError::NotSupported, "Quota backend requires space_limit");

        if (Volume->IsReadOnly())
            return TError(EError::NotSupported, "Quota backed doesn't support read_only");

        if (!Volume->IsAutoStorage())
            return TError(EError::NotSupported, "Quota backed doesn't support storage");

        if (Config->HasValue(V_LAYERS))
            return TError(EError::NotSupported, "Quota backed doesn't support layers");

        return TError::Success();
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TProjectQuota quota(path);
        TError error;

        Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
        L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
        return quota.Create();
    }

    TError Clear() override {
        return TError(EError::NotSupported, "Quota backend cannot be cleared");
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->GetPath());
        TError error;

        L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
        error = quota.Destroy();
        if (error)
            L_ERR() << "Can't destroy quota: " << error << std::endl;

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetPath());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        return TProjectQuota(Volume->GetPath()).StatFS(result);
    }
};

/* TVolumeNativeBackend - project quota + bindmount */

class TVolumeNativeBackend : public TVolumeBackend {
public:

    static bool Supported() {
        return TVolumeQuotaBackend::Supported();
    }

    TError Configure(std::shared_ptr<TValueMap> Config) override {

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TError error;

        if (Volume->HaveQuota()) {
            Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
            L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                    << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
            error = quota.Create();
            if (error)
                return error;
        }

        error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        return Volume->GetPath().BindRemount(storage, Volume->GetMountFlags());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        TProjectQuota quota(Volume->GetStorage());
        TPath path = Volume->GetPath();
        TError error;

        error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
            error = quota.Destroy();
            if (error)
                L_ERR() << "Can't destroy quota: " << error << std::endl;
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetStorage());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT() << "Creating project quota: " << quota.Path << std::endl;
            return quota.Create();
        }
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->GetStorage()).StatFS(result);
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    int LoopDev = -1;

public:

    TPath GetLoopImage() {
        return Volume->GetStorage() / "loop.img";
    }

    TPath GetLoopDevice() {
        if (LoopDev < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(LoopDev));
    }

    TError Save(std::shared_ptr<TValueMap> Config) override {
        return Config->Set<int>(V_LOOP_DEV, LoopDev);
    }

    TError Restore(std::shared_ptr<TValueMap> Config) override {
        LoopDev = Config->Get<int>(V_LOOP_DEV);
        return TError::Success();
    }

    static TError MakeImage(const TPath &path, const TCred &cred, off_t size, off_t guarantee) {
        int fd, status;
        TError error;

        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            return TError(EError::Unknown, errno, "creat(" + path.ToString() + ")");

        if (fchown(fd, cred.Uid, cred.Gid)) {
            error = TError(EError::Unknown, errno, "chown(" + path.ToString() + ")");
            goto remove_file;
        }

        if (ftruncate(fd, size)) {
            error = TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");
            goto remove_file;
        }

        if (guarantee && fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, guarantee)) {
            error = TError(EError::ResourceNotAvailable, errno,
                           "cannot fallocate guarantee " + std::to_string(guarantee));
            goto remove_file;
        }

        close(fd);
        fd = -1;

        error = Run({ "mkfs.ext4", "-F", "-m", "0", "-E", "nodiscard",
                     "-O", "^has_journal", path.ToString()}, status);
        if (error)
            goto remove_file;

        if (status) {
            error = TError(EError::Unknown, error.GetErrno(),
                    "mkfs.ext4 returned " + std::to_string(status) + ": " + error.GetMsg());
            goto remove_file;
        }

        return TError::Success();

remove_file:
        (void)path.Unlink();
        if (fd >= 0)
            close(fd);

        return error;
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TPath image = GetLoopImage();
        uint64_t space_limit, inode_limit;
        uint64_t space_guarantee, inode_guarantee;
        TError error;

        Volume->GetGuarantee(space_guarantee, inode_guarantee);
        Volume->GetQuota(space_limit, inode_limit);
        if (!space_limit)
            return TError(EError::InvalidValue, "loop backend requires space_limit");

        if (!image.Exists()) {
            L_ACT() << "Allocate loop image with size " << space_limit
                    << " guarantee " << space_guarantee << std::endl;
            error = MakeImage(image, Volume->GetCred(), space_limit, space_guarantee);
            if (error)
                return error;
        } else {
            //FIXME call resize2fs
        }

        error = SetupLoopDevice(image, LoopDev);
        if (error)
            return error;

        error = path.Mount(GetLoopDevice(), "ext4", Volume->GetMountFlags(), {});
        if (error)
            goto free_loop;

        if (!Volume->IsReadOnly()) {
            error = path.Chown(Volume->GetCred());
            if (error)
                goto umount_loop;

            error = path.Chmod(Volume->GetPermissions());
            if (error)
                goto umount_loop;
        }

        return TError::Success();

umount_loop:
        (void)path.UmountAll();
free_loop:
        PutLoopDev(LoopDev);
        LoopDev = -1;
        return error;
    }

    TError Destroy() override {
        TPath loop = GetLoopDevice();
        TPath path = Volume->GetPath();

        if (LoopDev < 0)
            return TError::Success();

        L_ACT() << "Destroy loop " << loop << std::endl;
        TError error = path.UmountAll();
        TError error2 = PutLoopDev(LoopDev);
        if (!error)
            error = error2;
        LoopDev = -1;
        return error;
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "loop backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};

/* TVolumeOverlayBackend - project quota + overlayfs */

class TVolumeOverlayBackend : public TVolumeBackend {
public:

    static bool Supported() {
        static bool supported = false, tested = false;

        if (!tested) {
            tested = true;
            if (!mount(NULL, "/", "overlay", MS_SILENT, NULL))
                L_ERR() << "Unexpected success when testing for overlayfs" << std::endl;
            if (errno == EINVAL)
                supported = true;
            else if (errno != ENODEV)
                L_ERR() << "Unexpected errno when testing for overlayfs " << errno << std::endl;
        }

        return supported;
    }

    TError Configure(std::shared_ptr<TValueMap> Config) override {

        if (!Supported())
            return TError(EError::InvalidValue, "overlay not supported");

        if (!config().volumes().enable_quota() && Volume->HaveQuota())
            return TError(EError::NotSupported, "project quota is disabled");

        return TError::Success();
    }

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TPath upper = storage / "upper";
        TPath work = storage / "work";
        TError error;
        std::stringstream lower;
        int index = 0;

        if (Volume->HaveQuota()) {
            Volume->GetQuota(quota.SpaceLimit, quota.InodeLimit);
            L_ACT() << "Creating project quota: " << quota.Path << " bytes: "
                    << quota.SpaceLimit << " inodes: " << quota.InodeLimit << std::endl;
            error = quota.Create();
            if (error)
                  return error;
        }

        for (auto layer: Volume->GetLayers()) {
            if (index++)
                lower << ":";
            lower << layer;
        }

        if (!upper.Exists()) {
            error = upper.Mkdir(0755);
            if (error)
                goto err;
        }

        error = upper.Chown(Volume->GetCred());
        if (error)
            goto err;

        error = upper.Chmod(Volume->GetPermissions());
        if (error)
            goto err;

        if (!work.Exists()) {
            error = work.Mkdir(0755);
            if (error)
                goto err;
        } else
            work.ClearDirectory();

        error = Volume->GetPath().Mount("overlay", "overlay",
                                        Volume->GetMountFlags(),
                                        { "lowerdir=" + lower.str(),
                                          "upperdir=" + upper.ToString(),
                                          "workdir=" + work.ToString() });
        if (!error)
            return error;
err:
        if (Volume->HaveQuota())
            (void)quota.Destroy();
        return error;
    }

    TError Clear() override {
        return (Volume->GetStorage() / "upper").ClearDirectory();
    }

    TError Destroy() override {
        TPath storage = Volume->GetStorage();
        TProjectQuota quota(storage);
        TPath path = Volume->GetPath();
        TError error, error2;

        error = path.UmountAll();
        if (error)
            L_ERR() << "Can't umount overlay: " << error << std::endl;

        if (Volume->IsAutoStorage()) {
            error2 = storage.ClearDirectory();
            if (error2) {
                if (!error)
                    error = error2;
                L_ERR() << "Can't clear overlay storage: " << error2 << std::endl;
                (void)(storage / "upper").RemoveAll();
            }
        }

        TPath work = storage / "work";
        if (work.Exists())
            (void)work.RemoveAll();

        if (Volume->HaveQuota() && quota.Exists()) {
            L_ACT() << "Destroying project quota: " << quota.Path << std::endl;
            error = quota.Destroy();
            if (error)
                L_ERR() << "Can't destroy quota: " << error << std::endl;
        }

        return error;
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        TProjectQuota quota(Volume->GetStorage());

        quota.SpaceLimit = space_limit;
        quota.InodeLimit = inode_limit;
        if (!Volume->HaveQuota()) {
            L_ACT() << "Creating project quota: " << quota.Path << std::endl;
            return quota.Create();
        }
        L_ACT() << "Resizing project quota: " << quota.Path << std::endl;
        return quota.Resize();
    }

    TError StatFS(TStatFS &result) override {
        if (Volume->HaveQuota())
            return TProjectQuota(Volume->GetStorage()).StatFS(result);
        return Volume->GetPath().StatFS(result);
    }
};


/* TVolumeRbdBackend - ext4 in ceph rados block device */

class TVolumeRbdBackend : public TVolumeBackend {
    int DeviceIndex = -1;

public:

    std::string GetDevice() {
        if (DeviceIndex < 0)
            return "";
        return "/dev/rbd" + std::to_string(DeviceIndex);
    }

    TError Save(std::shared_ptr<TValueMap> Config) override {
        return Config->Set<int>(V_LOOP_DEV, DeviceIndex);
    }

    TError Restore(std::shared_ptr<TValueMap> Config) override {
        DeviceIndex = Config->Get<int>(V_LOOP_DEV);
        return TError::Success();
    }

    TError MapDevice(std::string id, std::string pool, std::string image,
                     std::string &device) {
        std::vector<std::string> lines;
        L_ACT() << "Map rbd device " << id << "@" << pool << "/" << image << std::endl;
        TError error = Popen("rbd --id=\"" + id + "\" --pool=\"" + pool +
                             "\" map \"" + image + "\"", lines);
        if (error)
            return error;
        if (lines.size() != 1)
            return TError(EError::InvalidValue, "rbd map output have wrong lines count");
        device = StringTrim(lines[0]);
        return TError::Success();
    }

    TError UnmapDevice(std::string device) {
        int status;
        L_ACT() << "Unmap rbd device " << device << std::endl;
        TError error = Run({"rbd", "unmap", device}, status);
        if (!error && status)
            error = TError(EError::Unknown, "rbd unmap " + device +
                                " returned " + std::to_string(status));
        return error;
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        std::string id, pool, image, device;
        std::vector<std::string> tok;
        TError error, error2;

        error = SplitEscapedString(Volume->GetStorage().ToString(), '@', tok);
        if (error)
            return error;
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        id = tok[0];
        image = tok[1];
        tok.clear();
        error = SplitEscapedString(image, '/', tok);
        if (error)
            return error;
        if (tok.size() != 2)
            return TError(EError::InvalidValue, "Invalid rbd storage");
        pool = tok[0];
        image = tok[1];

        error = MapDevice(id, pool, image, device);
        if (error)
            return error;

        if (!StringStartsWith(device, "/dev/rbd")) {
            UnmapDevice(device);
            return TError(EError::InvalidValue, "not rbd device: " + device);
        }

        error = StringToInt(device.substr(8), DeviceIndex);
        if (error) {
            UnmapDevice(device);
            return error;
        }

        error = path.Mount(device, "ext4", Volume->GetMountFlags(), {});
        if (error)
            UnmapDevice(device);
        return error;
    }

    TError Destroy() override {
        std::string device = GetDevice();
        TPath path = Volume->GetPath();
        TError error, error2;

        if (DeviceIndex < 0)
            return TError::Success();

        error = path.UmountAll();
        error2 = UnmapDevice(device);
        if (!error)
            error = error2;
        DeviceIndex = -1;
        return error;
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "rbd backend doesn't suppport resize");
    }

    TError StatFS(TStatFS &result) override {
        return Volume->GetPath().StatFS(result);
    }
};


/* TVolume */

TError TVolume::OpenBackend() {
    if (GetBackend() == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend());
    else if (GetBackend() == "tmpfs")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeTmpfsBackend());
    else if (GetBackend() == "quota")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeQuotaBackend());
    else if (GetBackend() == "native")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeNativeBackend());
    else if (GetBackend() == "overlay")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeOverlayBackend());
    else if (GetBackend() == "loop")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeLoopBackend());
    else if (GetBackend() == "rbd")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeRbdBackend());
    else
        return TError(EError::InvalidValue, "Unknown volume backend: " + GetBackend());

    Backend->Volume = this;

    return TError::Success();
}

/* /place/porto_volumes/<id>/<type> */
TPath TVolume::GetInternal(std::string type) const {
    return Place / config().volumes().volume_dir() / GetId() / type;
}

/* /chroot/porto/<type>_<id> */
TPath TVolume::GetChrootInternal(TPath container_root, std::string type) const {
    TPath porto_path = container_root / config().container().chroot_porto_dir();
    if (!porto_path.Exists() && porto_path.Mkdir(0755))
        return TPath();
    return porto_path / (type + "_" + GetId());
}

TPath TVolume::GetPath() const {
    return Config->Get<std::string>(V_PATH);
}

bool TVolume::IsAutoPath() const {
    return Config->Get<bool>(V_AUTO_PATH);
}

bool TVolume::IsAutoStorage() const {
    return !Config->HasValue(V_STORAGE);
}

TPath TVolume::GetStorage() const {
    if (Config->HasValue(V_STORAGE))
        return TPath(Config->Get<std::string>(V_STORAGE));
    else
        return GetInternal(GetBackend());
}

unsigned long TVolume::GetMountFlags() const {
    unsigned flags = 0;

    if (IsReadOnly())
        flags |= MS_RDONLY;

    flags |= MS_NODEV | MS_NOSUID;

    return flags;
}

std::vector<TPath> TVolume::GetLayers() const {
    std::vector<TPath> result;

    for (auto layer: Config->Get<std::vector<std::string>>(V_LAYERS)) {
        TPath path(layer);
        if (!path.IsAbsolute())
            path = Place / config().volumes().layers_dir() / layer;
        result.push_back(path);
    }

    return result;
}

TError TVolume::CheckGuarantee(TVolumeHolder &holder,
        uint64_t space_guarantee, uint64_t inode_guarantee) const {
    auto backend = GetBackend();
    TStatFS current, total;
    TPath storage;

    if (backend == "rbd" || backend == "tmpfs")
        return TError::Success();

    if (!space_guarantee && !inode_guarantee)
        return TError::Success();

    if (IsAutoStorage())
        storage = Place / config().volumes().volume_dir();
    else
        storage = GetStorage();

    TError error = storage.StatFS(total);
    if (error)
        return error;

    if (!IsReady() || StatFS(current))
        current.Reset();

    /* Check available space as is */
    if (total.SpaceAvail + current.SpaceUsage < space_guarantee)
        return TError(EError::NoSpace, "Not enough space for volume guarantee: " +
                      std::to_string(total.SpaceAvail) + " available " +
                      std::to_string(current.SpaceUsage) + " used");

    if (total.InodeAvail + current.InodeUsage < inode_guarantee &&
            backend != "loop")
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used");

    /* Estimate unclaimed guarantees */
    uint64_t space_claimed = 0, space_guaranteed = 0;
    uint64_t inode_claimed = 0, inode_guaranteed = 0;
    for (auto path : holder.ListPaths()) {
        auto volume = holder.Find(path);
        if (volume == nullptr || volume.get() == this ||
                volume->GetStorage().GetDev() != storage.GetDev())
            continue;

        auto volume_backend = volume->GetBackend();

        /* rbd stored remotely, plain cannot provide usage */
        if (volume_backend == "rbd" || volume_backend == "plain")
            continue;

        TStatFS stat;
        uint64_t volume_space_guarantee, volume_inode_guarantee;

        volume->GetGuarantee(volume_space_guarantee, volume_inode_guarantee);
        if (!volume_space_guarantee && !volume_inode_guarantee)
            continue;

        if (!volume->IsReady() || volume->StatFS(stat))
            stat.Reset();

        space_guaranteed += volume_space_guarantee;
        if (stat.SpaceUsage < volume_space_guarantee)
            space_claimed += stat.SpaceUsage;
        else
            space_claimed += volume_space_guarantee;

        if (volume_backend != "loop") {
            inode_guaranteed += volume_inode_guarantee;
            if (stat.InodeUsage < volume_inode_guarantee)
                inode_claimed += stat.InodeUsage;
            else
                inode_claimed += volume_inode_guarantee;
        }
    }

    if (total.SpaceAvail + current.SpaceUsage + space_claimed <
            space_guarantee + space_guaranteed)
        return TError(EError::NoSpace, "Not enough space for volume guarantee: " +
                      std::to_string(total.SpaceAvail) + " available " +
                      std::to_string(current.SpaceUsage) + " used " +
                      std::to_string(space_claimed) + " claimed " +
                      std::to_string(space_guaranteed) + " guaranteed");

    if (backend != "loop" &&
            total.InodeAvail + current.InodeUsage + inode_claimed <
            inode_guarantee + inode_guaranteed)
        return TError(EError::NoSpace, "Not enough inodes for volume guarantee: " +
                      std::to_string(total.InodeAvail) + " available " +
                      std::to_string(current.InodeUsage) + " used " +
                      std::to_string(inode_claimed) + " claimed " +
                      std::to_string(inode_guaranteed) + " guaranteed");

    return TError::Success();
}

TError TVolume::Configure(const TPath &path, const TCred &creator_cred,
                          std::shared_ptr<TContainer> creator_container,
                          const std::map<std::string, std::string> &properties,
                          TVolumeHolder &holder) {
    auto backend = properties.count(V_BACKEND) ? properties.at(V_BACKEND) : "";
    TPath container_root = creator_container->RootPath();
    TError error;

    /* Verify place */
    if (properties.count(V_PLACE)) {
        Place = properties.at(V_PLACE);
        error = CheckPlace(Place);
        if (error)
            return error;
        CustomPlace = true;
    } else {
        Place = config().volumes().default_place();
        CustomPlace = false;
    }

    /* Verify volume path */
    if (!path.IsEmpty()) {
        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");
        if (!path.IsNormal())
            return TError(EError::InvalidValue, "Volume path must be normalized");
        if (!path.Exists())
            return TError(EError::InvalidValue, "Volume path does not exist");
        if (!path.IsDirectoryStrict())
            return TError(EError::InvalidValue, "Volume path must be a directory");
        if (!path.CanWrite(creator_cred))
            return TError(EError::Permission, "Volume path usage not permitted");
        error = Config->Set<std::string>(V_PATH, path.ToString());
        if (error)
            return error;
    } else {
        TPath volume_path;

        if (container_root.IsRoot())
            volume_path = GetInternal("volume");
        else
            volume_path = GetChrootInternal(container_root, "volume");
        if (volume_path.IsEmpty())
            return TError(EError::InvalidValue, "Cannot choose automatic volume path");

        error = Config->Set<std::string>(V_PATH, volume_path.ToString());
        if (error)
            return error;
        error = Config->Set<bool>(V_AUTO_PATH, true);
        if (error)
            return error;
    }

    /* Verify storage path */
    if (backend != "rbd" && backend != "tmpfs" && properties.count(V_STORAGE)) {
        TPath storage(properties.at(V_STORAGE));
        if (!storage.IsAbsolute())
            return TError(EError::InvalidValue, "Storage path must be absolute");
        if (!storage.IsNormal())
            return TError(EError::InvalidValue, "Storage path must be normalized");
        if (!storage.Exists())
            return TError(EError::InvalidValue, "Storage path does not exist");
        if (!storage.IsDirectoryFollow())
            return TError(EError::InvalidValue, "Storage path must be a directory");
        if (!storage.CanWrite(creator_cred))
            return TError(EError::Permission, "Storage path usage not permitted");
    }

    /* Save original creator. Just for the record. */
    error = Config->Set<std::string>(V_CREATOR, creator_container->GetName() + " " +
                    creator_cred.User() + " " + creator_cred.Group());
    if (error)
        return error;

    /* Set default credentials to creator */
    error = Config->Set<std::string>(V_USER, creator_cred.User());
    if (error)
        return error;
    error = Config->Set<std::string>(V_GROUP, creator_cred.Group());
    if (error)
        return error;

    /* Default permissions for volume root directory */
    error = Config->Set<std::string>(V_PERMISSIONS, "0775");
    if (error)
        return error;

    /* Apply properties */
    for (auto p: properties) {
        error = Config->SetValue(p.first, p.second);
        if (error)
            return error;
    }

    error = UserId(Config->Get<std::string>(V_USER), Cred.Uid);
    if (error)
        return error;

    error = GroupId(Config->Get<std::string>(V_GROUP), Cred.Gid);
    if (error)
        return error;

    /* Verify default credentials */
    if (Cred.Uid != creator_cred.Uid && !creator_cred.IsRootUser())
        return TError(EError::Permission, "Changing user is not permitted");

    if (Cred.Gid != creator_cred.Gid && !creator_cred.IsRootUser() &&
            !creator_cred.IsMemberOf(Cred.Gid))
        return TError(EError::Permission, "Changing group is not permitted");

    /* Verify default permissions */
    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    /* Verify and resolve layers */
    if (Config->HasValue(V_LAYERS)) {
        auto layers = Config->Get<std::vector<std::string>>(V_LAYERS);

        for (auto &l: layers) {
            TPath layer(l);
            if (!layer.IsNormal())
                return TError(EError::InvalidValue, "Layer path must be normalized");
            if (layer.IsAbsolute()) {
                layer = container_root / layer;
                l = layer.ToString();
                if (!layer.Exists())
                    return TError(EError::LayerNotFound, "Layer not found");
                if (!layer.CanWrite(creator_cred))
                    return TError(EError::Permission, "Layer path not permitted");
            } else {
                if (l.find('/') != std::string::npos)
                    return TError(EError::InvalidValue, "Internal layer storage has no directories");
                layer = Place / config().volumes().layers_dir() / layer;
            }
            if (!layer.Exists())
                return TError(EError::LayerNotFound, "Layer not found");
            if (!layer.IsDirectoryFollow())
                return TError(EError::InvalidValue, "Layer must be a directory");
        }

        error = Config->Set<std::vector<std::string>>(V_LAYERS, layers);
        if (error)
            return error;
    }

    /* Verify guarantees */
    if (Config->HasValue(V_SPACE_LIMIT) && Config->HasValue(V_SPACE_GUARANTEE) &&
            Config->Get<uint64_t>(V_SPACE_LIMIT) < Config->Get<uint64_t>(V_SPACE_GUARANTEE))
        return TError(EError::InvalidValue, "Space guarantree bigger than limit");

    if (Config->HasValue(V_INODE_LIMIT) && Config->HasValue(V_INODE_GUARANTEE) &&
            Config->Get<uint64_t>(V_INODE_LIMIT) < Config->Get<uint64_t>(V_INODE_GUARANTEE))
        return TError(EError::InvalidValue, "Inode guarantree bigger than limit");

    /* Autodetect volume backend */
    if (!Config->HasValue(V_BACKEND)) {
        if (HaveQuota() && !TVolumeNativeBackend::Supported())
            error = Config->Set<std::string>(V_BACKEND, "loop");
        else if (Config->HasValue(V_LAYERS) && TVolumeOverlayBackend::Supported())
            error = Config->Set<std::string>(V_BACKEND, "overlay");
        else if (TVolumeNativeBackend::Supported())
            error = Config->Set<std::string>(V_BACKEND, "native");
        else
            error = Config->Set<std::string>(V_BACKEND, "plain");
        if (error)
            return error;
    }

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Configure(Config);
    if (error)
        return error;

    uint64_t space_guarantee, inode_guarantee;
    GetGuarantee(space_guarantee, inode_guarantee);
    error = CheckGuarantee(holder, space_guarantee, inode_guarantee);
    if (error)
        return error;

    return TError::Success();
}

TError TVolume::Build() {
    TPath storage = GetStorage();
    TPath path = GetPath();
    TPath internal = GetInternal("");

    L_ACT() << "Build volume: " << path
            << " backend: " << GetBackend() << std::endl;

    TError error = internal.Mkdir(0755);
    if (error)
        goto err_internal;

    if (IsAutoStorage()) {
        error = storage.Mkdir(0755);
        if (error)
            goto err_storage;
    }

    if (IsAutoPath()) {
        error = path.Mkdir(0755);
        if (error)
            goto err_path;
    }

    error = Backend->Build();
    if (error)
        goto err_build;

    error = Backend->Save(Config);
    if (error)
        goto err_save;

    if (Config->HasValue(V_LAYERS) && GetBackend() != "overlay") {
        L_ACT() << "Merge layers into volume: " << path << std::endl;

        auto layers = GetLayers();
        for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
            error = CopyRecursive(*layer, path);
            if (error)
                goto err_merge;
        }

        error = SanitizeLayer(path, true);
        if (error)
            goto err_merge;

        error = path.Chown(GetCred());
        if (error)
            return error;

        error = path.Chmod(GetPermissions());
        if (error)
            return error;
    }

    return TError::Success();

err_merge:
err_save:
    (void)Backend->Destroy();
err_build:
    if (IsAutoPath()) {
        (void)path.RemoveAll();
    }
err_path:
    if (IsAutoStorage())
        (void)storage.RemoveAll();
err_storage:
    (void)internal.RemoveAll();
err_internal:
    return error;
}

TError TVolume::Clear() {
    L_ACT() << "Clear volume: " << GetPath() << std::endl;
    return Backend->Clear();
}

TError TVolume::Destroy(TVolumeHolder &holder) {
    TPath internal = GetInternal("");
    TPath storage = GetStorage();
    TPath path = GetPath();
    TError ret = TError::Success(), error;

    L_ACT() << "Destroy volume: " << GetPath()
            << " backend: " << GetBackend() << std::endl;

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR() << "Can't destroy volume backend: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsAutoStorage() && storage.Exists()) {
        error = storage.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (IsAutoPath() && path.Exists()) {
        error = path.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove volume path: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (internal.Exists()) {
        error = internal.RemoveAll();
        if (error) {
            L_ERR() << "Can't remove internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (Config->HasValue(V_LAYERS)) {
        auto layers = Config->Get<std::vector<std::string>>(V_LAYERS);
        Config->Set<std::vector<std::string>>(V_LAYERS, {});
        for (auto &layer: layers) {
            if (StringStartsWith(layer, "_weak_")) {
                error = holder.RemoveLayer(layer, Place);
                if (error && error.GetError() != EError::Busy)
                    L_ERR() << "Cannot remove layer: " << error << std::endl;
            }
        }
    }

    return ret;
}

TError TVolume::StatFS(TStatFS &result) const {
    return Backend->StatFS(result);
}

TError TVolume::Tune(TVolumeHolder &holder,
        const std::map<std::string, std::string> &properties) {
    TError error;

    for (auto p: properties) {
        auto prop = Config->Find(p.first);
        if (!prop)
            return TError(EError::InvalidProperty,
                    "Invalid volume property: " + p.first);
        if (!prop->HasFlag(DYNAMIC_VALUE))
            return TError(EError::InvalidProperty,
                    "Volume property " + p.first + " cannot be changed");
    }

    if (properties.count(V_SPACE_LIMIT) || properties.count(V_INODE_LIMIT)) {
        uint64_t spaceLimit, inodeLimit;

        GetQuota(spaceLimit, inodeLimit);
        if (properties.count(V_SPACE_LIMIT)) {
            error = StringToSize(properties.at(V_SPACE_LIMIT), spaceLimit);
            if (error)
                return error;
        }
        if (properties.count(V_INODE_LIMIT)) {
            error = StringToSize(properties.at(V_INODE_LIMIT), inodeLimit);
            if (error)
                return error;
        }
        error = Resize(spaceLimit, inodeLimit);
    }

    if (properties.count(V_SPACE_GUARANTEE) || properties.count(V_INODE_GUARANTEE)) {
        uint64_t space_guarantee, inode_guarantee;

        GetGuarantee(space_guarantee, inode_guarantee);
        if (properties.count(V_SPACE_GUARANTEE)) {
            error = StringToSize(properties.at(V_SPACE_GUARANTEE), space_guarantee);
            if (error)
                return error;
        }
        if (properties.count(V_INODE_GUARANTEE)) {
            error = StringToSize(properties.at(V_INODE_GUARANTEE), inode_guarantee);
            if (error)
                return error;
        }

        auto lock = holder.ScopedLock();
        error = CheckGuarantee(holder, space_guarantee, inode_guarantee);
        if (error)
            return error;
        Config->Set<uint64_t>(V_SPACE_GUARANTEE, space_guarantee);
        Config->Set<uint64_t>(V_INODE_GUARANTEE, inode_guarantee);
    }

    return error;
}

TError TVolume::Resize(uint64_t space_limit, uint64_t inode_limit) {
    L_ACT() << "Resize volume: " << GetPath() << " to bytes: "
            << space_limit << " inodes: " << inode_limit << std::endl;
    TError error = Backend->Resize(space_limit, inode_limit);
    if (error)
        return error;
    Config->Set<uint64_t>(V_SPACE_LIMIT, space_limit);
    Config->Set<uint64_t>(V_INODE_LIMIT, inode_limit);
    return TError::Success();
}

TError TVolume::GetUpperLayer(TPath &upper) {
    if (GetBackend() == "overlay")
        upper = GetStorage() / "upper";
    else
        upper = GetPath();
    return TError::Success();
}

TError TVolume::LinkContainer(std::string name) {
    std::vector<std::string> containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.push_back(name);
    return Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
}

bool TVolume::UnlinkContainer(std::string name) {
    auto containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.erase(std::remove(containers.begin(), containers.end(), name), containers.end());
    TError error = Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
    return containers.empty();
}

std::map<std::string, std::string> TVolume::GetProperties(TPath container_root) {
    std::map<std::string, std::string> ret;
    TStatFS stat;

    if (IsReady() && !StatFS(stat)) {
        ret[V_SPACE_USED] = std::to_string(stat.SpaceUsage);
        ret[V_INODE_USED] = std::to_string(stat.InodeUsage);
        ret[V_SPACE_AVAILABLE] = std::to_string(stat.SpaceAvail);
        ret[V_INODE_AVAILABLE] = std::to_string(stat.InodeAvail);
    }

    for (auto name: Config->List()) {
        auto property = Config->Find(name);
        if (!property->HasFlag(HIDDEN_VALUE) && property->HasValue())
            property->GetString(ret[name]);
    }

    if (Config->HasValue(V_LAYERS)) {
        auto layers = Config->Get<std::vector<std::string>>(V_LAYERS);
        for (auto &l: layers) {
            TPath path(l);
            if (path.IsAbsolute())
                l = container_root.InnerPath(path).ToString();
        }
        ret[V_LAYERS] = MergeEscapeStrings(layers, ";", "\\;");
    }

    if (CustomPlace)
        ret[V_PLACE] = Place.ToString();

    return ret;
}

TError TVolume::CheckPermission(const TCred &ucred) const {
    if (ucred.IsPermitted(Cred))
        return TError::Success();

    return TError(EError::Permission, "Permission denied");
}

TError TVolume::Restore() {
    TError error;

    if (!IsReady())
        return TError(EError::Busy, "Volume not ready");

    CustomPlace = Config->HasValue(V_PLACE);
    if (CustomPlace)
        Place = Config->Get<std::string>(V_PLACE);
    else
        Place = config().volumes().default_place();

    error = UserId(Config->Get<std::string>(V_USER), Cred.Uid);
    if (!error)
        error = GroupId(Config->Get<std::string>(V_GROUP), Cred.Gid);
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + GetPath().ToString() + " credentials: " +
                      Config->Get<std::string>(V_USER) + " " +
                      Config->Get<std::string>(V_GROUP));

    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore(Config);
    if (error)
        return error;

    return TError::Success();
}

/* TVolumeHolder */

const std::vector<std::pair<std::string, std::string>> TVolumeHolder::ListProperties() {
    return {
        { V_BACKEND,     "plain|tmpfs|quota|native|overlay|loop|rbd (default - autodetect)" },
        { V_STORAGE,     "path to data storage (default - internal)" },
        { V_PLACE,       "place for layers and default storage (optional)" },
        { V_READY,       "true|false - contruction complete (ro)" },
        { V_PRIVATE,     "user-defined property" },
        { V_USER,        "user (default - creator)" },
        { V_GROUP,       "group (default - creator)" },
        { V_PERMISSIONS, "directory permissions (default - 0775)" },
        { V_CREATOR,     "container user group (ro)" },
        { V_READ_ONLY,   "true|false (default - false)" },
        { V_LAYERS,      "top-layer;...;bottom-layer - overlayfs layers" },
        { V_SPACE_LIMIT, "disk space limit (dynamic, default zero - unlimited)" },
        { V_INODE_LIMIT, "disk inode limit (dynamic, default zero - unlimited)"},
        { V_SPACE_GUARANTEE,    "disk space guarantee (dynamic, default - zero)" },
        { V_INODE_GUARANTEE,    "disk inode guarantee (dynamic, default - zero)" },
        { V_SPACE_USED,  "current disk space usage (ro)" },
        { V_INODE_USED,  "current disk inode used (ro)" },
        { V_SPACE_AVAILABLE,    "available disk space (ro)" },
        { V_INODE_AVAILABLE,    "available disk inodes (ro)" },
    };
}

static void RegisterVolumeProperties(std::shared_ptr<TRawValueMap> m) {
    m->Add(V_PATH, new TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_AUTO_PATH, new TBoolValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_STORAGE, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_PLACE, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_BACKEND, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_USER, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_GROUP, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_PERMISSIONS, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_CREATOR, new TStringValue(READ_ONLY_VALUE | PERSISTENT_VALUE));

    m->Add(V_ID, new TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READY, new TBoolValue(READ_ONLY_VALUE | PERSISTENT_VALUE));
    m->Add(V_PRIVATE, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_CONTAINERS, new TListValue(HIDDEN_VALUE | PERSISTENT_VALUE));

    m->Add(V_LOOP_DEV, new TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READ_ONLY, new TBoolValue(PERSISTENT_VALUE));
    m->Add(V_LAYERS, new TListValue(HIDDEN_VALUE | PERSISTENT_VALUE));

    m->Add(V_SPACE_LIMIT, new TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE));
    m->Add(V_INODE_LIMIT, new TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE));

    m->Add(V_SPACE_GUARANTEE, new TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE));
    m->Add(V_INODE_GUARANTEE, new TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE));
}

TError TVolumeHolder::Create(std::shared_ptr<TVolume> &volume) {
    std::string id = std::to_string(NextId);
    auto node = Storage->GetNode(id);
    auto config = std::make_shared<TValueMap>(node);
    RegisterVolumeProperties(config);
    TError error = config->Set<std::string>(V_ID, id);
    if (error) {
        config->Remove();
        return error;
    }
    volume = std::make_shared<TVolume>(config);
    NextId++;
    return TError::Success();
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {
    volume->Config->Remove();
}

TError CheckPlace(const TPath &place, bool init) {
    struct stat st;
    TError error;
    uid_t RootUser = 0;
    gid_t PortoGroup = GetPortoGroupId();

    if (!place.IsAbsolute() || !place.IsNormal())
        return TError(EError::InvalidValue, "place path must be normalized");

    TPath volumes = place / config().volumes().volume_dir();
    if (init && !volumes.IsDirectoryStrict()) {
        (void)volumes.Unlink();
        error = volumes.MkdirAll(0755);
        if (error)
            return error;
    }
    error = volumes.StatStrict(st);
    if (error || !S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, "in place " + volumes.ToString() + " must be directory");
    if (st.st_uid != RootUser || st.st_gid != PortoGroup)
        volumes.Chown(RootUser, PortoGroup);
    if ((st.st_mode & 0777) != 0755)
        volumes.Chmod(0755);

    TPath layers = place / config().volumes().layers_dir();
    if (init && !layers.IsDirectoryStrict()) {
        (void)layers.Unlink();
        error = layers.MkdirAll(0700);
        if (error)
            return error;
    }
    error = layers.StatStrict(st);
    if (error || !S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, "in place " + layers.ToString() + " must be directory");
    if (st.st_uid != RootUser || st.st_gid != PortoGroup)
        layers.Chown(RootUser, PortoGroup);
    if ((st.st_mode & 0777) != 0700)
        layers.Chmod(0700);

    TPath layers_tmp = layers / "_tmp_";
    if (!layers_tmp.IsDirectoryStrict()) {
        (void)layers_tmp.Unlink();
        (void)layers_tmp.Mkdir(0700);
    }

    return TError::Success();
}

TError TVolumeHolder::RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder) {
    std::vector<std::shared_ptr<TKeyValueNode>> list;
    TError error;

    TPath place(config().volumes().default_place());
    error = CheckPlace(place, true);
    if (error)
        L_ERR() << "Cannot prepare place: " << error << std::endl;

    L_ACT() << "Remove stale layers..." << std::endl;
    TPath layers_tmp = place / config().volumes().layers_dir() / "_tmp_";
    error = layers_tmp.ClearDirectory();
    if (error)
        L_ERR() << "Cannot remove stale layers: " << error << std::endl; 

    error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &node : list) {
        L_ACT() << "Restore volume: " << node->Name << std::endl;

        auto config = std::make_shared<TValueMap>(node);
        RegisterVolumeProperties(config);
        error = config->Restore();
        if (error || !config->HasValue(V_ID)) {
            L_WRN() << "Corrupted volume config " << node << " removed: " << error << std::endl;
            (void)config->Remove();
            continue;
        }

        uint64_t id;
        if (!StringToUint64(config->Get<std::string>(V_ID), id)) {
            if (id >= NextId)
                NextId = id + 1;
        }

        auto volume = std::make_shared<TVolume>(config);
        error = volume->Restore();
        if (error) {
            L_WRN() << "Corrupted volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy(*this);
            (void)Remove(volume);
            continue;
        }

        error = Register(volume);
        if (error) {
            L_WRN() << "Cannot register volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy(*this);
            (void)Remove(volume);
            continue;
        }

        for (auto name: volume->GetContainers()) {
            std::shared_ptr<TContainer> container;
            if (!Cholder->Get(name, container)) {
                container->VolumeHolder = shared_from_this();
                container->Volumes.emplace_back(volume);
            } else if (!volume->UnlinkContainer(name)) {
                (void)volume->Destroy(*this);
                (void)Unregister(volume);
                (void)Remove(volume);
            }
        }

        L() << "Volume " << volume->GetPath() << " restored" << std::endl;
    }

    TPath volumes = place / config().volumes().volume_dir();

    L_ACT() << "Remove stale volumes..." << std::endl;

    std::vector<std::string> subdirs;
    error = volumes.ReadDirectory(subdirs);
    if (error)
        L_ERR() << "Cannot list " << volumes << std::endl;

    for (auto dir_name: subdirs) {
        bool used = false;
        for (auto v: Volumes) {
            if (v.second->GetId() == dir_name) {
                used = true;
                break;
            }
        }
        if (used)
            continue;

        TPath dir = volumes / dir_name;
        TPath mnt = dir / "volume";
        if (mnt.Exists()) {
            error = mnt.UmountAll();
            if (error)
                L_ERR() << "Cannot umount volume " << mnt << ": " << error << std::endl;
        }
        error = dir.RemoveAll();
        if (error)
            L_ERR() << "Cannot remove directory " << dir << std::endl;
    }

    return TError::Success();
}

void TVolumeHolder::Destroy() {
    while (Volumes.begin() != Volumes.end()) {
        auto name = Volumes.begin()->first;
        auto volume = Volumes.begin()->second;
        TError error = volume->Destroy(*this);
        if (error)
            L_ERR() << "Can't destroy volume " << name << ": " << error << std::endl;
        Unregister(volume);
        Remove(volume);
    }
}

TError TVolumeHolder::Register(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetPath()) == Volumes.end()) {
        Volumes[volume->GetPath()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "Volume already exists");
}

void TVolumeHolder::Unregister(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
}

std::shared_ptr<TVolume> TVolumeHolder::Find(const TPath &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<TPath> TVolumeHolder::ListPaths() const {
    std::vector<TPath> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}

bool TVolumeHolder::LayerInUse(const TPath &layer) {
    for (auto &volume : Volumes) {
        for (auto &l: volume.second->GetLayers()) {
            if (l.NormalPath() == layer)
                return true;
        }
    }
    return false;
}

TError TVolumeHolder::RemoveLayer(const std::string &name, const TPath &place) {
    TPath layers = place / config().volumes().layers_dir();
    TPath layer = layers / name;
    TError error;

    if (name.find('/') != std::string::npos)
        return TError(EError::InvalidValue, "Internal layer storage has no directories");

    if (!layer.Exists())
        return TError(EError::LayerNotFound, "Layer " + name + " not found");

    /* layers_tmp should already be created on startup */
    TPath layers_tmp = layers / "_tmp_";
    TPath layer_tmp = layers_tmp / name;

    auto lock = ScopedLock();
    if (LayerInUse(layer))
        error = TError(EError::Busy, "Layer " + name + " in use");
    else
        error = layer.Rename(layer_tmp);
    lock.unlock();

    if (!error)
        error = layer_tmp.RemoveAll();

    return error;
}

TError SanitizeLayer(TPath layer, bool merge) {
    std::vector<std::string> content;

    TError error = layer.ReadDirectory(content);
    if (error)
        return error;

    for (auto entry: content) {
        TPath path = layer / entry;

        /* Handle aufs whiteouts and metadata */
        if (entry.compare(0, 4, ".wh.") == 0) {

            /* Remove it completely */
            error = path.RemoveAll();
            if (error)
                return error;

            /* Opaque directory - hide entries in lower layers */
            if (entry == ".wh..wh..opq") {
                error = layer.SetXAttr("trusted.overlay.opaque", "y");
                if (error)
                    return error;
            }

            /* Metadata is done */
            if (entry.compare(0, 8, ".wh..wh.") == 0)
                continue;

            /* Remove whiteouted entry */
            path = layer / entry.substr(4);
            if (path.Exists()) {
                error = path.RemoveAll();
                if (error)
                    return error;
            }

            if (!merge) {
                /* Convert into overlayfs whiteout */
                error = path.Mknod(S_IFCHR, 0);
                if (error)
                    return error;
            }

            continue;
        }

        if (path.IsDirectoryStrict()) {
            error = SanitizeLayer(path, merge);
            if (error)
                return error;
        }
    }
    return TError::Success();
}
