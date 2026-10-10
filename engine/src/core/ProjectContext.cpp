#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

#include <nlohmann/json.hpp>

#include <myengine/core/ProjectContext.h>

namespace myengine::core
{
    namespace fs = std::filesystem;

    namespace
    {
        void SetError(std::string* error, std::string message)
        {
            if (error != nullptr)
            {
                *error = std::move(message);
            }
        }

        std::string ToLower(std::string text)
        {
            std::transform(
                text.begin(),
                text.end(),
                text.begin(),
                [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return text;
        }

        bool ReadTextFile(const fs::path& file, std::string& text)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return false;
            }

            std::ostringstream buffer;
            buffer << stream.rdbuf();
            text = buffer.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            {
                text.erase(0, 3);
            }
            return true;
        }

        bool WriteTextFile(const fs::path& file, const std::string& text)
        {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }

            stream << text;
            stream.close();
            return static_cast<bool>(stream);
        }

        // A project path stays inside the project folder
        bool IsProjectRelative(const std::string& value)
        {
            if (value.empty())
            {
                return false;
            }

            const fs::path path = fs::u8path(value);
            if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
            {
                return false;
            }

            for (const auto& part : path)
            {
                if (part == "..")
                {
                    return false;
                }
            }
            return true;
        }

        std::string NormalizedKey(const fs::path& path)
        {
            return ToLower(path.lexically_normal().generic_u8string());
        }

        // Shell dialog for a file or a folder; an empty path is a cancelled dialog
        fs::path ShowOpenDialog(const bool pickFolder)
        {
            const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            fs::path result;

            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                DWORD options = 0;
                dialog->GetOptions(&options);
                options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
                if (pickFolder)
                {
                    options |= FOS_PICKFOLDERS;
                    dialog->SetTitle(L"Choose a folder for the project");
                }
                else
                {
                    options |= FOS_FILEMUSTEXIST;
                    COMDLG_FILTERSPEC filter[] = {{L"myengine project (*.myproject)", L"*.myproject"}};
                    dialog->SetFileTypes(1, filter);
                    dialog->SetTitle(L"Open a project");
                }
                dialog->SetOptions(options);

                if (SUCCEEDED(dialog->Show(GetActiveWindow())))
                {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
                    {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
                        {
                            result = fs::path(path);
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }

            if (SUCCEEDED(initResult))
            {
                CoUninitialize();
            }
            return result;
        }
    }

    fs::path ProjectContext::ExecutableDirectory()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < buffer.size())
            {
                buffer.resize(length);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
        return fs::path(buffer).parent_path();
    }

    fs::path ProjectContext::EngineRoot()
    {
#ifdef MYENGINE_SOURCE_DIR
        const fs::path source = fs::u8path(MYENGINE_SOURCE_DIR);
        std::error_code error;
        if (fs::is_directory(source / "assets", error))
        {
            return source;
        }
#endif
        return ExecutableDirectory();
    }

    bool ProjectContext::Load(const fs::path& projectFile, std::string* error)
    {
        std::error_code fsError;
        if (!fs::is_regular_file(projectFile, fsError))
        {
            SetError(error, "The project file does not exist: " + projectFile.u8string());
            return false;
        }

        ProjectDescriptor descriptor;
        if (!project::ReadDescriptor(projectFile, descriptor, error))
        {
            return false;
        }

        fs::path absolute = fs::absolute(projectFile, fsError);
        if (fsError)
        {
            absolute = projectFile;
        }

        projectFile_ = absolute.lexically_normal();
        root_ = projectFile_.parent_path();
        descriptor_ = std::move(descriptor);
        initialized_ = true;
        return true;
    }

