#include "device.hpp"

extern "C" {
#include <sys/stat.h>
}

TError TDevice::Parse(TTuple &opt) {
    struct stat st;
    TError error;

    if (opt.size() < 2)
        return TError(EError::InvalidValue, "Invalid device config: " +
                      MergeEscapeStrings(opt, ' '));

    Name = opt[0];
    Path = TPath(Name);

    if (!Path.IsNormal())
        return TError(EError::InvalidValue, "Non-normalized name: " + Name);

    error = Path.StatFollow(st);
    if (error)
        return error;

    if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode))
        return TError(EError::InvalidValue, "Not a device node: " + Name);

    Device = st.st_rdev;
    User = st.st_uid;
    Group = st.st_gid;
    Mode = st.st_mode;

    Read = Write = Mknod = Wildcard = Privileged = false;

    for (char c: opt[1]) {
        switch (c) {
            case 'r':
                Read = true;
                break;
            case 'w':
                Write = true;
                break;
            case 'm':
                Mknod = true;
                break;
            case '*':
                Wildcard = true;
                Privileged = true;
                break;
            case '-':
                break;
            default:
                return TError(EError::InvalidValue, "Invalid access: " + opt[1]);
        }
    }

    if (opt.size() > 2)
        Name = opt[2];

    if (!TPath(Name).IsNormal())
        return TError(EError::InvalidValue, "Non-normalized name: " + Name);

    if (!StringStartsWith(Name, "/dev/"))
        Privileged = true;

    if (opt.size() > 3) {
        unsigned mode;
        error = StringToOct(opt[3], mode);
        if (error)
            return error;
        if (mode & ~0777)
            return TError(EError::InvalidValue, "invalid device mode: " + opt[3]);
        if ((Mode & 0777) < mode)
            Privileged = true;
        Mode = mode | (Mode & ~0777);
    }

    if (opt.size() > 4) {
        error = UserId(opt[4], User);
        if (error)
            return error;
        if (User != st.st_uid)
            Privileged = true;
    }

    if (opt.size() > 5) {
        error = GroupId(opt[5], Group);
        if (error)
            return error;
        if (Group != st.st_gid)
            Privileged = true;
    }

    return TError::Success();
}

std::string TDevice::Format() const {
    std::string perm;

    if (Read)
        perm += "r";
    if (Write)
        perm += "w";
    if (Mknod)
        perm += "m";
    if (Wildcard)
        perm += "*";
    if (perm == "")
        perm = "-";

    return Path.ToString() + " " + perm + " " + Name + " " +
           StringFormat("%o", Mode & 0777) + " " +
           UserName(User) + " " + GroupName(Group);
}

std::string TDevice::CgroupRule(bool allow) const {
    std::string rule;

    /* cgroup cannot parse rules with empty permissions */
    if (Read != allow && Write != allow && Mknod != allow)
        return "";

    if (S_ISBLK(Mode))
        rule = "b ";
    else
        rule = "c ";

    rule += std::to_string(major(Device)) + ":";

    if (Wildcard)
        rule += "* ";
    else
        rule += std::to_string(minor(Device)) + " ";

    if (Read == allow)
        rule += "r";
    if (Write == allow)
        rule += "w";
    if (Mknod == allow)
        rule += "m";

    return rule;
}

TError TDevice::Permitted(const TCred &cred) const {
    struct stat st;
    TError error = Path.StatFollow(st);
    if (error)
        return error;

    if (Read && !TFile::Access(st, cred, TFile::R))
        return TError(EError::Permission, cred.ToString() + " has no read permissions for " + Path.ToString());

    if (Write && !TFile::Access(st, cred, TFile::W))
        return TError(EError::Permission, cred.ToString() + " has no write permissions for " + Path.ToString());

    if (Privileged && !cred.IsRootUser())
        return TError(EError::Permission, cred.ToString() + " isn't root user for " + Path.ToString());

    return TError::Success();
}

TError TDevice::Makedev(const TPath &root) const {
    TPath path = root / Name;
    TError error;

    error = path.DirName().MkdirAll(0755);
    if (error)
        return error;

    if (!Wildcard) {
        struct stat st;

        if (!path.StatFollow(st)) {
            if (st.st_mode == Mode && st.st_rdev == Device &&
                    st.st_uid == User && st.st_gid == Group)
                return TError::Success();
            return TError(EError::Busy, "Different device node already exists: " + Name);
        }

        error = path.Mknod(Mode, Device);
        if (error)
            return error;

        error = path.Chown(User, Group);
        if (error)
            return error;
    }

    return TError::Success();
}
