// Import any directory tree into a local store as one content-addressed
// store path, then deduplicate the whole store.
// usage: import-dir <store-root> <name> <dir>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include <nix/store/globals.hh>
#include <nix/store/local-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/config-global.hh>
#include <nix/util/source-accessor.hh>

using namespace nix;

int main(int argc, char ** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <store-root> <name> <dir>\n", argv[0]);
		return 1;
	}

	initLibStore(false);
	verbosity = lvlError;

	/* we never build derivations, and under a fake-root userns the
	   root default "nixbld" doesn't exist */
	globalConfig.set("build-users-group", "");

	auto store = openStore(std::string("local?root=") + argv[1]);
	auto local = store.dynamic_pointer_cast<LocalStore>();
	auto dir = std::filesystem::absolute(argv[3]);

	auto t0 = std::chrono::steady_clock::now();
	auto path = store->addToStore(argv[2], {makeFSSourceAccessor(dir), CanonPath::root});
	auto t1 = std::chrono::steady_clock::now();
	printf("imported: %s (%.1f s)\n", store->printStorePath(path).c_str(),
		std::chrono::duration<double>(t1 - t0).count());

	auto info = store->queryPathInfo(path);
	printf("nar hash: %s\nnar size: %.1f MiB\n",
		info->narHash.to_string(HashFormat::SRI, true).c_str(),
		info->narSize / (1024.0 * 1024.0));

	OptimiseStats stats;
	auto t2 = std::chrono::steady_clock::now();
	local->optimiseStore(stats);
	auto t3 = std::chrono::steady_clock::now();
	printf("optimise: linked %lu files, freed %.1f MiB (%.1f s)\n",
		stats.filesLinked, stats.bytesFreed / (1024.0 * 1024.0),
		std::chrono::duration<double>(t3 - t2).count());
	return 0;
}
