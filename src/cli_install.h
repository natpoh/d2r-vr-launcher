// cli_install.h - the `releases` and `install` commands (the rest of the command line is in cli.cpp).
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "cli.h"

namespace vrml {

struct InstallCliArgs {
    std::vector<std::string> pos;           // pos[0] is the command, pos[1] (install) the target
    std::string game;                       // "" = none given
    std::string pin_file;                   // TEST switch (needs VRML_ALLOW_HTTP_HOST)
    bool json = false, dry_run = false, no_run = false, accept_unverified = false, force = false;
};

int RunReleasesCommand(const InstallCliArgs& a, std::ostream& out, std::ostream& err, CliEnv& env);
int RunInstallCommand(const InstallCliArgs& a, std::ostream& out, std::ostream& err, CliEnv& env, const fs::path& home, Logger& log);

}  // namespace vrml