    void ProjectContext::InitializeDefault()
    {
        descriptor_ = ProjectDescriptor{};
        descriptor_.name = "myengine";
        descriptor_.contentDir = "assets";
        descriptor_.mapsDir = "assets/scenes";
        descriptor_.scriptsDir = "assets/scripts";
        descriptor_.prefabsDir = "assets/prefabs";
        descriptor_.defaultMap = "assets/scenes/coin_guard_demo.json";
        root_ = EngineRoot();
        projectFile_.clear();
        initialized_ = true;
    }

    bool ProjectContext::IsInitialized() const
    {
        return initialized_;
    }

    bool ProjectContext::IsEngineProject() const
    {
        if (!initialized_)
        {
            return false;
        }

        std::error_code error;
        const bool same = fs::equivalent(root_, EngineRoot(), error);
        return !error && same;
    }

    const fs::path& ProjectContext::Root() const
    {
        return root_;
    }

    const fs::path& ProjectContext::ProjectFile() const
    {
        return projectFile_;
    }

    const ProjectDescriptor& ProjectContext::Descriptor() const
    {
        return descriptor_;
    }

    const std::string& ProjectContext::Name() const
    {
        return descriptor_.name;
    }

    fs::path ProjectContext::ContentDir() const
    {
        return (root_ / fs::u8path(descriptor_.contentDir)).lexically_normal();
    }

    fs::path ProjectContext::MapsDir() const
    {
        return (root_ / fs::u8path(descriptor_.mapsDir)).lexically_normal();
    }

    fs::path ProjectContext::ScriptsDir() const
    {
        return (root_ / fs::u8path(descriptor_.scriptsDir)).lexically_normal();
    }

    fs::path ProjectContext::PrefabsDir() const
    {
        return (root_ / fs::u8path(descriptor_.prefabsDir)).lexically_normal();
    }

    fs::path ProjectContext::DefaultMap() const
    {
        return (root_ / fs::u8path(descriptor_.defaultMap)).lexically_normal();
    }

    std::string ProjectContext::ToProjectRelative(const fs::path& absolute) const
    {
        const fs::path relative = absolute.lexically_normal().lexically_relative(root_);
        const std::string text = relative.generic_u8string();
        if (relative.empty() || text == ".." || text.rfind("../", 0) == 0 || relative.is_absolute())
        {
            return absolute.lexically_normal().generic_u8string();
        }
        return text;
    }

    fs::path ProjectContext::ResolveContentPath(const fs::path& path) const
    {
        std::error_code error;
        const auto normalize = [&](const fs::path& candidate)
        {
            const fs::path canonical = fs::weakly_canonical(candidate, error);
            return error ? candidate.lexically_normal() : canonical;
        };

        if (path.is_absolute())
        {
            return normalize(path);
        }

        const fs::path projectRelative = root_ / path;
        if (fs::exists(projectRelative, error))
        {
            return normalize(projectRelative);
        }

        const fs::path engineRoot = EngineRoot();
        if (!engineRoot.empty() && NormalizedKey(engineRoot) != NormalizedKey(root_))
        {
            const fs::path engineRelative = engineRoot / path;
            if (fs::exists(engineRelative, error))
            {
                return normalize(engineRelative);
            }
        }

        if (fs::exists(path, error))
        {
            return normalize(path);
        }

        const fs::path executableRelative = ExecutableDirectory() / path;
        if (!executableRelative.empty() && fs::exists(executableRelative, error))
        {
            return normalize(executableRelative);
        }

        return projectRelative.lexically_normal();
    }

