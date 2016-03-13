#include <iostream>
#include <cstdlib>
#include <vector>

#include "debug/log.hpp"
#include "benchmark.hpp"

void help()
{
    std::cout << "error: too few arguments." << std::endl
              << "usage: host port threads connections duration[s]"
              << std::endl;

    std::exit(0);
}

int main(int argc, char* argv[])
{
    if(argc < 6)
        help();

    auto host        = std::string(argv[1]);
    auto port        = std::string(argv[2]);
    auto threads     = std::stoi(argv[3]);
    auto connections = std::stoi(argv[4]);
    auto duration    = std::stod(argv[5]);

    std::vector<std::string> queries {
        "CREATE (n{id:@}) RETURN n",
        "MATCH (n{id:#}),(m{id:#}) CREATE (n)-[r:test]->(m) RETURN r",
        "MATCH (n{id:#}) SET n.prop = ^ RETURN n",
        "MATCH (n{id:#})-[r]->(m) RETURN count(r)"
    };

    std::cout << "Running queries on " << connections << " connections "
              << "using " << threads << " threads "
              << "for " << duration << " seconds." << std::endl
              << "..." << std::endl;

    auto result = benchmark(host, port, threads, connections,
                            std::chrono::duration<double>(duration), queries);

    auto& reqs = result.requests;
    auto elapsed = result.elapsed.count();

    auto total = std::accumulate(reqs.begin(), reqs.end(), 0.0,
        [](auto acc, auto x) { return acc + x; }
    );

    std::cout << "Total of " << total << " requests in "
              << elapsed  << "s (" << int(total / elapsed) << " req/s)."
              << std::endl;

    for(size_t i = 0; i < queries.size(); ++i)
        std::cout << queries[i] << " => "
                  << int(reqs[i] / elapsed) << " req/s." << std::endl;

    return 0;
}
