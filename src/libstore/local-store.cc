#include "nix/store/local-store.hh"
#include "nix/store/globals.hh"
#include "nix/util/archive.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/references.hh"
#include "nix/util/callback.hh"
#include "nix/util/topo-sort.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/users.hh"
#include "nix/store/store-registration.hh"

#include <algorithm>
#include <cstring>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#  include <grp.h>

#  include "nix/util/linux-namespaces.hh"


#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "nix/util/strings.hh"

#include "store-config-private.hh"

namespace nix {

void LocalStoreConfig::anchor() {}

void LocalBuildStoreConfig::anchor() {}

void LocalStore::anchor() {}

void GcStore::anchor() {}

LocalStoreConfig::LocalStoreConfig(const std::filesystem::path & path, const Params & params)
    : StoreConfig(params, FilePathType::Native)
    , LocalFSStoreConfig(path, params)
{
}

std::string LocalStoreConfig::doc()
{
    return
#include "local-store.md"
        ;
}

const LocalSettings & LocalBuildStoreConfig::getLocalSettings() const

{
    return settings.getLocalSettings();
}

std::filesystem::path LocalBuildStoreConfig::getBuildDir() const
{
    auto & bd = getLocalSettings().buildDir.get();
    return bd.has_value()               ? *bd
           : buildDir.get().has_value() ? *buildDir.get()
                                        : AbsolutePath{stateDir.get() / "builds"};
}

ref<Store> LocalStore::Config::openStore() const
{
    return make_ref<LocalStore>(ref{shared_from_this()});
}

struct LocalStore::State::Stmts
{
    /* Some precompiled SQLite statements. */
    SQLiteStmt RegisterValidPath;
    SQLiteStmt UpdatePathInfo;
    SQLiteStmt AddReference;
    SQLiteStmt QueryPathInfo;
    SQLiteStmt QueryReferences;
    SQLiteStmt QueryReferrers;
    SQLiteStmt InvalidatePath;
    SQLiteStmt AddDerivationOutput;
    SQLiteStmt RegisterRealisedOutput;
    SQLiteStmt UpdateRealisedOutput;
    SQLiteStmt QueryValidDerivers;
    SQLiteStmt QueryDerivationOutputs;
    SQLiteStmt QueryRealisedOutput;
    SQLiteStmt QueryPathFromHashPart;
    SQLiteStmt QueryValidPaths;
};

LocalStore::LocalStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , config{config}
    , _state(make_ref<Sync<State>>())
    , dbDir(config->stateDir.get() / "db")
    , linksDir(config->realStoreDir.get() / ".links")
    , reservedPath(dbDir / "reserved")
    , schemaPath(dbDir / "schema")
    , tempRootsDir(config->stateDir.get() / "temproots")
    , fnTempRoots(tempRootsDir / std::to_string(getpid()))
{
    auto state(_state->lock());
    state->stmts = std::make_unique<State::Stmts>();

    /* Create missing state directories if they don't already exist. */
    createDirs(config->realStoreDir.get());
    if (config->readOnly) {
        experimentalFeatureSettings.require(Xp::ReadOnlyLocalStore);
    } else {
        makeStoreWritable();
    }
    createDirs(linksDir);
    auto profilesDir = config->stateDir.get() / "profiles";
    createDirs(profilesDir);
    createDirs(tempRootsDir);
    createDirs(dbDir);
    auto gcRootsDir = config->stateDir.get() / "gcroots";
    const auto & localSettings = config->getLocalSettings();
    const auto & gcSettings = localSettings.getGCSettings();
    createDirs(gcRootsDir);

    for (auto & perUserDir : {profilesDir / "per-user", gcRootsDir / "per-user"}) {
        createDirs(perUserDir);
        if (!config->readOnly) {
            // Skip chmod call if the directory already has the correct permissions (0755).
            // This is to avoid failing when the executing user lacks permissions to change the directory's permissions
            // even if it would be no-op.
            chmodIfNeeded(perUserDir, 0755, S_IRWXU | S_IRWXG | S_IRWXO);
        }
    }

    /* Optionally, create directories and set permissions for a
       multi-user install. */
    if (isRootUser() && localSettings.buildUsersGroup != "") {
        mode_t perm = 01775;

        struct group * gr = getgrnam(localSettings.buildUsersGroup.get().c_str());
        if (!gr)
            printError(
                "warning: the group '%1%' specified in 'build-users-group' does not exist",
                localSettings.buildUsersGroup);
        else if (!config->readOnly) {
            auto st = stat(config->realStoreDir.get());

            if (st.st_uid != 0 || st.st_gid != gr->gr_gid || (st.st_mode & ~S_IFMT) != perm) {
                chown(config->realStoreDir.get(), 0, gr->gr_gid);
                chmod(config->realStoreDir.get(), perm);
            }
        }
    }

    /* Ensure that the store and its parents are not symlinks. */
    if (!localSettings.allowSymlinkedStore) {
        std::filesystem::path path = config->realStoreDir.get();
        std::filesystem::path root = path.root_path();
        while (path != root) {
            if (std::filesystem::is_symlink(path))
                throw Error(
                    "the path %1% is a symlink; "
                    "this is not allowed for the Nix store and its parent directories",
                    PathFmt(path));
            path = path.parent_path();
        }
    }

    /* We can't open a SQLite database if the disk is full.  Since
       this prevents the garbage collector from running when it's most
       needed, we reserve some dummy space that we can free just
       before doing a garbage collection. */
    try {
        auto st = maybeStat(reservedPath);
        if (!st || st->st_size != gcSettings.reservedSize) {
            AutoCloseFD fd = toDescriptor(open(
                reservedPath.string().c_str(),
                O_WRONLY | O_CREAT
                    | O_CLOEXEC
                ,
                0600));
            int res = -1;
#if HAVE_POSIX_FALLOCATE
            res = posix_fallocate(fd.get(), 0, gcSettings.reservedSize);
#endif
            if (res != 0) {
                writeFull(fd.get(), std::string(gcSettings.reservedSize, 'X'));
                [[gnu::unused]] auto res2 =

                    ftruncate(fd.get(), gcSettings.reservedSize)
                    ;
            }
        }
    } catch (SystemError & e) { /* don't care about errors */
    }

    /* Acquire the big fat lock in shared mode to make sure that no
       schema upgrade is in progress. */
    if (!config->readOnly) {
        auto globalLockPath = dbDir / "big-lock";
        try {
            globalLock = openLockFile(globalLockPath, true);
        } catch (SystemError & e) {
            if (e.is(std::errc::permission_denied) || e.is(std::errc::operation_not_permitted)) {
                e.addTrace(
                    "This command may have been run as non-root in a single-user Nix installation,\n"
                    "or the Nix daemon may have crashed.");
            }
            throw;
        }
    }

    if (!config->readOnly && !lockFile(globalLock.get(), ltRead, false)) {
        printInfo("waiting for the big Nix store lock...");
        lockFile(globalLock.get(), ltRead, true);
    }

    /* Check the current database schema and if necessary do an
       upgrade.  */
    int curSchema = getSchema();
    if (config->readOnly && curSchema < nixSchemaVersion) {
        debug("current schema version: %d", curSchema);
        debug("supported schema version: %d", nixSchemaVersion);
        throw Error(
            curSchema == 0 ? "database does not exist, and cannot be created in read-only mode"
                           : "database schema needs migrating, but this cannot be done in read-only mode");
    }

    auto acquireWriteLock = [&]() {
        if (!lockFile(globalLock.get(), ltWrite, false)) {
            printInfo("waiting for exclusive access to the Nix store...");
            // We have acquired a shared lock; release it to prevent deadlocks.
            // This can happen if someone else is trying to promote their read
            // lock into a write lock.
            lockFile(globalLock.get(), ltNone, false);
            lockFile(globalLock.get(), ltWrite, true);
        }
    };

    if (curSchema > nixSchemaVersion)
        throw Error("current Nix store schema is version %1%, but I only support %2%", curSchema, nixSchemaVersion);

    else if (curSchema == 0) { /* new store */
        curSchema = nixSchemaVersion;
        openDB(*state, true);
        writeFile(schemaPath, fmt("%1%", curSchema), 0666, FsSync::Yes);
    }

    else if (curSchema < nixSchemaVersion) {
        if (curSchema < 5)
            throw Error(
                "Your Nix store has a database in Berkeley DB format,\n"
                "which is no longer supported. To convert to the new format,\n"
                "please upgrade Nix to version 0.12 first.");

        if (curSchema < 6)
            throw Error(
                "Your Nix store has a database in flat file format,\n"
                "which is no longer supported. To convert to the new format,\n"
                "please upgrade Nix to version 1.11 first.");

        acquireWriteLock();

        /* Get the schema version again, because another process may
           have performed the upgrade already. */
        curSchema = getSchema();

        openDB(*state, false);

        /* Legacy database schema migrations. Don't bump 'schema' for
           new migrations; instead, add a migration to
           upgradeDBSchema(). */

        if (curSchema < 8) {
            SQLiteTxn txn(state->db);
            state->db.exec("alter table ValidPaths add column ultimate integer");
            state->db.exec("alter table ValidPaths add column sigs text");
            txn.commit();
        }

        if (curSchema < 9) {
            SQLiteTxn txn(state->db);
            state->db.exec("drop table FailedPaths");
            txn.commit();
        }

        if (curSchema < 10) {
            SQLiteTxn txn(state->db);
            state->db.exec("alter table ValidPaths add column ca text");
            txn.commit();
        }

        writeFile(schemaPath, fmt("%1%", nixSchemaVersion), 0666, FsSync::Yes);

        // Downgrade to a read lock and hold to prevent other processes from
        // upgrading the schema while we're using the store
        lockFile(globalLock.get(), ltRead, true);
    }

    else
        openDB(*state, false);

    if (!config->readOnly && upgradeDBSchema(*state, true)) {
        acquireWriteLock();
        upgradeDBSchema(*state, false);
        // Downgrade to a read lock and hold to prevent other processes from
        // upgrading the schema while we're using the store
        lockFile(globalLock.get(), ltRead, true);
    }

    /* Prepare SQL statements. */
    state->stmts->RegisterValidPath.create(
        state->db,
        "insert into ValidPaths (path, hash, registrationTime, deriver, narSize, ultimate, sigs, ca) values (?, ?, ?, ?, ?, ?, ?, ?);");
    state->stmts->UpdatePathInfo.create(
        state->db, "update ValidPaths set narSize = ?, hash = ?, ultimate = ?, sigs = ?, ca = ? where path = ?;");
    state->stmts->AddReference.create(state->db, "insert or replace into Refs (referrer, reference) values (?, ?);");
    state->stmts->QueryPathInfo.create(
        state->db,
        "select id, hash, registrationTime, deriver, narSize, ultimate, sigs, ca from ValidPaths where path = ?;");
    state->stmts->QueryReferences.create(
        state->db, "select path from Refs join ValidPaths on reference = id where referrer = ?;");
    state->stmts->QueryReferrers.create(
        state->db,
        "select path from Refs join ValidPaths on referrer = id where reference = (select id from ValidPaths where path = ?);");
    state->stmts->InvalidatePath.create(state->db, "delete from ValidPaths where path = ?;");
    state->stmts->AddDerivationOutput.create(
        state->db, "insert or replace into DerivationOutputs (drv, id, path) values (?, ?, ?);");
    state->stmts->QueryValidDerivers.create(
        state->db, "select v.id, v.path from DerivationOutputs d join ValidPaths v on d.drv = v.id where d.path = ?;");
    state->stmts->QueryDerivationOutputs.create(state->db, "select id, path from DerivationOutputs where drv = ?;");
    // Use "path >= ?" with limit 1 rather than "path like '?%'" to
    // ensure efficient lookup.
    state->stmts->QueryPathFromHashPart.create(state->db, "select path from ValidPaths where path >= ? limit 1;");
    state->stmts->QueryValidPaths.create(state->db, "select path from ValidPaths");
    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
        state->stmts->RegisterRealisedOutput.create(
            state->db,
            R"(
                insert into BuildTraceV3 (drvPath, outputName, outputPath, signatures)
                values (?, ?, ?, ?)
                ;
            )");
        state->stmts->UpdateRealisedOutput.create(
            state->db,
            R"(
                update BuildTraceV3
                    set signatures = ?
                where
                    drvPath = ? and
                    outputName = ?
                ;
            )");
        state->stmts->QueryRealisedOutput.create(
            state->db,
            R"(
                select id, outputPath, signatures from BuildTraceV3
                    where drvPath = ? and outputName = ?
                    ;
            )");
    }
}