    namespace project
    {
        bool ReadDescriptor(const fs::path& file, ProjectDescriptor& descriptor, std::string* error)
        {
            std::string text;
            if (!ReadTextFile(file, text))
            {
                SetError(error, "Could not read the project file: " + file.u8string());
                return false;
            }

            try
            {
                const nlohmann::json root = nlohmann::json::parse(text);
                if (!root.is_object())
                {
                    SetError(error, "The project file is not a JSON object: " + file.u8string());
                    return false;
                }

                ProjectDescriptor result;
                result.name = root.value("name", file.stem().u8string());
                result.engineVersion = root.value("engineVersion", kProjectFormatVersion);
                result.contentDir = root.value("contentDir", result.contentDir);
                result.mapsDir = root.value("mapsDir", result.mapsDir);
                result.scriptsDir = root.value("scriptsDir", result.scriptsDir);
                result.prefabsDir = root.value("prefabsDir", result.prefabsDir);
                result.defaultMap = root.value("defaultMap", result.defaultMap);

                if (result.name.empty())
                {
                    result.name = file.stem().u8string();
                }
                if (result.engineVersion > kProjectFormatVersion)
                {
                    SetError(error, "The project was made by a newer engine (format " + std::to_string(result.engineVersion) + ")");
                    return false;
                }

                const std::pair<const char*, const std::string*> paths[] = {
                    {"contentDir", &result.contentDir},
                    {"mapsDir", &result.mapsDir},
                    {"scriptsDir", &result.scriptsDir},
                    {"prefabsDir", &result.prefabsDir},
                    {"defaultMap", &result.defaultMap},
                };
                for (const auto& [field, value] : paths)
                {
                    if (!IsProjectRelative(*value))
                    {
                        SetError(error, std::string("The project field '") + field + "' must be a path inside the project");
                        return false;
                    }
                }

                descriptor = std::move(result);
                return true;
            }
            catch (const std::exception& exception)
            {
                SetError(error, std::string("The project file is not valid: ") + exception.what());
                return false;
            }
        }

        bool WriteDescriptor(const fs::path& file, const ProjectDescriptor& descriptor, std::string* error)
        {
            nlohmann::ordered_json root;
            root["name"] = descriptor.name;
            root["engineVersion"] = descriptor.engineVersion;
            root["contentDir"] = descriptor.contentDir;
            root["mapsDir"] = descriptor.mapsDir;
            root["scriptsDir"] = descriptor.scriptsDir;
            root["prefabsDir"] = descriptor.prefabsDir;
            root["defaultMap"] = descriptor.defaultMap;

            if (!WriteTextFile(file, root.dump(2) + "\n"))
            {
                SetError(error, "Could not write the project file: " + file.u8string());
                return false;
            }
            return true;
        }

        bool IsValidProjectName(const std::string& name)
        {
            if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
            {
                return false;
            }

            for (const char character : name)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (code < 32 || std::string_view("\\/:*?\"<>|").find(character) != std::string_view::npos)
                {
                    return false;
                }
            }
            return true;
        }

        std::string EmptyMapText()
        {
            using nlohmann::ordered_json;

            ordered_json camera;
            camera["id"] = 1;
            camera["Tag"] = {{"name", "Camera_1"}};
            camera["Camera"] = {
                {"isPrimary", true},
                {"fovYDeg", 60.0},
                {"nearPlane", 0.05},
                {"farPlane", 200.0},
                {"orthographicHalfHeight", 1.0},
                {"position", {0.0, 4.0, -8.0}},
                {"rotationDeg", {25.0, 0.0, 0.0}},
            };
            camera["CameraController"] = {
                {"moveSpeed", 8.0},
                {"rotateSpeedDeg", 80.0},
                {"mouseSensitivityDeg", 0.12},
                {"zoomSpeed", 1.0},
            };
            camera["WindowBinding"] = {{"windowId", 1}};

            ordered_json floor;
            floor["id"] = 2;
            floor["Tag"] = {{"name", "Floor_1"}};
            floor["Transform"] = {
                {"position", {0.0, -0.5, 3.0}},
                {"rotationDeg", {0.0, 0.0, 0.0}},
                {"scale", {14.0, 1.0, 14.0}},
            };
            floor["MeshRenderer"] = {
                {"meshPath", "assets/models/crate.obj"},
                {"materialPath", "assets/materials/default.material.json"},
                {"visible", true},
            };
            floor["Collider"] = {
                {"type", "box"},
                {"halfExtents", {0.5, 0.5, 0.5}},
                {"offset", {0.0, 0.0, 0.0}},
                {"radius", 0.5},
                {"isTrigger", false},
                {"friction", 0.65},
                {"bounciness", 0.0},
            };
            floor["WindowBinding"] = {{"windowId", 1}};

            ordered_json root;
            root["entities"] = ordered_json::array({camera, floor});
            return root.dump(2) + "\n";
        }

