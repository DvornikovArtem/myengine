// FacePhotos.h
// Faces from photographs (docs/editor/faceid.md, "Faces from photos"): a jpg or png file becomes one embedding when it shows exactly one face that is big enough, sharp enough
// and not turned away, and the file is refused with a reason (the words for the screen) when it does not. The same YuNet and SFace as the camera: the picture is not kept
// anywhere, it lives in memory until the embedding is made. No camera is involved, so a photo can not pass the "turn your head" check of the login: it only fills a profile.

#pragma once

#include <myengine/faceid/FaceModels.h>
#include <myengine/faceid/FaceTypes.h>

#include <filesystem>
#include <string>
#include <vector>

namespace myengine::faceid
{
    constexpr float kPhotoMinFaceWidth = 120.0f;   // the same as the recording by the camera: pixels of the original photo
    constexpr float kPhotoMinSharpness = 30.0f;    // the variance of the Laplacian of the aligned face, the same as the camera
    constexpr float kPhotoMaxYaw = 0.55f;          // the nose offset in eye distances (FaceDetection::Yaw): a light turn is fine, a profile is not
    constexpr float kPhotoOtherFaceShare = 0.25f;  // another face with more than this share of the area of the main one: "one person in a photo"
    constexpr float kPhotoSamePerson = 0.30f;      // two photos of one person look like each other at least this much (a chain counts); the NASA test set: other people up to 0.25, one person 0.42 and more, a helmet visor 0.2
    constexpr float kPhotoDuplicate = 0.995f;      // the same picture chosen twice
    constexpr int kPhotoWorkSide = 1280;           // a larger photo is shrunk (by whole factors, averaging) to this long side before the search
    constexpr std::uintmax_t kPhotoMaxBytes = 64u * 1024u * 1024u;

    // What came of one photo
    struct PhotoResult
    {
        bool accepted = false;
        std::string reason;                // the words when it is not accepted ("no face found", ...)
        FaceEmbedding embedding{};         // the face as it is on the photo
        FaceEmbedding mirrored{};          // the same face turned left to right: tops a short list up (docs/editor/faceid.md)
        float faceWidth = 0.0f;            // pixels of the original photo
        float sharpness = 0.0f;
        float yaw = 0.0f;
    };

    // Reads a jpg or png file into RGB; false and the reason when it is not a picture or is too large
    bool LoadPhoto(const std::filesystem::path& file, FaceImage& out, std::string& reason);

    // Finds the one face of the photo, checks it and makes the embeddings. A photo that was taken sideways (the phone's orientation flag is not read) is turned and looked at
    // again when no face is found upright. `models` must be loaded.
    PhotoResult ExaminePhoto(FaceModels& models, const FaceImage& photo);

    // Image helpers (also for the tests): a whole-factor shrink by averaging, a quarter turn clockwise, a mirror
    FaceImage ShrinkImage(const FaceImage& image, int factor);
    FaceImage RotateClockwise(const FaceImage& image);
    FaceImage MirrorImage(const FaceImage& image);

    // The standard Open dialog for several pictures at once (blocks the editor while it is open); an empty list is a cancelled dialog
    std::vector<std::filesystem::path> ChoosePhotoFiles();
}
