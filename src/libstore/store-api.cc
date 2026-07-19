#include "nix/util/logging.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/util/util.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/archive.hh"
#include "nix/util/callback.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#include "store-config-private.hh"

#include <filesystem>
#include <nlohmann/json.hpp>


using json = nlohmann::json;

namespace nix {

void StoreConfig::anchor() {}

void InvalidPath::anchor() {}

void Unsupported::anchor() {}

void SubstituteGone::anchor() {}

void SubstituterDisabled::anchor() {}

void InvalidStoreReference::anchor() {}

void StoreConfigBase::anchor() {}

static std::string canonStoreDir(std::string path)
{
    if (path.empty() || path[0] != '/')
        throw UsageError("store directory path \"%s\" is not an absolute path", path);
    return CanonPath(std::move(path)).abs();
}

static std::string canonStoreDir(std::filesystem::path path)
{
    if (!path.is_absolute())
        throw UsageError("store directory path %s is not an absolute path", PathFmt(path));
    return canonPath(std::move(path)).string();
}

StoreConfigBase::StoreDirSetting::StoreDirSetting(Config * options, FilePathType pathType)
    : BaseSetting<std::string>(
          [pathType]() -> std::string {
              auto envOverrides = getEnvOsNonEmpty(OS_STR("NIX_STORE_DIR")).or_else([] {
                  return getEnvOsNonEmpty(OS_STR("NIX_STORE"));
              });

              switch (pathType) {
              case FilePathType::Unix:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return os_string_to_string(s); })
                                           .value_or(NIX_STORE_DIR));

              case FilePathType::Native:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return std::filesystem::path(s); })
                                           .or_else([]() -> std::optional<std::filesystem::path> {
                                               return std::filesystem::path{NIX_STORE_DIR};
                                           })
                                           .value());
              }
              assert(false);
          }(),
          true,
          "store",
          R"(
            Logical location of the Nix store, usually `/nix/store`.

            Defaults to [`NIX_STORE_DIR`](@docroot@/command-ref/env-common.md#env-NIX_STORE_DIR) if unset.

            Note that you can only copy store paths between stores if they have the same `store` setting.
          )",
          {})
    , pathType(pathType)
{
    options->addSetting(this);
}

std::string StoreConfigBase::StoreDirSetting::parse(const std::string & str) const
{
    if (str.empty())
        throw UsageError("setting '%s' is a path and paths cannot be empty", name);

    switch (pathType) {
    case FilePathType::Unix:
        return canonStoreDir(str);
    case FilePathType::Native:
        return canonStoreDir(std::filesystem::path(str));
    }
    assert(false);
}

StoreConfigBase::StoreConfigBase(const StoreReference::Params & params, FilePathType pathType)
    : Config(params)
    , storeDir_{this, pathType}
{
}

StoreConfig::StoreConfig(const Params & params, FilePathType pathType)
    : StoreConfigBase(params, pathType)
    , StoreDirConfig{storeDir_}
{
}

bool StoreDirConfig::isInStore(std::string_view path) const
{
    return isInDir(path, storeDir);
}

std::pair<StorePath, CanonPath> StoreDirConfig::toStorePath(std::string_view path) const
{
    if (!isInStore(path))
        throw Error("path '%1%' is not in the Nix store", path);
    auto slash = path.find('/', storeDir.size() + 1);
    if (slash == std::string::npos)
        return {parseStorePath(path), CanonPath::root};
    else
        return {parseStorePath(path.substr(0, slash)), CanonPath{path.substr(slash)}};
}

std::filesystem::path Store::followLinksToStore(std::string_view _path) const
{
    auto path = absPath(std::string(_path));

    // Limit symlink follows to prevent infinite loops
    unsigned int followCount = 0;
    const unsigned int maxFollow = 1024;

    while (!isInStore(path.string())) {
        if (!std::filesystem::is_symlink(path))
            break;

        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while resolving '%s'", _path);

        auto target = readLink(path);
        auto parentPath = path.parent_path();
        path = absPath(target, &parentPath);
    }

    if (!isInStore(path.string()))
        throw BadStorePath("path %1% is not in the Nix store", PathFmt(path));
    return path;
}

StorePath Store::followLinksToStorePath(std::string_view path) const
{
    return toStorePath(followLinksToStore(path).string()).first;
}

