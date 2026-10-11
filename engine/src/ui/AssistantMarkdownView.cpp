// AssistantMarkdownView.cpp

#include "AssistantMarkdownView.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include <imgui/imgui.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        using assistant::MarkdownBlock;
        using assistant::MarkdownBlockKind;
        using assistant::MarkdownSpan;
        using assistant::MarkdownStyle;

        constexpr float kLineHeight = 21.0f;           // 14 px text, line height 1.5
        constexpr float kCodeLineHeight = 18.0f;
        constexpr float kCodeHeaderHeight = 26.0f;
        constexpr int kCodeMaxVisibleLines = 28;
        constexpr ImU32 kText = IM_COL32(0xE6, 0xE6, 0xE6, 255);
        constexpr ImU32 kItalic = IM_COL32(0xC4, 0xC8, 0xD0, 255);
        constexpr ImU32 kInlineCode = IM_COL32(0xF0, 0xC6, 0x74, 255);
        constexpr ImU32 kInlineCodeBackground = IM_COL32(0x2B, 0x2B, 0x2B, 255);
        constexpr ImU32 kCodeBorder = IM_COL32(0x2C, 0x2C, 0x2C, 255);
        constexpr ImU32 kCodeHeader = IM_COL32(0x20, 0x20, 0x20, 255);
        constexpr ImU32 kQuoteBar = IM_COL32(0x44, 0x44, 0x44, 255);

        struct Face
        {
            ImFont* font = nullptr;
            float size = 0.0f;
        };

        Face FaceOf(const FontRole role)
        {
            PushFontRole(role);
            const Face face{ImGui::GetFont(), ImGui::GetFontSize()};
            PopFontRole();
            return face;
        }

        float TextWidth(const Face& face, const char* begin, const char* end)
        {
            return face.font->CalcTextSizeA(face.size, FLT_MAX, 0.0f, begin, end).x;
        }

        struct Token
        {
            std::string text;
            MarkdownStyle style = MarkdownStyle::Normal;
            const std::string* url = nullptr;
            bool space = false;
            bool lineBreak = false;
            float width = 0.0f;
            float x = 0.0f;
            int line = 0;
        };

        struct Faces
        {
            Face body;
            Face strong;
            Face mono;
        };

        const Face& FaceFor(const Faces& faces, const MarkdownStyle style, const bool baseStrong)
        {
            switch (style)
            {
            case MarkdownStyle::Bold:
            case MarkdownStyle::BoldItalic:
                return faces.strong;
            case MarkdownStyle::Code:
                return faces.mono;
            default:
                return baseStrong ? faces.strong : faces.body;
            }
        }

        ImU32 ColorFor(const MarkdownStyle style, const ImU32 base)
        {
            switch (style)
            {
            case MarkdownStyle::Bold:
            case MarkdownStyle::BoldItalic:
                return style::kTextStrong;
            case MarkdownStyle::Italic:
                return kItalic;
            case MarkdownStyle::Code:
                return kInlineCode;
            case MarkdownStyle::Strike:
                return style::kTextDim;
            case MarkdownStyle::Link:
                return style::kLink;
            default:
                return base;
            }
        }

        // Words, spaces and hard line breaks of the spans, measured
        std::vector<Token> Tokenize(const std::vector<MarkdownSpan>& spans, const Faces& faces, const bool baseStrong)
        {
            std::vector<Token> tokens;
            for (const auto& span : spans)
            {
                const auto& face = FaceFor(faces, span.style, baseStrong);
                std::size_t i = 0;
                const auto& text = span.text;
                while (i < text.size())
                {
                    Token token;
                    token.style = span.style;
                    token.url = span.style == MarkdownStyle::Link ? &span.url : nullptr;
                    if (text[i] == '\n')
                    {
                        token.lineBreak = true;
                        ++i;
                    }
                    else if (text[i] == ' ' || text[i] == '\t')
                    {
                        std::size_t end = i;
                        while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
                        {
                            ++end;
                        }
                        token.space = true;
                        token.text = " ";
                        token.width = TextWidth(face, " ", nullptr);
                        i = end;
                    }
                    else
                    {
                        std::size_t end = i;
                        while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n')
                        {
                            ++end;
                        }
                        token.text = text.substr(i, end - i);
                        token.width = TextWidth(face, token.text.c_str(), token.text.c_str() + token.text.size());
                        if (span.style == MarkdownStyle::Code)
                        {
                            token.width += 6.0f; // padding of the background
                        }
                        i = end;
                    }
                    tokens.push_back(std::move(token));
                }
            }
            return tokens;
        }

        // Places the tokens into lines of `width`. Returns the number of lines
        int Layout(std::vector<Token>& tokens, const float width)
        {
            float x = 0.0f;
            int line = 0;
            for (auto& token : tokens)
            {
                if (token.lineBreak)
                {
                    ++line;
                    x = 0.0f;
                    token.line = line;
                    continue;
                }
                if (token.space)
                {
                    if (x > 0.0f)
                    {
                        token.x = x;
                        token.line = line;
                        x += token.width;
                    }
                    else
                    {
                        token.width = 0.0f; // no spaces at the start of a line
                        token.line = line;
                    }
                    continue;
                }
                if (x > 0.0f && x + token.width > width)
                {
                    ++line;
                    x = 0.0f;
                }
                token.x = x;
                token.line = line;
                x += token.width;
            }
            return line + 1;
        }

        // Draws wrapped inline spans from the cursor; returns the clicked link (empty if none)
        std::string DrawSpans(const std::vector<MarkdownSpan>& spans, const float width, const ImU32 baseColor, const bool baseStrong,
                              const float lineHeight = kLineHeight)
        {
            if (spans.empty())
            {
                return {};
            }
            Faces faces{FaceOf(FontRole::Body), FaceOf(FontRole::Strong), FaceOf(FontRole::Mono)};
            auto tokens = Tokenize(spans, faces, baseStrong);
            const int lines = Layout(tokens, width);

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            auto* drawList = ImGui::GetWindowDrawList();
            const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            std::string clicked;
            for (const auto& token : tokens)
            {
                if (token.lineBreak || token.space || token.text.empty())
                {
                    continue;
                }
                const auto& face = FaceFor(faces, token.style, baseStrong);
                const float y = origin.y + static_cast<float>(token.line) * lineHeight + (lineHeight - face.size) * 0.5f;
                float x = origin.x + token.x;
                const char* begin = token.text.c_str();
                const char* end = begin + token.text.size();
                if (token.style == MarkdownStyle::Code)
                {
                    drawList->AddRectFilled(ImVec2(x, y - 1.0f), ImVec2(x + token.width, y + face.size + 1.0f), kInlineCodeBackground, 3.0f);
                    x += 3.0f;
                }
                ImU32 color = ColorFor(token.style, baseColor);
                if (token.style == MarkdownStyle::Link)
                {
                    const ImVec2 min(x, y);
                    const ImVec2 max(x + token.width, y + face.size);
                    if (windowHovered && ImGui::IsMouseHoveringRect(min, max))
                    {
                        color = style::kPrimaryHover;
                        drawList->AddLine(ImVec2(x, y + face.size + 1.0f), ImVec2(x + token.width, y + face.size + 1.0f), color);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && token.url != nullptr)
                        {
                            const auto& url = *token.url;
                            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
                            {
                                clicked = url;
                            }
                        }
                    }
                }
                drawList->AddText(face.font, face.size, ImVec2(x, y), color, begin, end);
                if (token.style == MarkdownStyle::Strike)
                {
                    const float mid = y + face.size * 0.55f;
                    drawList->AddLine(ImVec2(x, mid), ImVec2(x + token.width, mid), color);
                }
            }
            ImGui::Dummy(ImVec2(width, static_cast<float>(lines) * lineHeight));
            return clicked;
        }

        void DrawCodeBlock(const MarkdownBlock& block, const float width, const int id, MarkdownDrawResult& result)
        {
            // lines of the code
            std::vector<std::pair<std::size_t, std::size_t>> lines;
            {
                std::size_t start = 0;
                for (std::size_t i = 0; i <= block.code.size(); ++i)
                {
                    if (i == block.code.size() || block.code[i] == '\n')
                    {
                        lines.emplace_back(start, i);
                        start = i + 1;
                    }
                }
            }
            const int lineCount = static_cast<int>(lines.size());
            const int visibleLines = std::min(lineCount, kCodeMaxVisibleLines);
            const float bodyHeight = static_cast<float>(visibleLines) * kCodeLineHeight + 16.0f;
            const float totalHeight = kCodeHeaderHeight + bodyHeight + 2.0f;

            ImGui::PushID(id);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + width, min.y + totalHeight);
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(min, max, style::kInput, 4.0f);
            drawList->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(max.x - 1.0f, min.y + kCodeHeaderHeight), kCodeHeader, 4.0f,
                                    ImDrawFlags_RoundCornersTop);
            drawList->AddRect(min, max, kCodeBorder, 4.0f);

            // header: language at the left, Copy at the right
            const Face small = FaceOf(FontRole::Tiny);
            const std::string language = block.language.empty() ? std::string("text") : block.language;
            drawList->AddText(small.font, small.size, ImVec2(min.x + 10.0f, min.y + (kCodeHeaderHeight - small.size) * 0.5f),
                              style::kTextDim, language.c_str());
            ImGui::SetCursorScreenPos(ImVec2(max.x - 26.0f, min.y + 2.0f));
            if (IconButton("##copy_code", ICON_COPY, "Copy code", false, true, 0, 22.0f, nullptr, IconSize::Chevron12))
            {
                result.copiedCode = block.code;
                result.codeCopied = true;
            }

            // body: monospace lines, a horizontal scrollbar instead of wrapping
            ImGui::SetCursorScreenPos(ImVec2(min.x + 1.0f, min.y + kCodeHeaderHeight));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            if (ImGui::BeginChild("##code_body", ImVec2(width - 2.0f, bodyHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                PushFontRole(FontRole::Mono);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kText));
                for (const auto& [begin, end] : lines)
                {
                    const char* data = block.code.c_str();
                    ImGui::TextUnformatted(data + begin, data + end);
                    if (end == begin)
                    {
                        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
                    }
                }
                ImGui::PopStyleColor();
                PopFontRole();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);

            ImGui::SetCursorScreenPos(min);
            ImGui::Dummy(ImVec2(width, totalHeight));
            ImGui::PopID();
        }

        void DrawTable(const MarkdownBlock& block, const float width, const int id)
        {
            std::size_t columns = 0;
            for (const auto& row : block.rows)
            {
                columns = std::max(columns, row.size());
            }
            if (columns == 0 || columns > 12)
            {
                return;
            }
            ImGui::PushID(id);
            if (ImGui::BeginTable("##md_table", static_cast<int>(columns),
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, ImVec2(width, 0.0f)))
            {
                for (std::size_t r = 0; r < block.rows.size(); ++r)
                {
                    ImGui::TableNextRow(r == 0 && block.header ? ImGuiTableRowFlags_Headers : 0);
                    for (std::size_t c = 0; c < columns; ++c)
                    {
                        ImGui::TableSetColumnIndex(static_cast<int>(c));
                        if (c < block.rows[r].size())
                        {
                            const auto text = assistant::MarkdownPlainText(block.rows[r][c]);
                            if (r == 0 && block.header)
                            {
                                PushFontRole(FontRole::Strong);
                            }
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextUnformatted(text.c_str());
                            ImGui::PopTextWrapPos();
                            if (r == 0 && block.header)
                            {
                                PopFontRole();
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }

    MarkdownDrawResult DrawMarkdown(const std::vector<MarkdownBlock>& blocks, const float width, const int idScope)
    {
        MarkdownDrawResult result;
        const float usable = std::max(width, 60.0f);
        int index = 0;
        for (const auto& block : blocks)
        {
            const int id = idScope * 1000 + index++;
            const float before = index == 1 ? 0.0f : 6.0f;
            switch (block.kind)
            {
            case MarkdownBlockKind::Heading:
            {
                ImGui::Dummy(ImVec2(0.0f, before + (block.level <= 2 ? 4.0f : 0.0f)));
                const auto link = DrawSpans(block.spans, usable, style::kTextStrong, true);
                if (!link.empty())
                {
                    result.clickedLink = link;
                }
                break;
            }
            case MarkdownBlockKind::Paragraph:
            {
                ImGui::Dummy(ImVec2(0.0f, before));
                const auto link = DrawSpans(block.spans, usable, kText, false);
                if (!link.empty())
                {
                    result.clickedLink = link;
                }
                break;
            }
            case MarkdownBlockKind::ListItem:
            {
                const float indent = 18.0f + static_cast<float>(block.level) * 18.0f;
                ImGui::Dummy(ImVec2(0.0f, before > 0.0f ? 2.0f : 0.0f));
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const Face body = FaceOf(FontRole::Body);
                std::string marker;
                if (block.checkbox)
                {
                    marker = block.checked ? "[x]" : "[ ]";
                }
                else if (block.ordered)
                {
                    marker = std::to_string(block.number) + ".";
                }
                else
                {
                    marker = "\xE2\x80\xA2"; // bullet
                }
                ImGui::GetWindowDrawList()->AddText(body.font, body.size,
                                                    ImVec2(origin.x + indent - 6.0f - TextWidth(body, marker.c_str(), nullptr),
                                                           origin.y + (kLineHeight - body.size) * 0.5f),
                                                    style::kTextDim, marker.c_str());
                ImGui::SetCursorScreenPos(ImVec2(origin.x + indent, origin.y));
                const auto link = DrawSpans(block.spans, std::max(usable - indent, 40.0f), kText, false);
                ImGui::SetCursorScreenPos(ImVec2(origin.x, ImGui::GetCursorScreenPos().y));
                if (!link.empty())
                {
                    result.clickedLink = link;
                }
                break;
            }
            case MarkdownBlockKind::Quote:
            {
                ImGui::Dummy(ImVec2(0.0f, before));
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float indent = 12.0f * static_cast<float>(std::max(block.level, 1));
                ImGui::SetCursorScreenPos(ImVec2(origin.x + indent, origin.y));
                const auto link = DrawSpans(block.spans, std::max(usable - indent, 40.0f), style::kTextDim, false);
                const float bottom = ImGui::GetCursorScreenPos().y;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(origin.x + indent - 8.0f, origin.y), ImVec2(origin.x + indent - 5.0f, bottom),
                                                          kQuoteBar);
                ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom));
                if (!link.empty())
                {
                    result.clickedLink = link;
                }
                break;
            }
            case MarkdownBlockKind::CodeBlock:
                ImGui::Dummy(ImVec2(0.0f, before));
                DrawCodeBlock(block, usable, id, result);
                break;
            case MarkdownBlockKind::Table:
                ImGui::Dummy(ImVec2(0.0f, before));
                DrawTable(block, usable, id);
                break;
            case MarkdownBlockKind::Rule:
                ImGui::Dummy(ImVec2(0.0f, before));
                ImGui::Separator();
                break;
            }
        }
        return result;
    }
}
