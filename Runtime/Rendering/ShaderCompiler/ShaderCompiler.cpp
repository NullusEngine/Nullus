#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#include <stdio.h>
#endif

namespace NLS::Render::ShaderCompiler
{
	namespace
	{
		using ShaderReflection = Resources::ShaderReflection;
		using ShaderPropertyDesc = Resources::ShaderPropertyDesc;
		using ShaderConstantBufferDesc = Resources::ShaderConstantBufferDesc;
		using ShaderCBufferMemberDesc = Resources::ShaderCBufferMemberDesc;
		using ShaderResourceKind = Resources::ShaderResourceKind;
		using UniformType = Resources::UniformType;

		struct ProcessResult
		{
			int exitCode = -1;
			std::string output;
		};

		std::string QuoteCommandArgument(const std::string& value)
		{
			if (value.empty())
				return "\"\"";

			const bool needsQuotes = value.find_first_of(" \t\"") != std::string::npos;
			if (!needsQuotes)
				return value;

			std::string quoted = "\"";
			for (const char ch : value)
			{
				if (ch == '"')
					quoted += "\\\"";
				else
					quoted += ch;
			}
			quoted += "\"";
			return quoted;
		}

		std::string Trim(std::string value)
		{
			const auto begin = value.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};

			const auto end = value.find_last_not_of(" \t\r\n");
			return value.substr(begin, end - begin + 1);
		}

		std::string StripCommentPrefix(const std::string& line)
		{
			if (!line.empty() && line[0] == ';')
				return Trim(line.substr(1));
			return Trim(line);
		}

		std::string Quote(const std::filesystem::path& path)
		{
			return "\"" + path.string() + "\"";
		}

		std::string Quote(const std::string& value)
		{
			return "\"" + value + "\"";
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return {};

			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;

			stream << content;
			return static_cast<bool>(stream);
		}

		std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return {};

			return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		uint64_t HashStringFNV1a(const std::string& value)
		{
			uint64_t hash = 1469598103934665603ull;
			for (const unsigned char c : value)
			{
				hash ^= static_cast<uint64_t>(c);
				hash *= 1099511628211ull;
			}

			return hash;
		}

		std::string ToHex(uint64_t value)
		{
			std::ostringstream stream;
			stream << std::hex << value;
			return stream.str();
		}

		std::string StageToProfilePrefix(ShaderStage stage)
		{
			switch (stage)
			{
			case ShaderStage::Vertex: return "vs";
			case ShaderStage::Pixel: return "ps";
			case ShaderStage::Compute: return "cs";
			default: return "unknown";
			}
		}

		std::string StageToString(ShaderStage stage)
		{
			switch (stage)
			{
			case ShaderStage::Vertex: return "vertex";
			case ShaderStage::Pixel: return "pixel";
			case ShaderStage::Compute: return "compute";
			default: return "unknown";
			}
		}

		std::string TargetPlatformToString(ShaderTargetPlatform platform)
		{
			switch (platform)
			{
			case ShaderTargetPlatform::DXIL: return "dxil";
			case ShaderTargetPlatform::SPIRV: return "spirv";
			case ShaderTargetPlatform::GLSL: return "glsl";
			default: return "unknown";
			}
		}

