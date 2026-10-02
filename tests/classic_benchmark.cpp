// Short and full timing modes use exactly the same linked VM binary.
#include <filesystem>
#include <cstdio>
#include <vector>
#include <unistd.h>
int main(int argc,char** argv) {
    const auto binary=(std::filesystem::path(argv[0]).parent_path()/"performance-benchmark").string();
    std::vector<char*> args;args.push_back(const_cast<char*>(binary.c_str()));
    if(argc<2||(std::string(argv[1])!="--paired-case"&&std::string(argv[1])!="--verify-classic"))
        args.push_back(const_cast<char*>("--classic-short"));
    for(int i=1;i<argc;++i)args.push_back(argv[i]);
    args.push_back(nullptr);execv(binary.c_str(),args.data());
    std::perror("performance-benchmark");return 1;
}

