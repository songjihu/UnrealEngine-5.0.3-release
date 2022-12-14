// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
D3D12View.h: D3D12 Resource Views
=============================================================================*/
#pragma once

enum ViewSubresourceSubsetFlags
{
	ViewSubresourceSubsetFlags_None = 0x0,
	ViewSubresourceSubsetFlags_DepthOnlyDsv = 0x1,
	ViewSubresourceSubsetFlags_StencilOnlyDsv = 0x2,
	ViewSubresourceSubsetFlags_DepthAndStencilDsv = (ViewSubresourceSubsetFlags_DepthOnlyDsv | ViewSubresourceSubsetFlags_StencilOnlyDsv),
};

/** Class to track subresources in a view */
struct CBufferView {};
class CSubresourceSubset
{
public:
	CSubresourceSubset() {}
	inline explicit CSubresourceSubset(const CBufferView&) :
		m_BeginArray(0),
		m_EndArray(1),
		m_BeginMip(0),
		m_EndMip(1),
		m_BeginPlane(0),
		m_EndPlane(1)
	{
	}
	inline explicit CSubresourceSubset(const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, DXGI_FORMAT ResourceFormat) :
		m_BeginArray(0),
		m_EndArray(1),
		m_BeginMip(0),
		m_EndMip(1),
		m_BeginPlane(0),
		m_EndPlane(1)
	{
		switch (Desc.ViewDimension)
		{
		default: UE_ASSUME(0 && "Corrupt Resource Type on Shader Resource View"); break;

		case (D3D12_SRV_DIMENSION_BUFFER) :
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE1D) :
			m_BeginMip = uint8(Desc.Texture1D.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.Texture1D.MipLevels);
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE1DARRAY) :
			m_BeginArray = uint16(Desc.Texture1DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture1DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture1DArray.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.Texture1DArray.MipLevels);
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE2D) :
			m_BeginMip = uint8(Desc.Texture2D.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.Texture2D.MipLevels);
			m_BeginPlane = uint8(Desc.Texture2D.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2D.PlaneSlice + 1);
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE2DARRAY) :
			m_BeginArray = uint16(Desc.Texture2DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture2DArray.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.Texture2DArray.MipLevels);
			m_BeginPlane = uint8(Desc.Texture2DArray.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2DArray.PlaneSlice + 1);
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE2DMS) :
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY) :
			m_BeginArray = uint16(Desc.Texture2DMSArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DMSArray.ArraySize);
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;

		case (D3D12_SRV_DIMENSION_TEXTURE3D) :
			m_EndArray = uint16(-1); //all slices
			m_BeginMip = uint8(Desc.Texture3D.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.Texture3D.MipLevels);
			break;

		case (D3D12_SRV_DIMENSION_TEXTURECUBE) :
			m_BeginMip = uint8(Desc.TextureCube.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.TextureCube.MipLevels);
			m_BeginArray = 0;
			m_EndArray = 6;
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;

		case (D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) :
			m_BeginArray = uint16(Desc.TextureCubeArray.First2DArrayFace);
			m_EndArray = uint16(m_BeginArray + Desc.TextureCubeArray.NumCubes * 6);
			m_BeginMip = uint8(Desc.TextureCubeArray.MostDetailedMip);
			m_EndMip = uint8(m_BeginMip + Desc.TextureCubeArray.MipLevels);
			m_BeginPlane = GetPlaneSliceFromViewFormat(ResourceFormat, Desc.Format);
			m_EndPlane = m_BeginPlane + 1;
			break;
#if D3D12_RHI_RAYTRACING
		case (D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE):
			// Nothing here
			break;
