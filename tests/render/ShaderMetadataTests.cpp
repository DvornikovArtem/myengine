// B2 (LR3): the metadata of the shader parameters, from the comments of the HLSL ("// @label ... @default ..."), without a GPU.
// The layouts come from the reflection of real HLSL files (tests/render/shaders), the metadata from the parser of the comments.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <myengine/render/MaterialParams.h>
#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/ShaderMetadata.h>

#include "ShaderCompiler.h"
#include "ShaderReflector.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine::render;
    using namespace myengine::render::dx12;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    fs::path ShadersDirectory()
    {
        return fs::u8path(MYENGINE_SOURCE_DIR) / "tests/render/shaders";
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        Check(stream.is_open(), "cannot read " + path.u8string());
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        Check(static_cast<bool>(stream), "cannot write " + path.u8string());
    }

    // The 1-based line of the n-th (from 0) occurrence of `part`
    std::uint32_t LineOf(const std::string& text, const std::string& part, const int occurrence = 0)
    {
        std::size_t at = std::string::npos;
        for (int i = 0; i <= occurrence; ++i)
        {
            at = text.find(part, at == std::string::npos ? 0 : at + part.size());
            Check(at != std::string::npos, "no '" + part + "' in the text");
        }
        return static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
    }

    ShaderProgramData Program(const fs::path& hlsl)
    {
        ShaderProgramData program;
        program.sourcePath = hlsl;
        program.vertexEntry = "VSMain";
        program.pixelEntry = "PSMain";
        return program;
    }

    ShaderCompileResult Compile(ShaderReflector& reflector, const fs::path& hlsl)
    {
        const ShaderCompileResult result = reflector.CompileProgram(Program(hlsl));
        std::string messages;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            messages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(result.ok && result.program != nullptr, hlsl.filename().u8string() + " must compile; a mistake in a comment is a warning only:" + messages);
        return result;
    }

    std::vector<ShaderDiagnostic> MetadataWarnings(const ShaderCompileResult& result)
    {
        std::vector<ShaderDiagnostic> warnings;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            if (diagnostic.message.rfind("shader metadata:", 0) == 0)
            {
                Check(diagnostic.warning, "a metadata message must be a warning: " + FormatShaderDiagnostic(diagnostic));
                warnings.push_back(diagnostic);
            }
        }
        return warnings;
    }

    std::string AllWarnings(const std::vector<ShaderDiagnostic>& warnings)
    {
        std::string text;
        for (const ShaderDiagnostic& warning : warnings)
        {
            text += "\n  " + FormatShaderDiagnostic(warning);
        }
        return text;
    }

    const ShaderVariableMetadata& Meta(const ShaderLayout& layout, const std::string& name)
    {
        const ShaderVariableRef found = layout.FindVariable(name);
        Check(found.variable != nullptr, "no variable '" + name + "' in the layout:\n" + FormatShaderLayout(layout));
        return found.variable->metadata;
    }

    const ShaderResource& Texture(const ShaderLayout& layout, const std::string& name)
    {
        const ShaderResource* resource = layout.FindResourceByName(name);
        Check(resource != nullptr, "no resource '" + name + "' in the layout:\n" + FormatShaderLayout(layout));
        return *resource;
    }

    bool Is(const std::vector<double>& values, const std::vector<double>& expected)
    {
        return values == expected;
    }

    // ---- every tag ----

    void TestAllTags()
    {
        ShaderReflector reflector;
        const ShaderCompileResult result = Compile(reflector, ShadersDirectory() / "metadata_main.hlsl");
        const ShaderLayout& layout = result.program->layout;
        const auto warnings = MetadataWarnings(result);
        Check(warnings.empty(), "no warnings for a shader that is written right:" + AllWarnings(warnings));

        const ShaderVariableMetadata& roughness = Meta(layout, "gRoughness");
        Check(roughness.label == "Roughness" && roughness.hasMin && roughness.min == 0.0 && roughness.hasMax && roughness.max == 1.0 &&
              roughness.hasStep && roughness.step == 0.01 && Is(roughness.defaultValue, {0.5}) && !roughness.color && !roughness.hidden,
            "all the tags on the line above: label, min, max, step, default");

        const ShaderVariableMetadata& tint = Meta(layout, "gTint");
        Check(tint.color && tint.label == "Tint" && Is(tint.defaultValue, {1.0, 0.75, 0.12, 1.0}), "the comment on the line of the declaration: color, label, default of four components");

        const ShaderVariableMetadata& scale = Meta(layout, "gScale");
        Check(scale.label == "Шероховатость, а не Roughness", "a label with spaces, a comma and Cyrillic letters: '" + scale.label + "'");
        Check(scale.hasMin && scale.min == 0.0 && scale.hasMax && scale.max == 10.0, "several comment lines above add up; an ordinary comment between them does not break the chain");

        const ShaderVariableMetadata& glow = Meta(layout, "gGlow");
        Check(glow.color && Is(glow.defaultValue, {0.0, 0.5, 1.0}), "a default with commas, a Float3 colour");

        const ShaderVariableMetadata& uv = Meta(layout, "gUv");
        Check(uv.hidden && Is(uv.defaultValue, {2.0, 3.0}) && !uv.color, "a default of two components and a flag after it");

        const ShaderVariableMetadata& mode = Meta(layout, "gMode");
        Check(mode.hasMin && mode.hasMax && mode.max == 3.0 && Is(mode.defaultValue, {1.0}), "an int with a range");
        Check(Is(Meta(layout, "gFlag").defaultValue, {1.0}), "a bool default 'true' is 1");
        Check(Meta(layout, "gPlain").Empty(), "a variable without comments has no metadata");
        Check(Meta(layout, "gModel").hidden, "@hidden on the line above");

        const ShaderVariableMetadata& shared = Meta(layout, "gShared");
        Check(shared.label == "From the include" && shared.min == -1.0 && shared.max == 1.0 && Is(shared.defaultValue, {0.25}), "the metadata of a variable that comes from an #include inside the cbuffer");

        Check(Texture(layout, "gAlbedo").label == "Albedo map" && !Texture(layout, "gAlbedo").hidden, "a texture: @label above");
        Check(Texture(layout, "gMask").hidden && Texture(layout, "gMask").label == "Mask", "a texture: @hidden and @label in the line of the declaration");

        // The table for people (--dump-shader-layouts, the Shader Layouts window) shows it
        const std::string table = FormatShaderLayout(layout);
        Check(Contains(table, "label \"Tint\", color, default 1 0.75 0.12 1"), "the table shows the metadata of gTint:\n" + table);
        Check(Contains(table, "label \"Roughness\", min 0, max 1, step 0.01, default 0.5"), "and of gRoughness:\n" + table);
        Check(Contains(table, "label \"Albedo map\""), "and of a texture:\n" + table);
    }

    // ---- every mistake: one warning, the right file and line ----

    void TestMistakes()
    {
        const fs::path errors = ShadersDirectory() / "metadata_errors.hlsl";
        const fs::path bad = ShadersDirectory() / "metadata_bad.hlsli";
        const std::string text = ReadText(errors);
        ShaderReflector reflector;
        const ShaderCompileResult result = Compile(reflector, errors); // the shader compiles and the program is made, whatever the comments say
        const auto warnings = MetadataWarnings(result);

        struct Expected
        {
            std::string file;
            std::uint32_t line;
            std::string part;
        };
        const std::string mainFile = errors.string();
        const std::vector<Expected> expected = {
            {mainFile, LineOf(text, "float gBadTag;"), "unknown tag @frobnicate"},
            {mainFile, LineOf(text, "float gBadDefault;"), "@default: abc is not a number"},
            {mainFile, LineOf(text, "float3 gShort;"), "@default needs 3 numbers for a Float3, got 2"},
            {mainFile, LineOf(text, "float gRange;"), "@min 5 is more than @max 1: both are ignored"},
            {mainFile, LineOf(text, "float gOutside;"), "@default 2 is outside of [0, 1]; it stays"},
            {mainFile, LineOf(text, "float gNotColor;"), "@color works for a Float3 or a Float4, this is a Float"},
            {mainFile, LineOf(text, "float gNoQuotes;"), "@label needs a text in quotes"},
            {mainFile, LineOf(text, "// @label \"this texture is not bound\""), "'gUnused' is not a texture in the layout"},
            {mainFile, LineOf(text, "// @label \"separated by a blank line\""), "not followed by a declaration"},
            {mainFile, LineOf(text, "Texture2D gUsed"), "@min does not work for a texture"},
            {mainFile, LineOf(text, "// @hidden", 0), "not followed by a declaration"},
            {mainFile, LineOf(text, "// @hidden", 1), "not followed by a declaration"},
        };
        for (const Expected& item : expected)
        {
            int found = 0;
            for (const ShaderDiagnostic& warning : warnings)
            {
                if (warning.file == item.file && warning.line == item.line && Contains(warning.message, item.part))
                {
                    ++found;
                }
            }
            Check(found == 1, "exactly one warning '" + item.part + "' at line " + std::to_string(item.line) + ", " + std::to_string(found) + " found:" + AllWarnings(warnings));
        }

        // A mistake in an include names the include and its line
        const std::string badText = ReadText(bad);
        int includeWarnings = 0;
        for (const ShaderDiagnostic& warning : warnings)
        {
            if (warning.file.size() >= 18 && warning.file.compare(warning.file.size() - 18, 18, "metadata_bad.hlsli") == 0)
            {
                ++includeWarnings;
                Check(warning.line == LineOf(badText, "float gIncludeBad;") && Contains(warning.message, "unknown tag @colour"),
                    "the warning of the include: its file and line: " + FormatShaderDiagnostic(warning));
            }
        }
        Check(includeWarnings == 1, "exactly one warning from the include");

        Check(warnings.size() == expected.size() + 1, "no other warnings (" + std::to_string(expected.size() + 1) + " expected):" + AllWarnings(warnings));
        Check(Contains(FormatShaderDiagnostic(warnings.front()), "warning: shader metadata:"), "the message is formatted like the others: " + FormatShaderDiagnostic(warnings.front()));

        // What was right is applied, what was wrong is not
        const ShaderLayout& layout = result.program->layout;
        const ShaderVariableMetadata& fine = Meta(layout, "gFine");
        Check(fine.label == "fine" && fine.hasMin && fine.hasMax && Is(fine.defaultValue, {0.5}), "a correct line next to the mistakes is applied");
        Check(Meta(layout, "gBadTag").Empty(), "an unknown tag adds nothing");
        Check(Meta(layout, "gBadDefault").defaultValue.empty() && Meta(layout, "gShort").defaultValue.empty(), "a wrong default is not stored");
        Check(!Meta(layout, "gRange").hasMin && !Meta(layout, "gRange").hasMax, "min > max: both are ignored");
        const ShaderVariableMetadata& outside = Meta(layout, "gOutside");
        Check(Is(outside.defaultValue, {2.0}) && outside.hasMin && outside.hasMax, "a default outside the range stays, the range stays");
        Check(!Meta(layout, "gNotColor").color, "@color on a Float is dropped");
        Check(Meta(layout, "gNoQuotes").label.empty(), "a label without quotes is dropped");
        Check(Meta(layout, "gIncludeBad").Empty(), "an unknown tag in the include adds nothing");
        Check(Meta(layout, "gSeparated").Empty(), "a comment that a blank line separates from the declaration does not belong to it");
    }

    // ---- @default in the material parameters ----

    float FloatAt(const MaterialParams& params, const std::string& name, const std::size_t component = 0)
    {
        const ShaderVariableRef found = params.Layout().FindVariable(name);
        Check(found.variable != nullptr, "no variable " + name);
        const std::size_t index = static_cast<std::size_t>(found.buffer - params.Layout().constantBuffers.data());
        float value = 0.0f;
        std::memcpy(&value, params.ConstantBufferBytes(index).data() + found.variable->offset + component * sizeof(float), sizeof(float));
        return value;
    }

    std::int32_t IntAt(const MaterialParams& params, const std::string& name)
    {
        const ShaderVariableRef found = params.Layout().FindVariable(name);
        Check(found.variable != nullptr, "no variable " + name);
        const std::size_t index = static_cast<std::size_t>(found.buffer - params.Layout().constantBuffers.data());
        std::int32_t value = 0;
        std::memcpy(&value, params.ConstantBufferBytes(index).data() + found.variable->offset, sizeof(value));
        return value;
    }

    MaterialTextureResolver AnyTexture()
    {
        return [](const std::string&) { return TextureHandle{1}; };
    }

    void TestDefaultsInMaterialParams()
    {
        ShaderReflector reflector;
        const ShaderLayout layout = Compile(reflector, ShadersDirectory() / "metadata_main.hlsl").program->layout;

        // A variable that the material does not set is its @default, not zero
        const MaterialParams params(layout);
        Check(FloatAt(params, "gRoughness") == 0.5f, "gRoughness is 0.5 by default");
        Check(FloatAt(params, "gTint", 0) == 1.0f && FloatAt(params, "gTint", 1) == 0.75f && FloatAt(params, "gTint", 2) == 0.12f && FloatAt(params, "gTint", 3) == 1.0f, "gTint is its four components");
        Check(FloatAt(params, "gGlow", 1) == 0.5f && FloatAt(params, "gGlow", 2) == 1.0f, "a Float3 default");
        Check(FloatAt(params, "gUv", 0) == 2.0f && FloatAt(params, "gUv", 1) == 3.0f, "a Float2 default");
        Check(IntAt(params, "gMode") == 1 && IntAt(params, "gFlag") == 1, "an int and a bool default");
        Check(FloatAt(params, "gShared") == 0.25f, "a default that comes from an include");
        Check(FloatAt(params, "gScale") == 0.0f && FloatAt(params, "gPlain") == 0.0f, "without a @default: zero");
        Check(FloatAt(params, "gModel", 0) == 0.0f && FloatAt(params, "gModel", 5) == 0.0f, "the matrix has no default");

        // The value from the material covers it
        MaterialParams covered(layout);
        MaterialValues values;
        values.params["gRoughness"] = MaterialValue::FromNumber(0.2);
        values.params["gTint"] = MaterialValue::FromVector({0.0, 0.0, 1.0, 1.0});
        const auto messages = ApplyMaterialValues(covered, values, AnyTexture(), "metadata_main.hlsl");
        Check(messages.empty(), "the values fit the layout: " + (messages.empty() ? std::string() : messages.front().message));
        Check(FloatAt(covered, "gRoughness") == 0.2f && FloatAt(covered, "gTint", 2) == 1.0f && FloatAt(covered, "gTint", 0) == 0.0f, "the material covers the defaults it names");
        Check(FloatAt(covered, "gShared") == 0.25f && IntAt(covered, "gMode") == 1, "and the others stay their defaults");
        // A value of the wrong type is rejected: the variable stays at its default (the @default, or zero without one)
        MaterialParams wrong(layout);
        MaterialValues wrongValues;
        wrongValues.params["gRoughness"] = MaterialValue::FromBool(true);
        wrongValues.params["gScale"] = MaterialValue::FromBool(true);
        const auto wrongMessages = ApplyMaterialValues(wrong, wrongValues, AnyTexture(), "metadata_main.hlsl");
        Check(wrongMessages.size() == 2 && wrongMessages[0].IsError() && wrongMessages[1].IsError(), "two type errors");
        Check(FloatAt(wrong, "gRoughness") == 0.5f && FloatAt(wrong, "gScale") == 0.0f, "after a rejected value: the @default, or zero when the shader gives none");
        MaterialParams byHand(layout);
        Check(byHand.SetFloat("gRoughness", 0.9f).IsOk() && FloatAt(byHand, "gRoughness") == 0.9f, "a setter covers it too");

        // The engine semantics are not touched, whatever a comment says
        ShaderLayout semantic;
        ShaderConstantBuffer buffer;
        buffer.name = "DrawCB";
        buffer.sizeBytes = 16;
        ShaderVariable light;
        light.name = "gLightDirection";
        light.type = ShaderValueType::Float4;
        light.size = 16;
        light.metadata.defaultValue = {1.0, 2.0, 3.0, 4.0};
        buffer.variables.push_back(light);
        semantic.constantBuffers.push_back(buffer);
        const MaterialParams semanticParams(semantic);
        Check(FloatAt(semanticParams, "gLightDirection", 0) == 0.0f, "a @default on gLightDirection is not written: the renderer owns it");
    }

    void TestGColorDefault()
    {
        ShaderReflector reflector;
        const ShaderLayout withDefault = Compile(reflector, ShadersDirectory() / "metadata_color.hlsl").program->layout;
        const ShaderLayout withoutDefault = Compile(reflector, ShadersDirectory() / "material_probe.hlsl").program->layout;
        const auto colour = [](const MaterialParams& params) { return std::array<float, 4>{FloatAt(params, "gColor", 0), FloatAt(params, "gColor", 1), FloatAt(params, "gColor", 2), FloatAt(params, "gColor", 3)}; };

        // A material that says nothing about the colour: the @default of the shader, and white when the shader gives none
        MaterialParams a(withDefault);
        Check(ApplyMaterialValues(a, MaterialValues{}, AnyTexture()).empty(), "an empty material applies");
        Check(colour(a) == std::array<float, 4>({0.5f, 0.25f, 0.125f, 1.0f}), "no tint, no gColor: the @default of the shader");
        MaterialParams b(withoutDefault);
        ApplyMaterialValues(b, MaterialValues{}, AnyTexture());
        Check(colour(b) == std::array<float, 4>({1.0f, 1.0f, 1.0f, 1.0f}), "no tint, no gColor, no @default: white, as the old materials had it");

        // A tint that the file says, white included, is what it says
        MaterialParams c(withDefault);
        MaterialValues white;
        white.hasTint = true;
        ApplyMaterialValues(c, white, AnyTexture());
        Check(colour(c) == std::array<float, 4>({1.0f, 1.0f, 1.0f, 1.0f}), "an explicit white tint covers the @default");
        MaterialParams d(withDefault);
        MaterialValues tinted;
        tinted.tint = {0.1f, 0.2f, 0.3f, 1.0f};
        ApplyMaterialValues(d, tinted, AnyTexture());
        Check(colour(d) == std::array<float, 4>({0.1f, 0.2f, 0.3f, 1.0f}), "a tint covers the @default");
        MaterialParams e(withDefault);
        MaterialValues named;
        named.params["gColor"] = MaterialValue::FromVector({0.9, 0.8, 0.7, 1.0});
        ApplyMaterialValues(e, named, AnyTexture());
        Check(colour(e) == std::array<float, 4>({0.9f, 0.8f, 0.7f, 1.0f}), "params.gColor covers the @default");
    }

    // ---- the real shader of the project ----

    void TestTheEngineShader()
    {
        ShaderReflector reflector;
        const ShaderCompileResult result = Compile(reflector, fs::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders/textured_lit.hlsl");
        const ShaderLayout& layout = result.program->layout;
        const auto warnings = MetadataWarnings(result);
        Check(warnings.empty(), "textured_lit.hlsl has no metadata warnings:" + AllWarnings(warnings));

        const ShaderVariableMetadata& colour = Meta(layout, "gColor");
        Check(colour.color && colour.label == "Tint" && Is(colour.defaultValue, {1.0, 1.0, 1.0, 1.0}), "gColor: a colour, the label Tint, white by default");
        for (const char* semantic : {"gModel", "gViewProjection", "gLightDirection"})
        {
            Check(Meta(layout, semantic).hidden, std::string(semantic) + " is hidden: the renderer writes it");
        }
        Check(Texture(layout, "gTexture").label == "Texture", "gTexture has the label Texture");

        // The default is the white that the old materials got
        MaterialParams params(layout);
        MaterialValues nothing;
        ApplyMaterialValues(params, nothing, AnyTexture());
        Check(FloatAt(params, "gColor", 0) == 1.0f && FloatAt(params, "gColor", 3) == 1.0f, "a material without a tint is white on textured_lit, as before");
    }

    // ---- a changed source gives new metadata ----

    void TestMetadataFollowsTheSource(const fs::path& directory)
    {
        const fs::path hlsl = directory / "metadata_main.hlsl";
        const fs::path include = directory / "metadata_ok.hlsli";
        fs::copy_file(ShadersDirectory() / "metadata_main.hlsl", hlsl, fs::copy_options::overwrite_existing);
        fs::copy_file(ShadersDirectory() / "metadata_ok.hlsli", include, fs::copy_options::overwrite_existing);

        ShaderReflector reflector;
        const ShaderCompileResult first = Compile(reflector, hlsl);
        Check(!first.fromCache && Is(Meta(first.program->layout, "gRoughness").defaultValue, {0.5}), "the first compile");
        const ShaderCompileResult again = Compile(reflector, hlsl);
        Check(again.fromCache && Is(Meta(again.program->layout, "gRoughness").defaultValue, {0.5}), "the same text: the cached program, with its metadata");

        // A comment changed: a new program with the new metadata (the cache key is the text, includes included)
        std::string text = ReadText(hlsl);
        const auto at = text.find("@default 0.5");
        Check(at != std::string::npos, "the test shader has the default 0.5");
        text.replace(at, std::strlen("@default 0.5"), "@default 0.75");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        WriteText(hlsl, text);
        const ShaderCompileResult edited = Compile(reflector, hlsl);
        Check(!edited.fromCache && Is(Meta(edited.program->layout, "gRoughness").defaultValue, {0.75}), "an edited comment gives new metadata");

        // A comment of the include changed
        std::string includeText = ReadText(include);
        const auto inIncludeAt = includeText.find("@default 0.25");
        Check(inIncludeAt != std::string::npos, "the include has the default 0.25");
        includeText.replace(inIncludeAt, std::strlen("@default 0.25"), "@default 0.5");
        WriteText(include, includeText);
        const ShaderCompileResult editedInclude = Compile(reflector, hlsl);
        Check(!editedInclude.fromCache && Is(Meta(editedInclude.program->layout, "gShared").defaultValue, {0.5}), "an edited comment of an #include gives new metadata too");

        // A mistake introduced by an edit is a warning: the program is still made (a hot reload does not stop)
        std::string broken = ReadText(hlsl);
        broken.replace(broken.find("@min 0 @max 3"), std::strlen("@min 0 @max 3"), "@min 9 @max 3");
        WriteText(hlsl, broken);
        const ShaderCompileResult withMistake = Compile(reflector, hlsl);
        Check(MetadataWarnings(withMistake).size() == 1 && withMistake.ok, "a new mistake: one warning, the program is built");
    }
}

int main()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine B2 метаданные " + std::to_string(stamp));
    int result = 0;
    try
    {
        fs::create_directories(directory);
        TestAllTags();
        TestMistakes();
        TestDefaultsInMaterialParams();
        TestGColorDefault();
        TestTheEngineShader();
        TestMetadataFollowsTheSource(directory);
        std::cout << "OK: shader metadata: every tag, every mistake with its file and line, includes, @default in the material parameters, gColor, textured_lit, a source that changes\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    return result;
}
