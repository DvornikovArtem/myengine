// Markdown.cpp

#include <myengine/assistant/Markdown.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace myengine::assistant
{
    namespace
    {
        bool IsSpace(const char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        bool IsAlnum(const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || (static_cast<unsigned char>(c) & 0x80) != 0;
        }

        std::string_view TrimView(std::string_view text)
        {
            while (!text.empty() && IsSpace(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && IsSpace(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        MarkdownStyle StyleOf(const bool bold, const bool italic, const bool strike)
        {
            if (strike)
            {
                return MarkdownStyle::Strike;
            }
            if (bold && italic)
            {
                return MarkdownStyle::BoldItalic;
            }
            if (bold)
            {
                return MarkdownStyle::Bold;
            }
            return italic ? MarkdownStyle::Italic : MarkdownStyle::Normal;
        }

        // The position of a closing delimiter `delimiter` x `count` after `from`, or npos. The character before it
        // must not be a space (so "a * b * c" is not italic).
        std::size_t FindClosing(const std::string_view text, const std::size_t from, const char delimiter, const std::size_t count)
        {
            for (std::size_t i = from; i < text.size(); ++i)
            {
                if (text[i] == '\\')
                {
                    ++i;
                    continue;
                }
                if (text[i] == '`')
                {
                    // a code span hides delimiters inside it
                    std::size_t run = 1;
                    while (i + run < text.size() && text[i + run] == '`')
                    {
                        ++run;
                    }
                    const auto close = text.find(std::string(run, '`'), i + run);
                    if (close != std::string_view::npos)
                    {
                        i = close + run - 1;
                    }
                    else
                    {
                        i += run - 1;
                    }
                    continue;
                }
                if (text[i] != delimiter)
                {
                    continue;
                }
                std::size_t run = 1;
                while (i + run < text.size() && text[i + run] == delimiter)
                {
                    ++run;
                }
                if (run >= count && i > from && !IsSpace(text[i - 1]))
                {
                    if (delimiter != '_' || i + count >= text.size() || !IsAlnum(text[i + count]))
                    {
                        return i;
                    }
                }
                i += run - 1;
            }
            return std::string_view::npos;
        }

        std::string StripInline(const std::string_view text)
        {
            std::string out;
            for (const auto& span : ParseMarkdownInline(text))
            {
                out += span.text;
            }
            return out;
        }

        bool IsUrlChar(const char c)
        {
            return !IsSpace(c) && c != '<' && c != '>' && c != '"';
        }

        // "- item", "* item", "+ item", "12. item", "3) item": the content start, or npos
        std::size_t ListMarker(const std::string_view line, bool& ordered, int& number)
        {
            ordered = false;
            number = 0;
            if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && (line[1] == ' ' || line[1] == '\t'))
            {
                return 2;
            }
            std::size_t i = 0;
            while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) != 0 && i < 9)
            {
                number = number * 10 + (line[i] - '0');
                ++i;
            }
            if (i > 0 && i + 1 < line.size() && (line[i] == '.' || line[i] == ')') && (line[i + 1] == ' ' || line[i + 1] == '\t'))
            {
                ordered = true;
                return i + 2;
            }
            return std::string_view::npos;
        }

        bool IsRule(const std::string_view line)
        {
            const auto text = TrimView(line);
            if (text.size() < 3 || (text[0] != '-' && text[0] != '*' && text[0] != '_'))
            {
                return false;
            }
            std::size_t count = 0;
            for (const char c : text)
            {
                if (c == text[0])
                {
                    ++count;
                }
                else if (c != ' ')
                {
                    return false;
                }
            }
            return count >= 3;
        }

        std::vector<std::string_view> SplitCells(std::string_view line)
        {
            line = TrimView(line);
            if (!line.empty() && line.front() == '|')
            {
                line.remove_prefix(1);
            }
            if (!line.empty() && line.back() == '|')
            {
                line.remove_suffix(1);
            }
            std::vector<std::string_view> cells;
            std::size_t start = 0;
            for (std::size_t i = 0; i <= line.size(); ++i)
            {
                if (i == line.size() || (line[i] == '|' && (i == 0 || line[i - 1] != '\\')))
                {
                    cells.push_back(TrimView(line.substr(start, i - start)));
                    start = i + 1;
                }
            }
            return cells;
        }

        bool IsTableSeparator(const std::string_view line)
        {
            if (line.find('-') == std::string_view::npos)
            {
                return false;
            }
            for (const char c : line)
            {
                if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t')
                {
                    return false;
                }
            }
            return line.find('|') != std::string_view::npos || line.find("--") != std::string_view::npos;
        }

        // Fence line: up to 3 spaces, then 3+ backticks or tildes. Fills the fence character, its length and the info string
        bool IsFence(const std::string_view line, char& character, std::size_t& length, std::string& info)
        {
            std::size_t i = 0;
            while (i < line.size() && line[i] == ' ' && i < 3)
            {
                ++i;
            }
            if (i >= line.size() || (line[i] != '`' && line[i] != '~'))
            {
                return false;
            }
            const char c = line[i];
            std::size_t run = 0;
            while (i + run < line.size() && line[i + run] == c)
            {
                ++run;
            }
            if (run < 3)
            {
                return false;
            }
            character = c;
            length = run;
            info = std::string(TrimView(line.substr(i + run)));
            return true;
        }
    }

    std::vector<MarkdownSpan> ParseMarkdownInline(const std::string_view text)
    {
        std::vector<MarkdownSpan> spans;
        std::string buffer;
        bool bold = false;
        bool italic = false;
        bool strike = false;

        const auto flush = [&]()
        {
            if (!buffer.empty())
            {
                MarkdownSpan span;
                span.text = std::move(buffer);
                span.style = StyleOf(bold, italic, strike);
                spans.push_back(std::move(span));
                buffer.clear();
            }
        };

        std::size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i];
            if (c == '\\' && i + 1 < text.size() && std::ispunct(static_cast<unsigned char>(text[i + 1])) != 0)
            {
                buffer += text[i + 1];
                i += 2;
                continue;
            }
            if (c == '`')
            {
                std::size_t run = 1;
                while (i + run < text.size() && text[i + run] == '`')
                {
                    ++run;
                }
                const auto close = text.find(std::string(run, '`'), i + run);
                if (close != std::string_view::npos)
                {
                    flush();
                    auto code = text.substr(i + run, close - (i + run));
                    if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
                    {
                        code = code.substr(1, code.size() - 2);
                    }
                    MarkdownSpan span;
                    span.text = std::string(code);
                    span.style = MarkdownStyle::Code;
                    spans.push_back(std::move(span));
                    i = close + run;
                    continue;
                }
                buffer.append(run, '`');
                i += run;
                continue;
            }
            if (c == '[')
            {
                const auto closeBracket = text.find(']', i + 1);
                if (closeBracket != std::string_view::npos && closeBracket + 1 < text.size() && text[closeBracket + 1] == '(')
                {
                    const auto closeParen = text.find(')', closeBracket + 2);
                    if (closeParen != std::string_view::npos)
                    {
                        flush();
                        MarkdownSpan span;
                        span.text = StripInline(text.substr(i + 1, closeBracket - i - 1));
                        span.url = std::string(TrimView(text.substr(closeBracket + 2, closeParen - closeBracket - 2)));
                        span.style = MarkdownStyle::Link;
                        if (span.text.empty())
                        {
                            span.text = span.url;
                        }
                        spans.push_back(std::move(span));
                        i = closeParen + 1;
                        continue;
                    }
                }
            }
            if ((c == 'h' || c == 'H') && (text.substr(i, 7) == "http://" || text.substr(i, 8) == "https://") &&
                (i == 0 || !IsAlnum(text[i - 1])))
            {
                std::size_t end = i;
                while (end < text.size() && IsUrlChar(text[end]))
                {
                    ++end;
                }
                while (end > i && (text[end - 1] == '.' || text[end - 1] == ',' || text[end - 1] == ')' || text[end - 1] == ';' ||
                                   text[end - 1] == ':' || text[end - 1] == '!' || text[end - 1] == '?'))
                {
                    --end;
                }
                flush();
                MarkdownSpan span;
                span.text = std::string(text.substr(i, end - i));
                span.url = span.text;
                span.style = MarkdownStyle::Link;
                spans.push_back(std::move(span));
                i = end;
                continue;
            }
            if (c == '*' || c == '_')
            {
                std::size_t run = 1;
                while (i + run < text.size() && text[i + run] == c)
                {
                    ++run;
                }
                const bool wordInside = c == '_' && i > 0 && IsAlnum(text[i - 1]);
                const std::size_t use = std::min<std::size_t>(run, 3);
                const bool afterSpace = i + run >= text.size() || IsSpace(text[i + run]);
                bool handled = false;
                if (!wordInside || (use == 2 && bold) || (use == 1 && italic))
                {
                    if (use >= 2 && bold && (i == 0 || !IsSpace(text[i - 1])))
                    {
                        flush();
                        bold = false;
                        if (use == 3 && italic)
                        {
                            italic = false;
                        }
                        i += use;
                        handled = true;
                    }
                    else if (use == 1 && italic && (i == 0 || !IsSpace(text[i - 1])))
                    {
                        flush();
                        italic = false;
                        i += 1;
                        handled = true;
                    }
                    else if (!afterSpace && FindClosing(text, i + run, c, use) != std::string_view::npos)
                    {
                        flush();
                        if (use >= 2)
                        {
                            bold = true;
                        }
                        if (use == 1 || use == 3)
                        {
                            italic = true;
                        }
                        i += use;
                        handled = true;
                    }
                }
                if (handled)
                {
                    continue;
                }
                buffer.append(run, c);
                i += run;
                continue;
            }
            if (c == '~' && i + 1 < text.size() && text[i + 1] == '~')
            {
                if (strike)
                {
                    flush();
                    strike = false;
                    i += 2;
                    continue;
                }
                if (i + 2 < text.size() && !IsSpace(text[i + 2]) && FindClosing(text, i + 2, '~', 2) != std::string_view::npos)
                {
                    flush();
                    strike = true;
                    i += 2;
                    continue;
                }
            }
            buffer += c;
            ++i;
        }
        flush();
        return spans;
    }

    std::string MarkdownPlainText(const std::vector<MarkdownSpan>& spans)
    {
        std::string out;
        for (const auto& span : spans)
        {
            out += span.text;
        }
        return out;
    }

    std::vector<MarkdownBlock> ParseMarkdown(const std::string_view text)
    {
        std::vector<std::string_view> lines;
        {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= text.size(); ++i)
            {
                if (i == text.size() || text[i] == '\n')
                {
                    auto line = text.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.remove_suffix(1);
                    }
                    lines.push_back(line);
                    start = i + 1;
                }
            }
        }

        std::vector<MarkdownBlock> blocks;
        std::string paragraph; // raw text of the paragraph being collected
        std::string itemText;  // raw text of the last list item (continuations are appended)
        std::size_t itemBlock = static_cast<std::size_t>(-1);

        const auto flushParagraph = [&]()
        {
            if (!paragraph.empty())
            {
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Paragraph;
                block.spans = ParseMarkdownInline(paragraph);
                blocks.push_back(std::move(block));
                paragraph.clear();
            }
        };
        const auto flushItem = [&]()
        {
            if (itemBlock < blocks.size())
            {
                blocks[itemBlock].spans = ParseMarkdownInline(itemText);
            }
            itemBlock = static_cast<std::size_t>(-1);
            itemText.clear();
        };

        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            const auto line = lines[index];
            const auto trimmed = TrimView(line);

            char fenceCharacter = 0;
            std::size_t fenceLength = 0;
            std::string info;
            if (IsFence(line, fenceCharacter, fenceLength, info))
            {
                flushParagraph();
                flushItem();
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::CodeBlock;
                block.language = info.substr(0, info.find_first_of(" \t{"));
                block.closed = false;
                std::size_t next = index + 1;
                for (; next < lines.size(); ++next)
                {
                    char closeCharacter = 0;
                    std::size_t closeLength = 0;
                    std::string closeInfo;
                    if (IsFence(lines[next], closeCharacter, closeLength, closeInfo) && closeCharacter == fenceCharacter &&
                        closeLength >= fenceLength && closeInfo.empty())
                    {
                        block.closed = true;
                        break;
                    }
                    if (!block.code.empty() || next > index + 1)
                    {
                        block.code += '\n';
                    }
                    block.code += std::string(lines[next]);
                }
                blocks.push_back(std::move(block));
                index = next; // the closing fence (or the end)
                continue;
            }

            if (trimmed.empty())
            {
                flushParagraph();
                flushItem();
                continue;
            }

            // Heading
            if (trimmed[0] == '#')
            {
                std::size_t level = 0;
                while (level < trimmed.size() && trimmed[level] == '#')
                {
                    ++level;
                }
                if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ')
                {
                    flushParagraph();
                    flushItem();
                    MarkdownBlock block;
                    block.kind = MarkdownBlockKind::Heading;
                    block.level = static_cast<int>(level);
                    auto content = TrimView(trimmed.substr(level));
                    while (!content.empty() && content.back() == '#')
                    {
                        content.remove_suffix(1);
                    }
                    block.spans = ParseMarkdownInline(TrimView(content));
                    blocks.push_back(std::move(block));
                    continue;
                }
            }

            if (IsRule(line))
            {
                flushParagraph();
                flushItem();
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Rule;
                blocks.push_back(std::move(block));
                continue;
            }

            // Quote
            if (trimmed[0] == '>')
            {
                flushParagraph();
                flushItem();
                int depth = 0;
                auto content = trimmed;
                while (!content.empty() && content[0] == '>')
                {
                    ++depth;
                    content.remove_prefix(1);
                    if (!content.empty() && content[0] == ' ')
                    {
                        content.remove_prefix(1);
                    }
                }
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Quote;
                block.level = depth;
                block.spans = ParseMarkdownInline(content);
                blocks.push_back(std::move(block));
                continue;
            }

            // Table: a row with pipes followed by a separator row
            if (trimmed.find('|') != std::string_view::npos && index + 1 < lines.size() && IsTableSeparator(TrimView(lines[index + 1])))
            {
                flushParagraph();
                flushItem();
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::Table;
                block.header = true;
                std::size_t next = index;
                for (; next < lines.size(); ++next)
                {
                    const auto row = TrimView(lines[next]);
                    if (row.empty() || row.find('|') == std::string_view::npos)
                    {
                        break;
                    }
                    if (next == index + 1)
                    {
                        continue; // the separator
                    }
                    std::vector<std::vector<MarkdownSpan>> cells;
                    for (const auto cell : SplitCells(row))
                    {
                        cells.push_back(ParseMarkdownInline(cell));
                    }
                    block.rows.push_back(std::move(cells));
                }
                blocks.push_back(std::move(block));
                index = next - 1;
                continue;
            }

            // List item (indentation gives the depth)
            std::size_t indent = 0;
            for (const char c : line)
            {
                if (c == ' ')
                {
                    ++indent;
                }
                else if (c == '\t')
                {
                    indent += 4;
                }
                else
                {
                    break;
                }
            }
            bool ordered = false;
            int number = 0;
            const auto marker = ListMarker(trimmed, ordered, number);
            if (marker != std::string_view::npos)
            {
                flushParagraph();
                flushItem();
                MarkdownBlock block;
                block.kind = MarkdownBlockKind::ListItem;
                block.level = static_cast<int>(indent / 2);
                block.ordered = ordered;
                block.number = number;
                auto content = TrimView(trimmed.substr(marker));
                if (content.size() >= 3 && content[0] == '[' && (content[1] == ' ' || content[1] == 'x' || content[1] == 'X') &&
                    content[2] == ']' && (content.size() == 3 || content[3] == ' '))
                {
                    block.checkbox = true;
                    block.checked = content[1] != ' ';
                    content = TrimView(content.substr(3));
                }
                blocks.push_back(std::move(block));
                itemBlock = blocks.size() - 1;
                itemText = std::string(content);
                continue;
            }

            // A continuation of a list item: indented text right after it
            if (itemBlock < blocks.size() && indent >= 2)
            {
                itemText += '\n';
                itemText += std::string(trimmed);
                continue;
            }

            flushItem();
            if (!paragraph.empty())
            {
                paragraph += '\n';
            }
            paragraph += std::string(trimmed);
        }
        flushParagraph();
        flushItem();
        return blocks;
    }
}