#endif // D3D12_RHI_RAYTRACING
		}
	}
	inline explicit CSubresourceSubset(const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc) :
		m_BeginArray(0),
		m_EndArray(1),
		m_BeginMip(0),
		m_BeginPlane(0),
		m_EndPlane(1)
	{
		switch (Desc.ViewDimension)
		{
		default: UE_ASSUME(0 && "Corrupt Resource Type on Unordered Access View"); break;

		case (D3D12_UAV_DIMENSION_BUFFER) : break;

		case (D3D12_UAV_DIMENSION_TEXTURE1D) :
			m_BeginMip = uint8(Desc.Texture1D.MipSlice);
			break;

		case (D3D12_UAV_DIMENSION_TEXTURE1DARRAY) :
			m_BeginArray = uint16(Desc.Texture1DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture1DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture1DArray.MipSlice);
			break;

		case (D3D12_UAV_DIMENSION_TEXTURE2D) :
			m_BeginMip = uint8(Desc.Texture2D.MipSlice);
			m_BeginPlane = uint8(Desc.Texture2D.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2D.PlaneSlice + 1);
			break;

		case (D3D12_UAV_DIMENSION_TEXTURE2DARRAY) :
			m_BeginArray = uint16(Desc.Texture2DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture2DArray.MipSlice);
			m_BeginPlane = uint8(Desc.Texture2DArray.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2DArray.PlaneSlice + 1);
			break;

		case (D3D12_UAV_DIMENSION_TEXTURE3D) :
			m_BeginArray = uint16(Desc.Texture3D.FirstWSlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture3D.WSize);
			m_BeginMip = uint8(Desc.Texture3D.MipSlice);
			break;
		}

		m_EndMip = m_BeginMip + 1;
	}
	inline explicit CSubresourceSubset(const D3D12_RENDER_TARGET_VIEW_DESC& Desc) :
		m_BeginArray(0),
		m_EndArray(1),
		m_BeginMip(0),
		m_BeginPlane(0),
		m_EndPlane(1)
	{
		switch (Desc.ViewDimension)
		{
		default: UE_ASSUME(0 && "Corrupt Resource Type on Render Target View"); break;

		case (D3D12_RTV_DIMENSION_BUFFER) : break;

		case (D3D12_RTV_DIMENSION_TEXTURE1D) :
			m_BeginMip = uint8(Desc.Texture1D.MipSlice);
			break;

		case (D3D12_RTV_DIMENSION_TEXTURE1DARRAY) :
			m_BeginArray = uint16(Desc.Texture1DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture1DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture1DArray.MipSlice);
			break;

		case (D3D12_RTV_DIMENSION_TEXTURE2D) :
			m_BeginMip = uint8(Desc.Texture2D.MipSlice);
			m_BeginPlane = uint8(Desc.Texture2D.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2D.PlaneSlice + 1);
			break;

		case (D3D12_RTV_DIMENSION_TEXTURE2DMS) : break;

		case (D3D12_RTV_DIMENSION_TEXTURE2DARRAY) :
			m_BeginArray = uint16(Desc.Texture2DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture2DArray.MipSlice);
			m_BeginPlane = uint8(Desc.Texture2DArray.PlaneSlice);
			m_EndPlane = uint8(Desc.Texture2DArray.PlaneSlice + 1);
			break;

		case (D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY) :
			m_BeginArray = uint16(Desc.Texture2DMSArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DMSArray.ArraySize);
			break;

		case (D3D12_RTV_DIMENSION_TEXTURE3D) :
			m_BeginArray = uint16(Desc.Texture3D.FirstWSlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture3D.WSize);
			m_BeginMip = uint8(Desc.Texture3D.MipSlice);
			break;
		}

		m_EndMip = m_BeginMip + 1;
	}

	inline explicit CSubresourceSubset(const D3D12_DEPTH_STENCIL_VIEW_DESC& Desc, DXGI_FORMAT ResourceFormat, ViewSubresourceSubsetFlags Flags) :
		m_BeginArray(0),
		m_EndArray(1),
		m_BeginMip(0),
		m_BeginPlane(0),
		m_EndPlane(GetPlaneCount(ResourceFormat))
	{
		switch (Desc.ViewDimension)
		{
		default: UE_ASSUME(0 && "Corrupt Resource Type on Depth Stencil View"); break;

		case (D3D12_DSV_DIMENSION_TEXTURE1D) :
			m_BeginMip = uint8(Desc.Texture1D.MipSlice);
			break;

		case (D3D12_DSV_DIMENSION_TEXTURE1DARRAY) :
			m_BeginArray = uint16(Desc.Texture1DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture1DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture1DArray.MipSlice);
			break;

		case (D3D12_DSV_DIMENSION_TEXTURE2D) :
			m_BeginMip = uint8(Desc.Texture2D.MipSlice);
			break;

		case (D3D12_DSV_DIMENSION_TEXTURE2DMS) : break;

		case (D3D12_DSV_DIMENSION_TEXTURE2DARRAY) :
			m_BeginArray = uint16(Desc.Texture2DArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DArray.ArraySize);
			m_BeginMip = uint8(Desc.Texture2DArray.MipSlice);
			break;

		case (D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY) :
			m_BeginArray = uint16(Desc.Texture2DMSArray.FirstArraySlice);
			m_EndArray = uint16(m_BeginArray + Desc.Texture2DMSArray.ArraySize);
			break;
		}

		m_EndMip = m_BeginMip + 1;

		if (m_EndPlane == 2)
		{
			if ((Flags & ViewSubresourceSubsetFlags_DepthAndStencilDsv) != ViewSubresourceSubsetFlags_DepthAndStencilDsv)
			{
				if (Flags & ViewSubresourceSubsetFlags_DepthOnlyDsv)
				{
					m_BeginPlane = 0;
					m_EndPlane = 1;
				}
				else if (Flags & ViewSubresourceSubsetFlags_StencilOnlyDsv)
				{
					m_BeginPlane = 1;
					m_EndPlane = 2;
				}
			}
		}
	}

	__forceinline bool DoesNotOverlap(const CSubresourceSubset& other) const
	{
		if (m_EndArray <= other.m_BeginArray)
		{
			return true;
		}

		if (other.m_EndArray <= m_BeginArray)
		{
			return true;
		}

		if (m_EndMip <= other.m_BeginMip)
		{
			return true;
		}

		if (other.m_EndMip <= m_BeginMip)
		{
			return true;
		}

		if (m_EndPlane <= other.m_BeginPlane)
		{
			return true;
		}

		if (other.m_EndPlane <= m_BeginPlane)
		{
			return true;
		}

		return false;
	}

protected:
	uint16 m_BeginArray; // Also used to store Tex3D slices.
	uint16 m_EndArray; // End - Begin == Array Slices
	uint8 m_BeginMip;
	uint8 m_EndMip; // End - Begin == Mip Levels
	uint8 m_BeginPlane;
	uint8 m_EndPlane;
};

template <typename TDesc>
class FD3D12View;

