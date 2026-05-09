#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
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
		constexpr const char* kDxcArgumentSchemaVersion = "dxc-args-v1";

		using ShaderReflection = Resources::ShaderReflection;
		using ShaderPropertyDesc = Resources::ShaderPropertyDesc;
		using ShaderConstantBufferDesc = Resources::ShaderConstantBufferDesc;
		using ShaderCBufferMemberDesc = Resources::ShaderCBufferMemberDesc;
		using ShaderResourceKind = Resources::ShaderResourceKind;
		using UniformType = Resources::UniformType;

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

		bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& content)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;

			if (!content.empty())
				stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
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

		std::string SanitizeArtifactPurpose(std::string_view value)
		{
			std::string sanitized;
			sanitized.reserve(value.size());
			for (const unsigned char ch : value)
			{
				if (std::isalnum(ch) || ch == '-' || ch == '_')
					sanitized.push_back(static_cast<char>(ch));
				else
					sanitized.push_back('_');
			}
			if (sanitized.empty())
				return "artifact";
			return sanitized;
		}

		bool TryCommitStagedShaderArtifact(
			const std::filesystem::path& temporaryPath,
			const std::filesystem::path& finalPath,
			std::string* diagnostics)
		{
			std::error_code error;
			std::filesystem::create_directories(finalPath.parent_path(), error);
			error.clear();

			std::filesystem::rename(temporaryPath, finalPath, error);
			if (!error)
				return true;

			std::filesystem::remove(finalPath, error);
			error.clear();
			std::filesystem::rename(temporaryPath, finalPath, error);
			if (!error)
				return true;

			if (diagnostics != nullptr)
			{
				*diagnostics += "Failed to commit shader artifact from temporary path '" +
					temporaryPath.string() + "' to '" + finalPath.string() + "': " + error.message() + "\n";
			}
			return false;
		}

		uint64_t CurrentUnixTimeMilliseconds()
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
		}

		std::string DefaultShaderArtifactLockOwner()
		{
			std::ostringstream stream;
			stream << "pid=";
#if defined(_WIN32)
			stream << GetCurrentProcessId();
#else
			stream << "unknown";
#endif
			stream << ";thread=" << std::this_thread::get_id();
			return stream.str();
		}

		std::string ReadShaderArtifactLockOwner(const std::filesystem::path& lockPath)
		{
			std::ifstream stream(lockPath / "owner.txt", std::ios::binary);
			if (!stream)
				return {};

			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		void WriteShaderArtifactLockOwner(
			const std::filesystem::path& lockPath,
			const ShaderArtifactLockOptions& options)
		{
			std::ofstream stream(lockPath / "owner.txt", std::ios::binary | std::ios::trunc);
			if (!stream)
				return;

			const auto owner = options.owner.empty()
				? DefaultShaderArtifactLockOwner()
				: options.owner;
			stream << "owner=" << owner << "\n";
			stream << "createdUnixMs=" << CurrentUnixTimeMilliseconds() << "\n";
		}

		bool IsShaderArtifactLockStale(
			const std::filesystem::path& lockPath,
			const ShaderArtifactLockOptions& options)
		{
			if (options.staleAfterMilliseconds == 0u)
				return false;

			std::error_code error;
			const auto writeTime = std::filesystem::last_write_time(lockPath, error);
			if (error)
				return false;

			const auto now = std::filesystem::file_time_type::clock::now();
			if (now < writeTime)
				return false;

			const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - writeTime);
			return age.count() >= options.staleAfterMilliseconds;
		}

		class ShaderArtifactLock final
		{
		public:
			ShaderArtifactLock(
				std::filesystem::path lockPath,
				ShaderArtifactLockOptions options = {},
				std::string* diagnostics = nullptr)
				: m_lockPath(std::move(lockPath))
				, m_options(std::move(options))
			{
				if (m_options.retryIntervalMilliseconds == 0u)
					m_options.retryIntervalMilliseconds = 1u;

				const auto deadline = std::chrono::steady_clock::now() +
					std::chrono::milliseconds(m_options.timeoutMilliseconds);
				do
				{
					std::error_code error;
					if (std::filesystem::create_directory(m_lockPath, error))
					{
						m_acquired = true;
						WriteShaderArtifactLockOwner(m_lockPath, m_options);
						return;
					}

					if (error && error != std::errc::file_exists)
						break;

					if (IsShaderArtifactLockStale(m_lockPath, m_options))
					{
						const std::string staleOwner = ReadShaderArtifactLockOwner(m_lockPath);
						std::filesystem::remove_all(m_lockPath, error);
						if (diagnostics != nullptr)
						{
							*diagnostics += "Removed stale shader artifact lock: " + m_lockPath.string();
							if (!staleOwner.empty())
								*diagnostics += " owner={" + staleOwner + "}";
							*diagnostics += "\n";
						}
						continue;
					}

					if (std::chrono::steady_clock::now() >= deadline)
						break;

					std::this_thread::sleep_for(std::chrono::milliseconds(m_options.retryIntervalMilliseconds));
				}
				while (m_options.timeoutMilliseconds != 0u || std::chrono::steady_clock::now() < deadline);

				if (diagnostics != nullptr)
				{
					*diagnostics += "timed out waiting for shader artifact lock: " + m_lockPath.string();
					const std::string currentOwner = ReadShaderArtifactLockOwner(m_lockPath);
					if (!currentOwner.empty())
						*diagnostics += " owner={" + currentOwner + "}";
					if (!m_options.owner.empty())
						*diagnostics += " waiter=" + m_options.owner;
					*diagnostics += "\n";
				}
			}

			~ShaderArtifactLock()
			{
				if (!m_acquired)
					return;

				std::error_code error;
				std::filesystem::remove_all(m_lockPath, error);
			}

			bool IsAcquired() const
			{
				return m_acquired;
			}

		private:
			std::filesystem::path m_lockPath;
			ShaderArtifactLockOptions m_options;
			bool m_acquired = false;
		};

		bool CommitShaderArtifactBytesAtomic(
			const ShaderArtifactStagingPlan& plan,
			const std::vector<uint8_t>& content,
			const ShaderArtifactLockOptions& lockOptions,
			std::string* diagnostics)
		{
			const std::filesystem::path finalPath(plan.finalPath);
			const std::filesystem::path temporaryPath(plan.temporaryPath);
			const std::filesystem::path lockPath(plan.lockPath);

			std::error_code error;
			std::filesystem::create_directories(finalPath.parent_path(), error);

			ShaderArtifactLock lock(lockPath, lockOptions, diagnostics);
			if (!lock.IsAcquired())
			{
				if (diagnostics != nullptr)
					*diagnostics += "Failed to acquire shader artifact lock: " + lockPath.string() + "\n";
				return false;
			}

			std::filesystem::remove(temporaryPath, error);
			if (!WriteBinaryFile(temporaryPath, content))
			{
				if (diagnostics != nullptr)
					*diagnostics += "Failed to write temporary shader artifact: " + temporaryPath.string() + "\n";
				std::filesystem::remove(temporaryPath, error);
				return false;
			}

			const bool committed = TryCommitStagedShaderArtifact(temporaryPath, finalPath, diagnostics);
			if (!committed)
				std::filesystem::remove(temporaryPath, error);
			return committed;
		}

		std::string BuildToolchainVersionFingerprint(const std::filesystem::path& executable)
		{
			std::error_code error;
			const auto fileSize = std::filesystem::file_size(executable, error);
			const auto writeTime = std::filesystem::last_write_time(executable, error);

			std::ostringstream stream;
			stream << "size=" << (error ? 0u : fileSize);
			stream << ";mtime=";
			if (!error)
				stream << writeTime.time_since_epoch().count();
			else
				stream << 0;
			return stream.str();
		}

		ShaderCompilerToolchainIdentity BuildDxcToolchainIdentity(const std::filesystem::path& dxcPath)
		{
			ShaderCompilerToolchainIdentity identity;
			std::error_code error;
			identity.compilerPath = std::filesystem::weakly_canonical(dxcPath, error).string();
			if (identity.compilerPath.empty())
				identity.compilerPath = dxcPath.string();
			identity.compilerVersion = BuildToolchainVersionFingerprint(dxcPath);
			identity.argumentSchemaVersion = kDxcArgumentSchemaVersion;
			return identity;
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

		std::string ShaderProcessStatusToString(ShaderProcessStatus status)
		{
			switch (status)
			{
			case ShaderProcessStatus::Succeeded: return "succeeded";
			case ShaderProcessStatus::Failed: return "failed";
			case ShaderProcessStatus::FailedToStart: return "failed-to-start";
			case ShaderProcessStatus::TimedOut: return "timed-out";
			case ShaderProcessStatus::Cancelled: return "cancelled";
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

				output.cacheKey = BuildShaderCompilationCacheKey(
					input,
					hashInput,
					BuildDxcToolchainIdentity(*dxcPath));

				const auto cacheDirectory = GetShaderCacheDirectory();
				const auto baseName = sourcePath.stem().string() + "_" + StageToProfilePrefix(input.stage) + "_" + output.cacheKey;
				const auto artifactExtension = input.options.targetPlatform == ShaderTargetPlatform::SPIRV ? ".spv" : ".dxil";
				const auto artifactPath = cacheDirectory / (baseName + artifactExtension);
				const auto stagingPlan = BuildShaderArtifactStagingPlan(artifactPath.string(), "dxc");
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
					"-Fo", stagingPlan.temporaryPath
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

				const auto process = ExecuteShaderCompilerProcess(dxcPath->string(), arguments);
				output.diagnostics = dependencyDiagnostics + process.output;
				if (!process.diagnostics.empty())
					output.diagnostics += "\n" + process.diagnostics;

				if (process.status == ShaderProcessStatus::Succeeded && std::filesystem::exists(stagingPlan.temporaryPath))
				{
					ShaderArtifactLock lock(stagingPlan.lockPath);
					if (!lock.IsAcquired())
					{
						output.status = ShaderCompilationStatus::Failed;
						output.diagnostics += "\n[artifact-lock-failed] " + stagingPlan.lockPath;
						return output;
					}

					if (!TryCommitStagedShaderArtifact(stagingPlan.temporaryPath, artifactPath, &output.diagnostics))
					{
						output.status = ShaderCompilationStatus::Failed;
						output.diagnostics += "\n[artifact-commit-failed] " + artifactPath.string();
						return output;
					}
				}

				if (process.status != ShaderProcessStatus::Succeeded || !std::filesystem::exists(artifactPath))
				{
					output.status = ShaderCompilationStatus::Failed;
					if (process.status != ShaderProcessStatus::Succeeded)
					{
						output.diagnostics += "\n[process-status] " + ShaderProcessStatusToString(process.status);
						output.diagnostics += "\n[dxc-exit-code] " + std::to_string(process.exitCode);
					}
					if (!std::filesystem::exists(artifactPath))
					{
						output.diagnostics += "\n[missing-artifact] " + artifactPath.string();
						if (std::filesystem::exists(stagingPlan.temporaryPath))
							output.diagnostics += "\n[staged-artifact] " + stagingPlan.temporaryPath;
					}
					output.diagnostics += "\n[dxc-command] " + process.commandLine;
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
				const auto process = ExecuteShaderCompilerProcess(dxcPath->string(), arguments);
				if (process.status != ShaderProcessStatus::Succeeded)
					return {};
				if (!process.output.empty())
					WriteShaderArtifactTextAtomic(reflectionCachePath.string(), process.output);
				return ParseReflectionDump(process.output, input.stage);
			}

			const char* GetBackendName() const override
			{
				return "DxcShaderCompilerBackend";
			}
		};
	}

	std::string BuildShaderCompilationCacheKey(
		const ShaderCompilationInput& input,
		std::string_view sourceDependencyFingerprint,
		const ShaderCompilerToolchainIdentity& toolchain)
	{
		std::string hashInput;
		hashInput += sourceDependencyFingerprint;
		hashInput += '\n';
		hashInput += input.assetPath;
		hashInput += '\n';
		hashInput += input.options.entryPoint;
		hashInput += '\n';
		hashInput += input.options.targetProfile;
		hashInput += '\n';
		hashInput += TargetPlatformToString(input.options.targetPlatform);
		hashInput += '\n';
		hashInput += StageToString(input.stage);
		hashInput += '\n';
		hashInput += input.options.enableDebugInfo ? "debug" : "nodebug";
		hashInput += '\n';
		hashInput += input.options.treatWarningsAsErrors ? "werror" : "nowerror";
		hashInput += '\n';
		hashInput += toolchain.compilerPath;
		hashInput += '\n';
		hashInput += toolchain.compilerVersion;
		hashInput += '\n';
		hashInput += toolchain.argumentSchemaVersion;
		hashInput += '\n';

		for (const auto& includeDirectory : input.options.includeDirectories)
		{
			hashInput += "include=";
			hashInput += includeDirectory;
			hashInput += '\n';
		}

		for (const auto& macro : input.options.macros)
		{
			hashInput += "macro=";
			hashInput += macro.name;
			hashInput += '=';
			hashInput += macro.value;
			hashInput += '\n';
		}

		return ToHex(HashStringFNV1a(hashInput));
	}

		ShaderArtifactStagingPlan BuildShaderArtifactStagingPlan(std::string_view finalPath, std::string_view purpose)
		{
			ShaderArtifactStagingPlan plan;
			plan.finalPath = std::string(finalPath);

			const std::filesystem::path finalArtifactPath(plan.finalPath);
			const auto parentPath = finalArtifactPath.parent_path();
			const auto filename = finalArtifactPath.filename().string();
			const auto purposeSuffix = SanitizeArtifactPurpose(purpose);
			std::ostringstream uniqueInput;
			uniqueInput << plan.finalPath << "|" << purposeSuffix << "|"
				<< std::chrono::steady_clock::now().time_since_epoch().count() << "|"
				<< std::this_thread::get_id();
			const auto uniqueSuffix = ToHex(HashStringFNV1a(uniqueInput.str()));

			plan.temporaryPath = (parentPath / (filename + "." + purposeSuffix + "." + uniqueSuffix + ".tmp")).string();
			plan.lockPath = (parentPath / (filename + "." + purposeSuffix + ".lock")).string();
			return plan;
		}

	bool WriteShaderArtifactTextAtomic(std::string_view finalPath, std::string_view content, std::string* diagnostics)
	{
		const auto plan = BuildShaderArtifactStagingPlan(finalPath, "text");
		return CommitShaderArtifactBytesAtomic(
			plan,
			std::vector<uint8_t>(content.begin(), content.end()),
			ShaderArtifactLockOptions{},
			diagnostics);
	}

	bool WriteShaderArtifactTextAtomic(
		std::string_view finalPath,
		std::string_view content,
		const ShaderArtifactLockOptions& lockOptions,
		std::string* diagnostics)
	{
		const auto plan = BuildShaderArtifactStagingPlan(finalPath, "text");
		return CommitShaderArtifactBytesAtomic(
			plan,
			std::vector<uint8_t>(content.begin(), content.end()),
			lockOptions,
			diagnostics);
	}

	ShaderProcessResult ExecuteShaderCompilerProcess(
		std::string_view executable,
		const std::vector<std::string>& arguments,
		const ShaderProcessOptions& options)
	{
		ShaderProcessResult result;

		std::ostringstream commandLineBuilder;
		commandLineBuilder << QuoteCommandArgument(std::string(executable));
		for (const auto& argument : arguments)
			commandLineBuilder << ' ' << QuoteCommandArgument(argument);
		result.commandLine = commandLineBuilder.str();

		if (options.cancellationFlag != nullptr && options.cancellationFlag->load())
		{
			result.status = ShaderProcessStatus::Cancelled;
			result.diagnostics = "Shader compiler process cancelled before launch: " + result.commandLine;
			return result;
		}

#if defined(_WIN32)
		SECURITY_ATTRIBUTES securityAttributes{};
		securityAttributes.nLength = sizeof(securityAttributes);
		securityAttributes.bInheritHandle = TRUE;

		HANDLE readPipe = nullptr;
		HANDLE writePipe = nullptr;
		if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0))
		{
			result.status = ShaderProcessStatus::FailedToStart;
			result.diagnostics = "Failed to create shader compiler process pipe for: " + result.commandLine;
			return result;
		}

		SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOA startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		startupInfo.dwFlags = STARTF_USESTDHANDLES;
		startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startupInfo.hStdOutput = writePipe;
		startupInfo.hStdError = writePipe;

		std::vector<char> mutableCommandLine(result.commandLine.begin(), result.commandLine.end());
		mutableCommandLine.push_back('\0');

		PROCESS_INFORMATION processInfo{};
		HANDLE jobObject = CreateJobObjectA(nullptr, nullptr);
		if (jobObject != nullptr)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
			jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(
				jobObject,
				JobObjectExtendedLimitInformation,
				&jobInfo,
				sizeof(jobInfo));
		}

		const BOOL created = CreateProcessA(
			nullptr,
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED,
			nullptr,
			nullptr,
			&startupInfo,
			&processInfo);

		CloseHandle(writePipe);

		if (!created)
		{
			const auto errorCode = GetLastError();
			result.status = ShaderProcessStatus::FailedToStart;
			result.diagnostics = "Failed to spawn shader compiler process (" + std::to_string(errorCode) + "): " + result.commandLine;
			CloseHandle(readPipe);
			if (jobObject != nullptr)
				CloseHandle(jobObject);
			return result;
		}

		if (jobObject != nullptr && !AssignProcessToJobObject(jobObject, processInfo.hProcess))
		{
			const auto errorCode = GetLastError();
			TerminateProcess(processInfo.hProcess, static_cast<UINT>(-1));
			result.status = ShaderProcessStatus::FailedToStart;
			result.diagnostics = "Failed to assign shader compiler process to cleanup job (" +
				std::to_string(errorCode) + "): " + result.commandLine;
			WaitForSingleObject(processInfo.hProcess, 5000);
			CloseHandle(readPipe);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			CloseHandle(jobObject);
			return result;
		}

		std::string processOutput;
		std::thread outputReader([readPipe, &processOutput]()
		{
			char buffer[4096];
			DWORD bytesRead = 0;
			while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &bytesRead, nullptr) && bytesRead > 0)
			{
				buffer[bytesRead] = '\0';
				processOutput += buffer;
			}
			CloseHandle(readPipe);
		});

		ResumeThread(processInfo.hThread);

		const auto timeout = std::chrono::milliseconds(options.timeoutMilliseconds);
		const auto startTime = std::chrono::steady_clock::now();
		bool running = true;
		while (running)
		{
			if (options.cancellationFlag != nullptr && options.cancellationFlag->load())
			{
				if (jobObject != nullptr)
					TerminateJobObject(jobObject, static_cast<UINT>(-1));
				else
					TerminateProcess(processInfo.hProcess, static_cast<UINT>(-1));
				result.status = ShaderProcessStatus::Cancelled;
				result.diagnostics = "Shader compiler process cancelled: " + result.commandLine;
				break;
			}

			if (options.timeoutMilliseconds != 0u && std::chrono::steady_clock::now() - startTime >= timeout)
			{
				if (jobObject != nullptr)
					TerminateJobObject(jobObject, static_cast<UINT>(-1));
				else
					TerminateProcess(processInfo.hProcess, static_cast<UINT>(-1));
				result.status = ShaderProcessStatus::TimedOut;
				result.diagnostics = "Shader compiler process timed out after " + std::to_string(options.timeoutMilliseconds) + " ms: " + result.commandLine;
				break;
			}

			const DWORD waitResult = WaitForSingleObject(
				processInfo.hProcess,
				options.timeoutMilliseconds == 0u ? 50u : 10u);
			if (waitResult == WAIT_OBJECT_0)
				running = false;
			else if (waitResult != WAIT_TIMEOUT)
			{
				if (jobObject != nullptr)
					TerminateJobObject(jobObject, static_cast<UINT>(-1));
				else
					TerminateProcess(processInfo.hProcess, static_cast<UINT>(-1));
				result.status = ShaderProcessStatus::Failed;
				result.diagnostics = "Shader compiler process wait failed: " + result.commandLine;
				running = false;
			}
		}

		if (WaitForSingleObject(processInfo.hProcess, 5000) != WAIT_OBJECT_0 && result.diagnostics.empty())
			result.diagnostics = "Shader compiler process cleanup wait timed out: " + result.commandLine;

		if (jobObject != nullptr &&
			(result.status == ShaderProcessStatus::TimedOut || result.status == ShaderProcessStatus::Cancelled))
		{
			TerminateJobObject(jobObject, static_cast<UINT>(-1));
		}

		if (outputReader.joinable())
			outputReader.join();
		result.output = std::move(processOutput);

		DWORD exitCode = static_cast<DWORD>(-1);
		GetExitCodeProcess(processInfo.hProcess, &exitCode);
		result.exitCode = static_cast<int>(exitCode);

		if (result.status != ShaderProcessStatus::TimedOut &&
			result.status != ShaderProcessStatus::Cancelled &&
			result.status != ShaderProcessStatus::FailedToStart &&
			result.diagnostics.empty())
		{
			result.status = exitCode == 0u ? ShaderProcessStatus::Succeeded : ShaderProcessStatus::Failed;
			if (result.status == ShaderProcessStatus::Failed)
				result.diagnostics = "Shader compiler process exited with code " + std::to_string(result.exitCode) + ": " + result.commandLine;
		}

		CloseHandle(readPipe);
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		if (jobObject != nullptr)
			CloseHandle(jobObject);
