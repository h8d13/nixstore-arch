#include "nix/store/globals.hh"
#include "nix/store/global-paths.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/config-global.hh"
#include "nix/util/current-process.hh"
#include "nix/util/executable-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/abstract-setting-to-json.hh"
#include "nix/util/executable-path.hh"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>


#ifdef __GLIBC__
#  include <gnu/lib-names.h>
#  include <nss.h>
#  include <dlfcn.h>
#endif




#include "store-config-private.hh"

namespace nix {

void Settings::anchor() {}





Settings settings;

static GlobalConfig::Register rSettings(&settings);

Settings::Settings()
    : nixStateDir(getEnvOsNonEmpty(OS_STR("NIX_STATE_DIR"))
                      .transform([](auto && s) { return std::filesystem::path(s); })
                      .or_else([]() -> std::optional<std::filesystem::path> {
                          return NIX_STATE_DIR;
                      })
                      .transform([](auto && s) { return canonPath(s); })
                      .value())
{
    buildUsersGroup = isRootUser() ? "nixbld" : "";
    allowSymlinkedStore = getEnv("NIX_IGNORE_SYMLINK_STORE") == "1";

}

void loadConfFile(AbstractConfig & config)
{
    auto applyConfigFile = [&](const std::filesystem::path & path) {
        try {
            std::string contents = readFile(path);
            config.applyConfig(contents, path.string());
        } catch (SystemError &) {
        }
    };

    applyConfigFile(nixConfFile());

    /* We only want to send overrides to the daemon, i.e. stuff from
       ~/.nix/nix.conf or the command line. */
    config.resetOverridden();

    auto files = nixUserConfFiles();
    for (auto file = files.rbegin(); file != files.rend(); file++) {
        applyConfigFile(file->string());
    }

    auto nixConfEnv = getEnv("NIX_CONFIG");
    if (nixConfEnv.has_value()) {
        config.applyConfig(nixConfEnv.value(), "NIX_CONFIG");
    }
}

/**
 * On Windows, NIX_CONF_DIR (and other directories like NIX_STATE_DIR, NIX_LOG_DIR)
 * are not defined at compile time, so we determine paths at runtime using the
 * Windows known folders API (FOLDERID_ProgramData). This allows Nix to work
 * correctly regardless of which drive Windows is installed on.
 */
const std::filesystem::path & nixConfDir()
{
    static const std::filesystem::path dir = getEnvOsNonEmpty(OS_STR("NIX_CONF_DIR"))
                                                 .transform([](auto && s) { return std::filesystem::path(s); })
                                                 .or_else([]() -> std::optional<std::filesystem::path> {
                                                     return NIX_CONF_DIR;
                                                 })
                                                 .transform([](auto && s) { return canonPath(s); })
                                                 .value();
    return dir;
}

const std::vector<std::filesystem::path> & nixUserConfFiles()
{
    static const std::vector<std::filesystem::path> files = [] {
        // Use the paths specified in NIX_USER_CONF_FILES if it has been defined
        auto nixConfFiles = getEnvOs(OS_STR("NIX_USER_CONF_FILES"));
        if (nixConfFiles.has_value()) {
            return ExecutablePath::parse(*nixConfFiles).directories;
        }

        // Use the paths specified by the XDG spec
        std::vector<std::filesystem::path> files;
        auto dirs = getConfigDirs();
        for (auto & dir : dirs) {
            files.insert(files.end(), dir / "nix.conf");
        }
        return files;
    }();
    return files;
}


std::string nixVersion = PACKAGE_VERSION;


template<>
struct BaseSetting<std::vector<StoreReference>>::trait
{
    static constexpr bool appendable = true;
};

template<>
struct BaseSetting<std::set<StoreReference>>::trait
{
    static constexpr bool appendable = true;
};

template<>
StoreReference BaseSetting<StoreReference>::parse(const std::string & str) const
{
    return StoreReference::parse(str);
}

template<>
std::string BaseSetting<StoreReference>::to_string() const
{
    return value.render();
}

template<>
std::vector<StoreReference> BaseSetting<std::vector<StoreReference>>::parse(const std::string & str) const
{
    std::vector<StoreReference> res;
    for (const auto & s : tokenizeString<Strings>(str))
        res.push_back(StoreReference::parse(s));
    return res;
}

template<>
std::string BaseSetting<std::vector<StoreReference>>::to_string() const
{
    Strings ss;
    for (const auto & ref : value)
        ss.push_back(ref.render());
    return concatStringsSep(" ", ss);
}

template<>
void BaseSetting<std::vector<StoreReference>>::appendOrSet(std::vector<StoreReference> newValue, bool append)
{
    if (append)
        value.insert(value.end(), std::make_move_iterator(newValue.begin()), std::make_move_iterator(newValue.end()));
    else
        value = std::move(newValue);
}

template<>
std::set<StoreReference> BaseSetting<std::set<StoreReference>>::parse(const std::string & str) const
{
    std::set<StoreReference> res;
    for (const auto & s : tokenizeString<Strings>(str))
        res.insert(StoreReference::parse(s));
    return res;
}

template<>
std::string BaseSetting<std::set<StoreReference>>::to_string() const
{
    Strings ss;
    for (const auto & ref : value)
        ss.push_back(ref.render());
    return concatStringsSep(" ", ss);
}

template<>
void BaseSetting<std::set<StoreReference>>::appendOrSet(std::set<StoreReference> newValue, bool append)
{
    if (append)
        value.insert(std::make_move_iterator(newValue.begin()), std::make_move_iterator(newValue.end()));
    else
        value = std::move(newValue);
}

template class BaseSetting<StoreReference>;
template class BaseSetting<std::vector<StoreReference>>;
template class BaseSetting<std::set<StoreReference>>;

static void preloadNSS()
{
    /* builtin:fetchurl can trigger a DNS lookup, which with glibc can trigger a dynamic library load of
       one of the glibc NSS libraries in a sandboxed child, which will fail unless the library's already
       been loaded in the parent. So we force a lookup of an invalid domain to force the NSS machinery to
       load its lookup libraries in the parent before any child gets a chance to. */
    static std::once_flag dns_resolve_flag;

    std::call_once(dns_resolve_flag, []() {
#ifdef __GLIBC__
        /* On linux, glibc will run every lookup through the nss layer.
         * That means every lookup goes, by default, through nscd, which acts as a local
         * cache.
         * Because we run builds in a sandbox, we also remove access to nscd otherwise
         * lookups would leak into the sandbox.
         *
         * But now we have a new problem, we need to make sure the nss_dns backend that
         * does the dns lookups when nscd is not available is loaded or available.
         *
         * We can't make it available without leaking nix's environment, so instead we'll
         * load the backend, and configure nss so it does not try to run dns lookups
         * through nscd.
         *
         * This is technically only used for builtins:fetch* functions so we only care
         * about dns.
         *
         * All other platforms are unaffected.
         */
        if (!dlopen(LIBNSS_DNS_SO, RTLD_NOW))
            warn("unable to load nss_dns backend");
        // FIXME: get hosts entry from nsswitch.conf.
        __nss_configure_lookup("hosts", "files dns");
#endif
    });
}

static bool initLibStoreDone = false;

void assertLibStoreInitialized()
{
    if (!initLibStoreDone) {
        printError("The program must call nix::initNix() before calling any libstore library functions.");
        abort();
    };
}

void initLibStore(bool loadConfig)
{
    if (initLibStoreDone)
        return;

    initLibUtil();

    if (loadConfig)
        loadConfFile(globalConfig);

    preloadNSS();

    initLibStoreDone = true;
}

} // namespace nix
