cmake_minimum_required(VERSION 3.22)

project(MetaParser LANGUAGES CSharp)

set(CMAKE_CSharp_FLAGS "${CMAKE_CSharp_FLAGS} /langversion:latest")

set(CMAKE_DOTNET_TARGET_FRAMEWORK net8.0)
set(CMAKE_DOTNET_SDK "Microsoft.NET.Sdk")

add_subdirectory(ThirdParty)
add_subdirectory(src)