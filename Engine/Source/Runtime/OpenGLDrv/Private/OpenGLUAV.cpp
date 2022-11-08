// Copyright Epic Games, Inc. All Rights Reserved.


#include "CoreMinimal.h"
#include "RenderUtils.h"
#include "OpenGLDrv.h"
#include "OpenGLDrvPrivate.h"
#include "ClearReplacementShaders.h"

FShaderResourceViewRHIRef FOpenGLDynamicRHI::RHICreateShaderResourceView(FRHIBuffer* BufferRHI, uint32 Stride, uint8 Format)
{
	ensureMsgf(Stride == GPixelFormats[Format].BlockBytes, TEXT("provided stride: %i was not consitent with Pixelformat: %s"), Stride, GPixelFormats[Format].Name);
	return FOpenGLDynamicRHI::RHICreateShaderResourceView(FShaderResourceViewInitializer(BufferRHI, EPixelFormat(Format)));
}

// Binds the specified buffer range to a texture resource and selects glTexBuffer or glTexBufferRange 
static void BindGLTexBufferRange(GLenum Target, GLenum InternalFormat, GLuint Buffer, uint32 StartOffsetBytes, uint32 NumElements, uint32 Stride)
{
	if (StartOffsetBytes == 0 && NumElements == UINT32_MAX)
	{
		FOpenGL::TexBuffer(Target, InternalFormat, Buffer);
	}
	else
	{
		// Validate buffer offset is a multiple of buffer offset alignment
		GLintptr Offset = StartOffsetBytes;
		GLsizeiptr Size = NumElements * Stride;

#if DO_CHECK
		GLint Alignment = FOpenGLBase::GetTextureBufferAlignment();
		check(Stride > 0 && Offset % Alignment == 0);
#endif

		FOpenGL::TexBufferRange(Target, InternalFormat, Buffer, Offset, Size);
	}
}

FShaderResourceViewRHIRef FOpenGLDynamicRHI::RHICreateShaderResourceView(const FShaderResourceViewInitializer& Initializer)
{
	FShaderResourceViewInitializer::FBufferShaderResourceViewInitializer Desc = Initializer.AsBufferSRV();
	FRHIBuffer* BufferRHI = Desc.Buffer;
	if (BufferRHI == nullptr)
	{
		return new FOpenGLShaderResourceViewProxy([=](FRHIShaderResourceView* OwnerRHI)
		{
			return new FOpenGLShaderResourceView(this, 0, GL_TEXTURE_BUFFER);
		});
	}

	EPixelFormat Format = Desc.Format;
	
	if (Initializer.GetType() == FShaderResourceViewInitializer::EType::IndexBufferSRV && Format == PF_Unknown)
	{		
		uint32 Stride = BufferRHI->GetStride();
		Format = (Stride == 2) ? PF_R16_UINT : PF_R32_UINT;
	}

	if (Format != PF_Unknown)
	{
		UE_CLOG(!GPixelFormats[Format].Supported, LogRHI, Error, TEXT("Unsupported EPixelFormat %d"), Format);
		
		return new FOpenGLShaderResourceViewProxy([=, OGLRHI = this](FRHIShaderResourceView* OwnerRHI)
		{
			VERIFY_GL_SCOPE();
			GLuint TextureID = 0;
			FOpenGL::GenTextures(1, &TextureID);
			FOpenGLBuffer* GLBuffer = FOpenGLDynamicRHI::ResourceCast(BufferRHI);
			const uint32 FormatBPP = GPixelFormats[Format].BlockBytes;
			const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];
			// Use a texture stage that's not likely to be used for draws, to avoid waiting
			OGLRHI->CachedSetupTextureStage(OGLRHI->GetContextStateForCurrentContext(), FOpenGL::GetMaxCombinedTextureImageUnits() - 1, GL_TEXTURE_BUFFER, TextureID, -1, 1);
			BindGLTexBufferRange(GL_TEXTURE_BUFFER, GLFormat.InternalFormat[0], GLBuffer->Resource, Desc.StartOffsetBytes, Desc.NumElements, FormatBPP);
			return new FOpenGLShaderResourceView(OGLRHI, TextureID, GL_TEXTURE_BUFFER, BufferRHI, Format);
		});
	}
	else
	{
		//TODO: add range views for SSBO
		ensure(Desc.IsWholeResource());

		return new FOpenGLShaderResourceViewProxy([=](FRHIShaderResourceView* OwnerRHI)
		{
			VERIFY_GL_SCOPE();
			FOpenGLBuffer* BufferGL = ResourceCast(BufferRHI);
			return new FOpenGLShaderResourceView(this, BufferGL->Resource, GL_SHADER_STORAGE_BUFFER, BufferRHI);
		});
	}
}

void FOpenGLDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* InBuffer)
{
	VERIFY_GL_SCOPE();

	FOpenGLShaderResourceView* SRVGL = FOpenGLDynamicRHI::ResourceCast(SRV);
	FOpenGLBuffer* BufferGL = FOpenGLDynamicRHI::ResourceCast(InBuffer);

	check(SRVGL);
	if (SRVGL->Target == GL_TEXTURE_BUFFER)
	{
		GLuint TextureID = SRVGL->Resource;
		CachedSetupTextureStage(GetContextStateForCurrentContext(), FOpenGL::GetMaxCombinedTextureImageUnits() - 1, GL_TEXTURE_BUFFER, TextureID, -1, 1);
		if (!BufferGL)
		{
			FOpenGL::TexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, 0); // format ignored here since we're detaching.
			SRVGL->BufferRHI = nullptr;
			SRVGL->ModificationVersion = 0;
		}
		else
		{
			uint32 Stride = InBuffer->GetStride();
			GLenum Format = (Stride == 2) ? GL_R16UI : GL_R32UI;
			uint32 NumElements = InBuffer->GetSize() / Stride;
			BindGLTexBufferRange(GL_TEXTURE_BUFFER, Format, BufferGL->Resource, 0, NumElements, Stride);
			SRVGL->BufferRHI = InBuffer;
			SRVGL->ModificationVersion = BufferGL->ModificationCount;
		}
	}
	else if (SRVGL->Target == GL_SHADER_STORAGE_BUFFER)
	{
		SRVGL->Resource = BufferGL->Resource;
		SRVGL->BufferRHI = InBuffer;
		SRVGL->ModificationVersion = BufferGL->ModificationCount;
	}
	else
	{
		checkNoEntry();
	}
}

void FOpenGLDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* InBuffer, uint32 Stride, uint8 Format)
{
	VERIFY_GL_SCOPE();

	FOpenGLShaderResourceView* SRVGL = FOpenGLDynamicRHI::ResourceCast(SRV);
	FOpenGLBuffer* BufferGL = FOpenGLDynamicRHI::ResourceCast(InBuffer);
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];
	
	check(SRVGL);
	check(SRVGL->Target == GL_TEXTURE_BUFFER); // add support for SSBO views?

	GLuint TextureID = SRVGL->Resource;
	CachedSetupTextureStage(GetContextStateForCurrentContext(), FOpenGL::GetMaxCombinedTextureImageUnits() - 1, GL_TEXTURE_BUFFER, TextureID, -1, 1);
	
	if (!BufferGL)
	{
		FOpenGL::TexBuffer(GL_TEXTURE_BUFFER, GLFormat.InternalFormat[0], 0);
		SRVGL->BufferRHI = nullptr;
		SRVGL->ModificationVersion = 0;
	}
	else
	{
		FOpenGL::TexBuffer(GL_TEXTURE_BUFFER, GLFormat.InternalFormat[0], BufferGL->Resource);
		SRVGL->BufferRHI = InBuffer;
		SRVGL->Format = Format;
		SRVGL->ModificationVersion = BufferGL->ModificationCount;
	}
}

FShaderResourceViewRHIRef FOpenGLDynamicRHI::RHICreateShaderResourceView(FRHIBuffer* BufferRHI)
{
	return FOpenGLDynamicRHI::RHICreateShaderResourceView(FShaderResourceViewInitializer(BufferRHI));
}

FOpenGLShaderResourceView::~FOpenGLShaderResourceView()
{
	if (Resource && OwnsResource)
	{
		RunOnGLRenderContextThread([OpenGLRHI= OpenGLRHI, Resource = Resource]()
		{
			VERIFY_GL_SCOPE();
			OpenGLRHI->InvalidateTextureResourceInCache(Resource);
			FOpenGL::DeleteTextures(1, &Resource);
		});
	}
}

FUnorderedAccessViewRHIRef FOpenGLDynamicRHI::RHICreateUnorderedAccessView(FRHITexture* TextureRHI, uint32 MipLevel, uint16 FirstArraySlice, uint16 NumArraySlices)
{
	check(TextureRHI->GetFlags() & TexCreate_UAV);
	// Slice selection of a texture array still need to be implemented on OpenGL
	check(FirstArraySlice == 0 && NumArraySlices == 0);
	return new FOpenGLTextureUnorderedAccessView(TextureRHI);
}

