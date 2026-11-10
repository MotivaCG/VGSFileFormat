// The normals estimates (VGSStaticNormals.h): the VGS editor's (editor/gaussiannormals.cpp and
// normalrigs.cpp, copied into Normals/ when the plugin is assembled) and the shortest-axis one,
// built here as third-party code, with the engine's Eigen. They are .inl files so that
// UnrealBuildTool does not build them on their own.
#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS
#include "Normals/gaussiannormals.inl"
#include "Normals/normalrigs.inl"
#include "VGSStaticNormals.inl"
PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
THIRD_PARTY_INCLUDES_END
