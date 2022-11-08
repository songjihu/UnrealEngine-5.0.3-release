// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureDerivedDataBuildUtils.h"

#if WITH_EDITOR
#include "DerivedDataBuild.h"
#include "DerivedDataBuildFunctionRegistry.h"
#include "DerivedDataSharedString.h"
#include "Engine/Texture.h"
#include "HAL/CriticalSection.h"
#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatManagerModule.h"
#include "Interfaces/ITextureFormatModule.h"
#include "Misc/ScopeRWLock.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"
#include "String/Find.h"
#include "TextureCompressorModule.h"
#include "TextureFormatManager.h"
#include "TextureResource.h"

const FGuid& GetTextureDerivedDataVersion();
void GetTextureDerivedMipKey(int32 MipIndex, const FTexture2DMipMap& Mip, const FString& KeySuffix, FString& OutKey);

template <typename ValueType>
static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const ValueType& Value)
{
	Writer << Name << Value;
}

static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const FName & Value)
{
	Writer << Name << WriteToString<128>(Value);
}

static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const FColor& Value)
{
	Writer.BeginArray(Name);
	Writer.AddInteger(Value.A);
	Writer.AddInteger(Value.R);
	Writer.AddInteger(Value.G);
	Writer.AddInteger(Value.B);
	Writer.EndArray();
}

static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const FVector2f& Value)
{
	Writer.BeginArray(Name);
	Writer.AddFloat(Value.X);
	Writer.AddFloat(Value.Y);
	Writer.EndArray();
}

static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const FVector4f& Value)
{
	Writer.BeginArray(Name);
	Writer.AddFloat(Value.X);
	Writer.AddFloat(Value.Y);
	Writer.AddFloat(Value.Z);
	Writer.AddFloat(Value.W);
	Writer.EndArray();
}

static void WriteCbField(FCbWriter& Writer, FAnsiStringView Name, const FIntPoint& Value)
{
	Writer.BeginArray(Name);
	Writer.AddInteger(Value.X);
	Writer.AddInteger(Value.Y);
	Writer.EndArray();
}

template <typename ValueType>
static void WriteCbFieldWithDefault(FCbWriter& Writer, FAnsiStringView Name, ValueType Value, ValueType Default)
{
	if (Value != Default)
	{
		WriteCbField(Writer, Name, Forward<ValueType>(Value));
	}
}