#else
		(void)executable;
		(void)arguments;
		(void)options;
		result.status = ShaderProcessStatus::FailedToStart;
		result.diagnostics = "Shader compiler process execution is only implemented on Windows: " + result.commandLine;
#endif

		return result;
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

	std::vector<ShaderCompilationOutput> ShaderCompiler::CompileBatch(const std::vector<ShaderCompilationInput>& inputs) const
	{
		std::vector<ShaderCompilationOutput> outputs(inputs.size());
		if (inputs.empty())
			return outputs;

		std::vector<std::future<ShaderCompilationOutput>> futures;
		futures.reserve(inputs.size());
		for (const auto& input : inputs)
		{
			futures.push_back(std::async(
				std::launch::async,
				[this, input]()
				{
					return Compile(input);
				}));
		}

		for (size_t index = 0u; index < futures.size(); ++index)
			outputs[index] = futures[index].get();
		return outputs;
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

	std::vector<ShaderReflection> ShaderCompiler::ReflectBatch(const std::vector<ShaderReflectionInput>& inputs) const
	{
		std::vector<ShaderReflection> reflections(inputs.size());
		if (inputs.empty())
			return reflections;

		std::vector<std::future<ShaderReflection>> futures;
		futures.reserve(inputs.size());
		for (const auto& input : inputs)
		{
			futures.push_back(std::async(
				std::launch::async,
				[this, input]()
				{
					return Reflect(input.input, input.compiledOutput);
				}));
		}

		for (size_t index = 0u; index < futures.size(); ++index)
			reflections[index] = futures[index].get();
		return reflections;
	}
}
