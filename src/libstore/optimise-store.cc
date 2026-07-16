#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/util/thread-pool.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system.hh"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <random>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "store-config-private.hh"

namespace nix {

static void makeWritable(const std::filesystem::path & path)
{
    auto st = lstat(path);
    chmod(path, st.st_mode | S_IWUSR);
}

struct MakeReadOnly
{
    std::filesystem::path path;

    MakeReadOnly(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~MakeReadOnly()
    {
        try {
            /* This will make the path read-only. */
            if (!path.empty())
                canonicaliseTimestampAndPermissions(path.string());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

LocalStore::InodeHash LocalStore::loadInodeHash()
{
    debug("loading hash inodes in memory");
    InodeHash inodeHash;

    AutoCloseDir dir(opendir(linksDir.string().c_str()));
    if (!dir)
        throw SysError("opening directory %1%", PathFmt(linksDir));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();
        // We don't care if we hit non-hash files, anything goes
        inodeHash.insert(dirent->d_ino);
    }
    if (errno)
        throw SysError("reading directory %1%", PathFmt(linksDir));

    printMsg(lvlTalkative, "loaded %1% hash inodes", inodeHash.size());

    return inodeHash;
}

Strings LocalStore::readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash)
{
    Strings names;

    AutoCloseDir dir(opendir(path.string().c_str()));
    if (!dir)
        throw SysError("opening directory %s", PathFmt(path));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();

        if (inodeHash.contains(dirent->d_ino)) {
            debug("'%1%' is already linked", dirent->d_name);
            continue;
        }

        std::string name = dirent->d_name;
        if (name == "." || name == "..")
            continue;
        names.push_back(name);
    }
    if (errno)
        throw SysError("reading directory %s", PathFmt(path));

    return names;
}

void LocalStore::optimisePath_(
    Activity * act,
    OptimiseStats & stats,
    const std::filesystem::path & path,
    InodeHash & inodeHash,
    RepairFlag repair,
    bool * parentToggled,
    const ImportFileHashes * fileHashes,
    size_t fileHashesBase)
{
    checkInterrupt();

    auto st = lstat(path);

    if (S_ISDIR(st.st_mode)) {
        Strings names = readDirectoryIgnoringInodes(path, inodeHash);

        /* The first child that relinks makes this directory writable;
           restore it once after all children instead of per file. */
        bool toggled = false;
        Finally restore([&]() {
            if (toggled) {
                try {
                    canonicaliseTimestampAndPermissions(path);
                } catch (...) {
                    ignoreExceptionInDestructor();
                }
            }
        });
        for (auto & i : names)
            optimisePath_(act, stats, path / i, inodeHash, repair, &toggled, fileHashes, fileHashesBase);
        return;
    }

    /* We can hard link regular files and maybe symlinks. */
    if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
        && !S_ISLNK(st.st_mode)
#endif
    )
        return;

    /* Sometimes SNAFUs can cause files in the Nix store to be
       modified, in particular when running programs as root under
       NixOS (example: $fontconfig/var/cache being modified).  Skip
       those files.  FIXME: check the modification time. */
    if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
        warn("skipping suspicious writable file '%s'", PathFmt(path));
        return;
    }

    /* Never link empty files: saves zero bytes and welds unrelated
       runtime-mutable paths (subuid, wtmp, lastlog, ...) into one
       inode, which a rootfs generation booted as an overlay lower
       layer then writes through. Also the classic too-many-links
       case. */
    if (S_ISREG(st.st_mode) && st.st_size == 0)
        return;

    /* This can still happen on top-level files. */
    if (st.st_nlink > 1 && inodeHash.contains(st.st_ino)) {
        debug("%s is already linked, with %d other file(s)", PathFmt(path), st.st_nlink - 2);
        return;
    }

