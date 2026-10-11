// AssistantImages.h: pictures for the assistant's composer (clipboard, thumbnails). Windows only (GDI+).
// Internal to engine/src/ui.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace myengine::ui::assistant_images
{
    // True when the clipboard has a bitmap or a list of files
    bool ClipboardHasContent();

    // Takes what Ctrl+V brings: files copied in Explorer come back as paths, a bitmap (a screenshot, "Copy image") is
    // written to `directory` as PNG and its path is returned. Empty result when the clipboard has neither.
    std::vector<std::filesystem::path> PasteFromClipboard(const std::filesystem::path& directory, std::string& error);

    // The media type of a picture the model accepts, chosen by the extension: png, jpeg, gif, webp. Empty otherwise
    std::string ImageMediaType(const std::filesystem::path& file);

    // A picture format the model does not accept (bmp, tga, dds, tiff, ico, hdr, psd): sending it as a file is no use
    bool IsUnsupportedPicture(const std::filesystem::path& file);

    // Decodes png / jpeg / gif / bmp and scales to fit `maxSize` x `maxSize`. False for other formats (webp) or errors
    bool LoadThumbnail(const std::filesystem::path& file, int maxSize, std::vector<unsigned char>& rgba, int& width, int& height);
}
