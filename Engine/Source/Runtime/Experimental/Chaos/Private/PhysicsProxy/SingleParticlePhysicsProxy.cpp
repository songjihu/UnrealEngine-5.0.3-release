// Copyright Epic Games, Inc. All Rights Reserved.

#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

#include "ChaosStats.h"
#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Chaos/Sphere.h"
#include "Chaos/ErrorReporter.h"
#include "Chaos/Serializable.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Framework/MultiBufferResource.h"
#include "PhysicsSolver.h"
#include "Chaos/ChaosMarshallingManager.h"
#include "Chaos/PullPhysicsDataImp.h"
#include "RewindData.h"

// This is a temporary workaround to avoid GT copying position from physics results for kinematics, as they are already at target.
// Velocity and such is still copied. This will be handled better in the future.
int32 SyncKinematicOnGameThread = 0;
FAutoConsoleVariableRef CVar_SyncKinematicOnGameThread(TEXT("P.Chaos.SyncKinematicOnGameThread"), SyncKinematicOnGameThread, TEXT("If set to 1, if a kinematic is flagged to send position back to game thread, move component, if 0, do not."));


FSingleParticlePhysicsProxy::FSingleParticlePhysicsProxy(TUniquePtr<PARTICLE_TYPE>&& InParticle, FParticleHandle* InHandle, UObject* InOwner)
	: IPhysicsProxyBase(EPhysicsProxyType::SingleParticleProxy, InOwner)
	, Particle(MoveTemp(InParticle))
	, Handle(InHandle)
	, PullDataInterpIdx_External(INDEX_NONE)
{
	Particle->SetProxy(this);
}


FSingleParticlePhysicsProxy::~FSingleParticlePhysicsProxy()
{
}

CHAOS_API int32 ForceNoCollisionIntoSQ = 0;
FAutoConsoleVariableRef CVarForceNoCollisionIntoSQ(TEXT("p.ForceNoCollisionIntoSQ"), ForceNoCollisionIntoSQ, TEXT("When enabled, all particles end up in sq structure, even ones with no collision"));

