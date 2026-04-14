#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

#include "Debug/FileHandler.h"

int main(int argc, char** argv)
{
    const auto logsDirectory = std::filesystem::path(NLS_BUILD_DIR) / "Logs";
    std::error_code errorCode;
    std::filesystem::create_directories(logsDirectory, errorCode);
    NLS::Debug::FileHandler::SetLogFilePath(logsDirectory.generic_string());

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