class CViewSubresourceSubset : public CSubresourceSubset
{
	friend class FD3D12View < D3D12_SHADER_RESOURCE_VIEW_DESC >;
	friend class FD3D12View < D3D12_RENDER_TARGET_VIEW_DESC >;
	friend class FD3D12View < D3D12_DEPTH_STENCIL_VIEW_DESC >;
	friend class FD3D12View < D3D12_UNORDERED_ACCESS_VIEW_DESC >;

public:
	CViewSubresourceSubset() {}
	inline explicit CViewSubresourceSubset(const CBufferView&)
		: CSubresourceSubset(CBufferView())
		, m_MipLevels(1)
		, m_ArraySlices(1)
		, m_MostDetailedMip(0)
		, m_ViewArraySize(1)
	{
	}

	inline CViewSubresourceSubset(uint32 Subresource, uint8 MipLevels, uint16 ArraySize, uint8 PlaneCount)
		: m_MipLevels(MipLevels)
		, m_ArraySlices(ArraySize)
		, m_PlaneCount(PlaneCount)
	{
		if (Subresource < uint32(MipLevels) * uint32(ArraySize))
		{
			m_BeginArray = Subresource / MipLevels;
			m_EndArray = m_BeginArray + 1;
			m_BeginMip = Subresource % MipLevels;
			m_EndMip = m_EndArray + 1;
		}
		else
		{
			m_BeginArray = 0;
			m_BeginMip = 0;
			if (Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
			{
				m_EndArray = ArraySize;
				m_EndMip = MipLevels;
			}
			else
			{
				m_EndArray = 0;
				m_EndMip = 0;
			}
		}
		m_MostDetailedMip = m_BeginMip;
		m_ViewArraySize = m_EndArray - m_BeginArray;
	}

	inline CViewSubresourceSubset(const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc, uint8 MipLevels, uint16 ArraySize, DXGI_FORMAT ResourceFormat, ViewSubresourceSubsetFlags /*Flags*/)
		: CSubresourceSubset(Desc, ResourceFormat)
		, m_MipLevels(MipLevels)
		, m_ArraySlices(ArraySize)
		, m_PlaneCount(GetPlaneCount(ResourceFormat))
	{
		if (Desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D)
		{
			check(m_BeginArray == 0);
			m_EndArray = 1;
		}
		m_MostDetailedMip = m_BeginMip;
		m_ViewArraySize = m_EndArray - m_BeginArray;
		Reduce();
	}

	inline CViewSubresourceSubset(const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc, uint8 MipLevels, uint16 ArraySize, DXGI_FORMAT ResourceFormat, ViewSubresourceSubsetFlags /*Flags*/)
		: CSubresourceSubset(Desc)
		, m_MipLevels(MipLevels)
		, m_ArraySlices(ArraySize)
		, m_PlaneCount(GetPlaneCount(ResourceFormat))
	{
		if (Desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D)
		{
			m_BeginArray = 0;
			m_EndArray = 1;
		}
		m_MostDetailedMip = m_BeginMip;
		m_ViewArraySize = m_EndArray - m_BeginArray;
		Reduce();
	}

	inline CViewSubresourceSubset(const D3D12_DEPTH_STENCIL_VIEW_DESC& Desc, uint8 MipLevels, uint16 ArraySize, DXGI_FORMAT ResourceFormat, ViewSubresourceSubsetFlags Flags)
		: CSubresourceSubset(Desc, ResourceFormat, Flags)
		, m_MipLevels(MipLevels)
		, m_ArraySlices(ArraySize)
		, m_PlaneCount(GetPlaneCount(ResourceFormat))
	{
		m_MostDetailedMip = m_BeginMip;
		m_ViewArraySize = m_EndArray - m_BeginArray;
		Reduce();
	}

	inline CViewSubresourceSubset(const D3D12_RENDER_TARGET_VIEW_DESC& Desc, uint8 MipLevels, uint16 ArraySize, DXGI_FORMAT ResourceFormat, ViewSubresourceSubsetFlags /*Flags*/)
		: CSubresourceSubset(Desc)
		, m_MipLevels(MipLevels)
		, m_ArraySlices(ArraySize)
		, m_PlaneCount(GetPlaneCount(ResourceFormat))
	{
		if (Desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D)
		{
			m_BeginArray = 0;
			m_EndArray = 1;
		}
		m_MostDetailedMip = m_BeginMip;
		m_ViewArraySize = m_EndArray - m_BeginArray;
		Reduce();
	}

	template<typename T>
	static CViewSubresourceSubset FromView(const T* pView)
	{
		return CViewSubresourceSubset(
			pView->Desc(),
			static_cast<uint8>(pView->GetResource()->GetMipLevels()),
			static_cast<uint16>(pView->GetResource()->GetArraySize()),
			static_cast<uint8>(pView->GetResource()->GetPlaneCount())
			);
	}

public:
	class CViewSubresourceIterator;

public:
	CViewSubresourceIterator begin() const;
	CViewSubresourceIterator end() const;
	bool IsWholeResource() const;
	uint32 ArraySize() const;

	uint8 MostDetailedMip() const;
	uint16 ViewArraySize() const;

	uint32 MinSubresource() const;
	uint32 MaxSubresource() const;

private:
	// Strictly for performance, allows coalescing contiguous subresource ranges into a single range
	inline void Reduce()
	{
		if (m_BeginMip == 0
			&& m_EndMip == m_MipLevels
			&& m_BeginArray == 0
			&& m_EndArray == m_ArraySlices
			&& m_BeginPlane == 0
			&& m_EndPlane == m_PlaneCount)
		{
			uint32 startSubresource = D3D12CalcSubresource(0, 0, m_BeginPlane, m_MipLevels, m_ArraySlices);
			uint32 endSubresource = D3D12CalcSubresource(0, 0, m_EndPlane, m_MipLevels, m_ArraySlices);

			// Only coalesce if the full-resolution UINTs fit in the UINT8s used for storage here
			if (endSubresource < static_cast<uint8>(-1))
			{
				m_BeginArray = 0;
				m_EndArray = 1;
				m_BeginPlane = 0;
				m_EndPlane = 1;
				m_BeginMip = static_cast<uint8>(startSubresource);
				m_EndMip = static_cast<uint8>(endSubresource);
			}
		}
	}

protected:
	uint8 m_MipLevels;
	uint16 m_ArraySlices;
	uint8 m_PlaneCount;
	uint8 m_MostDetailedMip;
	uint16 m_ViewArraySize;
};

// This iterator iterates over contiguous ranges of subresources within a subresource subset. eg:
//
// // For each contiguous subresource range.
// for( CViewSubresourceIterator it = ViewSubset.begin(); it != ViewSubset.end(); ++it )
// {
//      // StartSubresource and EndSubresource members of the iterator describe the contiguous range.
//      for( uint32 SubresourceIndex = it.StartSubresource(); SubresourceIndex < it.EndSubresource(); SubresourceIndex++ )
//      {
//          // Action for each subresource within the current range.
//      }
//  }
//
class CViewSubresourceSubset::CViewSubresourceIterator
{
public:
	inline CViewSubresourceIterator(CViewSubresourceSubset const& SubresourceSet, uint16 ArraySlice, uint8 PlaneSlice)
		: m_Subresources(SubresourceSet)
		, m_CurrentArraySlice(ArraySlice)
		, m_CurrentPlaneSlice(PlaneSlice)
	{
	}