FOpenGLTextureUnorderedAccessView::FOpenGLTextureUnorderedAccessView(FRHITexture* InTextureRHI):
	TextureRHI(InTextureRHI)
{
	VERIFY_GL_SCOPE();
	
	FOpenGLTextureBase* Texture = GetOpenGLTextureFromRHITexture(TextureRHI);
	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[TextureRHI->GetFormat()];

	check(!Texture->CanBeEvicted() && !Texture->IsEvicted());
	this->Resource = Texture->GetResource();
	this->Format = GLFormat.InternalFormat[0];
	this->UnrealFormat = TextureRHI->GetFormat();
	this->bLayered = (Texture->Target == GL_TEXTURE_3D);
}

FUnorderedAccessViewRHIRef FOpenGLDynamicRHI::RHICreateUnorderedAccessView(FRHIBuffer* BufferRHI, uint8 Format)
{
	check(BufferRHI->GetUsage() & BUF_UnorderedAccess);
	return new FOpenGLTexBufferUnorderedAccessView(this, BufferRHI, Format);
}

FOpenGLTexBufferUnorderedAccessView::FOpenGLTexBufferUnorderedAccessView(FOpenGLDynamicRHI* InOpenGLRHI, FRHIBuffer* InBufferRHI, uint8 Format):
	BufferRHI(InBufferRHI),
	OpenGLRHI(InOpenGLRHI)
{
	VERIFY_GL_SCOPE();
	FOpenGLBuffer* InBuffer = FOpenGLDynamicRHI::ResourceCast(InBufferRHI);

	const FOpenGLTextureFormat& GLFormat = GOpenGLTextureFormats[Format];

	GLuint TextureID = 0;
	FOpenGL::GenTextures(1, &TextureID);

	// Use a texture stage that's not likely to be used for draws, to avoid waiting
	OpenGLRHI->CachedSetupTextureStage(OpenGLRHI->GetContextStateForCurrentContext(), FOpenGL::GetMaxCombinedTextureImageUnits() - 1, GL_TEXTURE_BUFFER, TextureID, -1, 1);
	FOpenGL::TexBuffer(GL_TEXTURE_BUFFER, GLFormat.InternalFormat[0], InBuffer->Resource);

	// No need to restore texture stage; leave it like this,
	// and the next draw will take care of cleaning it up; or
	// next operation that needs the stage will switch something else in on it.
	
	this->Resource = TextureID;
	this->BufferResource = InBuffer->Resource;
	this->Format = GLFormat.InternalFormat[0];
	this->UnrealFormat = Format;
}

uint32 FOpenGLTexBufferUnorderedAccessView::GetBufferSize()
{
	return BufferRHI->GetSize();
}

FOpenGLTexBufferUnorderedAccessView::~FOpenGLTexBufferUnorderedAccessView()
{
	if (Resource)
	{
		RunOnGLRenderContextThread([OpenGLRHI= OpenGLRHI, Resource = Resource]()
		{
			VERIFY_GL_SCOPE();
			OpenGLRHI->InvalidateTextureResourceInCache(Resource);
			FOpenGL::DeleteTextures(1, &Resource);
		});
	}
}

FUnorderedAccessViewRHIRef FOpenGLDynamicRHI::RHICreateUnorderedAccessView(FRHIBuffer* BufferRHI, bool bUseUAVCounter, bool bAppendBuffer)
{
	check(BufferRHI->GetUsage() & BUF_UnorderedAccess);
	return new FOpenGLBufferUnorderedAccessView(this, BufferRHI);
}

FOpenGLBufferUnorderedAccessView::FOpenGLBufferUnorderedAccessView(FOpenGLDynamicRHI* InOpenGLRHI, FRHIBuffer* InBufferRHI)
	: BufferRHI(InBufferRHI)
	, OpenGLRHI(InOpenGLRHI)
{
	VERIFY_GL_SCOPE();
	FOpenGLBuffer* Buffer = FOpenGLDynamicRHI::ResourceCast(BufferRHI.GetReference());
	this->BufferResource = Buffer->Resource;
}

uint32 FOpenGLBufferUnorderedAccessView::GetBufferSize()
{
	return BufferRHI->GetSize();
}

FOpenGLBufferUnorderedAccessView::~FOpenGLBufferUnorderedAccessView()
{
	// not clearing cached state as SSBO can still be bound as SRV
}

