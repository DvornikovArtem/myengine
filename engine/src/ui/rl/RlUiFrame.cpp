#include <myengine/ui/rl/RlUiFrame.h>

#include <algorithm>
#include <cmath>

#include <myengine/ui/rl/RlUiPaint.h>

#include "RlUiInternal.h"

namespace myengine::rlui
{
    namespace
    {
        constexpr float kHoverEps = 0.5f;
    }

    void Frame::Begin(const FrameDesc& desc, const RawInput& raw, ImDrawList* drawList, const Fonts* fonts)
    {
        drawList_ = drawList;
        fonts_ = fonts;
        deltaTime_ = std::max(desc.deltaTime, 0.0f);
        time_ = desc.time;

        const float width = std::max(desc.width, 1.0f);
        const float height = std::max(desc.height, 1.0f);
        const float scale = std::max(std::min(height / 1080.0f, width / 1920.0f) * std::clamp(desc.uiScale, 0.5f, 2.0f), 0.05f);
        metrics_.scale = scale;
        metrics_.window = {width, height};
        metrics_.designWindow = {width / scale, height / scale};
        const float menuWidth = 1920.0f * scale;
        const float menuHeight = 1080.0f * scale;
        metrics_.menu = {(width - menuWidth) * 0.5f, (height - menuHeight) * 0.5f, menuWidth, menuHeight};

        input_.Update(raw, deltaTime_, textEditingNext_);
        textEditingNext_ = false;

        lastItems_ = std::move(items_);
        items_.clear();
        space_ = Space::Menu;
        alpha_ = 1.0f;
        scrollOffset_ = 0.0f;
        activeScroll_ = nullptr;

        // The scope of the frame is the one that was pushed last frame (the screen below the dialog is laid out before the dialog pushes it)
        if (activeScope_ != nextScope_)
        {
            if (activeScope_ == kNoId)
            {
                savedFocus_ = focus_;       // a dialog opens: remember where the focus was
            }
            const bool closing = nextScope_ == kNoId;
            activeScope_ = nextScope_;
            focus_ = closing ? savedFocus_ : kNoId;
        }
        nextScope_ = kNoId;
        scopes_.clear();

        transition_ = std::min(transition_ + deltaTime_ / style::kScreenTransition, 1.0f);
        for (Toast& toast : toasts_)
        {
            toast.left -= deltaTime_;
        }
        toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.left <= 0.0f; }), toasts_.end());
    }

    void Frame::SetSpace(const Space space) { space_ = space; }

    Vec2 Frame::ToScreen(const Vec2 p) const
    {
        if (space_ == Space::Screen)
        {
            return {p.x * metrics_.scale, p.y * metrics_.scale};
        }
        return {metrics_.menu.x + (p.x + TransitionOffset()) * metrics_.scale, metrics_.menu.y + (p.y - scrollOffset_) * metrics_.scale};
    }

    Rect Frame::ToScreen(const Rect& r) const
    {
        const Vec2 origin = ToScreen(Vec2{r.x, r.y});
        return {origin.x, origin.y, r.w * metrics_.scale, r.h * metrics_.scale};
    }

    Vec2 Frame::ToDesign(const Vec2 p) const
    {
        if (space_ == Space::Screen)
        {
            return {p.x / metrics_.scale, p.y / metrics_.scale};
        }
        return {(p.x - metrics_.menu.x) / metrics_.scale - TransitionOffset(), (p.y - metrics_.menu.y) / metrics_.scale + scrollOffset_};
    }

    bool Frame::TakeNav(const Nav nav)
    {
        if (!input_.Pressed(nav))
        {
            return false;
        }
        input_.Consume(nav);
        return true;
    }

    void Frame::SetFocus(const Id id)
    {
        focus_ = id;
        focusFromCode_ = id != kNoId;
    }

    void Frame::SetDefaultFocus(const Id id)
    {
        const Id scope = scopes_.empty() ? activeScope_ : scopes_.back();
        for (auto& entry : defaults_)
        {
            if (entry.first == scope)
            {
                entry.second = id;
                return;
            }
        }
        defaults_.push_back({scope, id});
    }

    void Frame::ResetFocus()
    {
        focus_ = kNoId;
        StartTransition();
    }

    void Frame::StartTransition() { transition_ = 0.0f; }

    float Frame::TransitionAlpha() const
    {
        const float t = std::clamp(transition_, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t);
    }

    float Frame::TransitionOffset() const
    {
        const float t = std::clamp(transition_, 0.0f, 1.0f);
        return -40.0f * (1.0f - t) * (1.0f - t);
    }

    Frame::StateRecord& Frame::State(const Id id)
    {
        for (StateRecord& record : states_)
        {
            if (record.id == id)
            {
                return record;
            }
        }
        states_.push_back({id, 0, 0.0f});
        return states_.back();
    }

    int& Frame::IntState(const Id id) { return State(id).i; }
    float& Frame::FloatState(const Id id) { return State(id).f; }

    void Frame::PushScope(const Id scope)
    {
        scopes_.push_back(scope);
        nextScope_ = scope;
    }

    void Frame::PopScope()
    {
        if (!scopes_.empty())
        {
            scopes_.pop_back();
        }
    }

    ItemState Frame::Item(const Id id, const Rect& designRect, const bool enabled)
    {
        ItemState state;
        const Id scope = scopes_.empty() ? kNoId : scopes_.back();
        const Rect screen = ToScreen(designRect);
        const bool live = enabled && scope == activeScope_;
        state.enabled = enabled;

        // a list: what is outside its window cannot be pointed at, and a focused item scrolls into the window
        bool visible = true;
        if (activeScroll_ != nullptr)
        {
            const Rect& view = scrollView_;
            const float top = designRect.y - scrollOffset_;
            visible = top + designRect.h > view.y && top < view.y + view.h;
        }

        ItemRecord record;
        record.id = id;
        record.scope = scope;
        record.rect = screen;
        record.enabled = live;
        record.design = designRect;
        items_.push_back(record);

        const Vec2 mouse = input_.MousePosition();
        state.hovered = live && visible && input_.Raw().windowActive && screen.Contains(mouse);
        if (state.hovered && input_.MouseMoved())
        {
            focus_ = id;
        }
        state.focused = live && focus_ == id;
        if (state.focused)
        {
            if (input_.Pressed(Nav::Confirm))
            {
                input_.Consume(Nav::Confirm);
                state.activated = true;
            }
        }
        if (state.hovered && input_.MousePressed())
        {
            focus_ = id;
            state.focused = true;
            state.activated = true;
        }

        // the glow of the focus fades in over 0.08 s and out as fast
        GlowRecord* glow = nullptr;
        for (GlowRecord& g : glows_)
        {
            if (g.id == id)
            {
                glow = &g;
                break;
            }
        }
        if (glow == nullptr)
        {
            glows_.push_back({id, 0.0f});
            glow = &glows_.back();
        }
        const float step = deltaTime_ / style::kFocusGlowIn;
        glow->value = state.focused ? std::min(glow->value + step, 1.0f) : std::max(glow->value - step, 0.0f);
        state.glow = glow->value;

        if (state.focused && activeScroll_ != nullptr)
        {
            // `designRect` is in content coordinates: the offset of the list brings the item into the window (from the next frame)
            const float margin = 8.0f;
            float& offset = State(scrollId_).f;
            if (designRect.y - offset < scrollView_.y + margin)
            {
                offset = std::max(designRect.y - scrollView_.y - margin, 0.0f);
            }
            else if (designRect.y + designRect.h - offset > scrollView_.y + scrollView_.h - margin)
            {
                offset = designRect.y + designRect.h - (scrollView_.y + scrollView_.h) + margin;
            }
        }
        return state;
    }

    void Frame::MoveFocus(const Nav direction)
    {
        const ItemRecord* current = nullptr;
        for (const ItemRecord& item : items_)
        {
            if (item.id == focus_ && item.enabled)
            {
                current = &item;
                break;
            }
        }
        if (current == nullptr)
        {
            return;
        }
        const float cx = current->rect.x + current->rect.w * 0.5f;
        const float cy = current->rect.y + current->rect.h * 0.5f;
        const ItemRecord* best = nullptr;
        float bestCost = 1e30f;
        for (const ItemRecord& item : items_)
        {
            if (!item.enabled || item.id == focus_ || item.scope != activeScope_)
            {
                continue;
            }
            const float ix = item.rect.x + item.rect.w * 0.5f;
            const float iy = item.rect.y + item.rect.h * 0.5f;
            float primary = 0.0f;
            float lateral = 0.0f;
            switch (direction)
            {
            case Nav::Up: primary = cy - iy; lateral = std::fabs(ix - cx); break;
            case Nav::Down: primary = iy - cy; lateral = std::fabs(ix - cx); break;
            case Nav::Left: primary = cx - ix; lateral = std::fabs(iy - cy); break;
            case Nav::Right: primary = ix - cx; lateral = std::fabs(iy - cy); break;
            default: return;
            }
            if (primary <= kHoverEps)
            {
                continue;
            }
            const float cost = primary + 3.0f * lateral;
            if (cost < bestCost)
            {
                bestCost = cost;
                best = &item;
            }
        }
        if (best != nullptr)
        {
            focus_ = best->id;
        }
    }

    void Frame::ShowToast(const std::string_view text, const Color color, const float seconds)
    {
        toasts_.push_back({std::string(text), color, seconds, seconds});
        if (toasts_.size() > 3)
        {
            toasts_.erase(toasts_.begin());
        }
    }

    void Frame::DrawToasts()
    {
        const Space saved = space_;
        space_ = Space::Menu;
        float y = 940.0f;
        for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it)
        {
            const float fade = std::clamp(std::min(it->left / 0.3f, (it->total - it->left) / 0.1f), 0.0f, 1.0f);
            const float width = paint::TextWidth(*this, FontStyle::Bold, 24.0f, it->text) + 64.0f;
            const Rect plate{960.0f - width * 0.5f, y, width, 48.0f};
            paint::Parallelogram(*this, plate, style::kNavySolid.Faded(0.95f * fade));
            paint::ParallelogramOutline(*this, plate, it->color.Faded(fade), 1.5f);
            paint::Text(*this, FontStyle::Bold, 24.0f, {960.0f, y + 12.0f}, it->color.Faded(fade), it->text, Align::Center);
            y -= 58.0f;
        }
        space_ = saved;
    }

    void Frame::End()
    {
        // The focus of the scope: still on an item that was laid out this frame, else the default one, else the first one
        bool found = false;
        for (const ItemRecord& item : items_)
        {
            if (item.id == focus_ && item.enabled && item.scope == activeScope_)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            Id fallback = kNoId;
            for (const auto& entry : defaults_)
            {
                if (entry.first == activeScope_)
                {
                    fallback = entry.second;
                }
            }
            bool fallbackLive = false;
            for (const ItemRecord& item : items_)
            {
                if (item.id == fallback && item.enabled && item.scope == activeScope_)
                {
                    fallbackLive = true;
                    break;
                }
            }
            if (!fallbackLive)
            {
                fallback = kNoId;
                for (const ItemRecord& item : items_)
                {
                    if (item.enabled && item.scope == activeScope_)
                    {
                        fallback = item.id;
                        break;
                    }
                }
            }
            focus_ = fallback;
        }
        else
        {
            // directions nobody used move the focus to the nearest item that way
            for (const Nav direction : {Nav::Up, Nav::Down, Nav::Left, Nav::Right})
            {
                if (input_.Pressed(direction))
                {
                    MoveFocus(direction);
                    input_.Consume(direction);
                    break;
                }
            }
        }
        defaults_.clear();
        if (!toasts_.empty())
        {
            DrawToasts();
        }
        activeScroll_ = nullptr;
    }
}