StorePath Store::addToStore(
    std::string_view name,
    const SourcePath & path,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    PathFilter & filter,
    RepairFlag repair)
{
    FileSerialisationMethod fsm;
    switch (method.getFileIngestionMethod()) {
    case FileIngestionMethod::Flat:
        fsm = FileSerialisationMethod::Flat;
        break;
    case FileIngestionMethod::NixArchive:
        fsm = FileSerialisationMethod::NixArchive;
        break;
    }
    std::optional<StorePath> storePath;
    auto sink = sourceToSink([&](Source & source) {
        LengthSource lengthSource(source);
        storePath = addToStoreFromDump(lengthSource, name, fsm, method, hashAlgo, references, repair);
        if (settings.warnLargePathThreshold && lengthSource.total >= settings.warnLargePathThreshold)
            warn("copied large path '%s' to the store (%s)", path, renderSize(lengthSource.total));
    });
    dumpPath(path, *sink, fsm, filter);
    sink->finish();
    return storePath.value();
}

void Store::addMultipleToStore(PathsSource && pathsToCopy, Activity & act, RepairFlag repair, CheckSigsFlag checkSigs)
{
    std::atomic<size_t> nrDone{0};
    std::atomic<size_t> nrFailed{0};
    std::atomic<uint64_t> nrRunning{0};

    using PathWithInfo = std::pair<ValidPathInfo, std::unique_ptr<Source>>;

    uint64_t bytesExpected = 0;

    std::map<StorePath, PathWithInfo *> infosMap;
    StorePathSet storePathsToAdd;
    for (auto & thingToAdd : pathsToCopy) {
        bytesExpected += thingToAdd.first.narSize;
        infosMap.insert_or_assign(thingToAdd.first.path, &thingToAdd);
        storePathsToAdd.insert(thingToAdd.first.path);
    }

    act.setExpected(actCopyPath, bytesExpected);

    auto showProgress = [&, nrTotal = pathsToCopy.size()]() { act.progress(nrDone, nrTotal, nrRunning, nrFailed); };

    processGraph<StorePath>(
        storePathsToAdd,

        [&](const StorePath & path) {
            auto & [info, _] = *infosMap.at(path);

            if (isValidPath(info.path)) {
                nrDone++;
                showProgress();
                return StorePathSet();
            }

            return info.references;
        },

        [&](const StorePath & path) {
            checkInterrupt();

            auto & [info_, source_] = *infosMap.at(path);
            auto info = info_;
            info.ultimate = false;

            /* Make sure that the Source object is destroyed when
               we're done. In particular, a SinkToSource object must
               be destroyed to ensure that the destructors on its
               stack frame are run; this includes
               LegacySSHStore::narFromPath()'s connection lock. */
            auto source = std::move(source_);

            if (!isValidPath(info.path)) {
                MaintainCount<decltype(nrRunning)> mc(nrRunning);
                showProgress();
                try {
                    addToStore(info, *source, repair, checkSigs);
                } catch (Error & e) {
                    throw e;
                }
            }

            nrDone++;
            showProgress();
        });
}