AutoCloseFD LocalStore::openGCLock()
{
    auto fnGCLock = config->stateDir.get() / "gc.lock";
    return openLockFile(fnGCLock, /*create=*/true);
}

void LocalStore::deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed, bool isKnownPath)
{
    try {
        deletePath(path, bytesFreed);
    } catch (SystemError & e) {
        if (config->ignoreGcDeleteFailure) {
            logWarning(
                {.msg = HintFmt(
                     isKnownPath ? "ignoring failure to remove store path %1%: %2%"
                                 : "ignoring failure to remove garbage in store directory %1%: %2%",
                     PathFmt(path),
                     e.info().msg)});
        } else {
            e.addTrace(
                isKnownPath ? "While deleting store path %1%" : "While deleting garbage in store directory %1%",
                PathFmt(path));
            throw;
        }
    }
}

LocalStore::~LocalStore()
{
    std::shared_future<void> future;

    {
        auto state(_state->lock());
        if (state->gcRunning)
            future = state->gcFuture;
    }

    if (future.valid()) {
        printInfo("waiting for auto-GC to finish on exit...");
        future.get();
    }

    {
        auto state(_state->lock());
        if (state->gcThread.joinable())
            state->gcThread.join();
    }

    try {
        auto fdTempRoots(_fdTempRoots.lock());
        if (*fdTempRoots) {
            fdTempRoots->close();
            tryUnlink(fnTempRoots);
        }
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

std::filesystem::path LocalStoreConfig::getRootsSocketPath() const
{
    return std::filesystem::path(stateDir.get()) / "gc-roots-socket" / "socket";
}

StoreReference LocalStoreConfig::getReference() const
{
    auto params = getQueryParams();
    /* Back-compatibility kludge. Tools like nix-output-monitor expect 'local'
       and can't parse 'local://'. */
    if (params.empty())
        /* TODO: Add the rootDir here as the authority? */
        return {.variant = StoreReference::Local{}};
    return {
        .variant =
            StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
                /* TODO: Add the rootDir here as the authority? */
            },
        .params = std::move(params),
    };
}

bool LocalStoreConfig::getReadOnly() const
{
    return readOnly.get() || StoreConfig::getReadOnly();
}

int LocalStore::getSchema()
{
    int curSchema = 0;
    if (pathExists(schemaPath)) {
        auto s = readFile(schemaPath);
        auto n = string2Int<int>(s);
        if (!n)
            throw Error("%1% is corrupt", PathFmt(schemaPath));
        curSchema = *n;
    }
    return curSchema;
}

void LocalStore::openDB(State & state, bool create)
{
    if (create && config->readOnly) {
        throw Error("cannot create database while in read-only mode");
    }

    if (access(dbDir.string().c_str(), R_OK | (config->readOnly ? 0 : W_OK)))
        throw SysError("Nix database directory %1% is not writable", PathFmt(dbDir));

    /* Open the Nix database. */
    auto & db(state.db);
    auto openMode = config->readOnly ? SQLiteOpenMode::Immutable
                    : create         ? SQLiteOpenMode::Normal
                                     : SQLiteOpenMode::NoCreate;
    state.db = SQLite(dbDir / "db.sqlite", {.mode = openMode, .useWAL = settings.useSQLiteWAL});


    /* !!! check whether sqlite has been built with foreign key
       support */

    /* Whether SQLite should fsync().  "Normal" synchronous mode
       should be safe enough.  If the user asks for it, don't sync at
       all.  This can cause database corruption if the system
       crashes. */
    std::string syncMode = config->getLocalSettings().fsyncMetadata ? "normal" : "off";
    db.exec("pragma synchronous = " + syncMode);

    /* Set the SQLite journal mode.  WAL mode is fastest, so it's the
       default. */
    std::string mode = settings.useSQLiteWAL ? "wal" : "truncate";
    std::string prevMode;
    {
        SQLiteStmt stmt;
        stmt.create(db, "pragma main.journal_mode;");
        if (sqlite3_step(stmt) != SQLITE_ROW)
            SQLiteError::throw_(db, "querying journal mode");
        prevMode = std::string((const char *) sqlite3_column_text(stmt, 0));
    }
    if (prevMode != mode
        && sqlite3_exec(db, ("pragma main.journal_mode = " + mode + ";").c_str(), 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting journal mode");

    if (mode == "wal") {
        /* persist the WAL files when the db connection is closed. This allows
           for read-only connections without write permissions on the
           containing directory to succeed on a closed db. Setting the
           journal_size_limit to 2^40 bytes results in the WAL files getting
           truncated to 0 on exit and limits the on disk size of the WAL files
           to 2^40 bytes following a checkpoint */
        if (sqlite3_exec(db, "pragma main.journal_size_limit = 1099511627776;", 0, 0, 0) == SQLITE_OK) {
            int enable = 1;
            sqlite3_file_control(db, NULL, SQLITE_FCNTL_PERSIST_WAL, &enable);
        }
    }

    /* Increase the auto-checkpoint interval to 40000 pages.  This
       seems enough to ensure that instantiating the NixOS system
       derivation is done in a single fsync(). */
    if (mode == "wal" && sqlite3_exec(db, "pragma wal_autocheckpoint = 40000;", 0, 0, 0) != SQLITE_OK)
        SQLiteError::throw_(db, "setting autocheckpoint interval");

    /* Initialise the database schema, if necessary. */
    if (create) {
        static const char schema[] =
#include "schema.sql.gen.hh"
            ;
        db.exec(schema);
    }
}

bool LocalStore::upgradeDBSchema(State & state, bool dryRun)
{
    bool ret = false;

    {
        SQLiteStmt queryHasSchemaMigrations;
        queryHasSchemaMigrations.create(
            state.db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='SchemaMigrations';");
        auto useQueryHasSchemaMigrations(queryHasSchemaMigrations.use());
        if (!useQueryHasSchemaMigrations.next()) {
            if (dryRun)
                return true;
            else {
                state.db.exec("create table SchemaMigrations (migration text primary key not null);");
                ret = true;
            }
        }
    }

    StringSet schemaMigrations;

    {
        SQLiteStmt querySchemaMigrations;
        querySchemaMigrations.create(state.db, "select migration from SchemaMigrations;");
        auto useQuerySchemaMigrations(querySchemaMigrations.use());
        while (useQuerySchemaMigrations.next())
            schemaMigrations.insert(useQuerySchemaMigrations.getStr(0));
    }

    auto needsMigration = [&](const std::string & migrationName) -> bool {
        return !schemaMigrations.contains(migrationName);
    };

    auto maybeUpgrade = [&](const std::string & migrationName, const std::string & stmt) {
        if (!needsMigration(migrationName))
            return;

        ret = true;
        if (dryRun)
            return;

        debug("executing Nix database schema migration '%s'...", migrationName);

        SQLiteTxn txn(state.db);
        state.db.exec(stmt + fmt(";\ninsert or ignore into SchemaMigrations values('%s')", migrationName));
        txn.commit();

        schemaMigrations.insert(migrationName);
    };

    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        maybeUpgrade(
            "20251017-ca-derivations",
#include "ca-specific-schema.sql.gen.hh"
        );

    maybeUpgrade("20260309-drop-redundant-indexreferrer", "drop index if exists IndexReferrer");

    return ret;
}

/* To improve purity, users may want to make the Nix store a read-only
   bind mount.  So make the Nix store writable for this process. */
void LocalStore::makeStoreWritable()
{
    if (!isRootUser())
        return;
    remountReadOnlyWritable(config->realStoreDir.get());
}




uint64_t LocalStore::addValidPath(State & state, const ValidPathInfo & info)
{
    if (info.ca.has_value() && !info.isContentAddressed(*this))
        throw Error(
            "cannot add path '%s' to the Nix store because it claims to be content-addressed but isn't",
            printStorePath(info.path));

    state.stmts->RegisterValidPath.use()
        .apply(printStorePath(info.path))
        .apply(info.narHash.to_string(HashFormat::Base16, true))
        .apply(info.registrationTime == 0 ? time(nullptr) : info.registrationTime)
        .apply(info.deriver ? printStorePath(*info.deriver) : "", (bool) info.deriver)
        .apply(info.narSize, info.narSize != 0)
        .apply(info.ultimate ? 1 : 0, info.ultimate)
        .apply(concatStringsSep(" ", Signature::toStrings(info.sigs)), !info.sigs.empty())
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .exec();
    uint64_t id = state.db.getLastInsertedRowId();

    pathInfoCache->lock()->upsert(info.path, PathInfoCacheValue{.value = std::make_shared<const ValidPathInfo>(info)});

    return id;
}

void LocalStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    try {
        callback(retrySQLite<std::shared_ptr<const ValidPathInfo>>([&]() {
            return queryPathInfoInternal(*_state->lock(), path);
        }));

    } catch (...) {
        callback.rethrow();
    }
}

std::shared_ptr<const ValidPathInfo> LocalStore::queryPathInfoInternal(State & state, const StorePath & path)
{
    /* Get the path info. */
    auto useQueryPathInfo(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));

    if (!useQueryPathInfo.next())
        return std::shared_ptr<ValidPathInfo>();

    auto id = useQueryPathInfo.getInt(0);

    auto narHash = Hash::dummy;
    try {
        narHash = Hash::parseAnyPrefixed(useQueryPathInfo.getStr(1));
    } catch (BadHash & e) {
        throw Error("invalid-path entry for '%s': %s", printStorePath(path), e.what());
    }

    auto info = std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo(*this, narHash));

    info->registrationTime = useQueryPathInfo.getInt(2);

    auto s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 3);
    if (s)
        info->deriver = parseStorePath(s);

    /* Note that narSize = NULL yields 0. */
    info->narSize = useQueryPathInfo.getInt(4);

    info->ultimate = useQueryPathInfo.getInt(5) == 1;

    s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 6);
    if (s)
        info->sigs = Signature::parseMany(tokenizeString<StringSet>(s, " "));

    s = (const char *) sqlite3_column_text(state.stmts->QueryPathInfo, 7);
    if (s)
        info->ca = ContentAddress::parseOpt(s);

    /* Get the references. */
    auto useQueryReferences(state.stmts->QueryReferences.use().apply(id));

    while (useQueryReferences.next())
        info->references.insert(parseStorePath(useQueryReferences.getStr(0)));

    return info;
}