static void WriteBuildSettings(FCbWriter& Writer, const FTextureBuildSettings& BuildSettings, const ITextureFormat* TextureFormat)
{
	FTextureBuildSettings DefaultSettings;

	Writer.BeginObject();

	if (BuildSettings.FormatConfigOverride)
	{
		Writer.AddObject("FormatConfigOverride", BuildSettings.FormatConfigOverride);
	}
	else if (FCbObject TextureFormatConfig = TextureFormat->ExportGlobalFormatConfig(BuildSettings))
	{
		Writer.AddObject("FormatConfigOverride", TextureFormatConfig);
	}

	if (BuildSettings.ColorAdjustment.AdjustBrightness != DefaultSettings.ColorAdjustment.AdjustBrightness ||
		BuildSettings.ColorAdjustment.AdjustBrightnessCurve != DefaultSettings.ColorAdjustment.AdjustBrightnessCurve ||
		BuildSettings.ColorAdjustment.AdjustSaturation != DefaultSettings.ColorAdjustment.AdjustSaturation ||
		BuildSettings.ColorAdjustment.AdjustVibrance != DefaultSettings.ColorAdjustment.AdjustVibrance ||
		BuildSettings.ColorAdjustment.AdjustRGBCurve != DefaultSettings.ColorAdjustment.AdjustRGBCurve ||
		BuildSettings.ColorAdjustment.AdjustHue != DefaultSettings.ColorAdjustment.AdjustHue ||
		BuildSettings.ColorAdjustment.AdjustMinAlpha != DefaultSettings.ColorAdjustment.AdjustMinAlpha ||
		BuildSettings.ColorAdjustment.AdjustMaxAlpha != DefaultSettings.ColorAdjustment.AdjustMaxAlpha)
	{
		Writer.BeginObject("ColorAdjustment");
		WriteCbFieldWithDefault(Writer, "AdjustBrightness", BuildSettings.ColorAdjustment.AdjustBrightness, DefaultSettings.ColorAdjustment.AdjustBrightness);
		WriteCbFieldWithDefault(Writer, "AdjustBrightnessCurve", BuildSettings.ColorAdjustment.AdjustBrightnessCurve, DefaultSettings.ColorAdjustment.AdjustBrightnessCurve);
		WriteCbFieldWithDefault(Writer, "AdjustSaturation", BuildSettings.ColorAdjustment.AdjustSaturation, DefaultSettings.ColorAdjustment.AdjustSaturation);
		WriteCbFieldWithDefault(Writer, "AdjustVibrance", BuildSettings.ColorAdjustment.AdjustVibrance, DefaultSettings.ColorAdjustment.AdjustVibrance);
		WriteCbFieldWithDefault(Writer, "AdjustRGBCurve", BuildSettings.ColorAdjustment.AdjustRGBCurve, DefaultSettings.ColorAdjustment.AdjustRGBCurve);
		WriteCbFieldWithDefault(Writer, "AdjustHue", BuildSettings.ColorAdjustment.AdjustHue, DefaultSettings.ColorAdjustment.AdjustHue);
		WriteCbFieldWithDefault(Writer, "AdjustMinAlpha", BuildSettings.ColorAdjustment.AdjustMinAlpha, DefaultSettings.ColorAdjustment.AdjustMinAlpha);
		WriteCbFieldWithDefault(Writer, "AdjustMaxAlpha", BuildSettings.ColorAdjustment.AdjustMaxAlpha, DefaultSettings.ColorAdjustment.AdjustMaxAlpha);
		Writer.EndObject();
	}
	
	WriteCbFieldWithDefault<bool>(Writer, "bDoScaleMipsForAlphaCoverage", BuildSettings.bDoScaleMipsForAlphaCoverage, DefaultSettings.bDoScaleMipsForAlphaCoverage);
	if ( BuildSettings.bDoScaleMipsForAlphaCoverage )
	{
		// AlphaCoverageThresholds do not affect build if bDoScaleMipsForAlphaCoverage is off
		WriteCbFieldWithDefault(Writer, "AlphaCoverageThresholds", BuildSettings.AlphaCoverageThresholds, DefaultSettings.AlphaCoverageThresholds);
	}
	WriteCbFieldWithDefault(Writer, "MipSharpening", BuildSettings.MipSharpening, DefaultSettings.MipSharpening);
	WriteCbFieldWithDefault(Writer, "DiffuseConvolveMipLevel", BuildSettings.DiffuseConvolveMipLevel, DefaultSettings.DiffuseConvolveMipLevel);
	WriteCbFieldWithDefault(Writer, "SharpenMipKernelSize", BuildSettings.SharpenMipKernelSize, DefaultSettings.SharpenMipKernelSize);
	WriteCbFieldWithDefault(Writer, "MaxTextureResolution", BuildSettings.MaxTextureResolution, DefaultSettings.MaxTextureResolution);
	WriteCbFieldWithDefault(Writer, "TextureFormatName", WriteToString<64>(BuildSettings.TextureFormatName).ToView(), TEXT(""_SV));
	WriteCbFieldWithDefault(Writer, "bHDRSource", BuildSettings.bHDRSource, DefaultSettings.bHDRSource);
	WriteCbFieldWithDefault(Writer, "MipGenSettings", BuildSettings.MipGenSettings, DefaultSettings.MipGenSettings);
	WriteCbFieldWithDefault<bool>(Writer, "bCubemap", BuildSettings.bCubemap, DefaultSettings.bCubemap);
	WriteCbFieldWithDefault<bool>(Writer, "bTextureArray", BuildSettings.bTextureArray, DefaultSettings.bTextureArray);
	WriteCbFieldWithDefault<bool>(Writer, "bVolume", BuildSettings.bVolume, DefaultSettings.bVolume);
	WriteCbFieldWithDefault<bool>(Writer, "bLongLatSource", BuildSettings.bLongLatSource, DefaultSettings.bLongLatSource);
	WriteCbFieldWithDefault<bool>(Writer, "bSRGB", BuildSettings.bSRGB, DefaultSettings.bSRGB);
	WriteCbFieldWithDefault(Writer, "SourceEncodingOverride", BuildSettings.SourceEncodingOverride, DefaultSettings.SourceEncodingOverride);
	WriteCbFieldWithDefault<bool>(Writer, "bHasColorSpaceDefinition", BuildSettings.bHasColorSpaceDefinition, DefaultSettings.bHasColorSpaceDefinition);
	WriteCbFieldWithDefault(Writer, "RedChromaticityCoordinate", BuildSettings.RedChromaticityCoordinate, DefaultSettings.RedChromaticityCoordinate);
	WriteCbFieldWithDefault(Writer, "GreenChromaticityCoordinate", BuildSettings.GreenChromaticityCoordinate, DefaultSettings.GreenChromaticityCoordinate);
	WriteCbFieldWithDefault(Writer, "BlueChromaticityCoordinate", BuildSettings.BlueChromaticityCoordinate, DefaultSettings.BlueChromaticityCoordinate);
	WriteCbFieldWithDefault(Writer, "WhiteChromaticityCoordinate", BuildSettings.WhiteChromaticityCoordinate, DefaultSettings.WhiteChromaticityCoordinate);
	WriteCbFieldWithDefault(Writer, "ChromaticAdaptationMethod", BuildSettings.ChromaticAdaptationMethod, DefaultSettings.ChromaticAdaptationMethod);
	WriteCbFieldWithDefault<bool>(Writer, "bUseLegacyGamma", BuildSettings.bUseLegacyGamma, DefaultSettings.bUseLegacyGamma);
	WriteCbFieldWithDefault<bool>(Writer, "bPreserveBorder", BuildSettings.bPreserveBorder, DefaultSettings.bPreserveBorder);
	WriteCbFieldWithDefault<bool>(Writer, "bForceNoAlphaChannel", BuildSettings.bForceNoAlphaChannel, DefaultSettings.bForceNoAlphaChannel);
	WriteCbFieldWithDefault<bool>(Writer, "bForceAlphaChannel", BuildSettings.bForceAlphaChannel, DefaultSettings.bForceAlphaChannel);
	WriteCbFieldWithDefault<bool>(Writer, "bDitherMipMapAlpha", BuildSettings.bDitherMipMapAlpha, DefaultSettings.bDitherMipMapAlpha);
	WriteCbFieldWithDefault<bool>(Writer, "bComputeBokehAlpha", BuildSettings.bComputeBokehAlpha, DefaultSettings.bComputeBokehAlpha);
	WriteCbFieldWithDefault<bool>(Writer, "bReplicateRed", BuildSettings.bReplicateRed, DefaultSettings.bReplicateRed);
	WriteCbFieldWithDefault<bool>(Writer, "bReplicateAlpha", BuildSettings.bReplicateAlpha, DefaultSettings.bReplicateAlpha);
	WriteCbFieldWithDefault<bool>(Writer, "bDownsampleWithAverage", BuildSettings.bDownsampleWithAverage, DefaultSettings.bDownsampleWithAverage);
	WriteCbFieldWithDefault<bool>(Writer, "bSharpenWithoutColorShift", BuildSettings.bSharpenWithoutColorShift, DefaultSettings.bSharpenWithoutColorShift);
	WriteCbFieldWithDefault<bool>(Writer, "bBorderColorBlack", BuildSettings.bBorderColorBlack, DefaultSettings.bBorderColorBlack);
	WriteCbFieldWithDefault<bool>(Writer, "bFlipGreenChannel", BuildSettings.bFlipGreenChannel, DefaultSettings.bFlipGreenChannel);
	WriteCbFieldWithDefault<bool>(Writer, "bApplyYCoCgBlockScale", BuildSettings.bApplyYCoCgBlockScale, DefaultSettings.bApplyYCoCgBlockScale);
	WriteCbFieldWithDefault<bool>(Writer, "bApplyKernelToTopMip", BuildSettings.bApplyKernelToTopMip, DefaultSettings.bApplyKernelToTopMip);
	WriteCbFieldWithDefault<bool>(Writer, "bRenormalizeTopMip", BuildSettings.bRenormalizeTopMip, DefaultSettings.bRenormalizeTopMip);
	WriteCbFieldWithDefault(Writer, "CompositeTextureMode", BuildSettings.CompositeTextureMode, DefaultSettings.CompositeTextureMode);
	WriteCbFieldWithDefault(Writer, "CompositePower", BuildSettings.CompositePower, DefaultSettings.CompositePower);
	WriteCbFieldWithDefault(Writer, "LODBias", BuildSettings.LODBias, DefaultSettings.LODBias);
	WriteCbFieldWithDefault(Writer, "LODBiasWithCinematicMips", BuildSettings.LODBiasWithCinematicMips, DefaultSettings.LODBiasWithCinematicMips);
	WriteCbFieldWithDefault(Writer, "TopMipSize", BuildSettings.TopMipSize, DefaultSettings.TopMipSize);
	WriteCbFieldWithDefault(Writer, "VolumeSizeZ", BuildSettings.VolumeSizeZ, DefaultSettings.VolumeSizeZ);
	WriteCbFieldWithDefault(Writer, "ArraySlices", BuildSettings.ArraySlices, DefaultSettings.ArraySlices);
	WriteCbFieldWithDefault<bool>(Writer, "bStreamable", BuildSettings.bStreamable, DefaultSettings.bStreamable);
	WriteCbFieldWithDefault<bool>(Writer, "bVirtualStreamable", BuildSettings.bVirtualStreamable, DefaultSettings.bVirtualStreamable);
	WriteCbFieldWithDefault<bool>(Writer, "bChromaKeyTexture", BuildSettings.bChromaKeyTexture, DefaultSettings.bChromaKeyTexture);
	WriteCbFieldWithDefault(Writer, "PowerOfTwoMode", BuildSettings.PowerOfTwoMode, DefaultSettings.PowerOfTwoMode);
	WriteCbFieldWithDefault(Writer, "PaddingColor", BuildSettings.PaddingColor, DefaultSettings.PaddingColor);
	WriteCbFieldWithDefault(Writer, "ChromaKeyColor", BuildSettings.ChromaKeyColor, DefaultSettings.ChromaKeyColor);
	WriteCbFieldWithDefault(Writer, "ChromaKeyThreshold", BuildSettings.ChromaKeyThreshold, DefaultSettings.ChromaKeyThreshold);
	WriteCbFieldWithDefault(Writer, "CompressionQuality", BuildSettings.CompressionQuality, DefaultSettings.CompressionQuality);
	WriteCbFieldWithDefault(Writer, "LossyCompressionAmount", BuildSettings.LossyCompressionAmount, DefaultSettings.LossyCompressionAmount);
	WriteCbFieldWithDefault(Writer, "Downscale", BuildSettings.Downscale, DefaultSettings.Downscale);
	WriteCbFieldWithDefault(Writer, "DownscaleOptions", BuildSettings.DownscaleOptions, DefaultSettings.DownscaleOptions);
	WriteCbFieldWithDefault(Writer, "VirtualAddressingModeX", BuildSettings.VirtualAddressingModeX, DefaultSettings.VirtualAddressingModeX);
	WriteCbFieldWithDefault(Writer, "VirtualAddressingModeY", BuildSettings.VirtualAddressingModeY, DefaultSettings.VirtualAddressingModeY);
	WriteCbFieldWithDefault(Writer, "VirtualTextureTileSize", BuildSettings.VirtualTextureTileSize, DefaultSettings.VirtualTextureTileSize);
	WriteCbFieldWithDefault(Writer, "VirtualTextureBorderSize", BuildSettings.VirtualTextureBorderSize, DefaultSettings.VirtualTextureBorderSize);
	WriteCbFieldWithDefault<bool>(Writer, "bVirtualTextureEnableCompressZlib", BuildSettings.bVirtualTextureEnableCompressZlib, DefaultSettings.bVirtualTextureEnableCompressZlib);
	WriteCbFieldWithDefault<bool>(Writer, "bVirtualTextureEnableCompressCrunch", BuildSettings.bVirtualTextureEnableCompressCrunch, DefaultSettings.bVirtualTextureEnableCompressCrunch);

	WriteCbFieldWithDefault<uint8>(Writer, "OodleEncodeEffort", (uint8)BuildSettings.OodleEncodeEffort, (uint8)DefaultSettings.OodleEncodeEffort);	
	WriteCbFieldWithDefault<uint8>(Writer, "OodleUniversalTiling", (uint8)BuildSettings.OodleUniversalTiling, (uint8)DefaultSettings.OodleUniversalTiling);
	WriteCbFieldWithDefault<uint8>(Writer, "OodleRDO", BuildSettings.OodleRDO, DefaultSettings.OodleRDO);
	WriteCbFieldWithDefault<bool>(Writer, "bOodleUsesRDO", BuildSettings.bOodleUsesRDO, DefaultSettings.bOodleUsesRDO);
	
	WriteCbFieldWithDefault(Writer, "OodleTextureSdkVersion", BuildSettings.OodleTextureSdkVersion, DefaultSettings.OodleTextureSdkVersion);
	
	Writer.EndObject();
}