		std::optional<std::filesystem::path> FindDxcExecutable()
		{
			if (const char* envPath = std::getenv("DXC_PATH"); envPath != nullptr && *envPath != '\0')
			{
				const std::filesystem::path path(envPath);
				if (std::filesystem::exists(path))
					return path;
			}

			const auto findSdkDxc = [](const std::filesystem::path& sdkRoot) -> std::optional<std::filesystem::path>
			{
				const std::filesystem::path candidates[] =
				{
					sdkRoot / "Bin/dxc.exe",
					sdkRoot / "Bin/x64/dxc.exe"
				};

				for (const auto& candidate : candidates)
				{
					if (std::filesystem::exists(candidate))
						return candidate;
				}

				return std::nullopt;
			};

			if (const char* envPath = std::getenv("VULKAN_SDK"); envPath != nullptr && *envPath != '\0')
			{
				if (const auto sdkDxc = findSdkDxc(std::filesystem::path(envPath)); sdkDxc.has_value())
					return sdkDxc;
			}

			if (const char* envPath = std::getenv("VK_SDK_PATH"); envPath != nullptr && *envPath != '\0')
			{
				if (const auto sdkDxc = findSdkDxc(std::filesystem::path(envPath)); sdkDxc.has_value())
					return sdkDxc;
			}

			const auto findBundledDxc = [](const std::filesystem::path& root) -> std::optional<std::filesystem::path>
			{
				const std::filesystem::path directCandidates[] =
				{
					root / "Tools/DXC/bin/x64/dxc.exe",
					root / "Tools/DXC/bin/arm64/dxc.exe",
					root / "ThirdParty/DirectXShaderCompiler/bin/x64/dxc.exe"
				};

				for (const auto& candidate : directCandidates)
				{
					if (std::filesystem::exists(candidate))
						return candidate;
				}

				const auto versionedToolsRoot = root / "Tools/DXC";
				if (!std::filesystem::exists(versionedToolsRoot))
					return std::nullopt;

				std::vector<std::filesystem::path> versionDirectories;
				for (const auto& entry : std::filesystem::directory_iterator(versionedToolsRoot))
				{
					if (entry.is_directory())
						versionDirectories.push_back(entry.path());
				}

				std::sort(versionDirectories.begin(), versionDirectories.end(), std::greater<>());
				for (const auto& versionDirectory : versionDirectories)
				{
					const auto candidate = versionDirectory / "bin/x64/dxc.exe";
					if (std::filesystem::exists(candidate))
						return candidate;
				}

				return std::nullopt;
			};

			for (auto probe = std::filesystem::current_path(); !probe.empty(); probe = probe.parent_path())
			{
				if (const auto bundledPath = findBundledDxc(probe); bundledPath.has_value())
					return bundledPath;

				if (probe == probe.parent_path())
					break;
			}

#if defined(_WIN32)
			const char* programFilesX86 = std::getenv("ProgramFiles(x86)");
			if (programFilesX86 != nullptr)
			{
				const std::filesystem::path root(programFilesX86);
				const std::filesystem::path candidates[] =
				{
					root / "Windows Kits/10/Redist/D3D/x64/dxc.exe",
					root / "Windows Kits/10/bin/10.0.22621.0/x64/dxc.exe",
					root / "Windows Kits/10/bin/x64/dxc.exe",
					root / "Windows Kits/10/bin/10.0.22621.0/x86/dxc.exe",
					root / "Windows Kits/10/bin/x86/dxc.exe"
				};

				for (const auto& candidate : candidates)
				{
					if (std::filesystem::exists(candidate))
						return candidate;
				}
			}
#endif

			return std::nullopt;
		}

