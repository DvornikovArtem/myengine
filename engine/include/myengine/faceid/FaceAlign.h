// FaceAlign.h
// The alignment of a face by the five points of YuNet to the 112 x 112 picture that SFace takes (the template of ArcFace / SFace). OpenCV is not used: the similarity
// transform (rotation, one scale, shift) is the least squares one of the five pairs of points (the closed form of Umeyama for two dimensions), the picture is
// warped with bilinear sampling.

#pragma once

#include <myengine/faceid/FaceTypes.h>

namespace myengine::faceid
{
    // x' = a * x - b * y + tx,  y' = b * x + a * y + ty
    struct Similarity
    {
        double a = 1.0;
        double b = 0.0;
        double tx = 0.0;
        double ty = 0.0;

        void Apply(double x, double y, double& outX, double& outY) const
        {
            outX = a * x - b * y + tx;
            outY = b * x + a * y + ty;
        }
        double Scale() const;
    };

    // Where the five points are in the aligned picture (112 x 112): the same order as FaceDetection::landmarks
    extern const float kAlignTemplate[10];

    // The transform that takes the five points `from` onto the five points `to`; false: the points are all in one place
    bool EstimateSimilarity(const float from[10], const float to[10], Similarity& out);

    // The face as a 112 x 112 picture; false when the image is empty or the points are degenerate
    bool AlignFace(const FaceImage& image, const float landmarks[10], FaceImage& out);

    // How sharp a picture is: the variance of its Laplacian over the grey values (a blurred face has a small one)
    float Sharpness(const FaceImage& image);
}
