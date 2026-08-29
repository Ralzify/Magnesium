#pragma once
#include "../../pch.h"

class UStreamableRenderAsset : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UStreamableRenderAsset);
};

class UTexture : public UStreamableRenderAsset
{
public:
    UCLASS_COMMON_MEMBERS(UTexture);
};

class UTexture2D : public UTexture
{
public:
    UCLASS_COMMON_MEMBERS(UTexture2D);
};