/*
The aim of this function is to compute in one pass the correct ValidPathInfo for
the files that we are trying to add to the store. To accomplish that in one
pass, given the different kind of inputs that we can take (normal nar archives,
nar archives with non SHA-256 hashes, and flat files), we set up a net of sinks
and aliases. Also, since the dataflow is obfuscated by this, we include here a
graphviz diagram:

digraph graphname {
    node [shape=box]
    fileSource -> narSink
    narSink [style=dashed]
    narSink -> unusualHashTee [style = dashed, label = "Recursive && !SHA-256"]
    narSink -> narHashSink [style = dashed, label = "else"]
    unusualHashTee -> narHashSink
    unusualHashTee -> caHashSink
    fileSource -> parseSink
    parseSink [style=dashed]
    parseSink-> fileSink [style = dashed, label = "Flat"]
    parseSink -> blank [style = dashed, label = "Recursive"]
    fileSink -> caHashSink
}
*/
ValidPathInfo Store::addToStoreSlow(
    std::string_view name,
    const SourcePath & srcPath,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    std::optional<Hash> expectedCAHash)
{
    HashSink narHashSink{HashAlgorithm::SHA256};
    HashSink caHashSink{hashAlgo};

    /* Note that fileSink and unusualHashTee must be mutually exclusive, since
       they both write to caHashSink. Note that that requisite is currently true
       because the former is only used in the flat case. */
    RegularFileSink fileSink{caHashSink};
    TeeSink unusualHashTee{narHashSink, caHashSink};

    auto & narSink = method == ContentAddressMethod::Raw::NixArchive && hashAlgo != HashAlgorithm::SHA256
                         ? static_cast<Sink &>(unusualHashTee)
                         : narHashSink;

    /* Functionally, this means that fileSource will yield the content of
       srcPath. The fact that we use scratchpadSink as a temporary buffer here
       is an implementation detail. */
    auto fileSource = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });

    /* tapped provides the same data as fileSource, but we also write all the
       information to narSink. */
    TeeSource tapped{*fileSource, narSink};

    NullFileSystemObjectSink blank;
    auto & parseSink = method.getFileIngestionMethod() == FileIngestionMethod::Flat
                           ? (FileSystemObjectSink &) fileSink
                           : (FileSystemObjectSink &) blank; // for recursive we do recursive

    /* The information that flows from tapped (besides being replicated in
       narSink), is now put in parseSink. */
    parseDump(parseSink, tapped);

    /* We extract the result of the computation from the sink by calling
       finish. */
    auto [narHash, narSize] = narHashSink.finish();

    auto hash = method == ContentAddressMethod::Raw::NixArchive && hashAlgo == HashAlgorithm::SHA256
                    ? narHash
                    : caHashSink.finish().hash;

    if (expectedCAHash && expectedCAHash != hash)
        throw Error("hash mismatch for '%s'", srcPath);

    auto info = ValidPathInfo::makeFromCA(
        *this,
        name,
        ContentAddressWithReferences::fromParts(
            method,
            hash,
            {
                .others = references,
                .self = false,
            }),
        narHash);
    info.narSize = narSize;

    if (!isValidPath(info.path)) {
        auto source = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });
        addToStore(info, *source);
    }

    return info;
}

void Store::narFromPath(const StorePath & path, Sink & sink)
{
    auto accessor = requireStoreObjectAccessor(path);
    SourcePath sourcePath{accessor};
    dumpPath(sourcePath, sink, FileSerialisationMethod::NixArchive);
}


Store::Store(const Store::Config & config)
    : StoreDirConfig{config}
    , config{config}
    , pathInfoCache(make_ref<decltype(pathInfoCache)::element_type>((size_t) config.pathInfoCacheSize))
{
    assertLibStoreInitialized();
}

StoreReference StoreConfig::getReference() const
{
    return {.variant = StoreReference::Auto{}};
}

bool StoreConfig::getReadOnly() const
{
    return settings.readOnlyMode;
}

bool Store::PathInfoCacheValue::isKnownNow()
{
    /* no substituter TTLs: cached local path info never expires */
    return true;
}

void Store::invalidatePathInfoCacheFor(const StorePath & path)
{
    pathInfoCache->lock()->erase(path);
}






bool Store::isValidPath(const StorePath & storePath)
{
    auto res = pathInfoCache->lock()->get(storePath);
    if (res && res->isKnownNow()) {
        stats.narInfoReadAverted++;
        return res->didExist();
    }

    return isValidPathUncached(storePath);
}

/* Default implementation for stores that only implement
   queryPathInfoUncached(). */
bool Store::isValidPathUncached(const StorePath & path)
{
    try {
        queryPathInfo(path);
        return true;
    } catch (InvalidPath &) {
        return false;
    }
}

ref<const ValidPathInfo> Store::queryPathInfo(const StorePath & storePath)
{
    std::promise<ref<const ValidPathInfo>> promise;

    queryPathInfo(storePath, {[&](std::future<ref<const ValidPathInfo>> result) {
                      try {
                          promise.set_value(result.get());
                      } catch (...) {
                          promise.set_exception(std::current_exception());
                      }
                  }});

    return promise.get_future().get();
}

static bool goodStorePath(const StorePath & expected, const StorePath & actual)
{
    return expected.hashPart() == actual.hashPart()
           && (expected.name() == Store::MissingName || expected.name() == actual.name());
}

