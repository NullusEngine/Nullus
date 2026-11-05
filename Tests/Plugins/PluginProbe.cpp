#include "Plugins/RuntimeDeployment.h"
int main(int argc, char** argv)
{
    return argc == 2 ? NLS::Plugins::RunRuntimePluginSelfTest(std::filesystem::absolute(argv[1])) : 2;
}
