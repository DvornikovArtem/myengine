#pragma once

namespace myengine::ui::detail
{
    // Dark editor theme in the spirit of Unreal Engine 5: near-black panels, one blue accent, 2-4 px rounding,
    // dense paddings. Applies to the current ImGui context, so call it right after ImGui::CreateContext.
    void ApplyEditorTheme();
}
