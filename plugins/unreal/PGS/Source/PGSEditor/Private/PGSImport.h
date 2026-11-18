#pragma once

#include "CoreMinimal.h"
#include "PGSPly.h"
#include "PGSProxy.h"

/** Decoded once at import; cooked assets only keep the normal PGS runtime data. */
struct FPGSImportData
{
	pgs::Splats Splats;
	pgs::ProxyMesh Proxy;
	bool bHasProxy = false;
};

/** Uses the shipped closed decoder, including authentication; never parses the PGS container here. */
bool ReadPGSImport(const FString& Filename, FPGSImportData& Out, FString& Error);