std::optional<std::shared_ptr<const ValidPathInfo>> Store::queryPathInfoFromClientCache(const StorePath & storePath)
{
    auto hashPart = std::string(storePath.hashPart());

    auto res = pathInfoCache->lock()->get(storePath);
    if (res && res->isKnownNow()) {
        stats.narInfoReadAverted++;
        if (res->didExist())
            return std::make_optional(res->value);
        else
            return std::make_optional(nullptr);
    }

    return std::nullopt;
}

void Store::queryPathInfo(const StorePath & storePath, Callback<ref<const ValidPathInfo>> callback) noexcept
{
    auto hashPart = std::string(storePath.hashPart());

    try {
        auto r = queryPathInfoFromClientCache(storePath);
        if (r.has_value()) {
            std::shared_ptr<const ValidPathInfo> & info = *r;
            if (info)
                return callback(ref(info));
            else
                throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
        }
    } catch (...) {
        return callback.rethrow();
    }

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    queryPathInfoUncached(
        storePath, {[this, storePath, hashPart, callbackPtr](std::future<std::shared_ptr<const ValidPathInfo>> fut) {
            try {
                auto info = fut.get();

                pathInfoCache->lock()->upsert(storePath, PathInfoCacheValue{.value = info});

                if (!info || !goodStorePath(storePath, info->path)) {
                    stats.narInfoMissing++;
                    throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
                }

                (*callbackPtr)(ref<const ValidPathInfo>(info));
            } catch (...) {
                callbackPtr->rethrow();
            }
        }});
}




StorePathSet Store::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    struct State
    {
        size_t left;
        StorePathSet valid;
        std::exception_ptr exc;
    };

    Sync<State> state_(State{paths.size(), StorePathSet()});

    std::condition_variable wakeup;
    ThreadPool pool;

    auto doQuery = [&](const StorePath & path) {
        checkInterrupt();
        queryPathInfo(path, {[path, &state_, &wakeup](std::future<ref<const ValidPathInfo>> fut) {
                          bool exists = false;
                          std::exception_ptr newExc{};

                          try {
                              auto info = fut.get();
                              exists = true;
                          } catch (InvalidPath &) {
                          } catch (...) {
                              newExc = std::current_exception();
                          }

                          auto state(state_.lock());

                          if (exists)
                              state->valid.insert(path);

                          if (newExc)
                              state->exc = newExc;

                          assert(state->left);
                          if (!--state->left)
                              wakeup.notify_one();
                      }});
    };

    for (auto & path : paths)
        pool.enqueue(std::bind(doQuery, path));

    pool.process();

    while (true) {
        auto state(state_.lock());
        if (!state->left) {
            if (state->exc)
                std::rethrow_exception(state->exc);
            return std::move(state->valid);
        }
        state.wait(wakeup);
    }
}

/* Return a string accepted by decodeValidPathInfo() that
   registers the specified paths as valid.  Note: it's the
   responsibility of the caller to provide a closure. */
std::string Store::makeValidityRegistration(const StorePathSet & paths, bool showDerivers, bool showHash)
{
    std::string s = "";

    for (auto & i : paths) {
        s += printStorePath(i) + "\n";

        auto info = queryPathInfo(i);

        if (showHash) {
            s += info->narHash.to_string(HashFormat::Base16, false) + "\n";
            s += fmt("%1%\n", info->narSize);
        }

        auto deriver = showDerivers && info->deriver ? printStorePath(*info->deriver) : "";
        s += deriver + "\n";

        s += fmt("%1%\n", info->references.size());

        for (auto & j : info->references)
            s += printStorePath(j) + "\n";
    }

    return s;
}

StorePathSet Store::exportReferences(const StorePathSet & storePaths, const StorePathSet & inputPaths)
{
    StorePathSet paths;

    for (auto & storePath : storePaths) {
        if (!inputPaths.count(storePath))
            throw Error(
                "cannot export references of path '%s' because it is not in the input closure of the derivation",
                printStorePath(storePath));

        computeFSClosure({storePath}, paths);
    }

    return paths;
}

const Store::Stats & Store::getStats()
{
    stats.pathInfoCacheSize = pathInfoCache->readLock()->size();
    return stats;
}