	inline CViewSubresourceSubset::CViewSubresourceIterator& operator++()
	{
		check(m_CurrentArraySlice < m_Subresources.m_EndArray);

		if (++m_CurrentArraySlice >= m_Subresources.m_EndArray)
		{
			check(m_CurrentPlaneSlice < m_Subresources.m_EndPlane);
			m_CurrentArraySlice = m_Subresources.m_BeginArray;
			++m_CurrentPlaneSlice;
		}

		return *this;
	}

	inline CViewSubresourceSubset::CViewSubresourceIterator& operator--()
	{
		if (m_CurrentArraySlice <= m_Subresources.m_BeginArray)
		{
			m_CurrentArraySlice = m_Subresources.m_EndArray;

			check(m_CurrentPlaneSlice > m_Subresources.m_BeginPlane);
			--m_CurrentPlaneSlice;
		}

		--m_CurrentArraySlice;

		return *this;
	}

	inline bool operator==(CViewSubresourceIterator const& other) const
	{
		return &other.m_Subresources == &m_Subresources
			&& other.m_CurrentArraySlice == m_CurrentArraySlice
			&& other.m_CurrentPlaneSlice == m_CurrentPlaneSlice;
	}

	inline bool operator!=(CViewSubresourceIterator const& other) const
	{
		return !(other == *this);
	}

	inline uint32 StartSubresource() const
	{
		return D3D12CalcSubresource(m_Subresources.m_BeginMip, m_CurrentArraySlice, m_CurrentPlaneSlice, m_Subresources.m_MipLevels, m_Subresources.m_ArraySlices);
	}

	inline uint32 EndSubresource() const
	{
		return D3D12CalcSubresource(m_Subresources.m_EndMip, m_CurrentArraySlice, m_CurrentPlaneSlice, m_Subresources.m_MipLevels, m_Subresources.m_ArraySlices);
	}

	inline TPair<uint32, uint32> operator*() const
	{
		TPair<uint32, uint32> NewPair;
		NewPair.Key = StartSubresource();
		NewPair.Value = EndSubresource();
		return NewPair;
	}

private:
	CViewSubresourceSubset const& m_Subresources;
	uint16 m_CurrentArraySlice;
	uint8 m_CurrentPlaneSlice;
};

inline CViewSubresourceSubset::CViewSubresourceIterator CViewSubresourceSubset::begin() const
{
	return CViewSubresourceIterator(*this, m_BeginArray, m_BeginPlane);
}

inline CViewSubresourceSubset::CViewSubresourceIterator CViewSubresourceSubset::end() const
{
	return CViewSubresourceIterator(*this, m_BeginArray, m_EndPlane);
}

inline bool CViewSubresourceSubset::IsWholeResource() const
{
	return m_BeginMip == 0 && m_BeginArray == 0 && m_BeginPlane == 0 && (m_EndMip * m_EndArray * m_EndPlane == m_MipLevels * m_ArraySlices * m_PlaneCount);
}

inline uint32 CViewSubresourceSubset::ArraySize() const
{
	return m_ArraySlices;
}

inline uint8 CViewSubresourceSubset::MostDetailedMip() const
{
	return m_MostDetailedMip;
}

inline uint16 CViewSubresourceSubset::ViewArraySize() const
{
	return m_ViewArraySize;
}

inline uint32 CViewSubresourceSubset::MinSubresource() const
{
	return (*begin()).Key;
}

inline uint32 CViewSubresourceSubset::MaxSubresource() const
{
	return (*(--end())).Value;
}

class FD3D12OfflineDescriptorManager;

template <typename TDesc>
class TD3D12ViewDescriptorHandle : public FD3D12DeviceChild
{
	D3D12_CPU_DESCRIPTOR_HANDLE Handle;
	uint32 Index;

public:
	TD3D12ViewDescriptorHandle(FD3D12Device* InParentDevice)
		: FD3D12DeviceChild(InParentDevice)
	{
		Handle.ptr = 0;
		AllocateDescriptorSlot();
	}

