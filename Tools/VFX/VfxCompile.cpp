#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <algorithm>

#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <array>

int main(int argc, char** argv)
{
    using namespace NLS::VFX;
    const std::string command = argc > 1 ? argv[1] : "";
    const bool libraryNew = command == "new-module" || command == "new-function" || command == "new-dynamic-input" || command == "new-emitter";
    const bool generateModules = command == "generate-modules";
    if ((!generateModules && argc != 4 && argc != 6) || (command != "compile" && command != "new" && !libraryNew && !generateModules))
    {
        std::cerr << "Usage: NullusVfxCompile compile <source.vfx> <compiled.json> [--backend dx12|vulkan|metal|all]\n"
                     "       NullusVfxCompile new <source.vfx> <name>\n"
                     "       NullusVfxCompile new-module|new-function|new-dynamic-input <source.vfxscript> <name>\n"
                     "       NullusVfxCompile new-emitter <source.vfxemitter> <name>\n"
                     "       NullusVfxCompile generate-modules <engine-modules-dir>\n";
        return 2;
    }
    std::string error;
    if (generateModules)
    {
        if (argc != 3) return 2;
        const std::filesystem::path root = argv[2];
        const auto sanitize = [](std::string value)
        {
            for (auto& character : value)
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != ' ' && character != '-' && character != '_')
                    character = '_';
            return value;
        };
        size_t written = 0, failures = 0, graphModules = 0, policyModules = 0;
        for (const auto& definition : Authoring::ModuleLibrary())
        {
            const auto directory = root/sanitize(definition.category.empty() ? "Uncategorized" : definition.category);
            std::error_code ignored; std::filesystem::create_directories(directory,ignored);
            const auto path = directory/(sanitize(definition.name)+".vfxscript");
            // Capture the code lowering as a module graph. Emitter-policy
            // modules (spawning, events, collision settings) have no graph
            // form and keep the native fallback.
            const auto sameGraph = [](const Graph& left,const Graph& right)
            {
                if (left.nodes.size() != right.nodes.size()) return false;
                for (size_t i = 0; i < left.nodes.size(); ++i)
                {
                    const auto& a = left.nodes[i]; const auto& b = right.nodes[i];
                    if (a.id != b.id || a.operation != b.operation || a.type != b.type ||
                        a.inputs != b.inputs || a.name != b.name || a.value != b.value) return false;
                }
                return true;
            };
            Graph graph; std::string graphError;
            bool captured = Authoring::BuildModuleGraph(Authoring::CreateModule(definition.id),graph,graphError,definition.stage);
            std::vector<Authoring::StageGraph> stageGraphs;
            if (captured)
            {
                std::vector<Authoring::Stage> capturedStages{definition.stage};
                for (const auto stage : definition.additionalStages)
                {
                    // Only particle stacks can select a stage-specific graph;
                    // simulation stages use the primary graph.
                    if (stage != Authoring::Stage::ParticleSpawn && stage != Authoring::Stage::ParticleUpdate) continue;
                    if (std::find(capturedStages.begin(),capturedStages.end(),stage) != capturedStages.end()) continue;
                    capturedStages.push_back(stage);
                    Graph staged; std::string stagedError;
                    if (!Authoring::BuildModuleGraph(Authoring::CreateModule(definition.id),staged,stagedError,stage))
                    { captured = false; graphError = stagedError; break; }
                    // Only store graphs that differ from the primary one.
                    if (!sameGraph(graph,staged)) stageGraphs.push_back({stage,std::move(staged)});
                }
            }
            if (captured) ++graphModules;
            else
            {
                ++policyModules;
                std::cerr << "Native-only module " << definition.id << ": " << graphError << "\n";
            }
            auto asset = captured ? Authoring::CreateEngineModuleAsset(definition,graph) : Authoring::CreateEngineModuleAsset(definition);
            if (captured && !stageGraphs.empty()) asset.versions.front().stageGraphs = std::move(stageGraphs);
            if (!Authoring::SaveScript(path,asset,error))
            { std::cerr << path.string() << ": " << error << "\n"; ++failures; continue; }
            ++written;
        }
        std::cout << "Wrote " << written << " engine module assets to " << root.string()
                  << " (" << graphModules << " graph, " << policyModules << " native-only)\n";
        if (failures) std::cerr << failures << " assets failed\n";
        return failures ? 1 : 0;
    }
    if (libraryNew)
    {
        if (argc != 4) return 2;
        const auto kind = command == "new-function" ? Authoring::AssetKind::Function : command == "new-dynamic-input" ? Authoring::AssetKind::DynamicInput : Authoring::AssetKind::Module;
        const bool saved = command == "new-emitter" ? Authoring::SaveEmitterSource(argv[2],Authoring::CreateEmitterSource(argv[3]),error) : Authoring::SaveScript(argv[2],Authoring::CreateScript(kind,argv[3]),error);
        if (!saved) { std::cerr << error << "\n"; return 1; } return 0;
    }
    if (command == "new")
    {
        if (argc != 4) return 2;
        if (Authoring::Save(argv[2],Authoring::CreateSystem(argv[3]),error)) return 0;
        std::cerr << error << "\n"; return 1;
    }
    if (std::filesystem::absolute(argv[2]).lexically_normal() == std::filesystem::absolute(argv[3]).lexically_normal())
    { std::cerr << "Compiled output must have a different path from its source\n"; return 2; }
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    {
        Authoring::System source; std::vector<Authoring::Issue> issues;
        if (!Authoring::Load(argv[2],source,error)) { std::cerr << error << "\n"; return 1; }
        Authoring::SourceLibrary library; const std::array roots{std::filesystem::absolute(argv[2]).parent_path()};
        if (!library.Refresh(roots,error)) { std::cerr << error << "\n"; return 1; }
        std::vector<Authoring::SourceDependency> dependencies;
        if (!Authoring::Compile(source,library,compiled,dependencies,issues))
        {
            for (const auto& issue : issues) std::cerr << issue.emitter << ":" << issue.stack << ":" << issue.module << ":" << issue.input << ": " << issue.message << "\n";
            return 1;
        }
    }
    if (argc == 6)
    {
        namespace rhi = NLS::Render::RHI;
        const std::string target = argv[5];
        if (std::string(argv[4]) != "--backend" || (target != "dx12" && target != "vulkan" && target != "metal" && target != "all"))
        { std::cerr << "Invalid particle backend option\n"; return 2; }
        std::vector<rhi::NativeBackendType> backends;
        if (target == "dx12" || target == "all") backends.push_back(rhi::NativeBackendType::DX12);
        if (target == "vulkan" || target == "all") backends.push_back(rhi::NativeBackendType::Vulkan);
        if (target == "metal" || target == "all") backends.push_back(rhi::NativeBackendType::Metal);
        if (!CompilePlatformPrograms(compiled,backends,std::filesystem::path(argv[3]).parent_path()/"VfxShaderCache",diagnostics))
        { for (const auto& diagnostic : diagnostics) std::cerr << diagnostic.message << "\n"; return 1; }
    }
    const auto source = SerializeCompiledSystem(compiled,argc == 4);
    std::ofstream output(argv[3],std::ios::binary|std::ios::trunc);
    output.write(source.data(),static_cast<std::streamsize>(source.size()));
    output.close();
    if (!output) { std::cerr << "Cannot write compiled VFX output\n"; return 1; }
    return 0;
}