		std::filesystem::path GetShaderCacheDirectory()
		{
#if defined(_WIN32)
			if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr && *localAppData != '\0')
			{
				const auto path = std::filesystem::path(localAppData) / "Nullus" / "ShaderCache";
				std::filesystem::create_directories(path);
				return path;
			}
#endif
			const auto path = std::filesystem::temp_directory_path() / "NullusShaderCache";
			std::filesystem::create_directories(path);
			return path;
		}

		ProcessResult ExecuteCommand(const std::string& command)
		{
			ProcessResult result;

#if defined(_WIN32)
			SECURITY_ATTRIBUTES securityAttributes{};
			securityAttributes.nLength = sizeof(securityAttributes);
			securityAttributes.bInheritHandle = TRUE;

			HANDLE readPipe = nullptr;
			HANDLE writePipe = nullptr;
			if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0))
			{
				result.output = "Failed to create process pipe for command: " + command;
				return result;
			}

			SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOA startupInfo{};
			startupInfo.cb = sizeof(startupInfo);
			startupInfo.dwFlags = STARTF_USESTDHANDLES;
			startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startupInfo.hStdOutput = writePipe;
			startupInfo.hStdError = writePipe;

			PROCESS_INFORMATION processInfo{};
			std::vector<char> commandLine(command.begin(), command.end());
			commandLine.push_back('\0');

			const BOOL created = CreateProcessA(
				nullptr,
				commandLine.data(),
				nullptr,
				nullptr,
				TRUE,
				CREATE_NO_WINDOW,
				nullptr,
				nullptr,
				&startupInfo,
				&processInfo);

			CloseHandle(writePipe);

			if (!created)
			{
				const auto errorCode = GetLastError();
				result.output = "Failed to spawn process (" + std::to_string(errorCode) + "): " + command;
				CloseHandle(readPipe);
				return result;
			}

			char buffer[4096];
			DWORD bytesRead = 0;
			while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytesRead, nullptr) && bytesRead > 0)
			{
				buffer[bytesRead] = '\0';
				result.output += buffer;
			}

			WaitForSingleObject(processInfo.hProcess, INFINITE);

			DWORD exitCode = static_cast<DWORD>(-1);
			GetExitCodeProcess(processInfo.hProcess, &exitCode);
			result.exitCode = static_cast<int>(exitCode);

			CloseHandle(readPipe);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
#else
			(void)command;
			result.output = "Shader compiler process execution is only implemented on Windows.";
#endif

			return result;
		}

		ProcessResult ExecuteProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
		{
			ProcessResult result;

#if defined(_WIN32)
			SECURITY_ATTRIBUTES securityAttributes{};
			securityAttributes.nLength = sizeof(securityAttributes);
			securityAttributes.bInheritHandle = TRUE;

			HANDLE readPipe = nullptr;
			HANDLE writePipe = nullptr;
			if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0))
			{
				result.output = "Failed to create process pipe for executable: " + executable.string();
				return result;
			}

			SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOA startupInfo{};
			startupInfo.cb = sizeof(startupInfo);
			startupInfo.dwFlags = STARTF_USESTDHANDLES;
			startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startupInfo.hStdOutput = writePipe;
			startupInfo.hStdError = writePipe;

			std::ostringstream commandLineBuilder;
			commandLineBuilder << QuoteCommandArgument(executable.string());
			for (const auto& argument : arguments)
				commandLineBuilder << ' ' << QuoteCommandArgument(argument);

			std::string commandLine = commandLineBuilder.str();
			std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
			mutableCommandLine.push_back('\0');

			PROCESS_INFORMATION processInfo{};
			const std::string executableString = executable.string();
			const BOOL created = CreateProcessA(
				executableString.c_str(),
				mutableCommandLine.data(),
				nullptr,
				nullptr,
				TRUE,
				CREATE_NO_WINDOW,
				nullptr,
				nullptr,
				&startupInfo,
				&processInfo);

			CloseHandle(writePipe);

			if (!created)
			{
				const auto errorCode = GetLastError();
				result.output = "Failed to spawn process (" + std::to_string(errorCode) + "): " + commandLine;
				CloseHandle(readPipe);
				return result;
			}

			char buffer[4096];
			DWORD bytesRead = 0;
			while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytesRead, nullptr) && bytesRead > 0)
			{
				buffer[bytesRead] = '\0';
				result.output += buffer;
			}

			WaitForSingleObject(processInfo.hProcess, INFINITE);

			DWORD exitCode = static_cast<DWORD>(-1);
			GetExitCodeProcess(processInfo.hProcess, &exitCode);
			result.exitCode = static_cast<int>(exitCode);

			CloseHandle(readPipe);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
#else
			(void)executable;
			(void)arguments;
			result.output = "Shader compiler process execution is only implemented on Windows.";
