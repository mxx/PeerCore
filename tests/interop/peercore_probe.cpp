#include "probe/probe_app.hpp"

#include <sodium.h>

#include <iostream>
int main(int argc, char** argv) {
    if (::sodium_init() < 0) {
        std::cerr << "failed to initialize libsodium\n";
        return 1;
    }

    auto config = peercore::interop::probe::parse_probe_args(argc, argv);
    if (config.is_err()) {
        std::cerr << config.error().message << "\n"
                  << peercore::interop::probe::usage_string() << "\n";
        return 2;
    }
    return peercore::interop::probe::run_probe(config.value());
}
