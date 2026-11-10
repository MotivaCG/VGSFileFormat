// VGS: a copy of Epic's GaussianSplat renderer (UE 5.9.0, CL 60351404), renamed by plugins/unreal/tools/copy_epic_splat_renderer.py.
// What VGS changed is marked "VGS CHANGE"; see plugins/unreal/EPIC_RENDERER_COPY.md.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#define COORDINATE_SPACE_LH_FRU 0
#define COORDINATE_SPACE_RH_RUB 1
#define COORDINATE_SPACE_RH_RDF 2

namespace FVGSSplatSHConstants
{
	static const float SH_C0 = 0.28209479177387814f;
	static const float SH_C1 = 0.4886025119029199f;

	static const float SH_C2_0 = 1.0925484305920792f;
	static const float SH_C2_1 = -1.0925484305920792f;
	static const float SH_C2_2 = 0.31539156525252005f;
	static const float SH_C2_3 = -1.0925484305920792f;
	static const float SH_C2_4 = 0.5462742152960396f;
	static const float SH_C3_0 = -0.5900435899266435f;

	static const float SH_C3_1 = 2.890611442640554f;
	static const float SH_C3_2 = -0.4570457994644658f;
	static const float SH_C3_3 = 0.3731763325901154f;
	static const float SH_C3_4 = -0.4570457994644658f;
	static const float SH_C3_5 = 1.445305721320277f;
	static const float SH_C3_6 = -0.5900435899266435f;
}

namespace FVGSSplatShared
{
	static const unsigned int EFloat_PositionOffset			= 0u;
	static const unsigned int EFloat_ScaleOffset			= 12u;
	static const unsigned int EFloat_RotationOffset			= 24u;
	static const unsigned int EFloat_ColorOpacityOffset		= 40u;
	static const unsigned int EFloat_Stride					= 56u;

	static const unsigned int EQuantized_PositionOffset		= 0u;
	static const unsigned int EQuantized_ScaleOffset		= 8u;
	static const unsigned int EQuantized_RotationOffset		= 12u;
	static const unsigned int EQuantized_ColorOpacityOffset	= 16u;
	static const unsigned int EQuantized_Stride				= 20u;

	static const float EQuantized_MinLinearScale = 1.0e-8f;

	static const float EQuantized_RotationScale11 = 2.0f / 2047.0f;
	static const float EQuantized_RotationScale10 = 2.0f / 1023.0f;

#ifdef GPU_SIMULATION
	float3 Unpack_10_10_10(uint Packed, float3 Scale, float3 Bias)
	{
		const uint Mask10 = (1 << 10) - 1;
		const float3 Unpacked = float3(float(Packed >> 20), float((Packed >> 10) & Mask10), float(Packed & Mask10));
		return (Unpacked * Scale) + Bias;
	}

	float3 Unpack_21_21_21(uint2 Packed, float3 Scale, float3 Bias)
	{
		const uint Mask21 = (1 << 21) - 1;
		const uint Z = Packed.x & Mask21;
		const uint Y = ((Packed.x >> 21) | (Packed.y << 11)) & Mask21;
		const uint X = (Packed.y >> 10) & Mask21;
		const float3 Unpacked = float3(float(X), float(Y), float(Z));
		return (Unpacked * Scale) + Bias;
	}

	float3 Unpack_11_11_10_Exp(uint Packed, float3 Scale, float3 Bias)
	{
		const uint Mask11 = (1 << 11) - 1;
		const uint Mask10 = (1 << 10) - 1;
		const float3 Unpacked = float3(float(Packed >> 21), float((Packed >> 10) & Mask11), float(Packed & Mask10));
		return exp((Unpacked * Scale) + Bias);
	}

	float4 Unpack_8_8_8_8(uint Packed, float4 Scale, float4 Bias)
	{
		const float4 Unpacked = float4(float(Packed >> 24), float((Packed >> 16) & 0xff), float((Packed >> 8) & 0xff), float(Packed & 0xff));
		return (Unpacked * Scale) + Bias;
	}

	float4 UnpackQuat(uint Packed)
	{
		const uint Mask11 = (1 << 11) - 1;
		const uint Mask10 = (1 << 10) - 1;

		float4 Unpacked;
		Unpacked.x = (float( Packed >> 21)           * EQuantized_RotationScale11) - 1.0f;
		Unpacked.y = (float((Packed >> 10) & Mask11) * EQuantized_RotationScale11) - 1.0f;
		Unpacked.z = (float((Packed >>  0) & Mask10) * EQuantized_RotationScale10) - 1.0f;
		Unpacked.w = sqrt(max(1.0f - dot(Unpacked.xyz, Unpacked.xyz), 0.0f));
		return Unpacked;
	}

