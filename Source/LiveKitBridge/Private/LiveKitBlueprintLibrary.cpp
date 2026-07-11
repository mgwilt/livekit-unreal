#include "LiveKitBlueprintLibrary.h"

FString ULiveKitBlueprintLibrary::DataMessageAsText(const FLiveKitDataMessage& Message)
{
    return Message.AsString();
}