static void WriteOutputSettings(FCbWriter& Writer, int32 NumInlineMips)
{
	Writer.BeginObject();

	Writer.AddInteger("NumInlineMips", NumInlineMips);

	Writer.EndObject();
}

static void WriteSource(FCbWriter& Writer, const UTexture& Texture, int32 LayerIndex, const FTextureBuildSettings& BuildSettings)
{
	const FTextureSource& Source = Texture.Source;

	FTextureFormatSettings TextureFormatSettings;
	Texture.GetLayerFormatSettings(LayerIndex, TextureFormatSettings);
	EGammaSpace GammaSpace = TextureFormatSettings.SRGB ? (Texture.bUseLegacyGamma ? EGammaSpace::Pow22 : EGammaSpace::sRGB) : EGammaSpace::Linear;

	Writer.BeginObject();

	ETextureSourceCompressionFormat CompressionFormat = Source.GetSourceCompression();
	if ((CompressionFormat == ETextureSourceCompressionFormat::TSCF_PNG) && !Source.IsPNGCompressed())
	{
		// CompressionFormat might mismatch with IsPNGCompressed.  In that case IsPNGCompressed is authoritative.
		// This behavior matches FTextureSource::Decompress
		CompressionFormat = ETextureSourceCompressionFormat::TSCF_None;
	}
	Writer.AddInteger("CompressionFormat", CompressionFormat);
	Writer.AddInteger("SourceFormat", Source.GetFormat(LayerIndex));
	Writer.AddInteger("GammaSpace", static_cast<uint8>(GammaSpace));
	Writer.AddInteger("NumSlices", (BuildSettings.bCubemap || BuildSettings.bTextureArray || BuildSettings.bVolume) ? Source.GetNumSlices() : 1);
	Writer.AddInteger("SizeX", Source.GetSizeX());
	Writer.AddInteger("SizeY", Source.GetSizeY());
	Writer.BeginArray("Mips");
	int32 NumMips = BuildSettings.MipGenSettings == TMGS_LeaveExistingMips ? Source.GetNumMips() : 1;
	int64 Offset = 0;
	for (int32 MipIndex = 0, MipCount = NumMips; MipIndex < MipCount; ++MipIndex)
	{
		Writer.BeginObject();
		Writer.AddInteger("Offset", Offset);
		const int64 MipSize = Source.CalcMipSize(MipIndex);
		Writer.AddInteger("Size", MipSize);
		Offset += MipSize;
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.EndObject();
}

static FRWLock GTextureBuildFunctionLock;
static TMap<FName, UE::DerivedData::FUtf8SharedString> GTextureBuildFunctionMap;

UE::DerivedData::FUtf8SharedString FindTextureBuildFunction(const FName TextureFormatName)
{
	using namespace UE::DerivedData;

	if (FReadScopeLock Lock(GTextureBuildFunctionLock); const FUtf8SharedString* Function = GTextureBuildFunctionMap.Find(TextureFormatName))
	{
		return *Function;
	}

	FName TextureFormatModuleName;

	if (ITextureFormatManagerModule* TFM = GetTextureFormatManager())
	{
		ITextureFormatModule* TextureFormatModule = nullptr;
		if (!TFM->FindTextureFormatAndModule(TextureFormatName, TextureFormatModuleName, TextureFormatModule))
		{
			return {};
		}
	}

	TStringBuilder<128> FunctionName;

	// Texture format modules are inconsistent in their naming, e.g., TextureFormatUncompressed, <Platform>TextureFormat.
	// Attempt to unify the naming of build functions as <Format>Texture.
	FunctionName << TextureFormatModuleName << TEXTVIEW("Texture");
	if (int32 Index = UE::String::FindFirst(FunctionName, TEXTVIEW("TextureFormat")); Index != INDEX_NONE)
	{
		FunctionName.RemoveAt(Index, TEXTVIEW("TextureFormat").Len());
	}

	FTCHARToUTF8 FunctionNameUtf8(FunctionName);

	if (!GetBuild().GetFunctionRegistry().FindFunctionVersion(FunctionNameUtf8).IsValid())
	{
		return {};
	}

	FWriteScopeLock Lock(GTextureBuildFunctionLock);
	FUtf8SharedString& Function = GTextureBuildFunctionMap.FindOrAdd(TextureFormatName);
	if (Function.IsEmpty())
	{
		Function = FunctionNameUtf8;
	}
	return Function;
}

FCbObject SaveTextureBuildSettings(const UTexture& Texture, const FTextureBuildSettings& BuildSettings, int32 LayerIndex, int32 NumInlineMips, bool bUseCompositeTexture)
{
	const ITextureFormat* TextureFormat = nullptr;
	if (ITextureFormatManagerModule* TFM = GetTextureFormatManager())
	{
		FName TextureFormatModuleName;
		ITextureFormatModule* TextureFormatModule = nullptr;
		TextureFormat = TFM->FindTextureFormatAndModule(BuildSettings.TextureFormatName, TextureFormatModuleName, TextureFormatModule);
	}
	if (TextureFormat == nullptr)
	{
		return FCbObject();
	}

	FCbWriter Writer;
	Writer.BeginObject();

	Writer.AddUuid("BuildVersion", GetTextureDerivedDataVersion());

	if (uint16 TextureFormatVersion = TextureFormat->GetVersion(BuildSettings.TextureFormatName, &BuildSettings))
	{
		Writer.AddInteger("FormatVersion", TextureFormatVersion);
	}

	Writer.SetName("Build");
	WriteBuildSettings(Writer, BuildSettings, TextureFormat);

	Writer.SetName("Output");
	WriteOutputSettings(Writer, NumInlineMips);

	Writer.SetName("Source");
	WriteSource(Writer, Texture, LayerIndex, BuildSettings);

	if (bUseCompositeTexture && Texture.CompositeTexture)
	{
		Writer.SetName("CompositeSource");
		WriteSource(Writer, *Texture.CompositeTexture, LayerIndex, BuildSettings);
	}

	Writer.EndObject();
	return Writer.Save().AsObject();
}

#endif // WITH_EDITOR
