#ifndef MEMGRAPH_UTILS_COMMAND_LINE_ARGUMENTS_HPP
#define MEMGRAPH_UTILS_COMMAND_LINE_ARGUMENTS_HPP

#include <string>
#include <vector>
#include <algorithm>

namespace
{

using std::string;
using std::vector;
using vector_str = vector<string>;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

decltype(auto) all_arguments(int argc, char *argv[])
{
    vector_str args(argv + 1, argv + argc);
    return args;
}

decltype(auto) get_argument(const vector_str& all,
                            const std::string& flag,
                            const std::string& default_value)
{
    // TODO: optimize this implementation
    auto it = std::find(all.begin(), all.end(), flag);
    if (it == all.end()) {
        return default_value;
    }
    auto pos = std::distance(all.begin(), it);
    return all[pos + 1];
}

#pragma clang diagnostic pop

}

#endif
