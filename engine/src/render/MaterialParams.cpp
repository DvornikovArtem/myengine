// MaterialParams.cpp

#include <myengine/render/MaterialParams.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace myengine::render
{
    namespace
    {
        const char* const kEngineSemantics[] = {"gModel", "gViewProjection", "gLightDirection"};

        MaterialParamResult Failure(MaterialParamStatus status, std::string message, MaterialParamReason reason = MaterialParamReason::Invalid, std::string name = {})
        {
            MaterialParamResult result;
            result.status = status;
            result.reason = reason;
            result.name = std::move(name);
            result.message = std::move(message);
            return result;
        }

        std::string Quoted(const std::string& name)
        {
            return "'" + name + "'";
        }

        bool IsWholeNumber(const double value)
        {
            return std::isfinite(value) && std::floor(value) == value;
        }
    }

    MaterialValue MaterialValue::FromNumber(const double value)
    {
        MaterialValue result;
        result.kind = Kind::Number;
        result.numbers = {value};
        return result;
    }

    MaterialValue MaterialValue::FromBool(const bool value)
    {
        MaterialValue result;
        result.kind = Kind::Bool;
        result.numbers = {value ? 1.0 : 0.0};
        return result;
    }

    MaterialValue MaterialValue::FromVector(std::vector<double> values)
    {
        MaterialValue result;
        result.kind = Kind::Vector;
        result.numbers = std::move(values);
        return result;
    }

    MaterialValue MaterialValue::FromMatrix(std::vector<double> values)
    {
        MaterialValue result;
        result.kind = Kind::Matrix;
        result.numbers = std::move(values);
        return result;
    }

    std::string DescribeMaterialValue(const MaterialValue& value)
    {
        switch (value.kind)
        {
            case MaterialValue::Kind::Number:
                return "Float"; // a number: the layout decides whether it is a Float, an Int or a UInt
            case MaterialValue::Kind::Bool:
                return "Bool";
            case MaterialValue::Kind::Vector:
                return value.numbers.size() == 2 ? "Float2" : value.numbers.size() == 3 ? "Float3" : value.numbers.size() == 4 ? "Float4"
                                                                                                                             : "a vector of " + std::to_string(value.numbers.size()) + " numbers";
            case MaterialValue::Kind::Matrix:
                return value.numbers.size() == 16 ? "Float4x4" : "a matrix of " + std::to_string(value.numbers.size()) + " numbers";
        }
        return "a value";
    }

    std::uint64_t MaterialLayoutHash(const ShaderLayout& layout)
    {
        std::uint64_t hash = 1469598103934665603ull; // FNV-1a
        const auto mix = [&hash](const void* data, const std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            }
        };
        const auto mixText = [&](const std::string& text)
        {
            mix(text.data(), text.size());
            const std::uint8_t end = 0;
            mix(&end, 1);
        };
        const auto mixNumber = [&](const std::uint32_t value) { mix(&value, sizeof(value)); };
        for (const ShaderConstantBuffer& buffer : layout.constantBuffers)
        {
            mixText(buffer.name);
            mixNumber(buffer.registerIndex);
            mixNumber(buffer.sizeBytes);
            for (const ShaderVariable& variable : buffer.variables)
            {
                mixText(variable.name);
                mixNumber(static_cast<std::uint32_t>(variable.type));
                mixNumber(variable.offset);
                mixNumber(variable.size);
                mixNumber(variable.arrayCount);
            }
        }
        for (const ShaderResource& resource : layout.resources)
        {
            if (resource.type == ShaderResourceType::Texture)
            {
                mixText(resource.name);
                mixNumber(resource.registerIndex);
            }
        }
        return hash;
    }

    MaterialParams::MaterialParams(ShaderLayout layout)
        : layout_(std::move(layout))
        , layoutHash_(MaterialLayoutHash(layout_))
    {
        constantBuffers_.reserve(layout_.constantBuffers.size());
        for (const ShaderConstantBuffer& buffer : layout_.constantBuffers)
        {
            constantBuffers_.emplace_back(buffer.sizeBytes, std::uint8_t{0}); // not set: zero
        }
        for (const ShaderResource& resource : layout_.resources)
        {
            if (resource.type == ShaderResourceType::Texture)
            {
                TextureSlot slot;
                slot.name = resource.name;
                slot.registerIndex = resource.registerIndex;
                textures_.push_back(std::move(slot));
            }
        }
    }

    bool MaterialParams::IsEngineSemantic(const std::string& name)
    {
        return std::any_of(std::begin(kEngineSemantics), std::end(kEngineSemantics), [&name](const char* semantic) { return name == semantic; });
    }

    const MaterialParams::TextureSlot* MaterialParams::FindTexture(const std::string& name) const
    {
        const auto found = std::find_if(textures_.begin(), textures_.end(), [&name](const TextureSlot& slot) { return slot.name == name; });
        return found == textures_.end() ? nullptr : &*found;
    }

    const MaterialParams::TextureSlot* MaterialParams::FirstTexture() const
    {
        const TextureSlot* first = nullptr;
        for (const TextureSlot& slot : textures_)
        {
            if (first == nullptr || slot.registerIndex < first->registerIndex)
            {
                first = &slot;
            }
        }
        return first;
    }

    std::string MaterialParams::DescribeNames() const
    {
        std::string text = "variables: ";
        bool any = false;
        for (const ShaderConstantBuffer& buffer : layout_.constantBuffers)
        {
            for (const ShaderVariable& variable : buffer.variables)
            {
                text += (any ? ", " : "") + variable.name + " (" + ToString(variable.type) + (IsEngineSemantic(variable.name) ? ", set by the renderer" : "") + ")";
                any = true;
            }
        }
        if (!any)
        {
            text += "none";
        }
        text += "; textures: ";
        if (textures_.empty())
        {
            text += "none";
        }
        for (std::size_t i = 0; i < textures_.size(); ++i)
        {
            text += (i > 0 ? ", " : "") + textures_[i].name;
        }
        return text;
    }

    MaterialParamResult MaterialParams::NoSuchName(const std::string& name, const char* what) const
    {
        return Failure(MaterialParamStatus::Error, "no " + std::string(what) + " " + Quoted(name) + " in the shader; " + DescribeNames(), MaterialParamReason::UnknownName, name);
    }

    MaterialParamResult MaterialParams::SetVariable(const std::string& name, const ShaderValueType type, const void* data, const std::size_t bytes)
    {
        if (IsEngineSemantic(name))
        {
            return Failure(MaterialParamStatus::Warning, Quoted(name) + " is set by the renderer: the value from the material is ignored", MaterialParamReason::EngineSemantic, name);
        }
        const ShaderVariableRef found = layout_.FindVariable(name);
        if (found.variable == nullptr)
        {
            return NoSuchName(name, "variable");
        }
        const ShaderVariable& variable = *found.variable;
        if (variable.type != type)
        {
            return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected " + ToString(variable.type) + ", got " + ToString(type), MaterialParamReason::TypeMismatch, name);
        }
        if (variable.arrayCount > 1)
        {
            return Failure(MaterialParamStatus::Error, Quoted(name) + ": arrays of " + ToString(variable.type) + " are not supported yet", MaterialParamReason::Invalid, name);
        }
        const std::size_t index = static_cast<std::size_t>(found.buffer - layout_.constantBuffers.data());
        std::vector<std::uint8_t>& shadow = constantBuffers_[index];
        if (variable.size < bytes || static_cast<std::size_t>(variable.offset) + bytes > shadow.size())
        {
            return Failure(MaterialParamStatus::Error, Quoted(name) + ": the variable is " + std::to_string(variable.size) + " bytes at offset " + std::to_string(variable.offset) +
                ", which does not fit a " + ToString(type));
        }
        std::memcpy(shadow.data() + variable.offset, data, bytes);
        return {};
    }

    MaterialParamResult MaterialParams::SetFloat(const std::string& name, const float value)
    {
        return SetVariable(name, ShaderValueType::Float, &value, sizeof(value));
    }

    MaterialParamResult MaterialParams::SetFloat2(const std::string& name, const float x, const float y)
    {
        const float values[2] = {x, y};
        return SetVariable(name, ShaderValueType::Float2, values, sizeof(values));
    }

    MaterialParamResult MaterialParams::SetFloat3(const std::string& name, const float x, const float y, const float z)
    {
        const float values[3] = {x, y, z};
        return SetVariable(name, ShaderValueType::Float3, values, sizeof(values));
    }

    MaterialParamResult MaterialParams::SetFloat4(const std::string& name, const float x, const float y, const float z, const float w)
    {
        const float values[4] = {x, y, z, w};
        return SetVariable(name, ShaderValueType::Float4, values, sizeof(values));
    }

    MaterialParamResult MaterialParams::SetMatrix(const std::string& name, const std::array<float, 16>& rows)
    {
        float columns[16];
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                columns[column * 4 + row] = rows[static_cast<std::size_t>(row * 4 + column)];
            }
        }
        return SetVariable(name, ShaderValueType::Float4x4, columns, sizeof(columns));
    }

    MaterialParamResult MaterialParams::SetInt(const std::string& name, const std::int32_t value)
    {
        return SetVariable(name, ShaderValueType::Int, &value, sizeof(value));
    }

    MaterialParamResult MaterialParams::SetUInt(const std::string& name, const std::uint32_t value)
    {
        return SetVariable(name, ShaderValueType::UInt, &value, sizeof(value));
    }

    MaterialParamResult MaterialParams::SetBool(const std::string& name, const bool value)
    {
        const std::uint32_t stored = value ? 1u : 0u; // a bool of a constant buffer is a DWORD
        return SetVariable(name, ShaderValueType::Bool, &stored, sizeof(stored));
    }

    MaterialParamResult MaterialParams::SetTexture(const std::string& name, const TextureHandle texture)
    {
        const auto slot = std::find_if(textures_.begin(), textures_.end(), [&name](const TextureSlot& candidate) { return candidate.name == name; });
        if (slot == textures_.end())
        {
            const ShaderResource* resource = layout_.FindResourceByName(name);
            if (resource != nullptr)
            {
                return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected a Texture, but this is a " + ToString(resource->type) + "; " + DescribeNames(), MaterialParamReason::TypeMismatch, name);
            }
            if (layout_.FindVariable(name).variable != nullptr)
            {
                return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected a Texture, but this is a " + ToString(layout_.FindVariable(name).variable->type) + " variable; " + DescribeNames(), MaterialParamReason::TypeMismatch, name);
            }
            return NoSuchName(name, "texture");
        }
        slot->texture = texture;
        return {};
    }

    MaterialParamResult MaterialParams::SetValue(const std::string& name, const MaterialValue& value)
    {
        if (IsEngineSemantic(name))
        {
            return Failure(MaterialParamStatus::Warning, Quoted(name) + " is set by the renderer: the value from the material is ignored", MaterialParamReason::EngineSemantic, name);
        }
        const ShaderVariableRef found = layout_.FindVariable(name);
        if (found.variable == nullptr)
        {
            return NoSuchName(name, "variable");
        }
        const ShaderValueType type = found.variable->type;
        const auto mismatch = [&]()
        {
            return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected " + ToString(type) + ", got " + DescribeMaterialValue(value), MaterialParamReason::TypeMismatch, name);
        };
        const std::vector<double>& n = value.numbers;
        switch (value.kind)
        {
            case MaterialValue::Kind::Number:
                if (n.size() != 1 || !std::isfinite(n[0]))
                {
                    return mismatch();
                }
                if (type == ShaderValueType::Float)
                {
                    return SetFloat(name, static_cast<float>(n[0]));
                }
                if (type == ShaderValueType::Int)
                {
                    if (!IsWholeNumber(n[0]) || n[0] < std::numeric_limits<std::int32_t>::min() || n[0] > std::numeric_limits<std::int32_t>::max())
                    {
                        return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected Int, got " + std::to_string(n[0]) + " (a whole number in the range of int is needed)", MaterialParamReason::TypeMismatch, name);
                    }
                    return SetInt(name, static_cast<std::int32_t>(n[0]));
                }
                if (type == ShaderValueType::UInt)
                {
                    if (!IsWholeNumber(n[0]) || n[0] < 0.0 || n[0] > std::numeric_limits<std::uint32_t>::max())
                    {
                        return Failure(MaterialParamStatus::Error, Quoted(name) + ": expected UInt, got " + std::to_string(n[0]) + " (a whole number from 0 is needed)", MaterialParamReason::TypeMismatch, name);
                    }
                    return SetUInt(name, static_cast<std::uint32_t>(n[0]));
                }
                return mismatch();
            case MaterialValue::Kind::Bool:
                if (type == ShaderValueType::Bool && n.size() == 1)
                {
                    return SetBool(name, n[0] != 0.0);
                }
                return mismatch();
            case MaterialValue::Kind::Vector:
                if (std::any_of(n.begin(), n.end(), [](const double v) { return !std::isfinite(v); }))
                {
                    return mismatch();
                }
                if (n.size() == 2 && type == ShaderValueType::Float2)
                {
                    return SetFloat2(name, static_cast<float>(n[0]), static_cast<float>(n[1]));
                }
                if (n.size() == 3 && type == ShaderValueType::Float3)
                {
                    return SetFloat3(name, static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]));
                }
                if (n.size() == 4 && type == ShaderValueType::Float4)
                {
                    return SetFloat4(name, static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]), static_cast<float>(n[3]));
                }
                return mismatch();
            case MaterialValue::Kind::Matrix:
                if (n.size() == 16 && type == ShaderValueType::Float4x4 && std::all_of(n.begin(), n.end(), [](const double v) { return std::isfinite(v); }))
                {
                    std::array<float, 16> rows{};
                    for (std::size_t i = 0; i < 16; ++i)
                    {
                        rows[i] = static_cast<float>(n[i]);
                    }
                    return SetMatrix(name, rows);
                }
                return mismatch();
        }
        return mismatch();
    }

    std::vector<MaterialParamResult> ApplyMaterialValues(MaterialParams& params, const MaterialValues& values, const MaterialTextureResolver& resolve, const std::string& programName)
    {
        std::vector<MaterialParamResult> messages;
        const auto note = [&](MaterialParamResult result)
        {
            if (result.IsOk())
            {
                return;
            }
            if (result.reason == MaterialParamReason::UnknownName)
            {
                // A name that the shader does not have (it was removed from the shader, or a typo) is one warning: the
                // rest of the material is still applied
                result.status = MaterialParamStatus::Warning;
                result.message = Quoted(result.name) + " is not in " + (programName.empty() ? std::string("the shader") : programName) + "; names: " + params.DescribeNames();
            }
            messages.push_back(std::move(result));
        };
        const auto setTexture = [&](const std::string& name, const std::string& path)
        {
            const TextureHandle handle = resolve ? resolve(path) : TextureHandle{};
            if (!handle.IsValid())
            {
                note(Failure(MaterialParamStatus::Error, "texture " + Quoted(name) + ": the file " + Quoted(path) + " could not be loaded", MaterialParamReason::Invalid, name));
                return;
            }
            note(params.SetTexture(name, handle));
        };

        // "texture": the first texture of the shader. The new key "textures" for the same texture wins. When the old key only
        // repeats a path of "textures" (the resource manager mirrors one into the other), there is nothing to say.
        if (!values.texture.empty())
        {
            const bool repeated = std::any_of(values.textures.begin(), values.textures.end(), [&values](const auto& entry) { return entry.second == values.texture; });
            const MaterialParams::TextureSlot* first = params.FirstTexture();
            if (repeated)
            {
                // nothing to do
            }
            else if (first == nullptr)
            {
                note(Failure(MaterialParamStatus::Warning, "'texture' is ignored: the shader has no texture; " + params.DescribeNames()));
            }
            else if (values.textures.count(first->name) > 0)
            {
                note(Failure(MaterialParamStatus::Warning, "'texture' is ignored: 'textures' sets " + Quoted(first->name) + ", the first texture of the shader, and the new key wins"));
            }
            else
            {
                setTexture(first->name, values.texture);
            }
        }
        for (const auto& [name, path] : values.textures)
        {
            setTexture(name, path);
        }

        // "tint": the variable gColor. "params" sets gColor itself: it wins.
        const bool tintIsWhite = values.tint == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        const auto color = values.params.find("gColor");
        if (color != values.params.end())
        {
            // (the same float: the resource manager mirrors gColor into the tint as floats)
            const std::vector<double>& gColor = color->second.numbers;
            const bool isColor = color->second.kind == MaterialValue::Kind::Vector && gColor.size() == 4;
            const bool sameColor = isColor &&
                static_cast<float>(gColor[0]) == values.tint[0] && static_cast<float>(gColor[1]) == values.tint[1] &&
                static_cast<float>(gColor[2]) == values.tint[2] && static_cast<float>(gColor[3]) == values.tint[3];
            // gColor that is not a colour is an error of its own (below); the tint is not a way round it
            if (isColor && !sameColor)
            {
                note(Failure(MaterialParamStatus::Warning, "'tint' is ignored: 'params' sets gColor and the new key wins"));
            }
        }
        else if (params.Layout().FindVariable("gColor").variable == nullptr)
        {
            if (!tintIsWhite)
            {
                note(Failure(MaterialParamStatus::Warning, "'tint' is ignored: the shader has no gColor; " + params.DescribeNames()));
            }
        }
        else
        {
            note(params.SetFloat4("gColor", values.tint[0], values.tint[1], values.tint[2], values.tint[3]));
        }

        for (const auto& [name, value] : values.params)
        {
            note(params.SetValue(name, value));
        }
        return messages;
    }
}