static std::string
makeCopyPathMessage(const StoreConfig & srcCfg, const StoreConfig & dstCfg, std::string_view storePath)
{
    auto src = srcCfg.getReference();
    auto dst = dstCfg.getReference();

    auto isShorthand = [](const StoreReference & ref) {
        /* At this point StoreReference **must** be resolved. */
        const auto & specified = std::visit(
            overloaded{
                [](const StoreReference::Auto &) -> const StoreReference::Specified & { unreachable(); },
                [](const StoreReference::Specified & specified) -> const StoreReference::Specified & {
                    return specified;
                }},
            ref.variant);
        const auto & scheme = specified.scheme;
        return (scheme == "local" || scheme == "unix") && specified.authority.empty();
    };

    if (isShorthand(src))
        return fmt("copying path '%s' to '%s'", storePath, dstCfg.getHumanReadableURI());

    if (isShorthand(dst))
        return fmt("copying path '%s' from '%s'", storePath, srcCfg.getHumanReadableURI());

    return fmt(
        "copying path '%s' from '%s' to '%s'", storePath, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI());
}

void copyStorePath(
    Store & srcStore, Store & dstStore, const StorePath & storePath, RepairFlag repair, CheckSigsFlag checkSigs)
{
    /* Bail out early (before starting a download from srcStore) if
       dstStore already has this path. */
    if (!repair && dstStore.isValidPath(storePath))
        return;

    const auto & srcCfg = srcStore.config;
    const auto & dstCfg = dstStore.config;
    auto storePathS = srcStore.printStorePath(storePath);
    Activity act(
        *logger,
        lvlInfo,
        actCopyPath,
        makeCopyPathMessage(srcCfg, dstCfg, storePathS),
        {storePathS, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI()});
    PushActivity pact(act.id);

    auto info = srcStore.queryPathInfo(storePath);

    uint64_t total = 0;

    // recompute store path on the chance dstStore does it differently
    if (info->ca && info->references.empty()) {
        auto info2 = make_ref<ValidPathInfo>(*info);
        info2->path =
            dstStore.makeFixedOutputPathFromCA(info->path.name(), info->contentAddressWithReferences().value());
        if (dstStore.storeDir == srcStore.storeDir)
            assert(info->path == info2->path);
        info = info2;
    }

    if (info->ultimate) {
        auto info2 = make_ref<ValidPathInfo>(*info);
        info2->ultimate = false;
        info = info2;
    }

    if (getEnv("_NIX_TEST_CONCURRENT_SUBSTITUTION"))
        std::this_thread::sleep_for(std::chrono::seconds(1));

    auto source = sinkToSource(
        [&](Sink & sink) {
            LambdaSink progressSink([&](std::string_view data) {
                total += data.size();
                act.progress(total, info->narSize);
            });
            TeeSink tee{sink, progressSink};
            srcStore.narFromPath(storePath, tee);
        },
        [&]() {
            throw EndOfFile(
                "NAR for '%s' fetched from '%s' is incomplete",
                srcStore.printStorePath(storePath),
                srcStore.config.getHumanReadableURI());
        });

    dstStore.addToStore(*info, *source, repair, checkSigs);
}