template <Chaos::EParticleType ParticleType, typename TEvolution>
void PushToPhysicsStateImp(const Chaos::FDirtyPropertiesManager& Manager, Chaos::FGeometryParticleHandle* Handle, int32 DataIdx, const Chaos::FDirtyProxy& Dirty, Chaos::FShapeDirtyData* ShapesData, TEvolution& Evolution, bool bResimInitialized, Chaos::FReal ExternalDt)
{
	using namespace Chaos;
	constexpr bool bHasKinematicData = ParticleType != EParticleType::Static;
	constexpr bool bHasDynamicData = ParticleType == EParticleType::Rigid;
	auto KinematicHandle = bHasKinematicData ? static_cast<Chaos::FKinematicGeometryParticleHandle*>(Handle) : nullptr;
	auto RigidHandle = bHasDynamicData ? static_cast<Chaos::FPBDRigidParticleHandle*>(Handle) : nullptr;
	const FDirtyChaosProperties& ParticleData = Dirty.PropertyData;

	if (bResimInitialized)	//todo: assumes particles are always initialized as enabled. This is not true in future versions of code, so check PushData
	{
		Evolution.EnableParticle(Handle, nullptr);
	}
	// move the copied game thread data into the handle
	{
		auto NewXR = ParticleData.FindXR(Manager, DataIdx);
		auto NewNonFrequentData = ParticleData.FindNonFrequentData(Manager, DataIdx);

		if(NewXR)
		{
			Handle->SetXR(*NewXR);
		}

		if(NewNonFrequentData)
		{
			Handle->SetNonFrequentData(*NewNonFrequentData);

			// geometry may have changed, we need to invalidate the particle so it can be removed from caching structures in the evolution
			// @todo(chaos): Remove collision constraints only. Invalidate particle may remove joint constraints in constraint graph. If joint constraint is persistent, this may cause issues.
			Evolution.InvalidateParticle(Handle);
			Evolution.DestroyParticleCollisionsInAllocator(Handle);
		}

		auto NewVelocities = bHasKinematicData ? ParticleData.FindVelocities(Manager, DataIdx) : nullptr;
		if(NewVelocities)
		{
			KinematicHandle->SetVelocities(*NewVelocities);
		}

		auto NewKinematicTargetGT = bHasKinematicData ? ParticleData.FindKinematicTarget(Manager, DataIdx) : nullptr;
		if (NewKinematicTargetGT)
		{
			Evolution.SetParticleKinematicTarget(KinematicHandle, *NewKinematicTargetGT);
		}

		if(NewXR || NewNonFrequentData || NewVelocities || NewKinematicTargetGT)
		{
			// Update world-space cached state like the bounds
			// @todo(chaos): do we need to do this here? It should be done in Integrate and ApplyKinematicTarget so only really Statics need this...
			const bool bHasKinematicTarget = (NewKinematicTargetGT != nullptr) && (NewKinematicTargetGT->GetMode() == EKinematicTargetMode::Position);
			const FRigidTransform3 WorldTransform = !bHasKinematicTarget ? FRigidTransform3(Handle->X(), Handle->R()) : NewKinematicTargetGT->GetTarget();
			Handle->UpdateWorldSpaceState(WorldTransform, FVec3(0));

			Evolution.DirtyParticle(*Handle);
		}

		if(bHasDynamicData)
		{
			if(auto NewData = ParticleData.FindMassProps(Manager,DataIdx))
			{
				RigidHandle->SetMassProps(*NewData);
			}

			if(auto NewData = ParticleData.FindDynamics(Manager, DataIdx))
			{
				RigidHandle->SetDynamics(*NewData);
				RigidHandle->ResetVSmoothFromForces();
			}

			if(auto NewData = ParticleData.FindDynamicMisc(Manager,DataIdx))
			{
				RigidHandle->SetDynamicMisc(*NewData, Evolution);				
			}
		}

		//shape properties
		bool bUpdateCollisionData = false;
		bool bHasCollision = false;
		for(int32 ShapeDataIdx : Dirty.ShapeDataIndices)
		{
			const FShapeDirtyData& ShapeData = ShapesData[ShapeDataIdx];
			const int32 ShapeIdx = ShapeData.GetShapeIdx();

			if(auto NewData = ShapeData.FindCollisionData(Manager, ShapeDataIdx))
			{
				bUpdateCollisionData = true;
				Handle->ShapesArray()[ShapeIdx]->SetCollisionData(*NewData);

				const FCollisionData& CollisionData = Handle->ShapesArray()[ShapeIdx]->GetCollisionData();
				bHasCollision |= CollisionData.HasCollisionData();
			}
			if(auto NewData = ShapeData.FindMaterials(Manager, ShapeDataIdx))
			{
				Handle->ShapesArray()[ShapeIdx]->SetMaterialData(*NewData);
			}

		}
		

		if(bUpdateCollisionData && !ForceNoCollisionIntoSQ)
		{
			//Some shapes were not dirty and may have collision - so have to iterate them all. TODO: find a better way to handle this case
			if(!bHasCollision && Dirty.ShapeDataIndices.Num() != Handle->ShapesArray().Num())
			{
				for (const TUniquePtr<FPerShapeData>& Shape : Handle->ShapesArray())
				{
					const FCollisionData& CollisionData = Shape->GetCollisionData();
					bHasCollision |= CollisionData.HasCollisionData();

					if (bHasCollision) { break; }
				}
			}

			Handle->SetHasCollision(bHasCollision);

			if(bHasCollision)
			{
				//make sure it's in acceleration structure
				Evolution.DirtyParticle(*Handle);
			}
			else
			{
				Evolution.RemoveParticleFromAccelerationStructure(*Handle);
			}
		}
	}
}

//
// TGeometryParticle<FReal, 3> template specialization 
//