	uint Quantize(float Value, float Scale, float Bias, uint MaxValue)
	{
		const float SafeScale = Scale != 0.0f ? Scale : 1.0f;
		const float Q = Scale != 0.0f ? round((Value - Bias) / SafeScale) : 0.0f;
		return uint(clamp(Q, 0.0f, float(MaxValue)));
	}

	uint2 Pack_21_21_21(float3 Value, float3 Scale, float3 Bias)
	{
		const uint Mask21 = (1 << 21) - 1;
		const uint X = Quantize(Value.x, Scale.x, Bias.x, Mask21);
		const uint Y = Quantize(Value.y, Scale.y, Bias.y, Mask21);
		const uint Z = Quantize(Value.z, Scale.z, Bias.z, Mask21);
		return uint2(Z | (Y << 21), (Y >> 11) | (X << 10));
	}

	uint Pack_11_11_10_Exp(float3 LinearValue, float3 Scale, float3 Bias)
	{
		const float3 LogValue = log(max(LinearValue, EQuantized_MinLinearScale));
		uint Packed;
		Packed  = Quantize(LogValue.x, Scale.x, Bias.x, (1 << 11) - 1) << 21;
		Packed |= Quantize(LogValue.y, Scale.y, Bias.y, (1 << 11) - 1) << 10;
		Packed |= Quantize(LogValue.z, Scale.z, Bias.z, (1 << 10) - 1);
		return Packed;
	}

	uint Pack_8_8_8_8(float4 Value, float4 Scale, float4 Bias)
	{
		uint Packed;
		Packed  = Quantize(Value.x, Scale.x, Bias.x, 0xff) << 24;
		Packed |= Quantize(Value.y, Scale.y, Bias.y, 0xff) << 16;
		Packed |= Quantize(Value.z, Scale.z, Bias.z, 0xff) << 8;
		Packed |= Quantize(Value.w, Scale.w, Bias.w, 0xff);
		return Packed;
	}

	uint PackQuat(float4 Value)
	{
		// Force non-negative w
		Value = Value.w < 0.0f ? -Value : Value;
		const float LengthSq = dot(Value, Value);
		Value = LengthSq > 0.0f ? Value * rsqrt(LengthSq) : float4(0.0f, 0.0f, 0.0f, 1.0f);

		uint Packed;
		Packed  = Quantize(Value.x, EQuantized_RotationScale11, -1.0f, (1 << 11) - 1) << 21;
		Packed |= Quantize(Value.y, EQuantized_RotationScale11, -1.0f, (1 << 11) - 1) << 10;
		Packed |= Quantize(Value.z, EQuantized_RotationScale10, -1.0f, (1 << 10) - 1);
		return Packed;
	}
#else
	inline FVector3f Unpack_10_10_10(uint32 Packed, const FVector3f& Scale, const FVector3f& Bias)
	{
		const uint32 Mask10 = (1 << 10) - 1;
		const FVector3f Unpacked = FVector3f(float(Packed >> 20), float((Packed >> 10) & Mask10), float(Packed & Mask10));
		return (Unpacked * Scale) + Bias;
	}

	inline FVector3f Unpack_21_21_21(uint32 Packed0, uint32 Packed1, const FVector3f& Scale, const FVector3f& Bias)
	{
		constexpr uint32 Mask21 = (1 << 21) - 1;
		const uint32 QZ = Packed0 & Mask21;
		const uint32 QY = ((Packed0 >> 21) | (Packed1 << 11)) & Mask21;
		const uint32 QX = (Packed1 >> 10) & Mask21;
		const FVector3f Unpacked = FVector3f(float(QX), float(QY), float(QZ));
		return (Unpacked * Scale) + Bias;
	}

	inline FVector3f Unpack_11_11_10_Exp(uint32 Packed, const FVector3f& Scale, const FVector3f& Bias)
	{
		constexpr uint32 Mask11 = (1 << 11) - 1;
		constexpr uint32 Mask10 = (1 << 10) - 1;
		const FVector3f Unpacked = FVector3f(float(Packed >> 21), float((Packed >> 10) & Mask11), float(Packed & Mask10));
		const FVector3f LogScale = (Unpacked * Scale) + Bias;
		return FVector3f(FMath::Exp(LogScale.X), FMath::Exp(LogScale.Y), FMath::Exp(LogScale.Z));
	}

