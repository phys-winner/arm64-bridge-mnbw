#include "shader_path_guard.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << "shader path guard test failed: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using namespace warband::shader_path_guard;

    Check(kMaxRootAndTechniqueBytes == 228u,
          "legacy 256-byte path boundary stays explicit");

    const std::string boundary_name(128u, 'a');
    Selection selected = SelectTechnique(100u, boundary_name);
    Check(!selected.used_fallback && selected.name == boundary_name,
          "root plus shader name at 228 bytes is accepted");

    const std::string overflowing_name(129u, 'a');
    selected = SelectTechnique(100u, overflowing_name);
    Check(selected.used_fallback && selected.name == kFallbackTechnique,
          "root plus shader name at 229 bytes uses the safe fallback");

    const std::string invalid_names[] = {
        std::string{}, "../skybox", "folder/skybox", "folder\\skybox",
        "skybox.glsl", std::string("sky") + static_cast<char>(0x80),
    };
    for (const std::string& invalid : invalid_names) {
        selected = SelectTechnique(32u, invalid);
        Check(selected.used_fallback && selected.name == kFallbackTechnique,
              "unsafe shader name uses the safe fallback");
    }

    selected = SelectTechnique(32u, "skybox_new_HDR");
    Check(!selected.used_fallback && selected.name == "skybox_new_HDR",
          "normal Warband shader name remains unchanged");

    std::cout << "shader path guard tests passed\n";
    return 0;
}
