#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LiveKitTypes.h"
#include "LiveKitBlueprintLibrary.generated.h"

/** Stateless Blueprint helpers for LiveKit value types. */
UCLASS()
class LIVEKITBRIDGE_API ULiveKitBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Decode a general data packet as UTF-8 text. */
    UFUNCTION(
        BlueprintPure,
        Category="LiveKit|Data",
        meta=(DisplayName="LiveKit Data Message As Text"))
    static FString DataMessageAsText(const FLiveKitDataMessage& Message);
};
