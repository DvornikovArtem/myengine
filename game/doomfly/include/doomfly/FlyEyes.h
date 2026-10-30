// FlyEyes.h
//
// The eyes of the fly player (docs/game/flydoom-fly.md): the frame of DOOM to the drive of the photoreceptors R1-R6 of the connectome.
//
// Where a receptor looks. The connectome gives a column of the optic lobe (assignedOlHex1/2) to the neurons of the lamina and the medulla,
// not to the photoreceptors; a receptor takes the column of the lamina cells it feeds (the most common column of its L1, L2, L3 targets), as DOOMFLY does.
// The lattice of the columns lies in the body of the fly like this (the fit of the positions of the lamina cells L1-L3 and of Mi1 against the columns,
// tests/doomfly/FlyProbe2.cpp, R^2 0.95-1.0 for the vertical axis, 0.7-0.9 for the horizontal one; both eyes the same, the labels are mirrored):
//     hx = hex2 - hex1 / 2,  hy = hex1 * sqrt(3) / 2
//     up (dorsal) is   0.6 hx + 0.8 hy
//     front (anterior) is  -0.8 hx + 0.6 hy        (the medulla has it the other way round: the chiasm between the lamina and the medulla turns the horizontal axis)
// The left eye gets the left 60 % of the screen with its front edge at 60 %, the right eye the right 60 % with its front edge at 40 %: the 20 % in
// the middle are seen by both (a fly sees about 20 degrees of the world with both eyes in front).
//
// What a receptor feels. Its column's pixel (bilinear, over a small box) gives a luminance (Rec. 709 of the linear colours), the luminance goes through a low
// pass (tau 10 ms, applied by the player in steps of a few ms), and the current is the saturating response of the receptor: drive = max * L / (half + L),
// as in DOOMFLY (30 mV and 0.02). The lamina cells L1, L2, L3, L5 get a constant 12 mV: without it they are silent (the histamine of the receptors inhibits them).
// A motion channel adds a transient: the change of the luminance against its slow average, ON positive, OFF negative.

#pragma once

#include <flybrain/Connectome.h>

#include <cstdint>
#include <string>
#include <vector>

namespace doomfly
{
    struct EyesConfig
    {
        float leftEyeFrom = 0.0f;               // the part of the width of the picture each eye sees
        float leftEyeTo = 0.6f;
        float rightEyeFrom = 0.4f;
        float rightEyeTo = 1.0f;
        float viewHeightFraction = 168.0f / 200.0f;   // the 3D view of DOOM without the status bar
        float maxDriveMv = 30.0f;
        float halfLuminance = 0.02f;
        float tonicMv = 12.0f;                  // the constant drive of L1, L2, L3, L5
        float lowPassMs = 10.0f;
        float motionGainMv = 0.0f;              // (not used: the motion goes through the columns, below)
        float motionAverageMs = 100.0f;         // the slow average the change of a column is measured against
        // The motion and the looming (D1): the neurons of the columns that carry the changes of the light, driven by the change of the luminance of their column
        // against its slow average ((L - slow) / (slow + 0.05), -1..1): the ON cells by a rise, the OFF cells by a fall. T4/T5, LPLC2 and LC4 take it from there.
        // 0: no motion channel (the eyes of N1: the photoreceptors only).
        float motionOnGainMv = 12.0f;
        float motionOffGainMv = 12.0f;
        std::vector<std::string> motionOnTypes{"Mi1", "Mi4", "Mi9"};
        std::vector<std::string> motionOffTypes{"Tm1", "Tm2", "Tm4", "Tm9"};
        int boxPixels = 3;                      // the box over which a pixel is averaged
    };

    struct Photoreceptor
    {
        uint32_t neuron = 0;
        uint8_t eye = 0;                        // 0 left, 1 right
        int16_t hex1 = 0;                       // its column
        int16_t hex2 = 0;
        float x = 0.0f;                         // where it looks in the picture, 0..1 of the width, 0..1 of the 3D view height (0 up)
        float y = 0.0f;
    };

    // A column of the optic lobe (a place of the visual field of one eye): the columnar neurons (Mi1, Tm3, ... L1) sit in it
    struct Column
    {
        int16_t hex1 = 0;
        int16_t hex2 = 0;
        uint8_t eye = 0;
        float x = 0.0f;                         // where it looks in the picture, like Photoreceptor
        float y = 0.0f;
    };

    struct EyesInfo
    {
        uint32_t receptors[2] = {};             // R1-R6 per eye that look somewhere
        uint32_t withoutColumn[2] = {};         // R1-R6 whose column could not be found
        uint32_t columns[2] = {};               // the columns of the eye (the distinct (hex1, hex2) of L1)
        uint32_t columnsCovered[2] = {};        // the columns that have at least one receptor
    };

    class FlyEyes
    {
    public:
        // false and `error` when the connectome has no R1-R6 or no lamina columns
        bool Build(const flybrain::Connectome& connectome, const EyesConfig& config, std::string& error);

        const std::vector<Photoreceptor>& Receptors() const { return receptors_; }
        const EyesInfo& Info() const { return info_; }
        const EyesConfig& Config() const { return config_; }
        const flybrain::Group& Lamina() const { return lamina_; }

        // The neurons of the motion channel with their columns
        struct MotionNeuron
        {
            uint32_t neuron = 0;
            int column = 0;
            bool on = true;
        };
        const std::vector<MotionNeuron>& MotionNeurons() const { return motion_; }
        // The current (mV) of a motion neuron for the change of the luminance of its column
        float MotionDriveMv(float luminance, float slowLuminance, bool on) const;

        // The columns of both eyes (the distinct columns of L1) and the column of a columnar neuron (-1: none)
        const std::vector<Column>& Columns() const { return columns_; }
        int ColumnOf(const flybrain::Connectome& connectome, uint32_t neuron) const;
        // The luminance (0..1, linear) at each column of the picture
        void SampleColumns(const uint32_t* frame, int width, int height, std::vector<float>& luminance) const;

        // The luminance (0..1, linear) each receptor sees in a frame of 0x00RRGGBB pixels
        void SampleLuminance(const uint32_t* frame, int width, int height, std::vector<float>& luminance) const;

        // The current (mV) of a receptor for a luminance (the saturating response of DOOMFLY)
        float ResponseMv(float luminance) const;

    private:
        EyesConfig config_;
        std::vector<Photoreceptor> receptors_;
        flybrain::Group lamina_;
        std::vector<Column> columns_;
        std::vector<MotionNeuron> motion_;
        std::vector<int> columnByKey_;          // (eye, hex1, hex2) -> index
        EyesInfo info_;
        float SampleAt(const uint32_t* frame, int width, int height, float x, float y) const;
    };
}