	~TD3D12ViewDescriptorHandle()
	{
		FreeDescriptorSlot();
	}

	void SetParentDevice(FD3D12Device* InParent)
	{
		check(!Parent && !Handle.ptr);
		FD3D12DeviceChild::SetParentDevice(InParent);
		AllocateDescriptorSlot();
	}

	// Implemented in D3D12Device.h due to dependencies on FD3D12Device
	inline void CreateView(const TDesc& Desc, ID3D12Resource* Resource);
	inline void CreateViewWithCounter(const TDesc& Desc, ID3D12Resource* Resource, ID3D12Resource* CounterResource);

	inline const D3D12_CPU_DESCRIPTOR_HANDLE& GetHandle() const { return Handle; }
	inline uint32 GetIndex() const { return Index; }

private:
	// Implemented in D3D12Device.h due to dependencies on FD3D12Device
	inline void AllocateDescriptorSlot();
	inline void FreeDescriptorSlot();
};

typedef TD3D12ViewDescriptorHandle<D3D12_SHADER_RESOURCE_VIEW_DESC>		FD3D12DescriptorHandleSRV;
typedef TD3D12ViewDescriptorHandle<D3D12_RENDER_TARGET_VIEW_DESC>		FD3D12DescriptorHandleRTV;
typedef TD3D12ViewDescriptorHandle<D3D12_DEPTH_STENCIL_VIEW_DESC>		FD3D12DescriptorHandleDSV;
typedef TD3D12ViewDescriptorHandle<D3D12_UNORDERED_ACCESS_VIEW_DESC>	FD3D12DescriptorHandleUAV;

template <typename TDesc>
class FD3D12View : public FD3D12ShaderResourceRenameListener
{
protected:
	TD3D12ViewDescriptorHandle<TDesc> Descriptor;

	ViewSubresourceSubsetFlags Flags;
	FD3D12BaseShaderResource* BaseShaderResource;
	FD3D12ResourceLocation* ResourceLocation;
	FD3D12ResidencyHandle* ResidencyHandle;
	FD3D12Resource* Resource;
	CViewSubresourceSubset ViewSubresourceSubset;
	TDesc Desc;

#if DO_CHECK || USING_CODE_ANALYSIS
	bool bInitialized;
#endif

	explicit FD3D12View(FD3D12Device* InParent, ViewSubresourceSubsetFlags InFlags)
		: Descriptor(InParent)
		, Flags(InFlags)
		, BaseShaderResource(nullptr)
#if DO_CHECK || USING_CODE_ANALYSIS
		, bInitialized(false)
#endif
	{}

	virtual ~FD3D12View()
	{
		if (BaseShaderResource)
		{
			BaseShaderResource->RemoveRenameListener(this);
		}

#if DO_CHECK || USING_CODE_ANALYSIS
		bInitialized = false;
#endif
	}

protected:
	void SetDesc(const TDesc& InDesc)
	{
		Desc = InDesc;
	}

	void InitializeInternal(FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation)
	{
		check(InBaseShaderResource);
		checkf(BaseShaderResource == nullptr || BaseShaderResource == InBaseShaderResource, TEXT("Either BaseShaderResource is not set yet or when it is it can't change (after rename)"));

		// Only register the first time - init can be called again during rename
		if (BaseShaderResource == nullptr)
		{
			InBaseShaderResource->AddRenameListener(this);
		}

		BaseShaderResource = InBaseShaderResource;
		ResourceLocation = &InResourceLocation;
		Resource = ResourceLocation->GetResource();

		// Transient resources might not have an actual resource yet
		if (Resource)
		{
			ResidencyHandle = Resource->GetResidencyHandle();
			ViewSubresourceSubset = CViewSubresourceSubset(Desc,
				Resource->GetMipLevels(),
				Resource->GetArraySize(),
				Resource->GetDesc().Format,
				Flags);
		}
		else
		{
			ResidencyHandle = nullptr;
			ViewSubresourceSubset = CViewSubresourceSubset();
		}

#if DO_CHECK || USING_CODE_ANALYSIS
		// Only mark initialize if an actual resource is created for the base shader resource
		bInitialized = (Resource != nullptr);
#endif
	}

	virtual void ResourceRenamed(FD3D12BaseShaderResource* InRenamedResource, FD3D12ResourceLocation* InNewResourceLocation) override
	{
		check(InRenamedResource == BaseShaderResource);
		if (InNewResourceLocation)
		{
			// Only recreate the view if new location is valid
			if (InNewResourceLocation->IsValid())
			{
				RecreateView();
			}
			else
			{
#if DO_CHECK || USING_CODE_ANALYSIS
				// Mark as invalid for usage
				bInitialized = false;
#endif
			}
		}
		else
		{
			// Marking not initialized will currently assert because views are used after the resource has been registered for delete
			// Is that wrong?
			BaseShaderResource = nullptr;
		}
	}

