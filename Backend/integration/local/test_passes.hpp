// The test clients' join passes (Dev/testkit/licence_fixture.py): every KCDS server requires a
// legal game, so each join of a test client asks the command in KCDS_TEST_PASS_SIGNER
// for a pass of its handshake's nonce ("<signer> <binding> 127.0.0.1:<port>", the pass on its
// output). Used by the roundtrip probe and the load probe; without the variable a client asks for
// none.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "runtime.hpp"

namespace kcd2s::local {

inline std::function<Client::LicenceProof(std::uint64_t)> test_passes(unsigned long port) {
  const char* signer = std::getenv("KCDS_TEST_PASS_SIGNER");
  if (!signer || !*signer) {
    return {};
  }
  const std::string command = signer;
  return [command, port](std::uint64_t nonce) {
    // cmd /c takes the whole line in quotes when it starts with a quoted program.
    const std::string line = "\"" + command + " " + transport::licence_binding(nonce) +
                             " 127.0.0.1:" + std::to_string(port) + "\"";
    std::string pass;
    if (FILE* output = _popen(line.c_str(), "r")) {
      char buffer[1024];
      while (std::fgets(buffer, sizeof(buffer), output)) {
        pass += buffer;
      }
      _pclose(output);
    }
    while (!pass.empty() && (pass.back() == '\n' || pass.back() == '\r')) {
      pass.pop_back();
    }
    return Client::LicenceProof(pass);
  };
}

}  // namespace kcd2s::local
