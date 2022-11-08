// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MeshDescription.h"

// predeclare tangents template
PREDECLARE_GEOMETRY(template<typename RealType> class TMeshTangents);
using UE::Geometry::FDynamicMesh3;

/**
 * Convert FMeshDescription to FDynamicMesh3
 * 
 * @todo handle missing UV/normals on MD?
 * @todo be able to ignore UV/Normals
 * @todo handle additional UV layers on MD
 * @todo option to disable UV/Normal welding
 */
class MESHCONVERSION_API FMeshDescriptionToDynamicMesh
{
public:
	/** If true, will print some possibly-helpful debugging spew to output log */
	bool bPrintDebugMessages = false;

	/** Should we initialize triangle groups on output mesh */
	bool bEnableOutputGroups = true;

	/** Should we calculate conversion index maps */
	bool bCalculateMaps = true;

	/** Ignore all mesh attributes (e.g. UV/Normal layers, color layer, material groups) */
	bool bDisableAttributes = false;


	/** map from DynamicMesh triangle ID to MeshDescription FTriangleID*/
	TArray<FTriangleID> TriIDMap;

	/**
	* map from DynamicMesh vertex Id to MeshDecription FVertexID. 
	* NB: due to vertex splitting, multiple DynamicMesh vertex ids 
	* may map to the same MeshDescription FVertexID.
	*  ( a vertex split is a result of reconciling non-manifold MeshDescription vertex ) 	  
	*/
	TArray<FVertexID> VertIDMap;

	/**
	 * Various modes can be used to create output triangle groups
	 */
	enum class EPrimaryGroupMode
	{
		SetToZero,
		SetToPolygonID,
		SetToPolygonGroupID,
		SetToPolyGroup
	};
	/**
	 * Which mode to use to create groups on output mesh. Ignored if bEnableOutputGroups = false.
	 */
	EPrimaryGroupMode GroupMode = EPrimaryGroupMode::SetToPolyGroup;


	/**
	 * Default conversion of MeshDescription to DynamicMesh
	 * @param bCopyTangents  - if bDisableAttributes is false, this requests the tangent plane vectors (tangent and bitangent) 
	 *                          be stored as overlays in the MeshOut DynamicAttributeSet, provided they exist on the MeshIn
	 */
	void Convert(const FMeshDescription* MeshIn, FDynamicMesh3& MeshOut, bool bCopyTangents = false);

	/**
	 * Copy tangents from MeshDescription to a FMeshTangents instance.
	 * @warning Convert() must have been used to create the TargetMesh before calling this function
	 */
	void CopyTangents(const FMeshDescription* SourceMesh, const FDynamicMesh3* TargetMesh, UE::Geometry::TMeshTangents<float>* TangentsOut);

	/**
	 * Copy tangents from MeshDescription to a FMeshTangents instance.
	 * @warning Convert() must have been used to create the TargetMesh before calling this function
	 */
	void CopyTangents(const FMeshDescription* SourceMesh, const FDynamicMesh3* TargetMesh, UE::Geometry::TMeshTangents<double>* TangentsOut);
};