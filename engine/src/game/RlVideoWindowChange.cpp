#include <myengine/game/RlVideoWindowChange.h>

namespace myengine::game
{
    bool SameWindow(const RlWindowChoice& a, const RlWindowChoice& b)
    {
        if (a.mode != b.mode)
        {
            return false;
        }
        return a.mode == 1 || (a.width == b.width && a.height == b.height);
    }

    void RlVideoWindowChange::Set(const RlWindowChoice& applied)
    {
        applied_ = applied;
        pending_ = applied;
        backup_ = applied;
        keeping_ = false;
        remaining_ = 0.0f;
    }

    void RlVideoWindowChange::SetPendingMode(const int mode)
    {
        if (!keeping_)
        {
            pending_.mode = mode;
        }
    }

    void RlVideoWindowChange::SetPendingSize(const int width, const int height)
    {
        if (!keeping_)
        {
            pending_.width = width;
            pending_.height = height;
        }
    }

    bool RlVideoWindowChange::Dirty() const
    {
        return !SameWindow(pending_, applied_);
    }

    bool RlVideoWindowChange::Apply()
    {
        if (keeping_ || !Dirty())
        {
            return false;
        }
        backup_ = applied_;
        applied_ = pending_;
        keeping_ = true;
        remaining_ = kKeepSeconds;
        return true;
    }

    void RlVideoWindowChange::Keep()
    {
        keeping_ = false;
        remaining_ = 0.0f;
        backup_ = applied_;
    }

    bool RlVideoWindowChange::Revert()
    {
        if (!keeping_)
        {
            return false;
        }
        applied_ = backup_;
        pending_ = applied_;
        keeping_ = false;
        remaining_ = 0.0f;
        return true;
    }

    bool RlVideoWindowChange::Tick(const float seconds)
    {
        if (!keeping_)
        {
            return false;
        }
        remaining_ -= seconds;
        return remaining_ <= 0.0f && Revert();
    }

    void RlVideoWindowChange::Discard()
    {
        if (!keeping_)
        {
            pending_ = applied_;
        }
    }
}