    /* Hash the file.  Note that hashPath() returns the hash over the
       NAR serialisation, which includes the execute bit on the file.
       Thus, executable and non-executable files with the same
       contents *won't* be linked (which is good because otherwise the
       permissions would be screwed up).

       Also note that if `path' is a symlink, then we're hashing the
       contents of the symlink (i.e. the result of readlink()), not
       the contents of the target (which may not even exist).

       An import that captured per-file hashes while restoring spares
       the content re-read; anything not in the map (concurrent
       changes, symlinks) falls back to hashing from disk. */
    Hash hash = [&] {
        if (fileHashes && S_ISREG(st.st_mode)) {
            auto rel = path.native().substr(fileHashesBase);
            if (rel.empty())
                rel = "/";
            if (auto it = fileHashes->files.find(rel); it != fileHashes->files.end())
                return it->second;
        }
        return hashPath(makeFSSourceAccessor(path), FileSerialisationMethod::NixArchive, HashAlgorithm::SHA256).hash;
    }();
    debug("%s has hash '%s'", PathFmt(path), hash.to_string(HashFormat::Nix32, true));

    /* Check if this is a known hash. Single component parse: this runs
       once per file. */
    std::filesystem::path linkPath{linksDir.native() + '/' + hash.to_string(HashFormat::Nix32, false)};

    auto stLink = maybeLstat(linkPath);

    /* Maybe delete the link, if it has been corrupted. */
    if (stLink) {
        if (st.st_size != stLink->st_size || (repair && hash != ({
                                                  hashPath(
                                                      makeFSSourceAccessor(linkPath),
                                                      FileSerialisationMethod::NixArchive,
                                                      HashAlgorithm::SHA256)
                                                      .hash;
                                              }))) {
            // XXX: Consider overwriting linkPath with our valid version.
            warn("removing corrupted link %s", PathFmt(linkPath));
            warn(
                "There may be more corrupted paths."
                "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
            unlinkIfExists(linkPath);
            stLink.reset();
        }
    }

    if (!stLink) {
        /* Nope, create a hard link in the links directory. */
        try {
            std::filesystem::create_hard_link(path, linkPath);
            inodeHash.insert(st.st_ino);
            /* Our file is now the canonical copy in the links
               directory; nothing left to replace. */
            return;
        } catch (std::filesystem::filesystem_error & e) {
            if (e.code() == std::errc::file_exists) {
                /* Fall through if another process created ‘linkPath’ before
                   we did. */
                stLink = maybeLstat(linkPath);

                /* A concurrent garbage collection may have removed the
                   link again already. Skip optimising this path; a
                   later pass will dedup it. */
                if (!stLink)
                    return;
            }

            else if (e.code() == std::errc::no_space_on_device) {
                /* On ext4, that probably means the directory index is
                   full.  When that happens, it's fine to ignore it: we
                   just effectively disable deduplication of this
                   file.
                   */
                printInfo("cannot link %s to '%s': %s", PathFmt(linkPath), PathFmt(path), e.code().message());
                return;
            }

            else
                throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(path));
        }
    }

    /* Yes!  We've seen a file with the same contents.  Replace the
       current file with a hard link to that file. */
    if (st.st_ino == stLink->st_ino) {
        debug("%1% is already linked to %2%", PathFmt(path), PathFmt(linkPath));
        return;
    }

    printMsg(lvlTalkative, "linking %1% to %2%", PathFmt(path), PathFmt(linkPath));

    /* Make the containing directory writable, but only if it's not
       the store itself (we don't want or need to mess with its
       permissions). Inside a directory recursion the parent toggles
       once for all its files and restores after; only a top-level
       call toggles (and restores) here. */
    MakeReadOnly makeReadOnly{std::filesystem::path{}};
    if (parentToggled) {
        if (!*parentToggled) {
            makeWritable(path.parent_path());
            *parentToggled = true;
        }
    } else {
        const auto dirOfPath = path.parent_path();
        if (dirOfPath != config->realStoreDir.get()) {
            makeWritable(dirOfPath);
            /* When we're done, make the directory read-only again and
               reset its timestamp back to 0. */
            makeReadOnly.path = dirOfPath;
        }
    }

    /* makeTempPath would re-canonicalise the (constant) store dir with
       symlink resolution and run boost::format, once per linked file;
       build the name directly instead. */
    static std::atomic<uint32_t> tmpCounter(std::random_device{}());
    std::filesystem::path tempLink{
        config->realStoreDir.get().path().native() + "/.tmp-link-" + std::to_string(getpid()) + "-"
        + std::to_string(tmpCounter.fetch_add(1, std::memory_order_relaxed))};

