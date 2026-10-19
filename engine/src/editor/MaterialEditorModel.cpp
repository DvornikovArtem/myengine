// MaterialEditorModel.cpp

#include <myengine/editor/MaterialEditorModel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace myengine::editor
{
    namespace
    {
        using render::ShaderValueType;
        using resource::MaterialAsset;

        const std::array<float, 4> kWhite{1.0f, 1.0f, 1.0f, 1.0f};

        std::string Number(const double value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", value);
            return text;
        }

        std::string Numbers(const std::vector<double>& values)
        {
            std::string text;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                text += (i > 0 ? " " : "") + Number(values[i]);
            }
            return text;
        }

        // The digits after the point that a step needs: 0.01 -> 2, 0.5 -> 1, 1 -> 0
        int DecimalsOf(const double step)
        {
            for (int digits = 0; digits <= 6; ++digits)
            {
                const double scaled = step * std::pow(10.0, digits);
                if (std::fabs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, std::fabs(scaled)))
                {
                    return digits;
                }
            }
            return 6;
        }

        bool IsScalarNumber(const ShaderValueType type)
        {
            return type == ShaderValueType::Float || type == ShaderValueType::Int || type == ShaderValueType::UInt;
        }

        MaterialWidget WidgetOf(const render::ShaderVariable& variable)
        {
            if (variable.arrayCount > 1)
            {
                return MaterialWidget::Unsupported;
            }
            switch (variable.type)
            {
                case ShaderValueType::Float:
                    return MaterialWidget::Float;
                case ShaderValueType::Float2:
                    return MaterialWidget::Vector2;
                case ShaderValueType::Float3:
                    return variable.metadata.color ? MaterialWidget::Color3 : MaterialWidget::Vector3;
                case ShaderValueType::Float4:
                    return variable.metadata.color ? MaterialWidget::Color4 : MaterialWidget::Vector4;
                case ShaderValueType::Int:
                    return MaterialWidget::Int;
                case ShaderValueType::UInt:
                    return MaterialWidget::UInt;
                case ShaderValueType::Bool:
                    return MaterialWidget::Bool;
                case ShaderValueType::Float4x4:
                    return MaterialWidget::Matrix;
                default:
                    return MaterialWidget::Unsupported;
            }
        }

        // The components of a variable in the bytes of the temporary MaterialParams: what the frame uses
        std::vector<double> ReadValue(const render::MaterialParams& params, const std::size_t bufferIndex, const render::ShaderVariable& variable)
        {
            const std::size_t index = bufferIndex;
            const std::uint8_t* bytes = params.ConstantBufferBytes(index).data() + variable.offset;
            const std::uint32_t components = render::ValueTypeComponents(variable.type);
            std::vector<double> value;
            switch (variable.type)
            {
                case ShaderValueType::Float:
                case ShaderValueType::Float2:
                case ShaderValueType::Float3:
                case ShaderValueType::Float4:
                    for (std::uint32_t i = 0; i < components; ++i)
                    {
                        float component = 0.0f;
                        std::memcpy(&component, bytes + i * sizeof(float), sizeof(float));
                        value.push_back(component);
                    }
                    break;
                case ShaderValueType::Int:
                {
                    std::int32_t component = 0;
                    std::memcpy(&component, bytes, sizeof(component));
                    value.push_back(component);
                    break;
                }
                case ShaderValueType::UInt:
                {
                    std::uint32_t component = 0;
                    std::memcpy(&component, bytes, sizeof(component));
                    value.push_back(component);
                    break;
                }
                case ShaderValueType::Bool:
                {
                    std::uint32_t component = 0;
                    std::memcpy(&component, bytes, sizeof(component));
                    value.push_back(component != 0 ? 1.0 : 0.0);
                    break;
                }
                default:
                    break; // matrices and the rest are not shown
            }
            return value;
        }

        std::string Detail(const MaterialRow& row)
        {
            std::vector<std::string> parts;
            if (row.hasMin && row.hasMax)
            {
                parts.push_back("Range " + Number(row.min) + " - " + Number(row.max));
            }
            else if (row.hasMin)
            {
                parts.push_back("Min " + Number(row.min));
            }
            else if (row.hasMax)
            {
                parts.push_back("Max " + Number(row.max));
            }
            if (row.hasStep)
            {
                parts.push_back("step " + Number(row.step));
            }
            if (row.hasDefault)
            {
                parts.push_back("default " + Numbers(row.defaultValue));
            }
            std::string text;
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                text += (i > 0 ? ", " : "") + parts[i];
            }
            return text;
        }

        std::string DescribeValue(const render::MaterialValue& value)
        {
            if (value.kind == render::MaterialValue::Kind::Bool)
            {
                return !value.numbers.empty() && value.numbers[0] != 0.0 ? "true" : "false";
            }
            if (value.kind == render::MaterialValue::Kind::Matrix)
            {
                return "matrix 4x4";
            }
            std::string text;
            for (std::size_t i = 0; i < value.numbers.size(); ++i)
            {
                text += (i > 0 ? ", " : "") + Number(value.numbers[i]);
            }
            return text;
        }

        bool PathInTextures(const MaterialAsset& asset, const std::string& path)
        {
            return std::any_of(asset.textures.begin(), asset.textures.end(), [&path](const auto& entry) { return entry.second == path; });
        }

        bool IsTintRow(const MaterialRow& row)
        {
            return row.isTint;
        }

        bool SameTint(const core::Color& a, const core::Color& b)
        {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        }

        core::Color DefaultTint(const MaterialRow& row)
        {
            if (row.defaultValue.size() == 4)
            {
                return {static_cast<float>(row.defaultValue[0]), static_cast<float>(row.defaultValue[1]), static_cast<float>(row.defaultValue[2]), static_cast<float>(row.defaultValue[3])};
            }
            return {kWhite[0], kWhite[1], kWhite[2], kWhite[3]};
        }

        std::uint64_t Mix(std::uint64_t hash, const void* data, const std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            }
            return hash;
        }

        std::uint64_t MixText(std::uint64_t hash, const std::string& text)
        {
            hash = Mix(hash, text.data(), text.size());
            const std::uint8_t end = 0;
            return Mix(hash, &end, 1);
        }

        // The metadata of the layout (labels, ranges, defaults, flags): MaterialLayoutHash does not cover it
        std::uint64_t MetadataHash(const render::ShaderLayout& layout)
        {
            std::uint64_t hash = 1469598103934665603ull;
            for (const render::ShaderConstantBuffer& buffer : layout.constantBuffers)
            {
                for (const render::ShaderVariable& variable : buffer.variables)
                {
                    const render::ShaderVariableMetadata& meta = variable.metadata;
                    hash = MixText(hash, variable.name);
                    hash = MixText(hash, meta.label);
                    const std::uint8_t flags = static_cast<std::uint8_t>((meta.hasMin ? 1 : 0) | (meta.hasMax ? 2 : 0) | (meta.hasStep ? 4 : 0) | (meta.color ? 8 : 0) | (meta.hidden ? 16 : 0));
                    hash = Mix(hash, &flags, 1);
                    hash = Mix(hash, &meta.min, sizeof(double));
                    hash = Mix(hash, &meta.max, sizeof(double));
                    hash = Mix(hash, &meta.step, sizeof(double));
                    hash = Mix(hash, meta.defaultValue.data(), meta.defaultValue.size() * sizeof(double));
                }
            }
            for (const render::ShaderResource& resource : layout.resources)
            {
                hash = MixText(hash, resource.name);
                hash = MixText(hash, resource.label);
                const std::uint8_t hidden = resource.hidden ? 1 : 0;
                hash = Mix(hash, &hidden, 1);
            }
            return hash;
        }
    }

    const MaterialRow* MaterialEditorModel::FindParameter(const std::string& name) const
    {
        for (const MaterialRow& row : parameters)
        {
            if (row.name == name)
            {
                return &row;
            }
        }
        return nullptr;
    }

    const MaterialRow* MaterialEditorModel::FindTexture(const std::string& name) const
    {
        for (const MaterialRow& row : textures)
        {
            if (row.name == name)
            {
                return &row;
            }
        }
        return nullptr;
    }

    MaterialEditorModel BuildMaterialEditorModel(const render::ShaderLayout& layout, const MaterialAsset& asset, const MaterialEditorOptions& options)
    {
        MaterialEditorModel model;

        // The material applied to a temporary MaterialParams: the values of the rows are its bytes, the textures are its slots,
        // and what does not fit is its messages. A path gets a number instead of a texture, to find the path of a slot again.
        render::MaterialParams params(layout);
        std::unordered_map<std::string, std::uint32_t> pathIds;
        std::vector<std::string> paths;
        model.messages = render::ApplyMaterialValues(params, resource::ToMaterialValues(asset),
            [&pathIds, &paths](const std::string& path) -> render::TextureHandle
            {
                const auto found = pathIds.find(path);
                if (found != pathIds.end())
                {
                    return {found->second};
                }
                paths.push_back(path);
                const std::uint32_t id = static_cast<std::uint32_t>(paths.size());
                pathIds[path] = id;
                return {id};
            });

        // ---- the parameters ----
        for (std::size_t bufferIndex = 0; bufferIndex < layout.constantBuffers.size(); ++bufferIndex)
        {
            for (const render::ShaderVariable& variable : layout.constantBuffers[bufferIndex].variables)
            {
                MaterialRow row;
                row.name = variable.name;
                row.label = variable.metadata.label.empty() ? variable.name : variable.metadata.label;
                row.type = variable.type;
                row.typeName = render::ToString(variable.type);
                row.tooltip = variable.name + " (" + row.typeName + ")";
                row.widget = WidgetOf(variable);
                row.isTint = variable.name == "gColor" && variable.type == ShaderValueType::Float4 && variable.arrayCount == 1;

                const render::ShaderVariableMetadata& meta = variable.metadata;
                const bool colour = row.widget == MaterialWidget::Color3 || row.widget == MaterialWidget::Color4;
                if (!colour) // the range of a colour is not used
                {
                    row.hasMin = meta.hasMin;
                    row.hasMax = meta.hasMax;
                    row.min = meta.min;
                    row.max = meta.max;
                }
                row.hasStep = meta.hasStep && (row.widget == MaterialWidget::Float || row.widget == MaterialWidget::Vector2 ||
                    row.widget == MaterialWidget::Vector3 || row.widget == MaterialWidget::Vector4);
                row.step = meta.step;
                if (row.widget == MaterialWidget::UInt && (!row.hasMin || row.min < 0.0))
                {
                    row.hasMin = true;
                    row.min = 0.0;
                }
                row.slider = IsScalarNumber(variable.type) && row.widget != MaterialWidget::Unsupported && row.hasMin && row.hasMax;
                row.decimals = row.hasStep ? DecimalsOf(row.step) : (row.widget == MaterialWidget::Float ? 3 : 2);
                row.dragSpeed = row.hasStep ? row.step : ((row.widget == MaterialWidget::Int || row.widget == MaterialWidget::UInt) ? 0.2 : 0.01);
                row.hasDefault = !meta.defaultValue.empty() && meta.defaultValue.size() == render::ValueTypeComponents(variable.type);
                if (row.hasDefault)
                {
                    row.defaultValue = meta.defaultValue;
                }
                row.detail = Detail(row);

                const bool supported = row.widget != MaterialWidget::Unsupported && row.widget != MaterialWidget::Matrix;
                row.hasKey = asset.params.count(variable.name) > 0 || (row.isTint && asset.tintSet);
                if (supported)
                {
                    row.value = ReadValue(params, bufferIndex, variable);
                }

                if (meta.hidden || render::MaterialParams::IsEngineSemantic(variable.name))
                {
                    row.state = MaterialRowState::Hidden;
                    model.hidden.push_back(std::move(row));
                    continue;
                }

                if (!supported)
                {
                    row.state = MaterialRowState::Unsupported;
                }
                else
                {
                    const auto mismatch = std::find_if(model.messages.begin(), model.messages.end(),
                        [&variable](const render::MaterialParamResult& result) { return result.reason == render::MaterialParamReason::TypeMismatch && result.name == variable.name; });
                    if (mismatch != model.messages.end() && asset.params.count(variable.name) > 0)
                    {
                        row.state = MaterialRowState::WrongType;
                        row.message = mismatch->message;
                    }
                    else
                    {
                        row.state = row.hasKey ? MaterialRowState::Set : MaterialRowState::NotSet;
                    }
                }
                model.parameters.push_back(std::move(row));
            }
        }

        // ---- the textures, in the order of the registers ----
        std::vector<const render::ShaderResource*> resources;
        for (const render::ShaderResource& resource : layout.resources)
        {
            if (resource.type == render::ShaderResourceType::Texture)
            {
                resources.push_back(&resource);
            }
        }
        std::stable_sort(resources.begin(), resources.end(), [](const render::ShaderResource* a, const render::ShaderResource* b) { return a->registerIndex < b->registerIndex; });
        const std::string firstName = resources.empty() ? std::string() : resources.front()->name;
        for (const render::ShaderResource* resource : resources)
        {
            MaterialRow row;
            row.name = resource->name;
            row.label = resource->label.empty() ? resource->name : resource->label;
            row.typeName = "Texture2D";
            row.tooltip = resource->name + " (t" + std::to_string(resource->registerIndex) + ")";
            row.widget = MaterialWidget::Texture;
            row.registerIndex = resource->registerIndex;
            row.firstTexture = resource->name == firstName;
            row.firstTextureName = firstName;
            if (const render::MaterialParams::TextureSlot* slot = params.FindTexture(resource->name); slot != nullptr && slot->texture.IsValid() && slot->texture.value <= paths.size())
            {
                row.texturePath = paths[slot->texture.value - 1];
            }
            row.hasKey = !row.texturePath.empty();
            row.state = row.hasKey ? MaterialRowState::Set : MaterialRowState::NotSet;
            row.fileMissing = row.hasKey && options.fileMissing && options.fileMissing(row.texturePath);
            if (resource->hidden)
            {
                row.state = MaterialRowState::Hidden;
                model.hidden.push_back(std::move(row));
                continue;
            }
            model.textures.push_back(std::move(row));
        }

        // ---- what the shader does not have ----
        for (const auto& [name, value] : asset.params)
        {
            if (render::MaterialParams::IsEngineSemantic(name))
            {
                model.notInShader.push_back({MaterialEntryKind::EngineSemantic, name, "set by the renderer, the value is ignored"});
            }
            else if (layout.FindVariable(name).variable == nullptr)
            {
                model.notInShader.push_back({MaterialEntryKind::Param, name, DescribeValue(value)});
            }
        }
        for (const auto& [name, path] : asset.textures)
        {
            const render::ShaderResource* resource = layout.FindResourceByName(name);
            if (resource == nullptr || resource->type != render::ShaderResourceType::Texture)
            {
                model.notInShader.push_back({MaterialEntryKind::Texture, name, path});
            }
        }
        const bool tintIsWhite = asset.tint.r == 1.0f && asset.tint.g == 1.0f && asset.tint.b == 1.0f && asset.tint.a == 1.0f;
        if (layout.FindVariable("gColor").variable == nullptr && !tintIsWhite)
        {
            model.notInShader.push_back({MaterialEntryKind::OldTint, "tint", Number(asset.tint.r) + ", " + Number(asset.tint.g) + ", " + Number(asset.tint.b) + ", " + Number(asset.tint.a)});
        }
        if (resources.empty() && !asset.texturePath.empty() && !PathInTextures(asset, asset.texturePath))
        {
            model.notInShader.push_back({MaterialEntryKind::OldTexture, "texture", asset.texturePath});
        }
        return model;
    }

    const MaterialEditorModel& MaterialEditorModelCache::Get(const render::ShaderLayout& layout, const MaterialAsset& asset, const MaterialEditorOptions& options)
    {
        const std::uint64_t contentHash = resource::HashMaterialAsset(asset);
        const std::uint64_t layoutHash = render::MaterialLayoutHash(layout);
        const std::uint64_t metadataHash = MetadataHash(layout);
        std::uint64_t missingHash = 1469598103934665603ull;
        if (options.fileMissing)
        {
            std::vector<std::string> keys;
            if (!asset.texturePath.empty())
            {
                keys.push_back(asset.texturePath);
            }
            for (const auto& [name, path] : asset.textures)
            {
                keys.push_back(path);
            }
            for (const std::string& key : keys)
            {
                const std::uint8_t missing = options.fileMissing(key) ? 1 : 0;
                missingHash = Mix(missingHash, &missing, 1);
            }
        }

        if (!valid_ || contentHash != contentHash_ || layoutHash != layoutHash_ || metadataHash != metadataHash_ || missingHash != missingHash_)
        {
            model_ = BuildMaterialEditorModel(layout, asset, options);
            contentHash_ = contentHash;
            layoutHash_ = layoutHash;
            metadataHash_ = metadataHash;
            missingHash_ = missingHash;
            valid_ = true;
            ++builds_;
        }
        return model_;
    }

    // ---- the edits ----

    double SnapMaterialValue(double value, const MaterialRow& row)
    {
        if (row.widget == MaterialWidget::Int || row.widget == MaterialWidget::UInt)
        {
            value = std::round(value);
            if (row.hasMin)
            {
                value = std::max(value, row.min);
            }
            if (row.hasMax)
            {
                value = std::min(value, row.max);
            }
            if (row.widget == MaterialWidget::UInt)
            {
                value = std::clamp(value, 0.0, 2147483647.0);
            }
            else
            {
                value = std::clamp(value, -2147483648.0, 2147483647.0);
            }
            return value;
        }
        if (row.hasStep && row.step > 0.0)
        {
            const double base = row.hasMin ? row.min : 0.0;
            value = base + std::round((value - base) / row.step) * row.step;
        }
        if (row.hasMin)
        {
            value = std::max(value, row.min);
        }
        if (row.hasMax)
        {
            value = std::min(value, row.max);
        }
        return static_cast<double>(static_cast<float>(value)); // the frame holds a float: the file gets the float, not an inexact double
    }

    bool EditMaterialParam(const MaterialRow& row, const std::vector<double>& components, MaterialAsset& asset)
    {
        if (row.widget == MaterialWidget::Matrix || row.widget == MaterialWidget::Unsupported || row.widget == MaterialWidget::Texture)
        {
            return false;
        }
        const std::uint32_t expected = row.widget == MaterialWidget::Color3 ? 3 : row.widget == MaterialWidget::Color4 ? 4 : render::ValueTypeComponents(row.type);
        if (components.size() != expected || expected == 0)
        {
            return false;
        }
        std::vector<double> value;
        for (const double component : components)
        {
            value.push_back(SnapMaterialValue(component, row));
        }

        if (IsTintRow(row))
        {
            const core::Color tint{static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2]), static_cast<float>(value[3])};
            bool changed = !asset.tintSet || !SameTint(tint, asset.tint);
            asset.tint = tint;
            asset.tintSet = true;
            const auto inParams = asset.params.find("gColor");
            if (inParams != asset.params.end())
            {
                const render::MaterialValue colour = render::MaterialValue::FromVector(value);
                if (inParams->second != colour)
                {
                    inParams->second = colour;
                    changed = true;
                }
            }
            return changed;
        }

        render::MaterialValue made;
        switch (row.widget)
        {
            case MaterialWidget::Bool:
                made = render::MaterialValue::FromBool(value[0] != 0.0);
                break;
            case MaterialWidget::Float:
            case MaterialWidget::Int:
            case MaterialWidget::UInt:
                made = render::MaterialValue::FromNumber(value[0]);
                break;
            default:
                made = render::MaterialValue::FromVector(value);
                break;
        }
        const auto existing = asset.params.find(row.name);
        if (existing != asset.params.end() && existing->second == made)
        {
            return false;
        }
        asset.params[row.name] = std::move(made);
        return true;
    }

    bool ResetMaterialParam(const MaterialRow& row, MaterialAsset& asset)
    {
        if (row.widget == MaterialWidget::Texture)
        {
            return false;
        }
        if (IsTintRow(row))
        {
            const core::Color tint = DefaultTint(row);
            const bool changed = asset.tintSet || asset.params.count("gColor") > 0 || !SameTint(asset.tint, tint);
            asset.tintSet = false;
            asset.params.erase("gColor");
            asset.tint = tint;
            return changed;
        }
        return asset.params.erase(row.name) > 0;
    }

    bool ResetAllMaterialParams(const MaterialEditorModel& model, MaterialAsset& asset)
    {
        bool changed = false;
        for (const MaterialRow& row : model.parameters)
        {
            if (row.hasKey)
            {
                changed = ResetMaterialParam(row, asset) || changed;
            }
        }
        return changed;
    }

    bool SetMaterialTexture(const MaterialRow& row, const std::string& textureKey, MaterialAsset& asset)
    {
        if (row.widget != MaterialWidget::Texture)
        {
            return false;
        }
        if (textureKey.empty())
        {
            return ResetMaterialTexture(row, asset);
        }
        const MaterialAsset before = asset;
        const auto existing = asset.textures.find(row.name);
        const std::string old = existing != asset.textures.end() ? existing->second : std::string();

        if (row.firstTexture && existing == asset.textures.end())
        {
            asset.texturePath = textureKey; // an old material (or none yet): the old "texture" key, as the panel always did
        }
        else
        {
            if (!row.firstTexture && existing == asset.textures.end() && !row.firstTextureName.empty() && asset.textures.count(row.firstTextureName) == 0 &&
                !asset.texturePath.empty() && !PathInTextures(asset, asset.texturePath))
            {
                // The second texture comes: the old "texture" moves to "textures", where it can be told from the others by name
                asset.textures[row.firstTextureName] = asset.texturePath;
            }
            asset.textures[row.name] = textureKey;
            if (!old.empty() && asset.texturePath == old)
            {
                asset.texturePath = textureKey; // the old key only mirrored this texture: it follows
            }
        }
        return asset.texturePath != before.texturePath || asset.textures != before.textures;
    }

    bool ResetMaterialTexture(const MaterialRow& row, MaterialAsset& asset)
    {
        if (row.widget != MaterialWidget::Texture)
        {
            return false;
        }
        const MaterialAsset before = asset;
        asset.textures.erase(row.name);
        if (row.firstTexture)
        {
            if (!asset.texturePath.empty() && !PathInTextures(asset, asset.texturePath))
            {
                asset.texturePath.clear(); // the old "texture" key was this texture
            }
        }
        else if (!before.texturePath.empty() && !PathInTextures(asset, before.texturePath))
        {
            const auto erased = before.textures.find(row.name);
            if (erased != before.textures.end() && erased->second == before.texturePath)
            {
                asset.texturePath.clear(); // the old key only mirrored this texture
            }
        }
        return asset.texturePath != before.texturePath || asset.textures != before.textures;
    }

    bool RemoveMaterialEntry(const MaterialEntry& entry, MaterialAsset& asset)
    {
        switch (entry.kind)
        {
            case MaterialEntryKind::Param:
            case MaterialEntryKind::EngineSemantic:
                return asset.params.erase(entry.name) > 0;
            case MaterialEntryKind::Texture:
            {
                const auto found = asset.textures.find(entry.name);
                if (found == asset.textures.end())
                {
                    return false;
                }
                const std::string erased = found->second;
                asset.textures.erase(found);
                if (asset.texturePath == erased && !PathInTextures(asset, erased))
                {
                    asset.texturePath.clear(); // it only mirrored the entry
                }
                return true;
            }
            case MaterialEntryKind::OldTint:
            {
                const bool changed = asset.tintSet || !SameTint(asset.tint, {kWhite[0], kWhite[1], kWhite[2], kWhite[3]});
                asset.tint = {kWhite[0], kWhite[1], kWhite[2], kWhite[3]};
                asset.tintSet = false;
                return changed;
            }
            case MaterialEntryKind::OldTexture:
            {
                const bool changed = !asset.texturePath.empty();
                asset.texturePath.clear();
                return changed;
            }
        }
        return false;
    }

    bool RemoveAllMaterialEntries(const MaterialEditorModel& model, MaterialAsset& asset)
    {
        bool changed = false;
        for (const MaterialEntry& entry : model.notInShader)
        {
            changed = RemoveMaterialEntry(entry, asset) || changed;
        }
        return changed;
    }

    bool ChangeMaterialShader(MaterialAsset& asset, const std::string& shaderKey)
    {
        if (asset.shaderPath == shaderKey)
        {
            return false;
        }
        asset.shaderPath = shaderKey;
        return true;
    }

    namespace
    {
        std::string FirstLine(const std::string& text)
        {
            std::size_t begin = 0;
            while (begin < text.size() && (text[begin] == '\n' || text[begin] == '\r' || text[begin] == ' ' || text[begin] == '\t'))
            {
                ++begin;
            }
            std::size_t end = begin;
            while (end < text.size() && text[end] != '\n' && text[end] != '\r')
            {
                ++end;
            }
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '.'))
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        std::string LowerAscii(std::string text)
        {
            for (char& c : text)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return text;
        }
    }

    ShaderBanner DescribeShaderBanner(const bool fallback, const std::string& error)
    {
        std::string first = FirstLine(error);
        if (fallback)
        {
            if (first.empty())
            {
                first = "the shader did not compile";
            }
            return {ShaderBannerKind::Error, "Shader error: " + first + ". The fallback shader is drawn. Fix the HLSL: this panel updates after the save."};
        }
        if (!first.empty())
        {
            return {ShaderBannerKind::Warning, "The last change of the shader did not compile, the previous version is used: " + first};
        }
        return {};
    }

    bool MaterialFilterIsShown(const MaterialEditorModel& model)
    {
        return model.parameters.size() > kMaterialFilterThreshold;
    }

    bool MaterialRowMatchesFilter(const MaterialRow& row, const std::string& filter)
    {
        if (filter.empty())
        {
            return true;
        }
        const std::string needle = LowerAscii(filter);
        return LowerAscii(row.label).find(needle) != std::string::npos || LowerAscii(row.name).find(needle) != std::string::npos;
    }

    std::string MaterialEntryName(const MaterialEntry& entry)
    {
        switch (entry.kind)
        {
            case MaterialEntryKind::OldTint:
            case MaterialEntryKind::OldTexture:
                return entry.name + " (old key)";
            default:
                return entry.name;
        }
    }

    std::string MaterialEntryValueText(const MaterialEntry& entry)
    {
        switch (entry.kind)
        {
            case MaterialEntryKind::Texture:
                return "texture \xC2\xB7 " + entry.text;
            default:
                return entry.text;
        }
    }

    std::string MaterialEntryTooltip(const MaterialEntry& entry)
    {
        switch (entry.kind)
        {
            case MaterialEntryKind::Param:
                return "The shader has no variable '" + entry.name + "'. The value stays in the file and is ignored. Put the variable back or remove it.";
            case MaterialEntryKind::Texture:
                return "The shader has no texture '" + entry.name + "'. The path stays in the file and is ignored. Put the texture back or remove it.";
            case MaterialEntryKind::EngineSemantic:
                return "'" + entry.name + "' is set by the renderer. The value in the material is ignored. Remove it.";
            case MaterialEntryKind::OldTint:
                return "The shader has no gColor: the old 'tint' key is ignored. Remove it.";
            case MaterialEntryKind::OldTexture:
                return "The shader has no texture: the old 'texture' key is ignored. Remove it.";
        }
        return {};
    }
}
