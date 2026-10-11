// Markdown.h

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace myengine::assistant
{
    // The subset of Markdown the assistant's answers use: headings, paragraphs, lists, quotes, fenced code, tables,
    // rules and the inline styles bold / italic / code / strike-through / links. Pure text processing, no UI.
    enum class MarkdownStyle : std::uint8_t
    {
        Normal,
        Bold,
        Italic,
        BoldItalic,
        Code,
        Strike,
        Link,
    };

    struct MarkdownSpan
    {
        std::string text;
        MarkdownStyle style = MarkdownStyle::Normal;
        std::string url; // only for Link
    };

    enum class MarkdownBlockKind : std::uint8_t
    {
        Paragraph,
        Heading,
        ListItem,
        Quote,
        CodeBlock,
        Table,
        Rule,
    };

    struct MarkdownBlock
    {
        MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
        int level = 0;        // Heading: 1..6, ListItem: nesting depth starting at 0, Quote: depth starting at 1
        bool ordered = false; // ListItem
        int number = 0;       // ListItem: the number of an ordered item
        bool checkbox = false;   // ListItem: a task list item "[ ]" / "[x]"
        bool checked = false;
        std::vector<MarkdownSpan> spans; // Paragraph, Heading, ListItem, Quote
        std::string code;                // CodeBlock: the text without the fences
        std::string language;            // CodeBlock: the info string ("cpp", "python", "diff"), may be empty
        bool closed = true;              // CodeBlock: the closing fence was seen (false while an answer is streaming)
        // Table: row-major cells; the first row is the header when `header` is set
        std::vector<std::vector<std::vector<MarkdownSpan>>> rows;
        bool header = false;
    };

    // Parses a whole text. Never throws; unknown constructs stay plain text. An unclosed code fence runs to the end,
    // so a half-streamed answer renders sensibly.
    std::vector<MarkdownBlock> ParseMarkdown(std::string_view text);

    // Inline styles of one line (or paragraph)
    std::vector<MarkdownSpan> ParseMarkdownInline(std::string_view text);

    // The plain text of spans: styles dropped (for tooltips and titles)
    std::string MarkdownPlainText(const std::vector<MarkdownSpan>& spans);
}