void FSingleParticlePhysicsProxy::PushToPhysicsState(const Chaos::FDirtyPropertiesManager& Manager, int32 DataIdx, const Chaos::FDirtyProxy& Dirty, Chaos::FShapeDirtyData* ShapesData, Chaos::FPBDRigidsEvolutionGBF& Evolution, Chaos::FReal ExternalDt)
{
	using namespace Chaos;
	const int32 CurFrame = static_cast<FPBDRigidsSolver*>(Solver)->GetCurrentFrame();
	const FRewindData* RewindData = static_cast<FPBDRigidsSolver*>(Solver)->GetRewindData();
	const bool bResimInitialized = RewindData && RewindData->IsResim() && CurFrame == InitializedOnStep;
	switch(Dirty.PropertyData.GetParticleBufferType())
	{
		
	case EParticleType::Static: PushToPhysicsStateImp<EParticleType::Static>(Manager, Handle, DataIdx, Dirty, ShapesData, Evolution, bResimInitialized, ExternalDt); break;
	case EParticleType::Kinematic: PushToPhysicsStateImp<EParticleType::Kinematic>(Manager, Handle, DataIdx, Dirty, ShapesData, Evolution, bResimInitialized, ExternalDt); break;
	case EParticleType::Rigid: PushToPhysicsStateImp<EParticleType::Rigid>(Manager, Handle, DataIdx, Dirty, ShapesData, Evolution, bResimInitialized, ExternalDt); break;
	default: check(false); //unexpected path
	}
}

void FSingleParticlePhysicsProxy::ClearAccumulatedData()
{
	if(auto Rigid = Particle->CastToRigidParticle())
	{
		Rigid->ClearForces(false);
		Rigid->ClearTorques(false);
		Rigid->SetLinearImpulseVelocity(Chaos::FVec3(0), false);
		Rigid->SetAngularImpulseVelocity(Chaos::FVec3(0), false);
	}
	
	Particle->ClearDirtyFlags();
}

template <typename T>
void BufferPhysicsResultsImp(Chaos::FDirtyRigidParticleData& PullData, T* Particle)
{
	PullData.X = Particle->X();
	PullData.R = Particle->R();
	PullData.V = Particle->V();
	PullData.W = Particle->W();
	PullData.ObjectState = Particle->ObjectState();
}

void FSingleParticlePhysicsProxy::BufferPhysicsResults(Chaos::FDirtyRigidParticleData& PullData)
{
	using namespace Chaos;
	// Move simulation results into the double buffer.
	FPBDRigidParticleHandle* RigidHandle = Handle ? Handle->CastToRigidParticle() : nullptr;	//TODO: can handle be null?
	if(RigidHandle)
	{
		PullData.SetProxy(*this);
		BufferPhysicsResultsImp(PullData, RigidHandle);
	}
}

void FSingleParticlePhysicsProxy::BufferPhysicsResults_External(Chaos::FDirtyRigidParticleData& PullData)
{
	if(auto Rigid = Particle->CastToRigidParticle())
	{
		PullData.SetProxy(*this);
		BufferPhysicsResultsImp(PullData, Rigid);
	}
}

