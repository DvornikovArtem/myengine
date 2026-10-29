#include <myengine/faceid/FaceMatcher.h>

#include <myengine/faceid/FaceProfileStore.h>

#include <algorithm>
#include <cmath>

namespace myengine::faceid
{
    void Normalize(FaceEmbedding& embedding)
    {
        double sum = 0.0;
        for (const float v : embedding)
        {
            sum += static_cast<double>(v) * v;
        }
        if (sum < 1e-12)
        {
            return;
        }
        const float inverse = static_cast<float>(1.0 / std::sqrt(sum));
        for (float& v : embedding)
        {
            v *= inverse;
        }
    }

    float Cosine(const FaceEmbedding& a, const FaceEmbedding& b)
    {
        double dot = 0.0;
        double na = 0.0;
        double nb = 0.0;
        for (int i = 0; i < kEmbeddingSize; ++i)
        {
            dot += static_cast<double>(a[static_cast<std::size_t>(i)]) * b[static_cast<std::size_t>(i)];
            na += static_cast<double>(a[static_cast<std::size_t>(i)]) * a[static_cast<std::size_t>(i)];
            nb += static_cast<double>(b[static_cast<std::size_t>(i)]) * b[static_cast<std::size_t>(i)];
        }
        if (na < 1e-12 || nb < 1e-12)
        {
            return 0.0f;
        }
        return static_cast<float>(dot / std::sqrt(na * nb));
    }

    MatchResult BestMatch(const std::vector<FaceProfile>& profiles, const FaceEmbedding& embedding)
    {
        MatchResult best;
        for (std::size_t p = 0; p < profiles.size(); ++p)
        {
            for (const FaceEmbedding& stored : profiles[p].embeddings)
            {
                const float score = Cosine(stored, embedding);
                if (score > best.score)
                {
                    best.score = score;
                    best.profile = static_cast<int>(p);
                }
            }
        }
        return best;
    }

    RecognitionDecider::RecognitionDecider(const int framesNeeded, const float threshold, const int rejectFrames)
        : framesNeeded_(std::max(framesNeeded, 1)), threshold_(threshold), rejectFrames_(std::max(rejectFrames, 1))
    {
    }

    void RecognitionDecider::Reset()
    {
        state_ = State::Collecting;
        profile_ = -1;
        streak_ = 0;
        misses_ = 0;
    }

    RecognitionDecider::State RecognitionDecider::Feed(const MatchResult& match)
    {
        if (state_ != State::Collecting)
        {
            return state_;
        }
        const bool hit = match.profile >= 0 && match.score >= threshold_;
        if (hit && (streak_ == 0 || match.profile == profile_))
        {
            profile_ = match.profile;
            ++streak_;
            misses_ = 0;
            if (streak_ >= framesNeeded_)
            {
                state_ = State::Accepted;
            }
        }
        else
        {
            // a frame of another profile or below the threshold breaks the row
            streak_ = hit ? 1 : 0;
            profile_ = hit ? match.profile : -1;
            ++misses_;
            if (hit)
            {
                misses_ = 0;
            }
            if (misses_ >= rejectFrames_)
            {
                state_ = State::Rejected;
            }
        }
        return state_;
    }

    AttemptLimiter::AttemptLimiter(const int maxFailures, const double lockSeconds) : maxFailures_(std::max(maxFailures, 1)), lockSeconds_(lockSeconds) {}

    void AttemptLimiter::Failure(const double now)
    {
        ++failures_;
        if (failures_ >= maxFailures_)
        {
            lockedUntil_ = now + lockSeconds_;
            failures_ = 0;
        }
    }

    void AttemptLimiter::Success()
    {
        failures_ = 0;
        lockedUntil_ = -1.0;
    }

    bool AttemptLimiter::Locked(const double now) const { return now < lockedUntil_; }

    double AttemptLimiter::RemainingSeconds(const double now) const { return std::max(0.0, lockedUntil_ - now); }

    void LivenessChallenge::Start(const Direction direction, const double now, const double timeoutSeconds, const float shift)
    {
        state_ = State::Waiting;
        direction_ = direction;
        started_ = now;
        timeout_ = timeoutSeconds;
        shift_ = shift;
        baselineSamples_ = 0;
        baseline_ = 0.0f;
        progress_ = 0.0f;
    }

    LivenessChallenge::State LivenessChallenge::Feed(const float yaw, const double now)
    {
        if (state_ != State::Waiting)
        {
            return state_;
        }
        if (now - started_ > timeout_)
        {
            state_ = State::Failed;
            return state_;
        }
        // the first frames give the neutral position of the head
        constexpr int kBaselineFrames = 4;
        if (baselineSamples_ < kBaselineFrames)
        {
            baseline_ += yaw;
            ++baselineSamples_;
            if (baselineSamples_ == kBaselineFrames)
            {
                baseline_ /= static_cast<float>(kBaselineFrames);
            }
            return state_;
        }
        const float sign = direction_ == Direction::ImageRight ? 1.0f : -1.0f;
        const float moved = (yaw - baseline_) * sign;
        progress_ = std::clamp(moved / shift_, 0.0f, 1.0f);
        if (moved >= shift_)
        {
            state_ = State::Passed;
        }
        return state_;
    }
}