/* Update path info in the database. */
void LocalStore::updatePathInfo(State & state, const ValidPathInfo & info)
{
    state.stmts->UpdatePathInfo.use()
        .apply(info.narSize, info.narSize != 0)
        .apply(info.narHash.to_string(HashFormat::Base16, true))
        .apply(info.ultimate ? 1 : 0, info.ultimate)
        .apply(concatStringsSep(" ", Signature::toStrings(info.sigs)), !info.sigs.empty())
        .apply(renderContentAddress(info.ca), (bool) info.ca)
        .apply(printStorePath(info.path))
        .exec();
}

uint64_t LocalStore::queryValidPathId(State & state, const StorePath & path)
{
    auto use(state.stmts->QueryPathInfo.use().apply(printStorePath(path)));
    if (!use.next())
        throw InvalidPath("path '%s' is not valid", printStorePath(path));
    return use.getInt(0);
}

bool LocalStore::isValidPath_(State & state, const StorePath & path)
{
    return state.stmts->QueryPathInfo.use().apply(printStorePath(path)).next();
}

bool LocalStore::isValidPathUncached(const StorePath & path)
{
    return retrySQLite<bool>([&]() { return isValidPath_(*_state->lock(), path); });
}

StorePathSet LocalStore::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    StorePathSet res;
    for (auto & i : paths)
        if (isValidPath(i))
            res.insert(i);
    return res;
}