        bool CreateProject(const fs::path& parentDirectory, const std::string& name, fs::path& projectFile, std::string* error)
        {
            if (!IsValidProjectName(name))
            {
                SetError(error, "The project name must not be empty or contain \\ / : * ? \" < > |");
                return false;
            }

            std::error_code fsError;
            if (!fs::is_directory(parentDirectory, fsError))
            {
                SetError(error, "The folder does not exist: " + parentDirectory.u8string());
                return false;
            }

            const fs::path directory = parentDirectory / fs::u8path(name);
            if (fs::exists(directory, fsError) && !fs::is_empty(directory, fsError))
            {
                SetError(error, "The folder already exists and is not empty: " + directory.u8string());
                return false;
            }

            for (const char* folder : {"Content/Scripts", "Content/Prefabs", "Content/Materials", "Content/Models", "Content/Textures", "Maps"})
            {
                fs::create_directories(directory / folder, fsError);
                if (fsError)
                {
                    SetError(error, "Could not create " + (directory / folder).u8string() + ": " + fsError.message());
                    return false;
                }
            }

            if (!WriteTextFile(directory / "Maps" / "Main.json", EmptyMapText()))
            {
                SetError(error, "Could not write the first map");
                return false;
            }

            ProjectDescriptor descriptor;
            descriptor.name = name;
            const fs::path file = directory / fs::u8path(name + kProjectFileExtension);
            if (!WriteDescriptor(file, descriptor, error))
            {
                return false;
            }

            projectFile = file;
            return true;
        }

        fs::path DefaultRecentProjectsFile()
        {
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
            const fs::path base = length > 0 && length < std::size(buffer) ? fs::path(buffer) : ProjectContext::ExecutableDirectory();
            return base / "myengine" / "recent_projects.json";
        }

        std::vector<fs::path> LoadRecentProjects(const fs::path& file)
        {
            std::vector<fs::path> result;
            std::string text;
            if (!ReadTextFile(file, text))
            {
                return result;
            }

            try
            {
                const nlohmann::json root = nlohmann::json::parse(text);
                if (root.is_object() && root.contains("recent") && root["recent"].is_array())
                {
                    for (const auto& item : root["recent"])
                    {
                        if (item.is_string())
                        {
                            result.push_back(fs::u8path(item.get<std::string>()));
                        }
                    }
                }
            }
            catch (const std::exception&)
            {
                result.clear();
            }
            return result;
        }

        void AddRecentProject(const fs::path& file, const fs::path& projectFile, const std::size_t maxCount)
        {
            if (projectFile.empty())
            {
                return;
            }

            std::vector<fs::path> list = LoadRecentProjects(file);
            const std::string key = NormalizedKey(projectFile);
            list.erase(
                std::remove_if(list.begin(), list.end(), [&](const fs::path& item) { return NormalizedKey(item) == key; }),
                list.end());
            list.insert(list.begin(), projectFile.lexically_normal());
            if (list.size() > maxCount)
            {
                list.resize(maxCount);
            }

            nlohmann::ordered_json root;
            root["recent"] = nlohmann::ordered_json::array();
            for (const auto& item : list)
            {
                root["recent"].push_back(item.generic_u8string());
            }

            std::error_code error;
            fs::create_directories(file.parent_path(), error);
            WriteTextFile(file, root.dump(2) + "\n");
        }

        fs::path PickProjectFile()
        {
            return ShowOpenDialog(false);
        }

        fs::path PickFolder()
        {
            return ShowOpenDialog(true);
        }
    }
}