bool FSingleParticlePhysicsProxy::PullFromPhysicsState(const Chaos::FDirtyRigidParticleData& PullData,int32 SolverSyncTimestamp, const Chaos::FDirtyRigidParticleData* NextPullData, const Chaos::FRealSingle* Alpha, const Chaos::FRealSingle* LeashAlpha)
{
	using namespace Chaos;
	// Move buffered data into the TPBDRigidParticle without triggering invalidation of the physics state.
	auto Rigid = Particle ? Particle->CastToRigidParticle() : nullptr;
	if(Rigid)
	{
		const bool bSyncXR = SyncKinematicOnGameThread || (Rigid->ObjectState() != EObjectStateType::Kinematic);

		const FProxyTimestamp* ProxyTimestamp = PullData.GetTimestamp();
		
		if(NextPullData)
		{
			auto LerpHelper = [SolverSyncTimestamp](const int32 PropertyTimestamp, const auto& Prev, const auto& Overwrite) -> const auto*
			{
				//if overwrite is in the future, do nothing
				//if overwrite is on this step, we want to interpolate from overwrite to the result of the frame that consumed the overwrite
				//if overwrite is in the past, just do normal interpolation

				//this is nested because otherwise compiler can't figure out the type of nullptr with an auto return type
				return PropertyTimestamp <= SolverSyncTimestamp ? (PropertyTimestamp < SolverSyncTimestamp ? &Prev : &Overwrite) : nullptr;
			};

			if (bSyncXR)
			{
				if (const FVec3* Prev = LerpHelper(ProxyTimestamp->XTimestamp, PullData.X, ProxyTimestamp->OverWriteX))
				{
					FVec3 Target = FMath::Lerp(*Prev, NextPullData->X, *Alpha);
					if (LeashAlpha)
					{
						Target = FMath::Lerp(Rigid->X(), Target, *LeashAlpha);
					}
					Rigid->SetX(Target, false);
				}

				if (const FQuat* Prev = LerpHelper(ProxyTimestamp->RTimestamp, PullData.R, ProxyTimestamp->OverWriteR))
				{
					FQuat Target = FMath::Lerp(*Prev, NextPullData->R, *Alpha);
					if (LeashAlpha)
					{
						Target = FMath::Lerp<FQuat>(Rigid->R(), Target, *LeashAlpha);
					}
					Rigid->SetR(Target, false);
				}
			}

			if (const FVec3* Prev = LerpHelper(ProxyTimestamp->VTimestamp, PullData.V, ProxyTimestamp->OverWriteV))
			{
				FVec3 Target = FMath::Lerp(*Prev, NextPullData->V, *Alpha);
				if (LeashAlpha)
				{
					Target = FMath::Lerp(Rigid->V(), Target, *LeashAlpha);
				}
				Rigid->SetV(Target, false);
			}

			if (const FVec3* Prev = LerpHelper(ProxyTimestamp->WTimestamp, PullData.W, ProxyTimestamp->OverWriteW))
			{
				FVec3 Target = FMath::Lerp(*Prev, NextPullData->W, *Alpha);
				if (LeashAlpha)
				{
					Target = FMath::Lerp(Rigid->W(), Target, *LeashAlpha);
				}
				Rigid->SetW(Target, false);
			}

			//we are interpolating from PullData to Next, but the timestamp is associated with Next
			//since we are interpolating it means we must have not seen Next yet, so the timestamp has to be strictly less than
			if (ProxyTimestamp->ObjectStateTimestamp < SolverSyncTimestamp)
			{
				Rigid->SetObjectState(PullData.ObjectState, true, /*bInvalidate=*/false);
			}
			else if(ProxyTimestamp->ObjectStateTimestamp == SolverSyncTimestamp && *Alpha == 1.f)
			{
				//if timestamp is the same as next, AND alpha is exactly 1, we are exactly at Next's time
				//so we can use its sleep state
				Rigid->SetObjectState(NextPullData->ObjectState, true, /*bInvalidate=*/false);
			}
		}
		else
		{
			if (bSyncXR)
			{
				//no interpolation, just ignore if overwrite comes after
				if (SolverSyncTimestamp >= ProxyTimestamp->XTimestamp)
				{
					Rigid->SetX(PullData.X, false);
				}

				if (SolverSyncTimestamp >= ProxyTimestamp->RTimestamp)
				{
					Rigid->SetR(PullData.R, false);
				}
			}

			if(SolverSyncTimestamp >= ProxyTimestamp->VTimestamp)
			{
				Rigid->SetV(PullData.V, false);
			}

			if(SolverSyncTimestamp >= ProxyTimestamp->WTimestamp)
			{
				Rigid->SetW(PullData.W, false);
			}

			if (SolverSyncTimestamp >= ProxyTimestamp->ObjectStateTimestamp)
			{
				Rigid->SetObjectState(PullData.ObjectState, true, /*bInvalidate=*/false);
			}
		}
		
		Rigid->UpdateShapeBounds();
	}
	return true;
}

bool FSingleParticlePhysicsProxy::IsDirty()
{
	return Particle->IsDirty();
}

Chaos::EWakeEventEntry FSingleParticlePhysicsProxy::GetWakeEvent() const
{
	//question: should this API exist on proxy?
	auto Rigid = Particle->CastToRigidParticle();
	return Rigid ? Rigid->GetWakeEvent() : Chaos::EWakeEventEntry::None;
}

void FSingleParticlePhysicsProxy::ClearEvents()
{
	//question: should this API exist on proxy?
	if(auto Rigid = Particle->CastToRigidParticle())
	{
		Rigid->ClearEvents();
	}
}