	virtual void RecreateView() = 0;

public:
	inline FD3D12Device*					GetParentDevice()			const { return Descriptor.GetParentDevice(); }
	inline FD3D12Device*					GetParentDevice_Unsafe()	const { return Descriptor.GetParentDevice_Unsafe(); }
	inline FD3D12ResourceLocation*			GetResourceLocation()		const { return ResourceLocation; }
	inline const TDesc&						GetDesc()					const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return Desc; }
	inline D3D12_CPU_DESCRIPTOR_HANDLE		GetView()					const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return Descriptor.GetHandle(); }
	inline uint32							GetDescriptorHeapIndex()	const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return Descriptor.GetIndex(); }
	inline FD3D12Resource*					GetResource()				const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return Resource; }
	inline FD3D12ResidencyHandle*			GetResidencyHandle()		const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return ResidencyHandle; }
	inline const CViewSubresourceSubset&	GetViewSubresourceSubset()	const { checkf(bInitialized, TEXT("Uninitialized D3D12View size %d"), (uint32)sizeof(TDesc)); return ViewSubresourceSubset; }

	void SetParentDevice(FD3D12Device* InParent)
	{
		Descriptor.SetParentDevice(InParent);
	}

	template< class T >
	inline bool DoesNotOverlap(const FD3D12View< T >& Other) const
	{
		return ViewSubresourceSubset.DoesNotOverlap(Other.GetViewSubresourceSubset());
	}
};