StorePathSet LocalStore::queryAllValidPaths()
{
    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());
        auto use(state->stmts->QueryValidPaths.use());
        StorePathSet res;
        while (use.next())
            res.insert(parseStorePath(use.getStr(0)));
        return res;
    });
}

void LocalStore::queryReferrers(State & state, const StorePath & path, StorePathSet & referrers)
{
    auto useQueryReferrers(state.stmts->QueryReferrers.use().apply(printStorePath(path)));

    while (useQueryReferrers.next())
        referrers.insert(parseStorePath(useQueryReferrers.getStr(0)));
}

void LocalStore::queryReferrers(const StorePath & path, StorePathSet & referrers)
{
    return retrySQLite<void>([&]() { queryReferrers(*_state->lock(), path, referrers); });
}

StorePathSet LocalStore::queryValidDerivers(const StorePath & path)
{
    return retrySQLite<StorePathSet>([&]() {
        auto state(_state->lock());

        auto useQueryValidDerivers(state->stmts->QueryValidDerivers.use().apply(printStorePath(path)));

        StorePathSet derivers;
        while (useQueryValidDerivers.next())
            derivers.insert(parseStorePath(useQueryValidDerivers.getStr(1)));

        return derivers;
    });
}

std::optional<StorePath> LocalStore::queryPathFromHashPart(const std::string & hashPart)
{
    if (hashPart.size() != StorePath::HashLen)
        throw Error("invalid hash part");

    std::string prefix = storeDir + "/" + hashPart;

    return retrySQLite<std::optional<StorePath>>([&]() -> std::optional<StorePath> {
        auto state(_state->lock());

        auto useQueryPathFromHashPart(state->stmts->QueryPathFromHashPart.use().apply(prefix));

        if (!useQueryPathFromHashPart.next())
            return {};

        const char * s = (const char *) sqlite3_column_text(state->stmts->QueryPathFromHashPart, 0);
        if (s && prefix.compare(0, prefix.size(), s, prefix.size()) == 0)
            return parseStorePath(s);
        return {};
    });
}

void LocalStore::registerValidPath(const ValidPathInfo & info)
{
    registerValidPaths({{info.path, info}});
}

void LocalStore::registerValidPaths(const ValidPathInfos & infos)
{
    /* SQLite will fsync by default, but the new valid paths may not
       be fsync-ed.  So some may want to fsync them before registering
       the validity, at the expense of some speed of the path
       registering operation. */
    if (config->getLocalSettings().syncBeforeRegistering)
        sync();

    return retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);
        StorePathSet paths;

        for (auto & [_, i] : infos) {
            assert(i.narHash.algo == HashAlgorithm::SHA256);
            if (isValidPath_(*state, i.path))
                updatePathInfo(*state, i);
            else
                addValidPath(*state, i);
            paths.insert(i.path);
        }

        for (auto & [_, i] : infos) {
            auto referrer = queryValidPathId(*state, i.path);
            for (auto & j : i.references)
                state->stmts->AddReference.use().apply(referrer).apply(queryValidPathId(*state, j)).exec();
        }

        /* Do a topological sort of the paths.  This will throw an
           error if a cycle is detected and roll back the
           transaction.  Cycles can only occur when a derivation
           has multiple outputs. */
        auto topoSortResult = topoSort(paths, [&](const StorePath & path) {
            auto i = infos.find(path);
            return i == infos.end() ? StorePathSet() : i->second.references;
        });

        std::visit(
            overloaded{
                [&](const Cycle<StorePath> & cycle) {
                    throw Error(
                        "cycle detected in the references of '%s' from '%s'",
                        printStorePath(cycle.path),
                        printStorePath(cycle.parent));
                },
                [](auto &) { /* Success, continue */ }},
            topoSortResult);

        txn.commit();
    });
}