    try {
        std::filesystem::create_hard_link(linkPath, tempLink);
        /* Note: do NOT insert st.st_ino here; that inode is being
           replaced. Marking it "linked" makes other files still on it
           skip the farm forever (dedup never converges). */
    } catch (std::filesystem::filesystem_error & e) {
        if (e.code() == std::errc::too_many_links) {
            /* Too many links to the same file (>= 32000 on most file
               systems).  This is likely to happen with empty files.
               Just shrug and ignore. */
            if (st.st_size)
                printInfo("%1% has maximum number of links", PathFmt(linkPath));
            return;
        }
        if (e.code() == std::errc::no_such_file_or_directory) {
            /* A concurrent garbage collection removed the link in the
               links directory. Skip optimising this path; a later pass
               will dedup it. */
            return;
        }
        throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(tempLink));
    }

    /* Atomically replace the old file with the new hard link. */
    try {
        std::filesystem::rename(tempLink, path);
    } catch (std::filesystem::filesystem_error & e) {
        {
            std::error_code ec;
            remove(tempLink, ec); /* Clean up after ourselves. */
            if (ec)
                printError("unable to unlink %1%: %2%", PathFmt(tempLink), ec.message());
        }
        if (e.code() == std::errc::too_many_links) {
            /* Some filesystems generate too many links on the rename,
               rather than on the original link.  (Probably it
               temporarily increases the st_nlink field before
               decreasing it again.) */
            debug("%s has reached maximum number of links", PathFmt(linkPath));
            return;
        }
        throw SystemError(e.code(), "renaming %1% to %2%", PathFmt(tempLink), PathFmt(path));
    }

    stats.filesLinked++;
    stats.bytesFreed += st.st_size;

    if (act)
        act->result(resFileLinked, st.st_size, st.st_blocks);
}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    std::lock_guard<std::mutex> runLock(optimiseStoreLock);

    Activity act(*logger, actOptimiseStore);

    auto paths = queryAllValidPaths();
    InodeHash inodeHash = loadInodeHash();

    act.progress(0, paths.size());

    /* Store paths are disjoint subtrees, so they can be deduplicated
       independently; the link farm races (create/unlink) are already
       handled for concurrent processes, which covers threads too. */
    std::atomic<uint64_t> done{0};
    std::mutex statsMutex;
    ThreadPool pool;

    for (auto & i : paths) {
        addTempRoot(i);
        if (!isValidPath(i))
            continue; /* path was GC'ed, probably */
        pool.enqueue([&, i] {
            OptimiseStats pathStats;
            {
                Activity act(*logger, lvlTalkative, actUnknown, fmt("optimising path '%s'", printStorePath(i)));
                optimisePath_(&act, pathStats, config->realStoreDir.get() / i.to_string(), inodeHash, NoRepair);
            }
            {
                std::lock_guard<std::mutex> lock(statsMutex);
                stats.filesLinked += pathStats.filesLinked;
                stats.bytesFreed += pathStats.bytesFreed;
            }
            act.progress(++done, paths.size());
        });
    }

    pool.process();
}

void LocalStore::optimiseStore()
{
    OptimiseStats stats;

    optimiseStore(stats);

    printInfo("%s freed by hard-linking %d files", renderSize(stats.bytesFreed), stats.filesLinked);
}

void LocalStore::optimisePath(const std::filesystem::path & path, RepairFlag repair)
{
    OptimiseStats stats;
    InodeHash inodeHash;

    if (config->getLocalSettings().autoOptimiseStore)
        optimisePath_(nullptr, stats, path, inodeHash, repair);
}

void LocalStore::optimisePath(const StorePath & path, OptimiseStats & stats, const ImportFileHashes * fileHashes)
{
    std::lock_guard<std::mutex> runLock(optimiseStoreLock);

    addTempRoot(path);
    if (!isValidPath(path))
        return; /* path was GC'ed, probably */

    InodeHash inodeHash = loadInodeHash();
    std::filesystem::path realPath = config->realStoreDir.get() / path.to_string();
    optimisePath_(nullptr, stats, realPath, inodeHash, NoRepair, nullptr, fileHashes, realPath.native().size());
}

} // namespace nix
