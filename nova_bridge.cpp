// nova_bridge.cpp — Nova Program Entry Glue
// Owns the ONE `main()` symbol for the whole binary. Registers whatever
// ecosystem modules this build links in, then hands off to the canonical
// nova_main() implemented in nova_rt.cpp. Do not define main() elsewhere,
// and do not redefine NovaValue or any nova_rt_* function here — this file
// only wires already-registered modules into the runtime entry point.

#include "nova_rt.hpp"

extern "C" {
    // These are expected to be defined by the corresponding per-domain
    // translation units elsewhere in the compiler tree (not part of this
    // refactor). Linking will fail loudly if one is missing from the build
    // — that's expected and correct, not something this file should paper
    // over with stub definitions.
    void nova_ecosystem_register();
    void nova_mobile_register();
    void nova_game_register();
    void nova_ai_register();
    void nova_ffi_register();
    void nova_metal_register();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    nova_ecosystem_register();
    nova_mobile_register();
    nova_game_register();
    nova_ai_register();
    nova_ffi_register();
    nova_metal_register();

    return nova_main();
}