/* Invalidate a path.  The caller is responsible for checking that
   there are no referrers. */
void LocalStore::invalidatePath(State & state, const StorePath & path)
{
    debug("invalidating path '%s'", printStorePath(path));

    state.stmts->InvalidatePath.use().apply(printStorePath(path)).exec();

    /* Note that the foreign key constraints on the Refs table take
       care of deleting the references entries for `path'. */

    invalidatePathInfoCacheFor(path);
}

void LocalStore::addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs)
{
    {
        addTempRoot(info.path);

        if (repair || !isValidPath(info.path)) {

            PathLocks outputLock;

            auto realPath = toRealPath(info.path);

            /* Lock the output path.  But don't lock if we're being called
               from a build hook (whose parent process already acquired a
               lock on this path). */
            if (!locksHeld.count(printStorePath(info.path)))
                outputLock.lockPaths({realPath});

            /* The path may have been created by another process in the meantime, so check again. */
            if (repair || !isValidPathUncached(info.path)) {

                deletePath(realPath);

                /* While restoring the path from the NAR, compute the hash
                   of the NAR. */
                HashSink hashSink(HashAlgorithm::SHA256);

                TeeSource wrapperSource{source, hashSink};

                restorePath(realPath, wrapperSource, config->getLocalSettings().fsyncStorePaths);

                auto hashResult = hashSink.finish();

                if (hashResult.hash != info.narHash)
                    throw Error(
                        "hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.narHash.to_string(HashFormat::SRI, true),
                        hashResult.hash.to_string(HashFormat::SRI, true));

                if (hashResult.numBytesDigested != info.narSize)
                    throw Error(
                        "size mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                        printStorePath(info.path),
                        info.narSize,
                        hashResult.numBytesDigested);

                if (info.ca) {
                    auto & specified = *info.ca;
                    auto actualHash = ({
                        SourcePath sourcePath = requireStoreObjectAccessor(info.path, /*requireValidPath=*/false);
                        Hash h{HashAlgorithm::SHA256}; // throwaway def to appease C++
                        auto fim = specified.method.getFileIngestionMethod();
                        switch (fim) {
                        case FileIngestionMethod::Flat:
                        case FileIngestionMethod::NixArchive: {
                            HashModuloSink caSink{
                                specified.hash.algo,
                                std::string{info.path.hashPart()},
                            };
                            dumpPath(sourcePath, caSink, (FileSerialisationMethod) fim);
                            h = caSink.finish().hash;
                            break;
                        }
                        }
                        ContentAddress{
                            .method = specified.method,
                            .hash = std::move(h),
                        };
                    });
                    if (specified.hash != actualHash.hash) {
                        throw Error(
                            "ca hash mismatch importing path '%s';\n  specified: %s\n  got:       %s",
                            printStorePath(info.path),
                            specified.hash.to_string(HashFormat::Nix32, true),
                            actualHash.hash.to_string(HashFormat::Nix32, true));
                    }
                }

                autoGC();

                canonicalisePathMetaData(realPath, {NIX_WHEN_SUPPORT_ACLS(config->getLocalSettings().ignoredAcls)});

                optimisePath(realPath, repair); // FIXME: combine with hashPath()

                if (config->getLocalSettings().fsyncStorePaths) {
                    recursiveSync(realPath);
                    syncParent(realPath);
                }

                registerValidPath(info);
            } else
                // We may have a negative cache entry for this path, so get rid of it.
                invalidatePathInfoCacheFor(info.path);

            outputLock.setDeletion(true);
        }
    }
}

/* True when farming hash work out to a thread can actually overlap
   with the producer; on a single hardware thread the queueing is pure
   overhead (chunk copies + context-switch churn). */
static bool hashingThreadPaysOff()
{
    return std::thread::hardware_concurrency() > 1;
}

/* Runs a HashSink on its own thread. An import hashes the stream
   twice (whole-NAR digest for the store path, per-file digests for
   dedup); this takes one of them off the restore's critical path.
   Bounded queue so a fast producer cannot balloon memory. Hashes
   inline on single-core machines. */
class AsyncHashSink : public Sink
{
    HashSink inner;
    bool threaded;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cvPush, cvPop;
    std::deque<std::string> chunks;
    bool closed = false;
    std::exception_ptr failure;
    static constexpr size_t maxQueued = 64;

public:
    AsyncHashSink(HashAlgorithm algo)
        : inner(algo)
        , threaded(hashingThreadPaysOff())
    {
        if (!threaded)
            return;
        worker = std::thread([this] {
            std::unique_lock lk(mtx);
            while (true) {
                cvPush.wait(lk, [&] { return closed || !chunks.empty(); });
                if (chunks.empty())
                    return;
                auto chunk = std::move(chunks.front());
                chunks.pop_front();
                lk.unlock();
                cvPop.notify_one();
                /* on failure keep draining so the producer never
                   blocks on a full queue; rethrow at finish() */
                if (!failure) {
                    try {
                        inner(chunk);
                    } catch (...) {
                        failure = std::current_exception();
                    }
                }
                lk.lock();
            }
        });
    }