	inline FVector4f Unpack_8_8_8_8(uint32 Packed, const FVector4f& Scale, const FVector4f& Bias)
	{
		const FVector4f Unpacked = FVector4f(float(Packed >> 24), float((Packed >> 16) & 0xff), float((Packed >> 8) & 0xff), float(Packed & 0xff));
		return (Unpacked * Scale) + Bias;
	}

	inline FQuat4f UnpackQuat(uint32 Packed)
	{
		constexpr uint32 Mask11 = (1 << 11) - 1;
		constexpr uint32 Mask10 = (1 << 10) - 1;

		FQuat4f Unpacked;
		Unpacked.X = (float( Packed >> 21)		    * EQuantized_RotationScale11) - 1.0f;
		Unpacked.Y = (float((Packed >> 10) & Mask11) * EQuantized_RotationScale11) - 1.0f;
		Unpacked.Z = (float((Packed >>  0) & Mask10) * EQuantized_RotationScale10) - 1.0f;
		Unpacked.W = FMath::Sqrt(FMath::Max(1.0f - (Unpacked.X * Unpacked.X) - (Unpacked.Y * Unpacked.Y) - (Unpacked.Z * Unpacked.Z), 0.0f));
		return Unpacked;
	}

	inline uint32 Quantize(float Value, float Scale, float Bias, uint32 MaxValue)
	{
		const float Q = Scale != 0.0f ? FMath::RoundToFloat((Value - Bias) / Scale) : 0.0f;
		return uint32(FMath::Clamp(Q, 0.0f, float(MaxValue)));
	}

	inline void Pack_21_21_21(const FVector3f& Value, const FVector3f& Scale, const FVector3f& Bias, uint32& OutPacked0, uint32& OutPacked1)
	{
		constexpr uint32 Mask21 = (1 << 21) - 1;
		const uint32 QX = Quantize(Value.X, Scale.X, Bias.X, Mask21);
		const uint32 QY = Quantize(Value.Y, Scale.Y, Bias.Y, Mask21);
		const uint32 QZ = Quantize(Value.Z, Scale.Z, Bias.Z, Mask21);
		OutPacked0 = QZ | (QY << 21);
		OutPacked1 = (QY >> 11) | (QX << 10);
	}

	inline uint32 Pack_11_11_10_Exp(const FVector3f& LinearValue, const FVector3f& Scale, const FVector3f& Bias)
	{
		const FVector3f LogValue(
			FMath::Loge(FMath::Max(LinearValue.X, EQuantized_MinLinearScale)),
			FMath::Loge(FMath::Max(LinearValue.Y, EQuantized_MinLinearScale)),
			FMath::Loge(FMath::Max(LinearValue.Z, EQuantized_MinLinearScale)));
		uint32 Packed;
		Packed  = Quantize(LogValue.X, Scale.X, Bias.X, (1 << 11) - 1) << 21;
		Packed |= Quantize(LogValue.Y, Scale.Y, Bias.Y, (1 << 11) - 1) << 10;
		Packed |= Quantize(LogValue.Z, Scale.Z, Bias.Z, (1 << 10) - 1);
		return Packed;
	}

	inline uint32 Pack_8_8_8_8(const FVector4f& Value, const FVector4f& Scale, const FVector4f& Bias)
	{
		uint32 Packed;
		Packed  = Quantize(Value.X, Scale.X, Bias.X, 0xff) << 24;
		Packed |= Quantize(Value.Y, Scale.Y, Bias.Y, 0xff) << 16;
		Packed |= Quantize(Value.Z, Scale.Z, Bias.Z, 0xff) << 8;
		Packed |= Quantize(Value.W, Scale.W, Bias.W, 0xff);
		return Packed;
	}

	inline uint32 PackQuat(const FQuat4f& InValue)
	{
		// Force non-negative w
		FQuat4f Value = InValue.W < 0.0f ? FQuat4f(-InValue.X, -InValue.Y, -InValue.Z, -InValue.W) : InValue;
		const float LengthSq = Value.SizeSquared();
		Value = LengthSq > 0.0f ? Value * FMath::InvSqrt(LengthSq) : FQuat4f::Identity;

		uint32 Packed;
		Packed  = Quantize(Value.X, EQuantized_RotationScale11, -1.0f, (1 << 11) - 1) << 21;
		Packed |= Quantize(Value.Y, EQuantized_RotationScale11, -1.0f, (1 << 11) - 1) << 10;
		Packed |= Quantize(Value.Z, EQuantized_RotationScale10, -1.0f, (1 << 10) - 1);
		return Packed;
	}
#endif
}
