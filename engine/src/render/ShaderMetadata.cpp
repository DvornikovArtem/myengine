// ShaderMetadata.cpp

#include <myengine/render/ShaderMetadata.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace myengine::render
{
    namespace
    {
        constexpr int kMaxIncludeDepth = 16;

        // One "// @..." comment: the text after the slashes and where it is
        struct TagLine
        {
            std::string file;
            std::uint32_t line = 0;
            std::string text;
        };

        // A declaration that has metadata comments: a variable of a constant buffer or a texture
        struct Declaration
        {
            std::string file;
            std::uint32_t line = 0;      // the line of the declaration
            std::string name;
            bool texture = false;
            std::vector<TagLine> tagLines;
        };

        struct Token
        {
            std::string text;
            bool quoted = false;
        };

        struct Tag
        {
            std::string name;
            std::vector<Token> values;
            bool unterminated = false;   // a quote that is not closed
        };

        void Warn(std::vector<ShaderDiagnostic>& diagnostics, const std::string& file, const std::uint32_t line, std::string message)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.file = file;
            diagnostic.line = line;
            diagnostic.warning = true;
            diagnostic.message = "shader metadata: " + std::move(message);
            diagnostics.push_back(std::move(diagnostic));
        }

        std::string Trim(const std::string& text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        // The tags of a comment: "@min 0 @max 1 @label "Two words"". The values of a tag run to the next '@'.
        std::vector<Tag> ParseTags(const std::string& text)
        {
            std::vector<Tag> tags;
            std::size_t pos = 0;
            const auto skipSeparators = [&]()
            {
                while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) || text[pos] == ','))
                {
                    ++pos;
                }
            };
            while (true)
            {
                skipSeparators();
                if (pos >= text.size())
                {
                    break;
                }
                if (text[pos] != '@')
                {
                    // text that is not a tag or a value of one (before the first tag): skip the word
                    while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos])) && text[pos] != '@')
                    {
                        ++pos;
                    }
                    continue;
                }
                ++pos;
                Tag tag;
                while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_'))
                {
                    tag.name += text[pos++];
                }
                while (true)
                {
                    skipSeparators();
                    if (pos >= text.size() || text[pos] == '@')
                    {
                        break;
                    }
                    Token token;
                    if (text[pos] == '"')
                    {
                        token.quoted = true;
                        ++pos;
                        bool closed = false;
                        while (pos < text.size())
                        {
                            const char c = text[pos++];
                            if (c == '\\' && pos < text.size() && (text[pos] == '"' || text[pos] == '\\'))
                            {
                                token.text += text[pos++];
                            }
                            else if (c == '"')
                            {
                                closed = true;
                                break;
                            }
                            else
                            {
                                token.text += c;
                            }
                        }
                        if (!closed)
                        {
                            tag.unterminated = true;
                        }
                    }
                    else
                    {
                        while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos])) && text[pos] != ',' && text[pos] != '@')
                        {
                            token.text += text[pos++];
                        }
                    }
                    tag.values.push_back(std::move(token));
                }
                tags.push_back(std::move(tag));
            }
            return tags;
        }

        bool ToNumber(const Token& token, double& out)
        {
            if (token.quoted || token.text.empty())
            {
                return false;
            }
            errno = 0;
            char* end = nullptr;
            const double value = std::strtod(token.text.c_str(), &end);
            if (end == token.text.c_str() || *end != '\0' || errno == ERANGE)
            {
                return false;
            }
            out = value;
            return true;
        }

        std::string Describe(const Token& token)
        {
            return token.quoted ? "\"" + token.text + "\"" : token.text;
        }

        std::string FormatNumber(const double value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", value);
            return text;
        }

        bool IsColorType(const ShaderValueType type)
        {
            return type == ShaderValueType::Float3 || type == ShaderValueType::Float4;
        }

        // ---- the reading of the source ----

        class Scanner
        {
        public:
            explicit Scanner(std::vector<Declaration>& declarations, std::vector<ShaderDiagnostic>& diagnostics)
                : declarations_(declarations)
                , diagnostics_(diagnostics)
            {
            }

            // `insideConstantBuffer`: the file is included from inside a cbuffer, so its declarations are variables of it
            void Scan(const std::string& text, const std::filesystem::path& directory, const bool canInclude, const std::string& displayName, const int depth, const bool insideConstantBuffer = false)
            {
                static const std::regex includePattern(R"re(^\s*#\s*include\s*"([^"]+)")re");
                static const std::regex cbufferPattern(R"(\bcbuffer\s+[A-Za-z_]\w*)");
                static const std::regex variablePattern(
                    R"(^\s*(?:(?:row_major|column_major|nointerpolation|static|const|uniform|precise|snorm|unorm|linear|centroid|noperspective|sample)\s+)*)"
                    R"([A-Za-z_]\w*(?:\s*<[^>;]*>)?\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?::\s*packoffset\s*\([^)]*\)\s*)?;)");
                static const std::regex texturePattern(
                    R"(^\s*Texture(?:1D|2D|3D|Cube)(?:Array)?(?:MS)?(?:\s*<[^>;]*>)?\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*:\s*register\s*\([^)]*\)\s*;)");

                std::vector<TagLine> pending;
                const auto reportOrphans = [&]()
                {
                    for (const TagLine& tagLine : pending)
                    {
                        Warn(diagnostics_, tagLine.file, tagLine.line, "this comment is not followed by a declaration of a variable of a constant buffer or of a texture");
                    }
                    pending.clear();
                };

                bool inBlockComment = false;
                bool pendingCbuffer = false;   // "cbuffer Name" was seen, the '{' has not
                bool inCbuffer = insideConstantBuffer;
                int depthOfBraces = 0;
                int cbufferDepth = 0;

                std::istringstream stream(text);
                std::string rawLine;
                std::uint32_t lineNumber = 0;
                while (std::getline(stream, rawLine))
                {
                    ++lineNumber;
                    while (!rawLine.empty() && (rawLine.back() == '\r' || rawLine.back() == '\n'))
                    {
                        rawLine.pop_back();
                    }

                    // The code of the line without the comments, and the text of the "//" comment
                    std::string code;
                    std::string comment;
                    bool hasComment = false;
                    for (std::size_t i = 0; i < rawLine.size(); ++i)
                    {
                        if (inBlockComment)
                        {
                            if (rawLine[i] == '*' && i + 1 < rawLine.size() && rawLine[i + 1] == '/')
                            {
                                inBlockComment = false;
                                ++i;
                                code += ' ';
                            }
                            continue;
                        }
                        if (rawLine[i] == '/' && i + 1 < rawLine.size() && rawLine[i + 1] == '/')
                        {
                            comment = rawLine.substr(i + 2);
                            hasComment = true;
                            break;
                        }
                        if (rawLine[i] == '/' && i + 1 < rawLine.size() && rawLine[i + 1] == '*')
                        {
                            inBlockComment = true;
                            ++i;
                            continue;
                        }
                        code += rawLine[i];
                    }

                    const std::string trimmedComment = Trim(comment);
                    const bool isMetadata = hasComment && !trimmedComment.empty() && trimmedComment[0] == '@';
                    const std::string trimmedCode = Trim(code);

                    if (trimmedCode.empty())
                    {
                        if (isMetadata)
                        {
                            pending.push_back({displayName, lineNumber, trimmedComment});
                        }
                        else if (!hasComment)
                        {
                            reportOrphans(); // a blank line: the comments above it were not for what comes below
                        }
                        continue;   // an ordinary comment line does not break the chain
                    }

                    std::smatch match;
                    if (trimmedCode[0] == '#')
                    {
                        reportOrphans();
                        if (canInclude && depth < kMaxIncludeDepth && std::regex_search(trimmedCode, match, includePattern))
                        {
                            Include(directory, match[1].str(), depth, inCbuffer && depthOfBraces == cbufferDepth);
                        }
                        continue;
                    }

                    std::vector<TagLine> tagLines = pending;
                    if (isMetadata)
                    {
                        tagLines.push_back({displayName, lineNumber, trimmedComment});
                    }
                    pending.clear();

                    bool declared = false;
                    if (inCbuffer && depthOfBraces == cbufferDepth)
                    {
                        if (std::regex_search(trimmedCode, match, variablePattern))
                        {
                            declared = true;
                            if (!tagLines.empty())
                            {
                                declarations_.push_back({displayName, lineNumber, match[1].str(), false, std::move(tagLines)});
                            }
                        }
                    }
                    else if (depthOfBraces == 0 && std::regex_search(trimmedCode, match, texturePattern))
                    {
                        declared = true;
                        if (!tagLines.empty())
                        {
                            declarations_.push_back({displayName, lineNumber, match[1].str(), true, std::move(tagLines)});
                        }
                    }
                    if (!declared)
                    {
                        for (const TagLine& tagLine : tagLines)
                        {
                            Warn(diagnostics_, tagLine.file, tagLine.line, "this comment is not followed by a declaration of a variable of a constant buffer or of a texture");
                        }
                    }

                    // The braces: where a constant buffer starts and ends
                    if (std::regex_search(trimmedCode, cbufferPattern))
                    {
                        pendingCbuffer = true;
                    }
                    for (const char c : trimmedCode)
                    {
                        if (c == '{')
                        {
                            ++depthOfBraces;
                            if (pendingCbuffer)
                            {
                                pendingCbuffer = false;
                                inCbuffer = true;
                                cbufferDepth = depthOfBraces;
                            }
                        }
                        else if (c == '}')
                        {
                            if (inCbuffer && depthOfBraces == cbufferDepth)
                            {
                                inCbuffer = false;
                            }
                            depthOfBraces = std::max(0, depthOfBraces - 1);
                        }
                    }
                }
                reportOrphans();
            }

        private:
            void Include(const std::filesystem::path& directory, const std::string& name, const int depth, const bool insideConstantBuffer)
            {
                const std::filesystem::path file = (directory / std::filesystem::u8path(name)).lexically_normal();
                const std::string key = file.string();
                if (!visited_.insert(key).second)
                {
                    return; // already read (or a cycle)
                }
                std::ifstream stream(file, std::ios::binary);
                if (!stream.is_open())
                {
                    return; // the compiler reports a missing include
                }
                const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                Scan(text, file.parent_path(), true, key, depth + 1, insideConstantBuffer);
            }

            std::vector<Declaration>& declarations_;
            std::vector<ShaderDiagnostic>& diagnostics_;
            std::set<std::string> visited_;
        };

        // ---- the meaning of the tags ----

        void ApplyToVariable(const Declaration& declaration, ShaderVariable& variable, std::vector<ShaderDiagnostic>& diagnostics)
        {
            ShaderVariableMetadata metadata;
            const std::uint32_t components = ValueTypeComponents(variable.type);
            std::uint32_t minLine = 0;
            std::uint32_t maxLine = 0;
            std::uint32_t defaultLine = 0;
            std::string defaultFile;

            for (const TagLine& tagLine : declaration.tagLines)
            {
                for (const Tag& tag : ParseTags(tagLine.text))
                {
                    const auto warn = [&](const std::string& message) { Warn(diagnostics, tagLine.file, tagLine.line, "'" + variable.name + "': " + message); };
                    const auto oneNumber = [&](double& out) -> bool
                    {
                        if (tag.values.size() != 1 || !ToNumber(tag.values[0], out))
                        {
                            warn("@" + tag.name + " needs one number" + (tag.values.empty() ? std::string() : ", got " + Describe(tag.values[0]) + (tag.values.size() > 1 ? " ..." : "")));
                            return false;
                        }
                        return true;
                    };

                    if (tag.name == "label")
                    {
                        if (tag.unterminated)
                        {
                            warn("@label: the quote is not closed");
                        }
                        else if (tag.values.size() != 1 || !tag.values[0].quoted)
                        {
                            warn("@label needs a text in quotes, like @label \"Roughness\"");
                        }
                        else
                        {
                            metadata.label = tag.values[0].text;
                        }
                    }
                    else if (tag.name == "min" || tag.name == "max" || tag.name == "step")
                    {
                        double value = 0.0;
                        if (!oneNumber(value))
                        {
                            continue;
                        }
                        if (tag.name == "step" && value <= 0.0)
                        {
                            warn("@step must be more than zero, got " + FormatNumber(value));
                        }
                        else if (tag.name == "min")
                        {
                            metadata.hasMin = true;
                            metadata.min = value;
                            minLine = tagLine.line;
                        }
                        else if (tag.name == "max")
                        {
                            metadata.hasMax = true;
                            metadata.max = value;
                            maxLine = tagLine.line;
                        }
                        else
                        {
                            metadata.hasStep = true;
                            metadata.step = value;
                        }
                    }
                    else if (tag.name == "default")
                    {
                        if (components == 0)
                        {
                            warn("@default is not supported for a variable of this type");
                            continue;
                        }
                        std::vector<double> values;
                        bool numbers = true;
                        for (const Token& token : tag.values)
                        {
                            double value = 0.0;
                            if (variable.type == ShaderValueType::Bool && !token.quoted && (token.text == "true" || token.text == "false"))
                            {
                                value = token.text == "true" ? 1.0 : 0.0;
                            }
                            else if (!ToNumber(token, value))
                            {
                                warn("@default: " + Describe(token) + " is not a number");
                                numbers = false;
                                break;
                            }
                            values.push_back(value);
                        }
                        if (!numbers)
                        {
                            continue;
                        }
                        if (values.size() != components)
                        {
                            warn("@default needs " + std::to_string(components) + " number" + (components == 1 ? "" : "s") + " for a " + ToString(variable.type) +
                                ", got " + std::to_string(values.size()));
                            continue;
                        }
                        metadata.defaultValue = std::move(values);
                        defaultLine = tagLine.line;
                        defaultFile = tagLine.file;
                    }
                    else if (tag.name == "color")
                    {
                        if (!tag.values.empty())
                        {
                            warn("@color takes no value, got " + Describe(tag.values[0]));
                        }
                        else if (!IsColorType(variable.type))
                        {
                            warn("@color works for a Float3 or a Float4, this is a " + ToString(variable.type));
                        }
                        else
                        {
                            metadata.color = true;
                        }
                    }
                    else if (tag.name == "hidden")
                    {
                        if (!tag.values.empty())
                        {
                            warn("@hidden takes no value, got " + Describe(tag.values[0]));
                        }
                        else
                        {
                            metadata.hidden = true;
                        }
                    }
                    else
                    {
                        warn("unknown tag @" + tag.name + " (the tags are @label, @min, @max, @step, @default, @color, @hidden)");
                    }
                }
            }

            // The range: both bounds are dropped when they contradict each other; a default outside stays, with a warning
            if (metadata.hasMin && metadata.hasMax && metadata.min > metadata.max)
            {
                Warn(diagnostics, declaration.file, std::max(minLine, maxLine), "'" + variable.name + "': @min " + FormatNumber(metadata.min) + " is more than @max " +
                    FormatNumber(metadata.max) + ": both are ignored");
                metadata.hasMin = false;
                metadata.hasMax = false;
            }
            if (!metadata.defaultValue.empty() && (metadata.hasMin || metadata.hasMax))
            {
                for (const double component : metadata.defaultValue)
                {
                    if ((metadata.hasMin && component < metadata.min) || (metadata.hasMax && component > metadata.max))
                    {
                        Warn(diagnostics, defaultFile, defaultLine, "'" + variable.name + "': @default " + FormatNumber(component) + " is outside of [" +
                            (metadata.hasMin ? FormatNumber(metadata.min) : std::string("-inf")) + ", " + (metadata.hasMax ? FormatNumber(metadata.max) : std::string("inf")) + "]; it stays");
                        break;
                    }
                }
            }
            variable.metadata = std::move(metadata);
        }

        void ApplyToTexture(const Declaration& declaration, ShaderResource& resource, std::vector<ShaderDiagnostic>& diagnostics)
        {
            for (const TagLine& tagLine : declaration.tagLines)
            {
                for (const Tag& tag : ParseTags(tagLine.text))
                {
                    const auto warn = [&](const std::string& message) { Warn(diagnostics, tagLine.file, tagLine.line, "'" + resource.name + "': " + message); };
                    if (tag.name == "label")
                    {
                        if (tag.unterminated)
                        {
                            warn("@label: the quote is not closed");
                        }
                        else if (tag.values.size() != 1 || !tag.values[0].quoted)
                        {
                            warn("@label needs a text in quotes, like @label \"Texture\"");
                        }
                        else
                        {
                            resource.label = tag.values[0].text;
                        }
                    }
                    else if (tag.name == "hidden")
                    {
                        if (!tag.values.empty())
                        {
                            warn("@hidden takes no value, got " + Describe(tag.values[0]));
                        }
                        else
                        {
                            resource.hidden = true;
                        }
                    }
                    else if (tag.name == "min" || tag.name == "max" || tag.name == "step" || tag.name == "default" || tag.name == "color")
                    {
                        warn("@" + tag.name + " does not work for a texture (only @label and @hidden do)");
                    }
                    else
                    {
                        warn("unknown tag @" + tag.name + " (a texture takes @label and @hidden)");
                    }
                }
            }
        }
    }

    void ApplyShaderMetadata(const std::filesystem::path& file, const std::string& source, const std::string& sourceName, ShaderLayout& layout, std::vector<ShaderDiagnostic>& diagnostics)
    {
        std::vector<Declaration> declarations;
        Scanner scanner(declarations, diagnostics);
        scanner.Scan(source, file.parent_path(), !file.empty(), sourceName, 0);

        for (const Declaration& declaration : declarations)
        {
            if (!declaration.texture)
            {
                bool found = false;
                for (ShaderConstantBuffer& buffer : layout.constantBuffers)
                {
                    for (ShaderVariable& variable : buffer.variables)
                    {
                        if (variable.name == declaration.name)
                        {
                            ApplyToVariable(declaration, variable, diagnostics);
                            found = true;
                        }
                    }
                }
                if (!found)
                {
                    Warn(diagnostics, declaration.file, declaration.tagLines.front().line,
                        "'" + declaration.name + "' is not a variable of a constant buffer in the layout of the shader");
                }
            }
            else
            {
                bool found = false;
                for (ShaderResource& resource : layout.resources)
                {
                    if (resource.type == ShaderResourceType::Texture && resource.name == declaration.name)
                    {
                        ApplyToTexture(declaration, resource, diagnostics);
                        found = true;
                    }
                }
                if (!found)
                {
                    Warn(diagnostics, declaration.file, declaration.tagLines.front().line,
                        "'" + declaration.name + "' is not a texture in the layout of the shader (a texture that the shader does not use is not bound)");
                }
            }
        }
    }
}
