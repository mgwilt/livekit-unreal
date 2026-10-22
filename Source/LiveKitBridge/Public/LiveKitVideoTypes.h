#pragma once
#include "CoreMinimal.h"

/** Packed BGRA pixels. One frame replaces the previous frame. */
struct LIVEKITBRIDGE_API FLiveKitVideoFrame
{
    TArray<uint8> Pixels;
    int32 Width = 0;
    int32 Height = 0;
    int64 TimestampMicroseconds = 0;
    bool IsValid() const {
        return Width > 0 && Height > 0 && Width <= 3840 && Height <= 3840 &&
            int64(Pixels.Num()) == int64(Width) * Height * 4;
    }
};
