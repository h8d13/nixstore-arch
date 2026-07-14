// Delete store paths from a local store, disk and db together, via the
// GC codepath: refuses paths that other valid paths still reference.
// usage: rm-path <store-root> <store-path-basename>...
#include <cstdio>

#include <nix/store/gc-store.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-cast.hh>
#include <nix/store/store-open.hh>
#include <nix/util/config-global.hh>

using namespace nix;

int main(int argc, char ** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <store-root> <store-path-basename>...\n", argv[0]);
		return 1;
	}

	initLibStore(false);
	verbosity = lvlError;
	globalConfig.set("build-users-group", "");

	auto store = openStore(std::string("local?root=") + argv[1]);
	auto & gcStore = require<GcStore>(*store);

	GCOptions::SpecificPaths specific;
	for (int i = 2; i < argc; i++)
		specific.paths.insert(StorePath(argv[i]));

	GCOptions opts;
	opts.action = GCOptions::gcDeleteSpecific;
	opts.pathsToDelete = std::move(specific);

	GCResults results;
	gcStore.collectGarbage(opts, results);
	printf("deleted %zu paths, freed %.1f MiB\n",
		results.paths.size(), results.bytesFreed / (1024.0 * 1024.0));
	return 0;
}
