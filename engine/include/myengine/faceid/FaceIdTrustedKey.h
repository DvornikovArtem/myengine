// FaceIdTrustedKey.h
// The public key of the publisher of the shared face database (allowed_faces.fdb): ECDSA P-256, X and Y, 128 hex digits in one string. A database that is not signed
// with the private key of this public key is not accepted (docs/editor/faceid.md). An empty string: the publisher has not put the key in yet, no database is
// accepted (the publisher's own machine accepts its own key until then).
//
// The publisher makes the key in the editor (Window > Face ID > Publisher > Create the publisher key), copies the public key and puts it here:
//     tools\faceid\set_trusted_key.ps1 <128 hex digits>      (or by hand: the string below)
// then builds the engine again. The private key is never here.

#pragma once

namespace myengine::faceid
{
    inline constexpr char kTrustedPublicKeyHex[] = "";
}
