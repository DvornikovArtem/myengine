// AudioSpatial.h
//
// The 3D of a sound without a device: distance attenuation, panning by the side, Doppler. A pure function, so the tests check it.

#pragma once

#include <myengine/audio/AudioTypes.h>

namespace myengine::audio
{
    // gain: 1 up to minDistance, then (minDistance / distance) ^ rolloff, and a smooth fade to 0 over the last fifth of the way to
    // maxDistance (nothing is heard from beyond it). pan: the direction to the source against the right vector of the listener
    // (forward x up), -1 .. 1. doppler: the pitch factor (c - listener speed towards the source) / (c - source speed towards the
    // listener), within 0.5 .. 2, 1 when `dopplerFactor` is 0.
    SpatialResult ComputeSpatial(const ListenerState& listener, const PlayParams& source, float speedOfSound, float globalDoppler);

    // The gains of the two ears for a pan (equal power): left = cos, right = sin
    void PanGains(float pan, float& left, float& right);
}
