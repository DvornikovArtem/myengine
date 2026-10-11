// AssistantMarkdownView.h: draws parsed Markdown (assistant/Markdown.h) with ImGui. Internal to engine/src/ui.

#pragma once

#include <string>
#include <vector>

#include <myengine/assistant/Markdown.h>

namespace myengine::ui
{
    struct MarkdownDrawResult
    {
        std::string clickedLink; // a link was clicked (http / https only)
        std::string copiedCode;  // the Copy button of a code block was pressed: exactly the code
        bool codeCopied = false;
    };

    // Draws the blocks from the cursor down, `width` wide, in the editor fonts (body 14, line height 1.5).
    // `idScope` keeps ids of the widgets inside code blocks apart between messages.
    MarkdownDrawResult DrawMarkdown(const std::vector<assistant::MarkdownBlock>& blocks, float width, int idScope);
}