std::map<StorePath, StorePath> copyPaths(
    Store & srcStore,
    Store & dstStore,
    const StorePathSet & storePaths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute)
{
    auto valid = dstStore.queryValidPaths(storePaths, substitute);

    StorePathSet missing;
    for (auto & path : storePaths)
        if (!valid.count(path))
            missing.insert(path);

    /* Don't start an activity if there's no work to do. */
    if (missing.empty())
        return {};

    Activity act(*logger, lvlInfo, actCopyPaths, fmt("copying %d paths", missing.size()));

    // In the general case, `addMultipleToStore` requires a sorted list of
    // store paths to add, so sort them right now
    auto sortedMissing = srcStore.topoSortPaths(missing);

    std::map<StorePath, StorePath> pathsMap;
    for (auto & path : storePaths)
        pathsMap.insert_or_assign(path, path);

    Store::PathsSource pathsToCopy;

    auto computeStorePathForDst = [&](const ValidPathInfo & currentPathInfo) -> StorePath {
        auto storePathForSrc = currentPathInfo.path;
        auto storePathForDst = storePathForSrc;
        if (currentPathInfo.ca && currentPathInfo.references.empty()) {
            storePathForDst = dstStore.makeFixedOutputPathFromCA(
                currentPathInfo.path.name(), currentPathInfo.contentAddressWithReferences().value());
            if (dstStore.storeDir == srcStore.storeDir)
                assert(storePathForDst == storePathForSrc);
            if (storePathForDst != storePathForSrc)
                debug(
                    "replaced path '%s' to '%s' for substituter '%s'",
                    srcStore.printStorePath(storePathForSrc),
                    dstStore.printStorePath(storePathForDst),
                    dstStore.config.getHumanReadableURI());
        }
        return storePathForDst;
    };

    for (auto & missingPath : sortedMissing | std::views::reverse) {
        auto info = srcStore.queryPathInfo(missingPath);

        auto storePathForDst = computeStorePathForDst(*info);
        pathsMap.insert_or_assign(missingPath, storePathForDst);

        ValidPathInfo infoForDst = *info;
        infoForDst.path = storePathForDst;

        auto source = sinkToSource([&, narSize = info->narSize](Sink & sink) {
            // We can reasonably assume that the copy will happen whenever we
            // read the path, so log something about that at that point
            uint64_t total = 0;
            const auto & srcCfg = srcStore.config;
            const auto & dstCfg = dstStore.config;
            auto storePathS = srcStore.printStorePath(missingPath);
            Activity act(
                *logger,
                lvlInfo,
                actCopyPath,
                makeCopyPathMessage(srcCfg, dstCfg, storePathS),
                {storePathS, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI()});
            PushActivity pact(act.id);

            LambdaSink progressSink([&](std::string_view data) {
                total += data.size();
                act.progress(total, narSize);
            });
            TeeSink tee{sink, progressSink};

            srcStore.narFromPath(missingPath, tee);
        });
        pathsToCopy.emplace_back(std::move(infoForDst), std::move(source));
    }

    dstStore.addMultipleToStore(std::move(pathsToCopy), act, repair, checkSigs);

    return pathsMap;
}


void copyClosure(
    Store & srcStore,
    Store & dstStore,
    const StorePathSet & storePaths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute,
    bool includeOutputs)
{
    if (&srcStore == &dstStore)
        return;

    StorePathSet closure;
    srcStore.computeFSClosure(storePaths, closure, false, includeOutputs);
    copyPaths(srcStore, dstStore, closure, repair, checkSigs, substitute);
}

std::optional<ValidPathInfo>
decodeValidPathInfo(const Store & store, std::istream & str, std::optional<HashResult> hashGiven)
{
    std::string path;
    getline(str, path);
    if (str.eof()) {
        return {};
    }
    if (!hashGiven) {
        std::string s;
        getline(str, s);
        auto narHash = Hash::parseAny(s, HashAlgorithm::SHA256);
        getline(str, s);
        auto narSize = string2Int<uint64_t>(s);
        if (!narSize)
            throw Error("number expected");
        hashGiven = {narHash, *narSize};
    }
    ValidPathInfo info(store.parseStorePath(path), {store, hashGiven->hash});
    info.narSize = hashGiven->numBytesDigested;
    std::string deriver;
    getline(str, deriver);
    if (deriver != "")
        info.deriver = store.parseStorePath(deriver);
    std::string s;
    getline(str, s);
    auto n = string2Int<int>(s);
    if (!n)
        throw Error("number expected");
    while ((*n)--) {
        getline(str, s);
        info.references.insert(store.parseStorePath(s));
    }
    if (!str || str.eof())
        throw Error("missing input");
    return std::optional<ValidPathInfo>(std::move(info));
}






const std::filesystem::path & StoreConfig::getStateDir() const
{
    return settings.nixStateDir;
}

const std::filesystem::path & StoreConfig::getLogDir() const
{
    static std::filesystem::path logDir = [] {
        return getEnvOsNonEmpty(OS_STR("NIX_LOG_DIR"))
            .transform([](auto && s) { return std::filesystem::path(s); })
            .or_else([]() -> std::optional<std::filesystem::path> {
                return NIX_LOG_DIR;
            })
            .transform([](auto && s) { return canonPath(s); })
            .value();
    }();
    return logDir;
}

} // namespace nix
