// Exercise the actual private build/cache functions without a network or retail data.
#include "../../src/build/build.cpp"

static int failures = 0;
static void check(bool value, const char* label) {
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
    if (!value) ++failures;
}
static void put(const retcomm::fs::path& p, const std::string& content = "fixture\n") {
    retcomm::fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}
int main(int argc, char** argv) {
    using namespace retcomm;
    if (argc < 2) return 2;
    const fs::path root = argv[1];
    fs::create_directories(root);
    for (const char* stem : {"OpenBIOS", "SCPH1001", "SCPH5552"}) {
        const fs::path src = root / stem;
        put(src / "generated" / "fixture_dispatch.c");
        put(src / "psxrecomp/generated" / (std::string(stem) + "_dispatch.c"));
        check(psx_generated_ready(src), (std::string(stem) + " game ready").c_str());
        check(bios_generated_present(src / "psxrecomp"), (std::string(stem) + " engine ready").c_str());
    }
    const fs::path bad = root / "negative";
    check(!psx_generated_ready(bad), "missing generated files rejected");
    put(bad / "generated/fixture_dispatch.c");
    put(bad / "psxrecomp/generated/SCPH5552_dispatch.c.txt");
    check(!psx_generated_ready(bad), "wrong suffix rejected");
    fs::create_directories(bad / "psxrecomp/generated/SCPH5552_dispatch.c");
    check(!psx_generated_ready(bad), "directory is not generated C");
    check(!bios_generated_present(bad / "psxrecomp"), "engine directory is not generated C");
    const fs::path only_bios = root / "only bios";
    put(only_bios / "psxrecomp/generated/SCPH5552_dispatch.c");
    check(!psx_generated_ready(only_bios), "missing game output rejected");

    const fs::path embedded = root / "embedded title";
    put(embedded / "psxrecomp/psxrecomp_cli.py");
    put(embedded / "psxrecomp/recompiler/build/psxrecomp-game");
    put(embedded / "psxrecomp/recompiler/build/psxrecomp-bios");
    put(embedded / "psxrecomp/bios/SCPH5552.toml", "[recompiler]\nseeds = 'recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json'\n");
    put(embedded / "psxrecomp/recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json", "[]\n");
    put(embedded / "psxrecomp/bios/SCPH5552.BIN", "private fixture must not be harvested\n");
    Paths paths;
    paths.config_dir = root / "config";
    paths.data_dir = root / "data";
    paths.apps_dir = root / "apps";
    paths.toolchains_dir = root / "toolchains";
    paths.sdks_dir = root / "sdks";
    paths.engines_dir = root / "engines";
    paths.catalog_dir = root / "catalog";
    Title title;
    title.platform = "psx";
    title.build.generate.engine = "psxrecomp";
    const auto sdk = harvest_embedded_sdk(paths, title, embedded, "fixture");
    check(sdk.ok, "embedded SDK harvest succeeds");
    check(files_content_equal(embedded / "psxrecomp/bios/SCPH5552.toml", sdk.root / "bios/SCPH5552.toml"), "SCPH5552 profile retained exactly");
    check(files_content_equal(embedded / "psxrecomp/recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json", sdk.root / "recompiler/seeds/phase2_ghidra_seeds_SCPH5552.json"), "SCPH5552 seeds retained exactly");
    check(!fs::exists(sdk.root / "bios/SCPH5552.BIN"), "owned BIOS excluded from shared SDK harvest");
    for (int i = 2; i < argc; ++i) {
        check(psx_generated_ready(argv[i]), "owned package output is ready");
        check(bios_generated_present(fs::path(argv[i]) / "psxrecomp"), "owned package BIOS is present");
    }
    return failures ? 1 : 0;
}
