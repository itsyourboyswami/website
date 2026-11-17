// p3cli - command line front-end for the 3P engine (also used for testing)
//   p3cli c <file> [outdir] [-t threads] [-v]     compress     -> <name>.3p
//   p3cli x <file.3p> [outdir] [-t threads]       extract      -> original name
//   p3cli i <file.3p>                              show stored metadata
#include "engine3p.hpp"
#include <chrono>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif
using namespace p3;
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: p3cli c|x|i <file> [outdir] [-t threads] [-v]\n"); return 2; }
    std::string mode = argv[1]; fs::path file = argv[2]; Options o; bool quiet = false;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-t" && i + 1 < argc) o.threads = (unsigned)atoi(argv[++i]);
        else if (a == "-v") o.verify = true;
        else if (a == "-q") quiet = true;
        else o.outDir = a;
    }
    if (mode == "i") {
        Result r = inspect_file(file);
        if (r.status != Status::Ok) { std::printf("error: %s\n", r.message.c_str()); return 1; }
        std::printf("name=%s original=%llu packed=%llu\n", r.storedName.c_str(), (unsigned long long)r.outSize, (unsigned long long)r.inSize); return 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    ProgressFn pf = quiet ? ProgressFn() : ProgressFn([&](double f, u64 out) { std::fprintf(stderr, "\r%5.1f%%  out %llu   ", f * 100, (unsigned long long)out); return true; });
    Result r = mode == "c" ? compress_file(file, o, pf) : restore_file(file, o, pf);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!quiet) std::fprintf(stderr, "\n");
    if (r.status != Status::Ok) { std::printf("error(%d): %s\n", (int)r.status, r.message.c_str()); return 1; }
    long rss = 0;
#ifndef _WIN32
    struct rusage ru; getrusage(RUSAGE_SELF, &ru); rss = ru.ru_maxrss / 1024;
#endif
    std::printf("%s -> %s  in=%llu out=%llu  %.2fs  %.2f MB/s  peakRAM=%ldMB\n", file.string().c_str(), r.outPath.string().c_str(),
                (unsigned long long)r.inSize, (unsigned long long)r.outSize, s, (double)(mode == "c" ? r.inSize : r.outSize) / 1e6 / (s > 0 ? s : 1), rss);
    return 0;
}