    void operator()(std::string_view data) override
    {
        if (!threaded) {
            inner(data);
            return;
        }
        std::unique_lock lk(mtx);
        cvPop.wait(lk, [&] { return chunks.size() < maxQueued; });
        chunks.emplace_back(data);
        lk.unlock();
        cvPush.notify_one();
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed = true;
        }
        cvPush.notify_one();
        if (worker.joinable())
            worker.join();
    }

    HashResult finish()
    {
        close();
        if (failure)
            std::rethrow_exception(failure);
        return inner.finish();
    }

    ~AsyncHashSink()
    {
        try {
            close();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Hashes each regular file's single-file NAR serialisation (same
   framing SourceAccessor::dumpPath emits, so the digest equals what
   hashPath() would recompute from disk) on a worker thread, fed a
   strictly ordered event stream by the restore below; inline on
   single-core machines. Contents are hashed once, at restore time,
   off the critical path, and optimisePath() never has to read them
   back. */
class AsyncFileHasher
{
    struct Ev
    {
        enum
        {
            Begin, /* data = map key */
            Exec,
            Size, /* size = contents length */
            Data, /* data = contents chunk */
            End,
        } tag;

        std::string data;
        uint64_t size = 0;
    };

    LocalStore::ImportFileHashes & out;

    /* when set, end() swaps a just-restored file for a hard link into
       the farm if its content is already there (streaming dedup) */
    const std::filesystem::path * dedupRoot;
    const std::filesystem::path * linksDir;

    bool threaded;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cvPush, cvPop;
    std::deque<Ev> events;
    bool closed = false;
    std::exception_ptr failure;
    static constexpr size_t maxQueued = 64;

    /* per-file state machine; worker-owned when threaded, caller-owned
       otherwise (events per file are strictly ordered either way, so
       the digests are identical) */
    std::string key;
    std::unique_ptr<HashSink> hash;
    uint64_t size = 0;

    void begin(std::string k)
    {
        key = std::move(k);
        hash = std::make_unique<HashSink>(HashAlgorithm::SHA256);
        *hash << narVersionMagic1 << "(" << "type" << "regular";
    }

    void exec()
    {
        *hash << "executable" << "";
    }

    void setSize(uint64_t s)
    {
        size = s;
        *hash << "contents" << s;
    }

    void data(std::string_view d)
    {
        (*hash)(d);
    }

    void end()
    {
        writePadding(size, *hash);
        *hash << ")";
        auto h = hash->finish().hash;
        tryDedup(h);
        out.files.insert_or_assign(std::move(key), h);
        hash.reset();
    }

    /* The file just restored is complete and NAR restores are
       sequential, so nothing touches it again: if the link farm holds
       its content (same NAR hash, so the execute bit matches too),
       swap the fresh copy for a hard link via link+rename and give the
       data pages back. The later canonicalise/optimise passes see a
       farm inode that is already canonical (0444/0555, mtime 1) and
       apply the same values. Opportunistic: any failure leaves the
       copy for optimisePath() to farm; never throws into the import.
       Empty files are skipped, same rule (and reason) as optimise. */
    void tryDedup(const Hash & h)
    {
        if (!dedupRoot || !linksDir || size == 0)
            return;
        try {
            std::filesystem::path link{
                linksDir->native() + '/' + h.to_string(HashFormat::Nix32, false)};
            struct stat stLink;
            if (::lstat(link.c_str(), &stLink) == -1
                || uint64_t(stLink.st_size) != size)
                return;
            std::filesystem::path file{
                dedupRoot->native() + (key == "/" ? std::string() : key)};
            std::filesystem::path tmp{file.native() + ".dedup~"};
            if (::link(link.c_str(), tmp.c_str()) == -1)
                return;
            if (::rename(tmp.c_str(), file.c_str()) == -1) {
                ::unlink(tmp.c_str());
                return;
            }
            out.dedupedFiles++;
            out.dedupedBytes += size;
        } catch (...) {
            /* opportunistic */
        }
    }

    void push(Ev ev)
    {
        std::unique_lock lk(mtx);
        cvPop.wait(lk, [&] { return events.size() < maxQueued; });
        events.push_back(std::move(ev));
        lk.unlock();
        cvPush.notify_one();
    }

    void run()
    {
        std::unique_lock lk(mtx);
        while (true) {
            cvPush.wait(lk, [&] { return closed || !events.empty(); });
            if (events.empty())
                return;
            auto ev = std::move(events.front());
            events.pop_front();
            lk.unlock();
            cvPop.notify_one();
            /* on failure keep draining so the producer never blocks
               on a full queue; rethrow at finish() */
            if (!failure) {
                try {
                    switch (ev.tag) {
                    case Ev::Begin:
                        begin(std::move(ev.data));
                        break;
                    case Ev::Exec:
                        exec();
                        break;
                    case Ev::Size:
                        setSize(ev.size);
                        break;
                    case Ev::Data:
                        data(ev.data);
                        break;
                    case Ev::End:
                        end();
                        break;
                    }
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            lk.lock();
        }
    }

public:
    AsyncFileHasher(
        LocalStore::ImportFileHashes & out,
        const std::filesystem::path * dedupRoot,
        const std::filesystem::path * linksDir)
        : out(out)
        , dedupRoot(dedupRoot)
        , linksDir(linksDir)
        , threaded(hashingThreadPaysOff())
    {
        if (threaded)
            worker = std::thread([this] { run(); });
    }

    void fileBegin(std::string k)
    {
        if (threaded)
            push({Ev::Begin, std::move(k)});
        else
            begin(std::move(k));
    }

    void fileExec()
    {
        if (threaded)
            push({Ev::Exec});
        else
            exec();
    }

    void fileSize(uint64_t s)
    {
        if (threaded)
            push({Ev::Size, {}, s});
        else
            setSize(s);
    }

    void fileData(std::string_view d)
    {
        if (threaded)
            push({Ev::Data, std::string(d)});
        else
            data(d);
    }

    void fileEnd()
    {
        if (threaded)
            push({Ev::End});
        else
            end();
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            closed = true;
        }
        cvPush.notify_one();
        if (worker.joinable())
            worker.join();
    }

    /* after this, `out` is complete and owned by the caller */
    void finish()
    {
        close();
        if (failure)
            std::rethrow_exception(failure);
    }

    ~AsyncFileHasher()
    {
        try {
            close();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* Forwards restore-sink calls to an inner sink, feeding regular-file
   events to the AsyncFileHasher on the side. */
struct FileHashingSink : FileSystemObjectSink
{
    FileSystemObjectSink & inner;
    CanonPath prefix;
    AsyncFileHasher & hasher;

    FileHashingSink(FileSystemObjectSink & inner, CanonPath prefix, AsyncFileHasher & hasher)
        : inner(inner)
        , prefix(std::move(prefix))
        , hasher(hasher)
    {
    }

    void createDirectory(const CanonPath & path) override
    {
        inner.createDirectory(path);
    }

    void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override
    {
        inner.createDirectory(path, [&](FileSystemObjectSink & dirSink, const CanonPath & rel) {
            /* RestoreSink hands back a rerooted sink (rel = root); the
               default impl hands back itself (rel = path). Either way
               dirSink's root sits at prefix/path stripped of rel. */
            assert(rel.isRoot() || rel == path);
            FileHashingSink sub{dirSink, rel.isRoot() ? prefix / path : prefix, hasher};
            callback(sub, rel);
        });
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        inner.createSymlink(path, target);
    }

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
    {
        inner.createRegularFile(path, [&](CreateRegularFileSink & crf) {
            struct HashingCRF : CreateRegularFileSink
            {
                CreateRegularFileSink & inner;
                AsyncFileHasher & hasher;

                HashingCRF(CreateRegularFileSink & inner, AsyncFileHasher & hasher)
                    : inner(inner)
                    , hasher(hasher)
                {
                }

                void isExecutable() override
                {
                    /* the parser reports this before contents,
                       matching dump order */
                    hasher.fileExec();
                    inner.isExecutable();
                }

                void preallocateContents(uint64_t s) override
                {
                    hasher.fileSize(s);
                    inner.preallocateContents(s);
                }

                void operator()(std::string_view data) override
                {
                    hasher.fileData(data);
                    inner(data);
                }
            } hcrf{crf, hasher};
            hasher.fileBegin((prefix / path).abs());
            func(hcrf);
            hasher.fileEnd();
        });
    }
};

/* restorePath(), optionally capturing per-file hashes. Only NAR dumps
   carry the per-file structure; Flat dumps fall back unrecorded. */
static void restorePathCapturingHashes(
    const std::filesystem::path & path,
    Source & source,
    FileSerialisationMethod method,
    bool startFsync,
    LocalStore::ImportFileHashes * fileHashes,
    const std::filesystem::path * linksDir)
{
    if (!fileHashes || method != FileSerialisationMethod::NixArchive) {
        restorePath(path, source, method, startFsync);
        return;
    }
    RestoreSink inner{startFsync};
    inner.dstPath = path;
    AsyncFileHasher hasher{*fileHashes, linksDir ? &path : nullptr, linksDir};
    FileHashingSink sink{inner, CanonPath::root, hasher};
    parseDump(sink, source);
    hasher.finish();
}

StorePath LocalStore::addToStoreFromDump(
    Source & source0,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    RepairFlag repair)
{
    return addToStoreFromDump(source0, name, dumpMethod, hashMethod, hashAlgo, references, repair, nullptr);
}

StorePath LocalStore::addToStoreFromDump(
    Source & source0,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    RepairFlag repair,
    ImportFileHashes * fileHashes)
{
    /* For computing the store path; hashed off-thread so it overlaps
       with the restore below. */
    auto hashSink = std::make_unique<AsyncHashSink>(hashAlgo);
    TeeSource source{source0, *hashSink};
    const LocalSettings & localSettings = config->getLocalSettings();

    /* Read the source path into memory, but only if it's up to
       narBufferSize bytes. If it's larger, write it to a temporary
       location in the Nix store. If the subsequently computed
       destination store path is already valid, we just delete the
       temporary path. Otherwise, we move it to the destination store
       path. */
    bool inMemory = false;

    struct Free
    {
        void operator()(void * v)
        {
            free(v);
        }
    };

    std::unique_ptr<char, Free> dumpBuffer(nullptr);
    std::string_view dump;

    /* Fill out buffer, and decide whether we are working strictly in
       memory based on whether we break out because the buffer is full
       or the original source is empty */
    while (dump.size() < localSettings.narBufferSize) {
        auto oldSize = dump.size();
        constexpr size_t chunkSize = 65536;
        auto want = std::min(chunkSize, localSettings.narBufferSize - oldSize);
        if (auto tmp = realloc(dumpBuffer.get(), oldSize + want)) {
            dumpBuffer.release();
            dumpBuffer.reset((char *) tmp);
        } else {
            throw std::bad_alloc();
        }
        auto got = 0;
        Finally cleanup([&]() { dump = {dumpBuffer.get(), dump.size() + got}; });
        try {
            got = source.read(dumpBuffer.get() + oldSize, want);
        } catch (EndOfFile &) {
            inMemory = true;
            break;
        }
    }

    std::unique_ptr<AutoDelete> delTempDir;
    std::filesystem::path tempPath;
    std::filesystem::path tempDir;
    AutoCloseFD tempDirFd;

    bool methodsMatch = static_cast<FileIngestionMethod>(dumpMethod) == hashMethod.getFileIngestionMethod();

    /* If the methods don't match, our streaming hash of the dump is the
       wrong sort, and we need to rehash. */
    bool inMemoryAndDontNeedRestore = inMemory && methodsMatch;

    if (!inMemoryAndDontNeedRestore) {
        /* Drain what we pulled so far, and then keep on pulling */
        StringSource dumpSource{dump};
        ChainSource bothSource{dumpSource, source};

        std::tie(tempDir, tempDirFd) = createTempDirInStore();
        delTempDir = std::make_unique<AutoDelete>(tempDir);
        tempPath = tempDir / "x";

        restorePathCapturingHashes(
            tempPath, bothSource, dumpMethod, localSettings.fsyncStorePaths, fileHashes, &linksDir);

        dumpBuffer.reset();
        dump = {};
    }

    auto [dumpHash, size] = hashSink->finish();

    auto desc = ContentAddressWithReferences::fromParts(
        hashMethod,
        methodsMatch ? dumpHash
                     : hashPath(makeFSSourceAccessor(tempPath), hashMethod.getFileIngestionMethod(), hashAlgo).first,
        {
            .others = references,
            // caller is not capable of creating a self-reference, because this is content-addressed without modulus
            .self = false,
        });

    auto dstPath = makeFixedOutputPathFromCA(name, desc);

    addTempRoot(dstPath);

    if (repair || !isValidPath(dstPath)) {

        /* The first check above is an optimisation to prevent
           unnecessary lock acquisition. */

        auto realPath = toRealPath(dstPath);

        PathLocks outputLock({realPath});

        /* The path may have been created by another process in the meantime, so check again. */
        if (repair || !isValidPathUncached(dstPath)) {

            deletePath(realPath);

            autoGC();

            if (inMemoryAndDontNeedRestore) {
                StringSource dumpSource{dump};
                /* Restore from the buffer in memory. */
                auto fim = hashMethod.getFileIngestionMethod();
                switch (fim) {
                case FileIngestionMethod::Flat:
                case FileIngestionMethod::NixArchive:
                    restorePathCapturingHashes(
                        realPath,
                        dumpSource,
                        (FileSerialisationMethod) fim,
                        localSettings.fsyncStorePaths,
                        fileHashes,
                        &linksDir);
                    break;
                }
            } else {
                /* Move the temporary path we restored above. */
                moveFile(tempPath, realPath);
            }

            /* For computing the nar hash. In recursive SHA-256 mode, this
               is the same as the store hash, so no need to do it again. */
            HashResult narHash = {dumpHash, size};
            if (dumpMethod != FileSerialisationMethod::NixArchive || hashAlgo != HashAlgorithm::SHA256) {
                HashSink narSink{HashAlgorithm::SHA256};
                dumpPath(realPath, narSink);
                narHash = narSink.finish();
            }

            canonicalisePathMetaData(
                realPath, {NIX_WHEN_SUPPORT_ACLS(localSettings.ignoredAcls)}); // FIXME: merge into restorePath

            optimisePath(realPath, repair);

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), narHash.hash);
            info.narSize = narHash.numBytesDigested;
            registerValidPath(info);
        } else
            // We may have a negative cache entry for this path, so get rid of it.
            invalidatePathInfoCacheFor(dstPath);

        outputLock.setDeletion(true);
    }

    return dstPath;
}

/* Create a temporary directory in the store that won't be
   garbage-collected until the returned FD is closed. */
std::pair<std::filesystem::path, AutoCloseFD> LocalStore::createTempDirInStore()
{
    std::filesystem::path tmpDirFn;
    AutoCloseFD tmpDirFd;
    bool lockedByUs = false;
    do {
        /* There is a slight possibility that `tmpDir' gets deleted by
           the GC between createTempDir() and when we acquire a lock on it.
           We'll repeat until 'tmpDir' exists and we've locked it.
           Make the directory accessible only to the current user. */
        tmpDirFn = createTempDir(std::filesystem::path{config->realStoreDir.get()}, "tmp", /*mode=*/0700);
        tmpDirFd = openDirectory(tmpDirFn, FinalSymlink::DontFollow);
        if (!tmpDirFd) {
            continue;
        }
        lockedByUs = lockFile(tmpDirFd.get(), ltWrite, true);
    } while (!pathExists(tmpDirFn) || !lockedByUs);
    return {tmpDirFn, std::move(tmpDirFd)};
}

void PathInUse::anchor() {}

void LocalStore::invalidatePathChecked(const StorePath & path)
{
    retrySQLite<void>([&]() {
        auto state(_state->lock());

        SQLiteTxn txn(state->db);

        if (isValidPath_(*state, path)) {
            StorePathSet referrers;
            queryReferrers(*state, path, referrers);
            referrers.erase(path); /* ignore self-references */
            if (!referrers.empty())
                throw PathInUse(
                    "cannot delete path '%s' because it is in use by %s",
                    printStorePath(path),
                    concatMapStringsSep(", ", referrers, [&](auto & p) { return "'" + printStorePath(p) + "'"; }));
            invalidatePath(*state, path);
        }

        txn.commit();
    });
}

bool LocalStore::verifyStore(bool checkContents, RepairFlag repair)
{
    /* repairPath went with the substituter machinery: verify can
       detect, not heal. Refuse instead of silently ignoring the flag */
    if (repair)
        throw Unsupported("repair is not supported by this store extraction");

    printInfo("reading the Nix store...");

    /* Acquire the global GC lock to get a consistent snapshot of
       existing and valid paths. */
    auto fdGCLock = openGCLock();
    FdLock gcLock(fdGCLock.get(), ltRead, true, "waiting for the big garbage collector lock...");

    auto [errors, validPaths] = verifyAllValidPaths(repair);

    /* Optionally, check the content hashes (slow). */
    if (checkContents) {

        printInfo("checking link hashes...");

        for (auto & link : DirectoryIterator{linksDir}) {
            checkInterrupt();
            auto name = link.path().filename();
            printMsg(lvlTalkative, "checking contents of %s", PathFmt(name));
            std::string hash =
                hashPath(makeFSSourceAccessor(link.path()), FileIngestionMethod::NixArchive, HashAlgorithm::SHA256)
                    .first.to_string(HashFormat::Nix32, false);
            if (hash != name.string()) {
                printError(
                    "link %s was modified! expected hash %s, got '%s'", PathFmt(link.path()), name.string(), hash);
                if (repair) {
                    unlinkIfExists(link.path());
                    printInfo("removed link %s", PathFmt(link.path()));
                } else {
                    errors = true;
                }
            }
        }

        printInfo("checking store hashes...");

        Hash nullHash(HashAlgorithm::SHA256);

        for (auto & i : validPaths) {
            try {
                auto info =
                    std::const_pointer_cast<ValidPathInfo>(std::shared_ptr<const ValidPathInfo>(queryPathInfo(i)));

                /* Check the content hash (optionally - slow). */
                printMsg(lvlTalkative, "checking contents of '%s'", printStorePath(i));

                auto hashSink = HashSink(info->narHash.algo);

                dumpPath(toRealPath(i), hashSink);
                auto current = hashSink.finish();

                if (info->narHash != nullHash && info->narHash != current.hash) {
                    printError(
                        "path '%s' was modified! expected hash '%s', got '%s'",
                        printStorePath(i),
                        info->narHash.to_string(HashFormat::Nix32, true),
                        current.hash.to_string(HashFormat::Nix32, true));
                    errors = true;
                } else {

                    bool update = false;

                    /* Fill in missing hashes. */
                    if (info->narHash == nullHash) {
                        printInfo("fixing missing hash on '%s'", printStorePath(i));
                        info->narHash = current.hash;
                        update = true;
                    }

                    /* Fill in missing narSize fields (from old stores). */
                    if (info->narSize == 0) {
                        printInfo("updating size field on '%s' to %s", printStorePath(i), current.numBytesDigested);
                        info->narSize = current.numBytesDigested;
                        update = true;
                    }

                    if (update)
                        updatePathInfo(*_state->lock(), *info);
                }

            } catch (Error & e) {
                /* It's possible that the path got GC'ed, so ignore
                   errors on invalid paths. */
                if (isValidPath(i))
                    logError(e.info());
                else
                    logWarning(e.info());
                errors = true;
            }
        }
    }

    return errors;
}

LocalStore::VerificationResult LocalStore::verifyAllValidPaths(RepairFlag repair)
{
    StorePathSet storePathsInStoreDir;
    /* Why aren't we using `queryAllValidPaths`? Because that would
       tell us about all the paths than the database knows about. Here we
       want to know about all the store paths in the store directory,
       regardless of what the database thinks.

       We will end up cross-referencing these two sources of truth (the
       database and the filesystem) in the loop below, in order to catch
       invalid states.
     */
    for (auto & i : DirectoryIterator{config->realStoreDir.get()}) {
        checkInterrupt();
        try {
            storePathsInStoreDir.insert({i.path().filename().string()});
        } catch (BadStorePath &) {
        }
    }

    /* Check whether all valid paths actually exist. */
    printInfo("checking path existence...");

    StorePathSet done;

    auto existsInStoreDir = [&](const StorePath & storePath) { return storePathsInStoreDir.count(storePath); };

    bool errors = false;
    StorePathSet validPaths;

    for (auto & i : queryAllValidPaths())
        verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);

    return {
        .errors = errors,
        .validPaths = validPaths,
    };
}

void LocalStore::verifyPath(
    const StorePath & path,
    fun<bool(const StorePath &)> existsInStoreDir,
    StorePathSet & done,
    StorePathSet & validPaths,
    RepairFlag repair,
    bool & errors)
{
    checkInterrupt();

    if (!done.insert(path).second)
        return;

    if (!existsInStoreDir(path)) {
        /* Check any referrers first.  If we can invalidate them
           first, then we can invalidate this path as well. */
        bool canInvalidate = true;
        StorePathSet referrers;
        queryReferrers(path, referrers);
        for (auto & i : referrers)
            if (i != path) {
                verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);
                if (validPaths.count(i))
                    canInvalidate = false;
            }

        auto pathS = printStorePath(path);

        if (canInvalidate) {
            printInfo("path '%s' disappeared, removing from database...", pathS);
            invalidatePath(*_state->lock(), path);
        } else {
            printError("path '%s' disappeared, but it still has valid referrers!", pathS);
            errors = true;
        }

        return;
    }

    validPaths.insert(std::move(path));
}


std::optional<TrustedFlag> LocalStore::isTrustedClient()
{
    return Trusted;
}

void LocalStore::vacuumDB()
{
    _state->lock()->db.exec("vacuum");
}

std::optional<std::string> LocalStore::getVersion()
{
    return nixVersion;
}

static RegisterStoreImplementation<LocalStore::Config> regLocalStore;

} // namespace nix
