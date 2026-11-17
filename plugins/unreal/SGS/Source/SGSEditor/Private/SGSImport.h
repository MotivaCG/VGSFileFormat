#pragma once

#include "CoreMinimal.h"
#include "SGSPly.h"
#include "SGSProxy.h"

/** Decoded once at import; cooked assets only keep the normal SGS runtime data. */
struct FSGSImportData
{
	sgs::Splats Splats;
	sgs::ProxyMesh Proxy;
	bool bHasProxy = false;
};

/** Uses the shipped closed decoder, including authentication; never parses the SGS container here. */
bool ReadSGSImport(const FString& Filename, FSGSImportData& Out, FString& Error);