/** Shader resource view class. */
class FD3D12ShaderResourceView : public FRHIShaderResourceView, public FD3D12View<D3D12_SHADER_RESOURCE_VIEW_DESC>, public FD3D12LinkedAdapterObject<FD3D12ShaderResourceView>
{
	bool bContainsDepthPlane;
	bool bContainsStencilPlane;
	bool bSkipFastClearFinalize;
	bool bRequiresResourceStateTracking;
	uint32 Stride;
	uint32 StartOffsetBytes;

public:
	// Used for dynamic buffer SRVs, which can be renamed. Must be explicitly Initialize()'d before it can be used.
	FD3D12ShaderResourceView(FD3D12Device* InParent)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_None)
	{}

	// Used for all other SRV resource types. Initialization is immediate on the calling thread.
	// Should not be used for dynamic resources which can be renamed.
	FD3D12ShaderResourceView(FD3D12Device* InParent, D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, uint32 InStride = -1, uint32 InStartOffsetBytes = 0, bool InSkipFastClearFinalize = false)
		: FD3D12ShaderResourceView(InParent)
	{
		Initialize(InDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation, InStride, InStartOffsetBytes, InSkipFastClearFinalize);
	}

	template<typename TextureType>
	FD3D12ShaderResourceView(TextureType* InTexture, D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc)
		: FD3D12ShaderResourceView(InTexture->GetParentDevice(), InDesc, InTexture)
	{
	}

	~FD3D12ShaderResourceView()
	{
	}

	void PreCreateView(FD3D12ResourceLocation& InResourceLocation, uint32 InStride, uint32 InStartOffsetBytes, bool InSkipFastClearFinalize)
	{
		Stride = InStride;
		StartOffsetBytes = InStartOffsetBytes;
		bSkipFastClearFinalize = InSkipFastClearFinalize;

		if (InResourceLocation.GetResource())
		{
			bContainsDepthPlane = InResourceLocation.GetResource()->IsDepthStencilResource() && GetPlaneSliceFromViewFormat(InResourceLocation.GetResource()->GetDesc().Format, Desc.Format) == 0;
			bContainsStencilPlane = InResourceLocation.GetResource()->IsDepthStencilResource() && GetPlaneSliceFromViewFormat(InResourceLocation.GetResource()->GetDesc().Format, Desc.Format) == 1;
			bRequiresResourceStateTracking = InResourceLocation.GetResource()->RequiresResourceStateTracking();

#if DO_CHECK
			// Check the plane slice of the SRV matches the texture format
			// Texture2DMS does not have explicit plane index (it's implied by the format)
			if (Desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
			{
				check(GetPlaneSliceFromViewFormat(InResourceLocation.GetResource()->GetDesc().Format, Desc.Format) == Desc.Texture2D.PlaneSlice);
			}

			if (Desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
			{
				//check(InResourceLocation.GetOffsetFromBaseOfResource() == Desc.Buffer.FirstElement * Stride);
			}
#endif
		}
	}

	void CreateView(const D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation);

	void Initialize(D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation, uint32 InStride, uint32 InStartOffsetBytes, bool InSkipFastClearFinalize)
	{
		SetDesc(InDesc);

		PreCreateView(InResourceLocation, InStride, InStartOffsetBytes, InSkipFastClearFinalize);
		CreateView(InDesc, InBaseShaderResource, InResourceLocation);
	}

	void Initialize(D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, uint32 InStride, uint32 InStartOffsetBytes, bool InSkipFastClearFinalize = false)
	{
		Initialize(InDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation, InStride, InStartOffsetBytes, InSkipFastClearFinalize);
	}

	void Initialize(FD3D12Device* InParent, D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, uint32 InStride, bool InSkipFastClearFinalize = false)
	{
		if (!this->GetParentDevice_Unsafe())
		{
			// This is a null SRV created without viewing on any resource
			// We need to set its device and allocate a descriptor slot before moving forward
			this->SetParentDevice(InParent);
		}
		check(GetParentDevice() == InParent);
		Initialize(InDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation, InStride, 0, InSkipFastClearFinalize);
	}

	void Rename(FD3D12BaseShaderResource* InBaseShaderResource)
	{
		check(InBaseShaderResource == BaseShaderResource);
		check(&InBaseShaderResource->ResourceLocation == ResourceLocation);

		// Update the first element index, then reinitialize the SRV
		if (Desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
		{
			uint32 StartElement = StartOffsetBytes / Stride;
			Desc.Buffer.FirstElement = ResourceLocation->GetOffsetFromBaseOfResource() / Stride + StartElement;
		}

		Initialize(Desc, BaseShaderResource, Stride, StartOffsetBytes);
	}

	void Rename(float ResourceMinLODClamp)
	{
		check(bInitialized);
		check(ResourceLocation);
		check(Desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);

		// Update the LODClamp, the reinitialize the SRV
		Desc.Texture2D.ResourceMinLODClamp = ResourceMinLODClamp;
		CreateView(Desc, BaseShaderResource, *ResourceLocation);
	}

	virtual void RecreateView() override
	{
		Rename(BaseShaderResource);
	}

	FORCEINLINE bool IsDepthStencilResource()	const { return bContainsDepthPlane || bContainsStencilPlane; }
	FORCEINLINE bool IsDepthPlaneResource()		const { return bContainsDepthPlane; }
	FORCEINLINE bool IsStencilPlaneResource()	const { return bContainsStencilPlane; }
	FORCEINLINE bool GetSkipFastClearFinalize()	const { return bSkipFastClearFinalize; }
	FORCEINLINE bool RequiresResourceStateTracking() const { return bRequiresResourceStateTracking; }
};

class FD3D12ShaderResourceViewWithLocation : public FD3D12ShaderResourceView
{
public:
	FD3D12ShaderResourceViewWithLocation(FD3D12Device* InParent)
		: FD3D12ShaderResourceView(InParent)
		, ViewLocation(InParent)
	{}

	FD3D12ShaderResourceViewWithLocation(FD3D12Device* InParent, D3D12_SHADER_RESOURCE_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, uint32 InStride = -1, bool InSkipFastClearFinalize = false)
		: FD3D12ShaderResourceView(InParent)
		, ViewLocation(InParent)
	{
		// Should not be pool allocated
		check(InBaseShaderResource->ResourceLocation.GetAllocatorType() != FD3D12ResourceLocation::EAllocatorType::AT_Pool);
		Initialize(InDesc, InBaseShaderResource, ViewLocation, InStride, 0, InSkipFastClearFinalize);
	}

	virtual void RecreateView() override
	{
		// Currently unsupported because the view location resource would also need to be recreated then
		check(false);
	}

	FD3D12ResourceLocation ViewLocation;
};

class FD3D12UnorderedAccessView : public FRHIUnorderedAccessView, public FD3D12View < D3D12_UNORDERED_ACCESS_VIEW_DESC >, public FD3D12LinkedAdapterObject<FD3D12UnorderedAccessView>
{
public:

	FD3D12UnorderedAccessView(FD3D12Device* InParent)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_None)
	{
	}

	FD3D12UnorderedAccessView(FD3D12Device* InParent, D3D12_UNORDERED_ACCESS_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12Resource* InCounterResource = nullptr)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_None)
		, CounterResource(InCounterResource)
	{
		Initialize(InDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation, InCounterResource);
	}

	void CreateView(const D3D12_UNORDERED_ACCESS_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation, FD3D12Resource* InCounterResource);

	void Initialize(D3D12_UNORDERED_ACCESS_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation, FD3D12Resource* InCounterResource = nullptr)
	{
		SetDesc(InDesc);
		CreateView(InDesc, InBaseShaderResource, InResourceLocation, InCounterResource);
	}

	virtual void RecreateView() override
	{
		check(CounterResource == nullptr);
		check(ResourceLocation->GetOffsetFromBaseOfResource() == 0);
		CreateView(Desc, BaseShaderResource, *ResourceLocation, nullptr);
	}

	bool IsCounterResourceInitialized() const { return CounterResourceInitialized; }
	void MarkCounterResourceInitialized() { CounterResourceInitialized = true; }

	FD3D12Resource* GetCounterResource() { return CounterResource; }

protected:
	TRefCountPtr<FD3D12Resource> CounterResource;
	bool CounterResourceInitialized = false;
};

class FD3D12UnorderedAccessViewWithLocation : public FD3D12UnorderedAccessView
{
public:
	FD3D12UnorderedAccessViewWithLocation(FD3D12Device* InParent)
	: FD3D12UnorderedAccessView(InParent)
	, ViewLocation(InParent)
	{
	}

	virtual void RecreateView() override
	{
		// Currently unsupported because the view location resource would also need to be recreated then
		check(false);
	}

	FD3D12ResourceLocation ViewLocation;
};

#if USE_STATIC_ROOT_SIGNATURE
class FD3D12ConstantBufferView : public FD3D12DeviceChild
{
			public:
//protected:
	/** The handle to the descriptor in the offline descriptor heap */
	D3D12_CPU_DESCRIPTOR_HANDLE OfflineDescriptorHandle;

	/** Index of the descriptor in the offline heap */
	uint32 OfflineHeapIndex;

	D3D12_CONSTANT_BUFFER_VIEW_DESC Desc;

//protected:

