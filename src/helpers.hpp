#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

/// Reads the memgraph configuration files.
///
/// Load flags in this order, the last one has the highest priority:
///   1) /etc/memgraph/memgraph.conf
///   2) ~/.memgraph/config
///   3) env - MEMGRAPH_CONFIG
inline void LoadConfig(const std::string &product_name) {
  namespace fs = std::filesystem;
  std::vector<fs::path> configs = {fs::path("/etc/memgraph/memgraph.conf")};
  if (getenv("HOME") != nullptr)
    configs.emplace_back(fs::path(getenv("HOME")) /
                         fs::path(".memgraph/config"));
  {
    auto memgraph_config = getenv("MEMGRAPH_CONFIG");
    if (memgraph_config != nullptr) {
      auto path = fs::path(memgraph_config);
      CHECK(fs::exists(path))
          << "MEMGRAPH_CONFIG environment variable set to nonexisting path: "
          << path.generic_string();
      configs.emplace_back(path);
    }
  }

  std::vector<std::string> flagfile_arguments;
  for (const auto &config : configs)
    if (fs::exists(config)) {
      flagfile_arguments.emplace_back(
          std::string("--flag-file=" + config.generic_string()));
    }

  int custom_argc = static_cast<int>(flagfile_arguments.size()) + 1;
  char **custom_argv = new char *[custom_argc];

  custom_argv[0] = strdup(product_name.c_str());
  for (int i = 0; i < static_cast<int>(flagfile_arguments.size()); ++i) {
    custom_argv[i + 1] = strdup(flagfile_arguments[i].c_str());
  }

  // setup flags from config flags
  gflags::ParseCommandLineFlags(&custom_argc, &custom_argv, false);

  // unconsumed arguments have to be freed to avoid memory leak since they are
  // strdup-ed.
  for (int i = 0; i < custom_argc; ++i) free(custom_argv[i]);
  delete[] custom_argv;
}
