// vegmodel: the same as "reno_cli model" without Qt (see src/cli/modeldump.h), for the engine-only test builds.
#include "../../src/cli/modeldump.cpp"
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    return reno::runModelCommand(args);
}