#endif

			return result;
		}

		bool ResolveSourceDependencies(
			const std::filesystem::path& filePath,
			const std::vector<std::string>& includeDirectories,
			std::set<std::filesystem::path>& visited,
			std::vector<std::string>& dependencies,
			std::string& hashInput,
			std::string& diagnostics)
		{
			const auto absolutePath = std::filesystem::weakly_canonical(filePath);
			if (!visited.insert(absolutePath).second)
				return true;

			const auto source = ReadTextFile(absolutePath);
			if (source.empty())
			{
				diagnostics += "Failed to read shader source: " + absolutePath.string() + "\n";
				return false;
			}

			dependencies.push_back(absolutePath.string());
			hashInput += absolutePath.string();
			hashInput += '\n';
			hashInput += source;
			hashInput += '\n';

			static const std::regex includeRegex(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
			std::istringstream stream(source);
			std::string line;
			while (std::getline(stream, line))
			{
				std::smatch match;
				if (!std::regex_search(line, match, includeRegex))
					continue;

				const std::string includeName = match[1].str();
				std::optional<std::filesystem::path> resolvedInclude;

				const auto localCandidate = absolutePath.parent_path() / includeName;
				if (std::filesystem::exists(localCandidate))
					resolvedInclude = localCandidate;

				if (!resolvedInclude.has_value())
				{
					for (const auto& includeDirectory : includeDirectories)
					{
						const auto candidate = std::filesystem::path(includeDirectory) / includeName;
						if (std::filesystem::exists(candidate))
						{
							resolvedInclude = candidate;
							break;
						}
					}
				}

				if (!resolvedInclude.has_value())
				{
					diagnostics += "Failed to resolve include '" + includeName + "' from '" + absolutePath.string() + "'.\n";
					continue;
				}

				ResolveSourceDependencies(*resolvedInclude, includeDirectories, visited, dependencies, hashInput, diagnostics);
			}

			return true;
		}

		UniformType ParseUniformType(const std::string& typeToken)
		{
			std::string token = typeToken;
			token = Trim(token);
			token = std::regex_replace(token, std::regex(R"(\b(column_major|row_major|const|static)\b)"), "");
			token = Trim(token);

			if (token == "bool") return UniformType::UNIFORM_BOOL;
			if (token == "int" || token == "uint") return UniformType::UNIFORM_INT;
			if (token == "float") return UniformType::UNIFORM_FLOAT;
			if (token == "float2") return UniformType::UNIFORM_FLOAT_VEC2;
			if (token == "float3") return UniformType::UNIFORM_FLOAT_VEC3;
			if (token == "float4") return UniformType::UNIFORM_FLOAT_VEC4;
			if (token == "float4x4") return UniformType::UNIFORM_FLOAT_MAT4;
			return UniformType::UNIFORM_FLOAT;
		}

		uint32_t GetUniformTypeSize(UniformType type)
		{
			switch (type)
			{
			case UniformType::UNIFORM_BOOL:
			case UniformType::UNIFORM_INT:
			case UniformType::UNIFORM_FLOAT:
				return 4;
			case UniformType::UNIFORM_FLOAT_VEC2:
				return 8;
			case UniformType::UNIFORM_FLOAT_VEC3:
				return 12;
			case UniformType::UNIFORM_FLOAT_VEC4:
				return 16;
			case UniformType::UNIFORM_FLOAT_MAT4:
				return 64;
			case UniformType::UNIFORM_SAMPLER_2D:
			case UniformType::UNIFORM_SAMPLER_CUBE:
			default:
				return 0;
			}
		}

		struct BindingInfo
		{
			uint32_t index = 0;
			uint32_t space = 0;
		};

		std::optional<BindingInfo> ParseHlslBinding(const std::string& token)
		{
			static const std::regex bindingRegex(R"(([a-z]+)(\d+)(?:,space(\d+))?)", std::regex::icase);
			std::smatch match;
			if (!std::regex_match(token, match, bindingRegex))
				return std::nullopt;

			BindingInfo binding;
			binding.index = static_cast<uint32_t>(std::stoul(match[2].str()));
			binding.space = match[3].matched ? static_cast<uint32_t>(std::stoul(match[3].str())) : 0u;
			return binding;
		}

		ShaderResourceKind ParseResourceKind(const std::string& typeToken)
		{
			if (typeToken == "cbuffer")
				return ShaderResourceKind::UniformBuffer;
			if (typeToken == "sampler")
				return ShaderResourceKind::Sampler;
			if (typeToken == "texture")
				return ShaderResourceKind::SampledTexture;
			if (typeToken.find("StructuredBuffer") != std::string::npos)
				return ShaderResourceKind::StructuredBuffer;
			if (typeToken.rfind("RW", 0) == 0)
				return ShaderResourceKind::StorageBuffer;
			return ShaderResourceKind::Value;
		}

		UniformType ParseResourceUniformType(const std::string& typeToken, const std::string& dimToken)
		{
			if (typeToken == "texture")
			{
				if (dimToken == "cube")
					return UniformType::UNIFORM_SAMPLER_CUBE;
				return UniformType::UNIFORM_SAMPLER_2D;
			}

			return UniformType::UNIFORM_INT;
		}

		ShaderReflection ParseReflectionDump(const std::string& dumpText, ShaderStage stage)
		{
			ShaderReflection reflection;
			std::unordered_map<std::string, size_t> constantBufferIndexByName;

			enum class Section
			{
				None,
				BufferDefinitions,
				ResourceBindings
			};

			Section section = Section::None;
			ShaderConstantBufferDesc* currentBuffer = nullptr;

			static const std::regex memberRegex(R"(^(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?;\s*;\s*Offset:\s*(\d+))");
			static const std::regex bufferSizeRegex(R"(^\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;\s*;\s*Offset:\s*\d+\s+Size:\s*(\d+))");
			static const std::regex bindingLineRegex(R"(^(.+?)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s*$)");

			std::istringstream stream(dumpText);
			std::string rawLine;
			while (std::getline(stream, rawLine))
			{
				const auto line = StripCommentPrefix(rawLine);
				if (line.empty())
					continue;

				if (line == "Buffer Definitions:")
				{
					section = Section::BufferDefinitions;
					currentBuffer = nullptr;
					continue;
				}

				if (line == "Resource Bindings:")
				{
					section = Section::ResourceBindings;
					currentBuffer = nullptr;
					continue;
				}

				if (section == Section::BufferDefinitions)
				{
					if (line.rfind("cbuffer ", 0) == 0)
					{
						ShaderConstantBufferDesc buffer;
						buffer.name = Trim(line.substr(std::string("cbuffer ").size()));
						buffer.stage = stage;
						reflection.constantBuffers.push_back(buffer);
						constantBufferIndexByName[buffer.name] = reflection.constantBuffers.size() - 1;
						currentBuffer = &reflection.constantBuffers.back();
						continue;
					}

					if (currentBuffer == nullptr)
						continue;

					std::smatch match;
					if (std::regex_search(line, match, memberRegex))
					{
						ShaderCBufferMemberDesc member;
						member.type = ParseUniformType(match[1].str());
						member.name = match[2].str();
						if (member.name == currentBuffer->name)
							continue;
						member.arraySize = match[3].matched ? static_cast<uint32_t>(std::stoul(match[3].str())) : 1u;
						member.byteOffset = static_cast<uint32_t>(std::stoul(match[4].str()));
						member.byteSize = GetUniformTypeSize(member.type) * member.arraySize;
						currentBuffer->members.push_back(member);
						continue;
					}

					if (std::regex_search(line, match, bufferSizeRegex))
					{
						currentBuffer->byteSize = static_cast<uint32_t>(std::stoul(match[2].str()));
						currentBuffer = nullptr;
						continue;
					}
				}
				else if (section == Section::ResourceBindings)
				{
					if (line.rfind("---", 0) == 0 || line.rfind("Name", 0) == 0)
						continue;

					std::smatch match;
					if (!std::regex_match(line, match, bindingLineRegex))
						continue;

					const std::string resourceName = Trim(match[1].str());
					const std::string typeToken = match[2].str();
					const std::string dimToken = match[4].str();
					const std::string bindToken = match[6].str();
					const std::string countToken = match[7].str();

					const auto binding = ParseHlslBinding(bindToken);
					if (!binding.has_value())
						continue;

					if (typeToken == "cbuffer")
					{
						const auto found = constantBufferIndexByName.find(resourceName);
						if (found != constantBufferIndexByName.end())
						{
							auto& buffer = reflection.constantBuffers[found->second];
							buffer.bindingIndex = binding->index;
							buffer.bindingSpace = binding->space;
						}
						continue;
					}

					ShaderPropertyDesc property;
					property.name = resourceName;
					property.kind = ParseResourceKind(typeToken);
					property.type = ParseResourceUniformType(typeToken, dimToken);
					property.stage = stage;
					property.bindingIndex = binding->index;
					property.bindingSpace = binding->space;
					property.arraySize = static_cast<int32_t>(std::stoul(countToken));
					reflection.properties.push_back(property);
				}
			}

		for (auto& buffer : reflection.constantBuffers)
		{
			if (buffer.byteSize == 0 && !buffer.members.empty())
			{
				uint32_t computedSize = 0;
				for (const auto& member : buffer.members)
				{
					const auto memberEnd = member.byteOffset + member.byteSize;
					if (memberEnd > computedSize)
						computedSize = memberEnd;
				}

				constexpr uint32_t kCBufferAlignment = 16u;
				buffer.byteSize = (computedSize + (kCBufferAlignment - 1u)) & ~(kCBufferAlignment - 1u);
			}

			for (const auto& member : buffer.members)
			{
				ShaderPropertyDesc property;
					property.name = member.name;
					property.type = member.type;
					property.kind = ShaderResourceKind::Value;
					property.stage = buffer.stage;
					property.bindingSpace = buffer.bindingSpace;
					property.bindingIndex = buffer.bindingIndex;
					property.arraySize = static_cast<int32_t>(member.arraySize);
					property.byteOffset = member.byteOffset;
					property.byteSize = member.byteSize;
					property.parentConstantBuffer = buffer.name;
					reflection.properties.push_back(property);
				}
			}

			return reflection;
		}

		class NullShaderCompilerBackend final : public IShaderCompilerBackend
		{
		public:
			ShaderCompilationOutput Compile(const ShaderCompilationInput& input) override
			{
				ShaderCompilationOutput output;
				output.status = ShaderCompilationStatus::Failed;

				std::ostringstream stream;
				stream << "Shader compilation backend is not configured. "
					<< "Asset='" << input.assetPath << "', Stage=" << static_cast<int>(input.stage)
					<< ", EntryPoint='" << input.options.entryPoint << "'.";
				output.diagnostics = stream.str();
				return output;
			}

			ShaderReflection Reflect(const ShaderCompilationInput&) override
			{
				return {};
			}

			ShaderReflection Reflect(const ShaderCompilationInput&, const ShaderCompilationOutput&) override
			{
				return {};
			}

			const char* GetBackendName() const override
			{
				return "NullShaderCompilerBackend";
			}
		};

		class DxcShaderCompilerBackend final : public IShaderCompilerBackend
		{
		public:
			ShaderCompilationOutput Compile(const ShaderCompilationInput& input) override
			{
				ShaderCompilationOutput output;
				output.dependencyPaths.clear();

				const auto dxcPath = FindDxcExecutable();
				if (!dxcPath.has_value())
				{
					output.status = ShaderCompilationStatus::Failed;
					output.diagnostics = "Unable to locate dxc.exe. Set DXC_PATH or install the Windows 10 SDK.";
					return output;
				}

				const std::filesystem::path sourcePath = std::filesystem::weakly_canonical(std::filesystem::path(input.assetPath));
				if (!std::filesystem::exists(sourcePath))
				{
					output.status = ShaderCompilationStatus::Failed;
					output.diagnostics = "Shader source file does not exist: " + sourcePath.string();
					return output;
				}

				std::vector<std::string> includeDirectories = input.options.includeDirectories;
				includeDirectories.push_back(sourcePath.parent_path().string());

				std::set<std::filesystem::path> visited;
				std::string hashInput;
				std::string dependencyDiagnostics;
				ResolveSourceDependencies(sourcePath, includeDirectories, visited, output.dependencyPaths, hashInput, dependencyDiagnostics);

				hashInput += sourcePath.string();
				hashInput += input.options.entryPoint;
				hashInput += input.options.targetProfile;
				hashInput += TargetPlatformToString(input.options.targetPlatform);
				hashInput += StageToString(input.stage);
				hashInput += input.options.enableDebugInfo ? "debug" : "nodebug";
				hashInput += input.options.treatWarningsAsErrors ? "werror" : "nowerror";

				for (const auto& macro : input.options.macros)
				{
					hashInput += macro.name;
					hashInput += '=';
					hashInput += macro.value;
					hashInput += ';';
				}

				output.cacheKey = ToHex(HashStringFNV1a(hashInput));

				const auto cacheDirectory = GetShaderCacheDirectory();
				const auto baseName = sourcePath.stem().string() + "_" + StageToProfilePrefix(input.stage) + "_" + output.cacheKey;
				const auto artifactExtension = input.options.targetPlatform == ShaderTargetPlatform::SPIRV ? ".spv" : ".dxil";
				const auto artifactPath = cacheDirectory / (baseName + artifactExtension);
				output.artifactPath = artifactPath.string();

				if (std::filesystem::exists(artifactPath))
				{
					output.bytecode = ReadBinaryFile(artifactPath);
					output.status = output.bytecode.empty() ? ShaderCompilationStatus::Failed : ShaderCompilationStatus::Succeeded;
					if (output.status == ShaderCompilationStatus::Failed)
						output.diagnostics = "Cached shader artifact exists but could not be read: " + artifactPath.string();
					return output;
				}

				std::vector<std::string> arguments =
				{
					"-nologo",
					"-E", input.options.entryPoint,
					"-T", input.options.targetProfile,
					"-Fo", artifactPath.string()
				};

				if (input.options.enableDebugInfo)
					arguments.push_back("-Zi");
				if (input.options.treatWarningsAsErrors)
					arguments.push_back("-WX");
				if (input.options.targetPlatform == ShaderTargetPlatform::SPIRV)
				{
					arguments.push_back("-spirv");
					arguments.push_back("-fspv-reflect");
					arguments.push_back("-fspv-target-env=vulkan1.1");
				}

				for (const auto& includeDirectory : includeDirectories)
				{
					arguments.push_back("-I");
					arguments.push_back(includeDirectory);
				}

				for (const auto& macro : input.options.macros)
				{
					arguments.push_back("-D");
					arguments.push_back(macro.name + "=" + macro.value);
				}

				arguments.push_back(sourcePath.string());

				const auto process = ExecuteProcess(*dxcPath, arguments);
				output.diagnostics = dependencyDiagnostics + process.output;

				if (process.exitCode != 0 || !std::filesystem::exists(artifactPath))
				{
					output.status = ShaderCompilationStatus::Failed;
					if (process.exitCode != 0)
						output.diagnostics += "\n[dxc-exit-code] " + std::to_string(process.exitCode);
					if (!std::filesystem::exists(artifactPath))
						output.diagnostics += "\n[missing-artifact] " + artifactPath.string();
					std::ostringstream commandLine;
					commandLine << QuoteCommandArgument(dxcPath->string());
					for (const auto& argument : arguments)
						commandLine << ' ' << QuoteCommandArgument(argument);
					output.diagnostics += "\n[dxc-command] " + commandLine.str();
					return output;
				}

				output.bytecode = ReadBinaryFile(artifactPath);
				output.status = output.bytecode.empty() ? ShaderCompilationStatus::Failed : ShaderCompilationStatus::Succeeded;
				if (output.status == ShaderCompilationStatus::Failed)
					output.diagnostics += "Compiled shader artifact was produced but could not be read.\n";

				return output;
			}

			ShaderReflection Reflect(const ShaderCompilationInput& input) override
			{
				auto compileOutput = Compile(input);
				return Reflect(input, compileOutput);
			}

			ShaderReflection Reflect(const ShaderCompilationInput& input, const ShaderCompilationOutput& compileOutput) override
			{
				if (compileOutput.status != ShaderCompilationStatus::Succeeded || compileOutput.artifactPath.empty())
					return {};

				const auto reflectionCachePath = std::filesystem::path(compileOutput.artifactPath).replace_extension(
					std::filesystem::path(compileOutput.artifactPath).extension().string() + ".reflect.txt");
				if (std::filesystem::exists(reflectionCachePath))
				{
					const auto cachedDump = ReadTextFile(reflectionCachePath);
					if (!cachedDump.empty())
						return ParseReflectionDump(cachedDump, input.stage);
				}

				const auto dxcPath = FindDxcExecutable();
				if (!dxcPath.has_value())
					return {};

				const std::vector<std::string> arguments =
				{
					"-dumpbin",
					compileOutput.artifactPath
				};
				const auto process = ExecuteProcess(*dxcPath, arguments);
				if (process.exitCode != 0)
					return {};
				if (!process.output.empty())
					WriteTextFile(reflectionCachePath, process.output);
				return ParseReflectionDump(process.output, input.stage);
			}

			const char* GetBackendName() const override
			{
				return "DxcShaderCompilerBackend";
			}
		};
	}

	ShaderCompiler::ShaderCompiler()
		: m_backend(std::make_unique<DxcShaderCompilerBackend>())
	{
	}

	ShaderCompiler::ShaderCompiler(std::unique_ptr<IShaderCompilerBackend> backend)
		: m_backend(backend ? std::move(backend) : std::make_unique<DxcShaderCompilerBackend>())
	{
	}

	void ShaderCompiler::SetBackend(std::unique_ptr<IShaderCompilerBackend> backend)
	{
		m_backend = backend ? std::move(backend) : std::make_unique<DxcShaderCompilerBackend>();
	}

	const IShaderCompilerBackend* ShaderCompiler::GetBackend() const
	{
		return m_backend.get();
	}

	ShaderCompilationOutput ShaderCompiler::Compile(const ShaderCompilationInput& input) const
	{
		return m_backend->Compile(input);
	}

	ShaderCompilationOutput ShaderCompiler::Compile(ShaderAsset& asset, const ShaderVariantKey& variantKey, const ShaderCompileOptions& options) const
	{
		ShaderCompilationInput input;
		input.assetPath = asset.GetSourcePath().empty() ? variantKey.assetPath : asset.GetSourcePath();
		input.stage = variantKey.stage;
		input.options = options;

		if (input.options.entryPoint.empty())
		{
			input.options.entryPoint = asset.GetEntryPoint(variantKey.stage);
		}

		if (input.options.targetProfile.empty())
		{
			input.options.targetProfile = asset.GetTargetProfile(variantKey.stage);
		}

		input.options.macros.insert(input.options.macros.end(), variantKey.macros.begin(), variantKey.macros.end());

		ShaderCompilationOutput output = Compile(input);
		asset.SetCompiledVariant(variantKey, output);
		return output;
	}

	ShaderReflection ShaderCompiler::Reflect(const ShaderCompilationInput& input) const
	{
		return m_backend->Reflect(input);
	}

	ShaderReflection ShaderCompiler::Reflect(const ShaderCompilationInput& input, const ShaderCompilationOutput& compiledOutput) const
	{
		return m_backend->Reflect(input, compiledOutput);
	}
}