void FOpenGLDynamicRHI::RHIClearUAVFloat(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FVector4f& Values)
{
	FOpenGLUnorderedAccessView* Texture = ResourceCast(UnorderedAccessViewRHI);

	// Use compute on ES3.1
	TRHICommandList_RecursiveHazardous<FOpenGLDynamicRHI> RHICmdList(this);

	if (Texture->GetBufferSize() == 0)
	{
		FOpenGLTextureUnorderedAccessView* Texture2D = static_cast<FOpenGLTextureUnorderedAccessView*>(Texture);

		FIntVector Size = Texture2D->TextureRHI->GetSizeXYZ();

		if (Texture->IsLayered())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture3D, EClearReplacementValueType::Float, 4, false>(RHICmdList, UnorderedAccessViewRHI, Size.X, Size.Y, Size.Z, *reinterpret_cast<const float(*)[4]>(&Values));
		}
		else
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture2D, EClearReplacementValueType::Float, 4, false>(RHICmdList, UnorderedAccessViewRHI, Size.X, Size.Y, Size.Z, *reinterpret_cast<const float(*)[4]>(&Values));
		}
	}
	else
	{
		check(Texture->BufferResource);
		{
			int32 NumComponents = 0;
			uint32 NumElements = 0;

			if (Texture->UnrealFormat != 0)
			{
				NumComponents = GPixelFormats[Texture->UnrealFormat].NumComponents;
				NumElements = Texture->GetBufferSize() / GPixelFormats[Texture->UnrealFormat].BlockBytes;
			}
			else
			{
				NumElements = Texture->GetBufferSize() / sizeof(float);
				NumComponents = 1;
			}
					
			switch (NumComponents)
			{
			case 1:
				ClearUAVShader_T<EClearReplacementResourceType::Buffer, EClearReplacementValueType::Float, 1, false>(RHICmdList, UnorderedAccessViewRHI, NumElements, 1, 1, *reinterpret_cast<const float(*)[1]>(&Values));
				break;
			case 4:
				ClearUAVShader_T<EClearReplacementResourceType::Buffer, EClearReplacementValueType::Float, 4, false>(RHICmdList, UnorderedAccessViewRHI, NumElements, 1, 1, *reinterpret_cast<const float(*)[4]>(&Values));
				break;
			default:
				check(false);
			};
		}
	}
}

void FOpenGLDynamicRHI::RHIClearUAVUint(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FUintVector4& Values)
{
	FOpenGLUnorderedAccessView* Texture = ResourceCast(UnorderedAccessViewRHI);
	
	TRHICommandList_RecursiveHazardous<FOpenGLDynamicRHI> RHICmdList(this);

	if (Texture->GetBufferSize() == 0)
	{
		FOpenGLTextureUnorderedAccessView* Texture2D = static_cast<FOpenGLTextureUnorderedAccessView*>(Texture);

		FIntVector Size = Texture2D->TextureRHI->GetSizeXYZ();
		
		if (Texture->IsLayered())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture3D, EClearReplacementValueType::Uint32, 4, false>(RHICmdList, UnorderedAccessViewRHI, Size.X, Size.Y, Size.Z, *reinterpret_cast<const uint32(*)[4]>(&Values));
		}
		else
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture2D, EClearReplacementValueType::Uint32, 4, false>(RHICmdList, UnorderedAccessViewRHI, Size.X, Size.Y, Size.Z, *reinterpret_cast<const uint32(*)[4]>(&Values));
		}
	}
	else
	{
		check(Texture->BufferResource);
		{
			int32 NumComponents = 0;
			uint32 NumElements = 0;

			if (Texture->UnrealFormat != 0)
			{
				NumComponents = GPixelFormats[Texture->UnrealFormat].NumComponents;
				NumElements = Texture->GetBufferSize() / GPixelFormats[Texture->UnrealFormat].BlockBytes;
			}
			else
			{
				NumElements = Texture->GetBufferSize() / sizeof(uint32);
				NumComponents = 1;
			}

			switch (NumComponents)
			{
			case 1:
				ClearUAVShader_T<EClearReplacementResourceType::Buffer, EClearReplacementValueType::Uint32, 1, false>(RHICmdList, UnorderedAccessViewRHI, NumElements, 1, 1, *reinterpret_cast<const uint32(*)[1]>(&Values));
				break;
			case 4:
				ClearUAVShader_T<EClearReplacementResourceType::Buffer, EClearReplacementValueType::Uint32, 4, false>(RHICmdList, UnorderedAccessViewRHI, NumElements, 1, 1, *reinterpret_cast<const uint32(*)[4]>(&Values));
				break;
			default:
				check(false);
			};
		}
	}
}
