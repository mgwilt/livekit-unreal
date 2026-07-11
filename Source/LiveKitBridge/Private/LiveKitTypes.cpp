#include "LiveKitTypes.h"

FString FLiveKitDataMessage::AsString() const
{
    if (Data.IsEmpty())
    {
        return FString();
    }

    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data.GetData()), Data.Num());
    return FString(Converted.Length(), Converted.Get());
}
