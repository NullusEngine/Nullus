"""Exercise real package intents, CMake regeneration and SDK link isolation."""
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

repo, tool, cmake = map(Path, sys.argv[1:4])
work_parent = Path(sys.argv[4]); work_parent.mkdir(parents=True, exist_ok=True)
host_library, zstd_library, mode = sys.argv[5:8]

def run(args, success=True):
    result = subprocess.run([str(arg) for arg in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    if (result.returncode == 0) != success:
        raise RuntimeError("Command failed expectation: " + str(args) + "\n" + result.stdout[-8000:])
    return result.stdout

with tempfile.TemporaryDirectory(prefix="plugin-cmake-", dir=work_parent) as temporary:
    root = Path(temporary); project = root / "Project"; source = root / "Source"; source.mkdir()
    data = dict(schemaVersion=1, id="org.fixture.source", name="Source", version="1.0.0", engineVersion="^1.0.0", pluginApiVersion=1,
                modules=[dict(name="Runtime", type="Runtime", loadingPhase="PostEngine", sourceTarget="FixtureSource")])
    (source / "plugin.json").write_text(json.dumps(data))
    (source / "Source.cpp").write_text('''#include "Source.h"
struct Module : NLS::Core::Modules::IModule {
 NLS::Core::Modules::RegistrationToken token;
 bool Startup(NLS::Core::Modules::ModuleContext& context) override {
  token = context.extensions.Register(context.scope,{"fixture.source","runtime.play",0},[](auto& call) { call.value = 42; }); return bool(token);
 }
 bool IsStopped() const noexcept override { return true; }
 void Shutdown() noexcept override { token.Reset(); }
};
std::unique_ptr<NLS::Core::Modules::IModule> FixtureFactory() { return std::make_unique<Module>(); }
''')
    (source / "Source.h").write_text('#pragma once\n#include "Core/Modules/ModuleSystem.h"\nstd::unique_ptr<NLS::Core::Modules::IModule> FixtureFactory();\n')
    declaration = 'add_library(FixtureSource STATIC Source.cpp)\ntarget_link_libraries(FixtureSource PRIVATE FixtureHost)\ntarget_include_directories(FixtureSource PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")\nset_target_properties(FixtureSource PROPERTIES NLS_MODULE_HEADER Source.h NLS_MODULE_FACTORY FixtureFactory)\n'
    declaration += f'execute_process(COMMAND "{Path(sys.executable).as_posix()}" "${{CMAKE_CURRENT_SOURCE_DIR}}/build.py" "${{CMAKE_BINARY_DIR}}/approval-script.txt" COMMAND_ERROR_IS_FATAL ANY)\n'
    (source / "build.py").write_text('from pathlib import Path\nimport sys\nPath(sys.argv[1]).write_text("trusted")\n')
    (source / "CMakeLists.txt").write_text(declaration)
    harness = root / "Harness"; harness.mkdir()
    (harness / "main.cpp").write_text('''#include "Plugins/PluginManager.h"
#include "Plugins/SourcePluginRegistry.h"
int main(int count,char** arguments) {
 NLS::Plugins::PluginHost host(NLS::Core::Modules::HostKind::Game);
 NLS::Plugins::PluginManager manager(host,NLS::Core::Modules::HostKind::Game);
 NLS::Plugins::RegisterCompiledSourcePlugins(manager);
 if (count != 2 || !manager.OpenProject(arguments[1]) || !manager.Advance(NLS::Core::Modules::LoadingPhase::PostEngine)) return 1;
 auto extension = host.Extensions().Find("fixture.source");
 NLS::Core::Modules::ExtensionCall call; return extension && extension.Invoke(call) && call.value == 42 ? 0 : 2;
}
''')
    (harness / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.22)
project(PluginInputs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_SUPPRESS_REGENERATION OFF)
add_library(FixtureHost UNKNOWN IMPORTED)
set_target_properties(FixtureHost PROPERTIES IMPORTED_LOCATION "{Path(host_library).as_posix()}"
 INTERFACE_INCLUDE_DIRECTORIES "{repo.as_posix()}/Runtime;{repo.as_posix()}/Runtime/Base;{repo.as_posix()}/Runtime/Core;{repo.as_posix()}/Runtime/Platform;{repo.as_posix()}/ThirdParty"
 INTERFACE_LINK_LIBRARIES "{Path(zstd_library).as_posix()}")
if("{mode}" STREQUAL "Shared")
 set_property(TARGET FixtureHost PROPERTY INTERFACE_COMPILE_DEFINITIONS NLS_SHARED_LIB)
endif()
set(NLS_ROOT_DIR "{repo.as_posix()}")
set(NLS_PLUGIN_PROJECT_ROOT "{project.as_posix()}")
include("{repo.as_posix()}/Tools/CMake/NullusPlugins.cmake")
nls_prepare_source_plugins()
add_executable(Game main.cpp)
target_link_libraries(Game PRIVATE FixtureHost)
nls_attach_source_plugins(Game Game)
''')
    build = root / "Build"
    run([tool, project, "install-dir", source]); run([tool, project, "enable", data["id"]])
    # Regeneration must never execute a newly installed, unapproved CMake file.
    run([cmake, "-S", harness, "-B", build], False)
    assert not (build / "approval-script.txt").exists()
    run([tool, project, "approve-build", data["id"]]); run([cmake, "-S", harness, "-B", build])
    assert (build / "approval-script.txt").read_text() == "trusted"
    registry = build / "Game.SourcePlugins.cpp"
    assert 'AddSourceFactory("FixtureSource"' in registry.read_text()
    run([cmake, "--build", build, "--config", "Debug", "--target", "Game"])
    game = build / "Debug/Game.exe" if sys.platform == "win32" else build / "Game"
    if mode == "Shared":
        libraries = [library for library in tool.parent.glob("*NullusPluginHostShared*")
                     if library.suffix.lower() in (".dll", ".so", ".dylib")]
        if not libraries:
            raise RuntimeError("Shared fixture runtime was not found beside " + str(tool))
        for library in libraries:
            shutil.copy2(library, game.parent / library.name)
    run([game, project])
    # A Lockfile edit triggers ZERO_CHECK and a stale-snapshot error.
    lock = project / "Packages/packages-lock.json"; lock.write_text(lock.read_text() + "\n")
    run([cmake, "--build", build, "--config", "Debug", "--target", "Game"], False)
    run([tool, project, "resolve"]); run([cmake, "-S", harness, "-B", build])
    run([tool, project, "disable", data["id"]]); run([cmake, "--build", build, "--config", "Debug", "--target", "ZERO_CHECK"])
    assert 'AddSourceFactory("FixtureSource"' not in registry.read_text()
    run([tool, project, "enable", data["id"]]); run([cmake, "--build", build, "--config", "Debug", "--target", "ZERO_CHECK"])
    assert 'AddSourceFactory("FixtureSource"' in registry.read_text()
    other_source = root / "OtherSource"; shutil.copytree(source, other_source)
    run([tool, project, "install-dir", other_source]); run([cmake, "-S", harness, "-B", build], False)
    run([tool, project, "approve-build", data["id"]]); run([cmake, "-S", harness, "-B", build])
    # Same origin, version and CMakeLists: an indirectly executed Python file
    # still requires fresh consent, and ZERO_CHECK must not execute it first.
    (other_source / "build.py").write_text('from pathlib import Path\nimport sys\nPath(sys.argv[1]).write_text("changed")\n')
    run([tool, project, "install-dir", other_source])
    run([cmake, "--build", build, "--config", "Debug", "--target", "ZERO_CHECK"], False)
    assert (build / "approval-script.txt").read_text() == "trusted"
    run([tool, project, "approve-build", data["id"]])
    run([cmake, "--build", build, "--config", "Debug", "--target", "ZERO_CHECK"])
    assert (build / "approval-script.txt").read_text() == "changed"
    run([game, project], False)  # Previous static factory now has stale inputs.
    run([cmake, "--build", build, "--config", "Debug", "--target", "Game"])
    run([game, project])
    # Update a build declaration without changing semantic version.
    (source / "CMakeLists.txt").write_text(declaration + "target_compile_definitions(FixtureSource PRIVATE UPDATED=1)\n")
    run([tool, project, "install-dir", source]); run([cmake, "-S", harness, "-B", build], False)
    run([tool, project, "approve-build", data["id"]]); run([cmake, "-S", harness, "-B", build])
    run([tool, project, "disable", data["id"]]); run([tool, project, "uninstall", data["id"]])
    run([cmake, "--build", build, "--config", "Debug", "--target", "ZERO_CHECK"])
    assert 'AddSourceFactory("FixtureSource"' not in registry.read_text()
    bad = root / "BadSDK"; bad.mkdir(); (bad / "bad.cpp").write_text("int bad() { return 0; }\n")
    (bad / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.22)
project(BadSDK LANGUAGES CXX)
add_library(NullusPluginSDK INTERFACE)
add_library(NLS_Core STATIC bad.cpp)
include("{repo.as_posix()}/Runtime/Plugins/SDK/NullusPluginSDK.cmake")
nullus_add_binary_plugin(BadPlugin bad.cpp)
target_link_libraries(BadPlugin PRIVATE NLS_Core)
''')
    run([cmake, "-S", bad, "-B", root / "BadBuild"], False)
print("CMake inputs, source selection and binary SDK isolation: passed")
