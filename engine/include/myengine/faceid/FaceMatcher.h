// FaceMatcher.h
// The decisions of Face ID that need neither the camera nor the models: the cosine similarity of embeddings, the decision "this is the profile" after several frames in a
// row, the lockout after failed attempts and the check "turn your head" (docs/editor/faceid.md). Time comes as an argument, so the tests do not wait.

#pragma once

#include <myengine/faceid/FaceTypes.h>

#include <cstddef>
#include <vector>

namespace myengine::faceid
{
    struct FaceProfile;

    // 0.363: the threshold of SFace for the cosine similarity from the documentation of OpenCV (the value that the authors measured on LFW); the editor
    // takes the threshold from its settings and the doc tells how it was chosen
    constexpr float kDefaultThreshold = 0.363f;

    void Normalize(FaceEmbedding& embedding);
    // The cosine of the angle between two embeddings (not normalised ones are fine)
    float Cosine(const FaceEmbedding& a, const FaceEmbedding& b);

    struct MatchResult
    {
        int profile = -1;      // the index of the best profile, -1: there is none
        float score = -1.0f;   // the best cosine similarity to one of its embeddings
    };
    MatchResult BestMatch(const std::vector<FaceProfile>& profiles, const FaceEmbedding& embedding);

    // "This is the profile" after `framesNeeded` frames in a row of the same profile above the threshold; "this is nobody" after `rejectFrames` frames in a row
    // that do not reach it. A frame without a face is not fed at all.
    class RecognitionDecider
    {
    public:
        enum class State
        {
            Collecting,
            Accepted,
            Rejected
        };

        explicit RecognitionDecider(int framesNeeded = 5, float threshold = kDefaultThreshold, int rejectFrames = 12);
        void Reset();
        State Feed(const MatchResult& match);

        State GetState() const { return state_; }
        int AcceptedProfile() const { return state_ == State::Accepted ? profile_ : -1; }
        int Streak() const { return streak_; }
        int FramesNeeded() const { return framesNeeded_; }
        float Threshold() const { return threshold_; }
        void SetThreshold(float threshold) { threshold_ = threshold; }

    private:
        int framesNeeded_;
        float threshold_;
        int rejectFrames_;
        State state_ = State::Collecting;
        int profile_ = -1;
        int streak_ = 0;
        int misses_ = 0;
    };

    // Three failed attempts (a stranger, a timeout) lock the login for thirty seconds. A success clears the count.
    class AttemptLimiter
    {
    public:
        explicit AttemptLimiter(int maxFailures = 3, double lockSeconds = 30.0);
        void Failure(double now);
        void Success();
        bool Locked(double now) const;
        double RemainingSeconds(double now) const;
        int Failures() const { return failures_; }

    private:
        int maxFailures_;
        double lockSeconds_;
        int failures_ = 0;
        double lockedUntil_ = -1.0;
    };

    // The check against a photo: the person is asked to turn the head to one side, and the points of YuNet must really move (Yaw of the face changes by a fraction
    // of an eye distance). A photo held to the camera does not turn its head; a video or a mask do, so this is a protection against the simple case only.
    class LivenessChallenge
    {
    public:
        enum class Direction
        {
            ImageLeft,    // the nose goes to the left of the picture of the camera
            ImageRight
        };
        enum class State
        {
            Idle,
            Waiting,
            Passed,
            Failed
        };

        void Start(Direction direction, double now, double timeoutSeconds = 8.0, float shift = 0.25f);
        // One frame with a face: its Yaw; with no face call NoFace
        State Feed(float yaw, double now);
        void NoFace() {}
        void Reset() { state_ = State::Idle; }

        State GetState() const { return state_; }
        Direction GetDirection() const { return direction_; }
        // 0..1: how far the head has turned of what is asked
        float Progress() const { return progress_; }

    private:
        State state_ = State::Idle;
        Direction direction_ = Direction::ImageLeft;
        double started_ = 0.0;
        double timeout_ = 8.0;
        float shift_ = 0.25f;
        int baselineSamples_ = 0;
        float baseline_ = 0.0f;
        float progress_ = 0.0f;
    };
}
