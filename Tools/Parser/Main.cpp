/* ----------------------------------------------------------------------------
** Copyright (c) 2016 Austin Brunkhorst, All Rights Reserved.
** Copyright (c) 2024 Fredrik A. Kristiansen, All Rights Reserved.
**
** Main.cpp
** --------------------------------------------------------------------------*/

#include "Precompiled.h"

#include "ReflectionOptions.h"
#include "ReflectionParser.h"

#include "Switches.h"

#include <chrono>
#include <fstream>
#include <string>

int main(int argc, char *argv[])
{
    for(int i = 0; i < argc; i++)
    {
        std::cout << argv[i] << std::endl;
    }


    auto start = std::chrono::system_clock::now( );
    
    po::parser parser;

    try {
        auto exeDir = fs::path( argv[ 0 ] ).parent_path( );
        if (!exeDir.empty( ))
            fs::current_path( exeDir );

        auto& help = parser["help"]
            .abbreviation('?')
            .description("print this help screen");

        auto& targetName = parser["target-name"]
            .abbreviation('t')
            .description("Input target project name.")
            .type(po::string);

        auto& sourceRoot = parser["source-root"]
            .abbreviation('r')
            .description("Root source directory that is shared by all header files.")
            .type(po::string);

        auto& inputSource = parser["in-source"]
            .abbreviation('i')
            .description("Source file (header) to compile reflection data from.")
            .type(po::string);

        auto& moduleHeaderFile = parser["module-header"]
            .abbreviation('m')
            .description("Header file that declares this reflection module.")
            .type(po::string);

        auto& outputModuleSource = parser["out-source"]
            .abbreviation('s')
            .description("Output generated C++ module source file.")
            .type(po::string);

        auto& outputModuleFileDirectory = parser["out-dir"]
            .abbreviation('c')
            .description("Output directory for generated C++ module file, header/source files.")
            .type(po::string);

        auto& templateDirectory = parser["tmpl-directory"]
            .abbreviation('d')
            .description("Directory that contains the mustache templates.")
            .type(po::string)
            .fallback("Templates/");

        auto& precompiledHeader = parser["pch"]
            .abbreviation('p')
            .description("Optional name of the precompiled header file for the project.")
            .type(po::string);

        auto& forceRebuild = parser["force-rebuild"]
            .abbreviation('e')
            .description("Whether or not to ignore cache and write the header / source files.");

        auto& displayDiagnostics = parser["display-diagnostics"]
            .abbreviation('o')
            .description("Whether or not to display diagnostics from clang.");

        auto& compilerIncludes = parser["includes"]
            .abbreviation('f')
            .description("Optional file that includes the include directories for this target.")
            .type(po::string);

        auto& compilerDefines = parser["defines"]
            .abbreviation('x')
            .description("Optional list of definitions to include for the compiler.")
            .type(po::string)
            .multi();

        if(!parser(argc, argv))
            return 1;

        if(help.was_set()) {
            std::cout << parser << '\n';
            return 0;
        }

        ReflectionOptions options;
        options.forceRebuild = true; //forceRebuild.was_set();
        options.displayDiagnostics = displayDiagnostics.was_set();
        options.targetName = targetName.get().string;
        options.sourceRoot = sourceRoot.get().string;
        options.inputSourceFile = inputSource.get().string;
        options.moduleHeaderFile = moduleHeaderFile.get().string;
        options.outputModuleSource = outputModuleSource.get().string;
        options.outputModuleFileDirectory = outputModuleFileDirectory.get().string;
        options.templateDirectory = templateDirectory.get().string;
        options.precompiledHeader = precompiledHeader.was_set() ? precompiledHeader.get().string : std::string();

        // default arguments
        options.arguments =
        { {
            "-x",
            "c++",
            "-std=c++17",
            "-D__REFLECTION_PARSER__"
        } };
        
        if (compilerIncludes.count() > 0)
        {
            auto includes = 
                compilerIncludes.get().string;

            std::ifstream includesFile( includes );

            std::string include;

            while (std::getline( includesFile, include ))
                options.arguments.emplace_back( "-I"+ include );
        }

        if (compilerDefines.count() > 0)
        {
            for (auto &define : compilerDefines)
                options.arguments.emplace_back("-D" + define.string);
        }
        
        std::cout << std::endl;
        std::cout << "Parsing reflection data for target \"" 
                << options.targetName << "\"" 
                << std::endl;

        ReflectionParser parser( options );

        parser.Parse( );

        try
        {
            parser.GenerateFiles( );
        }
        catch (std::exception &e)
        {
            utils::FatalError( e.what( ) );
        }
    }
    catch (std::exception &e)
    {
        utils::FatalError( e.what( ) );
    }
    catch (...) 
    {
        utils::FatalError( "Unhandled exception occurred!" );
    }

    auto duration = std::chrono::system_clock::now( ) - start;

    std::cout << "Completed in " 
              << std::chrono::duration_cast<std::chrono::milliseconds>( duration ).count( ) 
              << "ms" << std::endl;

    return EXIT_SUCCESS;
}