	explicit FD3D12ConstantBufferView(FD3D12Device* InParent, const D3D12_CONSTANT_BUFFER_VIEW_DESC* InDesc)
		: FD3D12DeviceChild(InParent)
		, OfflineHeapIndex(UINT_MAX)
	{
		OfflineDescriptorHandle.ptr = 0;
		Init(InDesc);
	}

	virtual ~FD3D12ConstantBufferView()
	{
		FreeHeapSlot();
	}

	void Init(const D3D12_CONSTANT_BUFFER_VIEW_DESC* InDesc)
	{
		if (InDesc)
		{
			Desc = *InDesc;
		}
		else
		{
			FMemory::Memzero(&Desc, sizeof(Desc));
		}
		AllocateHeapSlot();
	}

	void AllocateHeapSlot();

	void FreeHeapSlot();

	void Create(D3D12_GPU_VIRTUAL_ADDRESS GPUAddress, const uint32 AlignedSize);

public:
	const D3D12_CONSTANT_BUFFER_VIEW_DESC& GetDesc() const { return Desc; }
};
#endif

class FD3D12RenderTargetView : public FD3D12View<D3D12_RENDER_TARGET_VIEW_DESC>, public FRHIResource, public FD3D12LinkedAdapterObject<FD3D12RenderTargetView>
{
public:
	FD3D12RenderTargetView(FD3D12Device* InParent, const D3D12_RENDER_TARGET_VIEW_DESC& InRTVDesc, FD3D12BaseShaderResource* InBaseShaderResource)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_None)
		, FRHIResource(RRT_None)
	{
		CreateView(InRTVDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation);
	}

	FD3D12RenderTargetView(FD3D12Device* InParent, const D3D12_RENDER_TARGET_VIEW_DESC& InRTVDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_None)
		, FRHIResource(RRT_None)
	{
		CreateView(InRTVDesc, InBaseShaderResource, InResourceLocation);
	}

	void CreateView(const D3D12_RENDER_TARGET_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation);

	virtual void RecreateView() override
	{
		check(ResourceLocation->GetOffsetFromBaseOfResource() == 0);
		CreateView(Desc, BaseShaderResource, BaseShaderResource->ResourceLocation);
	}
};

class FD3D12DepthStencilView : public FD3D12View<D3D12_DEPTH_STENCIL_VIEW_DESC>, public FRHIResource, public FD3D12LinkedAdapterObject<FD3D12DepthStencilView>
{
	const bool bHasDepth;
	const bool bHasStencil;
	CViewSubresourceSubset DepthOnlyViewSubresourceSubset;
	CViewSubresourceSubset StencilOnlyViewSubresourceSubset;

public:
	FD3D12DepthStencilView(FD3D12Device* InParent, const D3D12_DEPTH_STENCIL_VIEW_DESC& InDSVDesc, FD3D12BaseShaderResource* InBaseShaderResource, bool InHasStencil)
		: FD3D12View(InParent, ViewSubresourceSubsetFlags_DepthAndStencilDsv)
		, FRHIResource(RRT_None)
		, bHasDepth(true)				// Assume all DSVs have depth bits in their format
		, bHasStencil(InHasStencil)		// Only some DSVs have stencil bits in their format
	{
		CreateView(InDSVDesc, InBaseShaderResource, InBaseShaderResource->ResourceLocation);
		SetupDepthStencilViewSubresourceSubset();
	}

	void CreateView(const D3D12_DEPTH_STENCIL_VIEW_DESC& InDesc, FD3D12BaseShaderResource* InBaseShaderResource, FD3D12ResourceLocation& InResourceLocation);

	bool HasDepth() const
	{
		return bHasDepth;
	}

	bool HasStencil() const
	{
		return bHasStencil;
	}

	void SetupDepthStencilViewSubresourceSubset()
	{
		if (Resource)
		{
			// Create individual subresource subsets for each plane
			if (bHasDepth)
			{
				DepthOnlyViewSubresourceSubset = CViewSubresourceSubset(Desc,
					Resource->GetMipLevels(),
					Resource->GetArraySize(),
					Resource->GetDesc().Format,
					ViewSubresourceSubsetFlags_DepthOnlyDsv);
			}

			if (bHasStencil)
			{
				StencilOnlyViewSubresourceSubset = CViewSubresourceSubset(Desc,
					Resource->GetMipLevels(),
					Resource->GetArraySize(),
					Resource->GetDesc().Format,
					ViewSubresourceSubsetFlags_StencilOnlyDsv);
			}
		}
	}

	CViewSubresourceSubset& GetDepthOnlyViewSubresourceSubset()
	{
		check(bHasDepth);
		return DepthOnlyViewSubresourceSubset;
	}

	CViewSubresourceSubset& GetStencilOnlyViewSubresourceSubset()
	{
		check(bHasStencil);
		return StencilOnlyViewSubresourceSubset;
	}

	virtual void RecreateView() override
	{
		check(ResourceLocation->GetOffsetFromBaseOfResource() == 0);
		CreateView(Desc, BaseShaderResource, BaseShaderResource->ResourceLocation);
		SetupDepthStencilViewSubresourceSubset();
	}
};

template<>
struct TD3D12ResourceTraits<FRHIShaderResourceView>
{
	typedef FD3D12ShaderResourceView TConcreteType;
};
template<>
struct TD3D12ResourceTraits<FRHIUnorderedAccessView>
{
	typedef FD3D12UnorderedAccessView TConcreteType;
};
