// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PBDSuspensionConstraints.h"
#include "Chaos/Evolution/SolverDatas.h"
#include "Chaos/DebugDrawQueue.h"

// Private includes
#include "Chaos/Collision/PBDCollisionSolver.h"

bool bChaos_Suspension_Spring_Enabled = true;
FAutoConsoleVariableRef CVarChaosSuspensionSpringEnabled(TEXT("p.Chaos.Suspension.Spring.Enabled"), bChaos_Suspension_Spring_Enabled, TEXT("Enable/Disable Spring part of suspension constraint"));

bool bChaos_Suspension_Hardstop_Enabled = true;
FAutoConsoleVariableRef CVarChaosSuspensionHardstopEnabled(TEXT("p.Chaos.Suspension.Hardstop.Enabled"), bChaos_Suspension_Hardstop_Enabled, TEXT("Enable/Disable Hardstop part of suspension constraint"));

bool bChaos_Suspension_VelocitySolve = true;
FAutoConsoleVariableRef CVarChaosSuspensionVelocitySolve(TEXT("p.Chaos.Suspension.VelocitySolve"), bChaos_Suspension_VelocitySolve, TEXT("Enable/Disable VelocitySolve"));

float Chaos_Suspension_MaxPushoutVelocity = 100.f;
FAutoConsoleVariableRef CVarChaosSuspensionMaxPushoutVelocity(TEXT("p.Chaos.Suspension.MaxPushoutVelocity"), Chaos_Suspension_MaxPushoutVelocity, TEXT("Chaos Suspension Max Pushout Velocity Value"));

float Chaos_Suspension_MaxPushout = 5.f;
FAutoConsoleVariableRef CVarChaosSuspensionMaxPushout(TEXT("p.Chaos.Suspension.MaxPushout"), Chaos_Suspension_MaxPushout, TEXT("Chaos Suspension Max Pushout Value"));

#if CHAOS_DEBUG_DRAW
bool bChaos_Suspension_DebugDraw_Hardstop = false;
FAutoConsoleVariableRef CVarChaosSuspensionDebugDrawHardstop(TEXT("p.Chaos.Suspension.DebugDraw.Hardstop"), bChaos_Suspension_DebugDraw_Hardstop, TEXT("Debug draw suspension hardstop manifold"));
#endif

namespace Chaos
{
	FPBDSuspensionConstraintHandle::FPBDSuspensionConstraintHandle(FConstraintContainer* InConstraintContainer, int32 InConstraintIndex) : 
		TIndexedContainerConstraintHandle<FPBDSuspensionConstraints>(InConstraintContainer, InConstraintIndex)
	{
	}

	const FPBDSuspensionSettings& FPBDSuspensionConstraintHandle::GetSettings() const
	{
		return ConcreteContainer()->GetSettings(ConstraintIndex);
	}

	FPBDSuspensionSettings& FPBDSuspensionConstraintHandle::GetSettings()
	{
		return ConcreteContainer()->GetSettings(ConstraintIndex);
	}

	void FPBDSuspensionConstraintHandle::SetSettings(const FPBDSuspensionSettings& Settings)
	{
		ConcreteContainer()->SetSettings(ConstraintIndex, Settings);
	}

	TVec2<FGeometryParticleHandle*> FPBDSuspensionConstraintHandle::GetConstrainedParticles() const
	{
		return ConcreteContainer()->GetConstrainedParticles(ConstraintIndex);
	}

	void FPBDSuspensionConstraintHandle::PreGatherInput(const FReal Dt, FPBDIslandSolverData& SolverData)
	{
		ConcreteContainer()->PreGatherInput(Dt, ConstraintIndex, SolverData);
	}

	void FPBDSuspensionConstraintHandle::GatherInput(const FReal Dt, const int32 Particle0Level, const int32 Particle1Level, FPBDIslandSolverData& SolverData)
	{
		ConcreteContainer()->GatherInput(Dt, ConstraintIndex, Particle0Level, Particle1Level, SolverData);
	}

	Chaos::FPBDSuspensionConstraints::FConstraintContainerHandle* FPBDSuspensionConstraints::AddConstraint(TGeometryParticleHandle<FReal, 3>* Particle, const FVec3& InSuspensionLocalOffset, const FPBDSuspensionSettings& InConstraintSettings)
	{
		int32 NewIndex = ConstrainedParticles.Num();
		ConstrainedParticles.Add(Particle);
		SuspensionLocalOffset.Add(InSuspensionLocalOffset);
		ConstraintSettings.Add(InConstraintSettings);
		ConstraintResults.AddDefaulted();
		ConstraintEnabledStates.Add(true); // Note: assumes always enabled on creation
		ConstraintSolverBodies.Add(nullptr);
		StaticCollisionBodies.Add(FSolverBody());
		CollisionSolvers.Add( new FPBDCollisionSolver() );

		Handles.Add(HandleAllocator.AllocHandle(this, NewIndex));
		return Handles[NewIndex];
	}


	void FPBDSuspensionConstraints::RemoveConstraint(int ConstraintIndex)
	{
		FConstraintContainerHandle* ConstraintHandle = Handles[ConstraintIndex];
		if (ConstraintHandle != nullptr)
		{
			if (ConstrainedParticles[ConstraintIndex])
			{
				ConstrainedParticles[ConstraintIndex]->RemoveConstraintHandle(ConstraintHandle);
			}

			// Release the handle for the freed constraint
			HandleAllocator.FreeHandle(ConstraintHandle);
			Handles[ConstraintIndex] = nullptr;
		}

		delete CollisionSolvers[ConstraintIndex];

		// Swap the last constraint into the gap to keep the array packed
		ConstrainedParticles.RemoveAtSwap(ConstraintIndex);
		SuspensionLocalOffset.RemoveAtSwap(ConstraintIndex);
		ConstraintSettings.RemoveAtSwap(ConstraintIndex);
		ConstraintResults.RemoveAtSwap(ConstraintIndex);
		ConstraintEnabledStates.RemoveAtSwap(ConstraintIndex);
		ConstraintSolverBodies.RemoveAtSwap(ConstraintIndex);
		CollisionSolvers.RemoveAtSwap(ConstraintIndex);
		StaticCollisionBodies.RemoveAtSwap(ConstraintIndex);

		Handles.RemoveAtSwap(ConstraintIndex);

		// Update the handle for the constraint that was moved
		if (ConstraintIndex < Handles.Num())
		{
			SetConstraintIndex(Handles[ConstraintIndex], ConstraintIndex);
		}
	}
	
	void FPBDSuspensionConstraints::SetNumIslandConstraints(const int32 NumIslandConstraints, FPBDIslandSolverData& SolverData)
	{
		SolverData.GetConstraintIndices(ContainerId).Reset(NumIslandConstraints);
	}

	// @todo: This does per body work so can't be safely parallelized
	void FPBDSuspensionConstraints::PreGatherInput(const FReal Dt, const int32 ConstraintIndex, FPBDIslandSolverData& SolverData)
	{
		SolverData.GetConstraintIndices(ContainerId).Add(ConstraintIndex);

		ConstraintSolverBodies[ConstraintIndex] = SolverData.GetBodyContainer().FindOrAdd(ConstrainedParticles[ConstraintIndex]);

		ConstraintResults[ConstraintIndex].Reset();

		// Hard-stop Collision Manifold Generation
		{
			check(CollisionSolvers[ConstraintIndex] != nullptr);
			check(ConstraintSolverBodies[ConstraintIndex] != nullptr);

			FPBDCollisionSolver* Solver = CollisionSolvers[ConstraintIndex];
			Solver->Reset(); // clear previous manifolds

			FSolverBody* Body0 = ConstraintSolverBodies[ConstraintIndex];	// vehicle chassis			
			FSolverBody* Body1 = &StaticCollisionBodies[ConstraintIndex];	// Spoofed terrain

			const FPBDSuspensionSettings& Setting = ConstraintSettings[ConstraintIndex];

			if (Body0->IsDynamic() && Setting.Enabled)
			{
				const FVec3& T = Setting.Target;

				// \todo(chaos): we can cache the CoM-relative connector once per frame rather than recalculate per iteration
				// (we should not be accessing particle state in the solver methods, although this one actually is ok because it only uses frame constrants)
				const FGenericParticleHandle Particle = ConstrainedParticles[ConstraintIndex];
				const FVec3& SuspensionActorOffset = SuspensionLocalOffset[ConstraintIndex];
				const FVec3 SuspensionCoMOffset = Particle->RotationOfMass().UnrotateVector(SuspensionActorOffset - Particle->CenterOfMass());
				const FVec3 SuspensionCoMAxis = Particle->RotationOfMass().UnrotateVector(Setting.Axis);
			
				const FRotation3 BodyQ = Body0->CorrectedQ();
				const FVec3 BodyP = Body0->CorrectedP();
				const FVec3 WorldSpaceX = BodyQ.RotateVector(SuspensionCoMOffset) + BodyP;
				FVec3 AxisWorld = BodyQ.RotateVector(SuspensionCoMAxis);
				FReal Distance = FVec3::DotProduct(WorldSpaceX - T, AxisWorld);

				const FVec3 WorldArm = BodyQ.RotateVector(SuspensionCoMOffset);

				// The hard-stop can only apply correction perpendicular to the surface
				const FVec3& WorldContactNormal = Setting.Normal;

				const FReal HardStopDistance = Setting.MinLength - Distance;
				const FReal WorldContactDeltaNormal = FVec3::DotProduct(HardStopDistance * AxisWorld, WorldContactNormal);

				// position the spoofed terrain at the same position at body0
				FVec3 PosBody1 = T + Distance*AxisWorld;
				Body1->SetP(PosBody1);
				Body1->SetX(PosBody1);
				check(Body1->InvM() == 0);

				Solver->SetSolverBodies(Body0, Body1);
				Solver->SetNumManifoldPoints(1);
				Solver->SetFriction(0, 0, 0);

#if CHAOS_DEBUG_DRAW
				if (bChaos_Suspension_DebugDraw_Hardstop)
				{
					FVec3 Body0Center = WorldArm + BodyP;
					FVec3 Body1Center = PosBody1;
					FReal Radius = 30;
					FDebugDrawQueue::GetInstance().DrawDebugCircle(Body0Center, Radius, 60, FColor::Yellow, false, -1.0f, 0, 3, FVector(1, 0, 0), FVector(0, 1, 0), false);
					FDebugDrawQueue::GetInstance().DrawDebugCircle(Body1Center, Radius, 60, FColor::Green, false, -1.0f, 0, 3, FVector(1, 0, 0), FVector(0, 1, 0), false);
					FString TextOut = FString::Format(TEXT("{0}"), { WorldContactDeltaNormal });
					FDebugDrawQueue::GetInstance().DrawDebugString(Body0Center + FVec3(0, 50, 50), TextOut, nullptr, FColor::White, -1.f, true, 1.0f);
				}
#endif

				// inject a manifold for our suspension Hard-stop - behaves like a regular friction-less collision, prevents the vehicle chassis from ever hitting the ground
				Solver->SetManifoldPoint(
					0,										// ManifoldIndex
					FSolverReal(Dt),						// Delta Time
					FSolverReal(0.0f),						// Restitution,
					FSolverReal(0.1f),						// RestitutionVelocityThreshold,
					FSolverVec3(WorldArm),					// RelativeContactPosition0,
					FSolverVec3(0, 0, 0),					// RelativeContactPosition1,
					FSolverVec3(WorldContactNormal),		// WorldContactNormal
					FSolverVec3(0, 0, 0),					// WorldContactTangentU,
					FSolverVec3(0, 0, 0),					// WorldContactTangentV,
					FSolverReal(-WorldContactDeltaNormal),	// WorldContactDeltaNormal
					FSolverReal(0),							// WorldContactDeltaTangentU,
					FSolverReal(0)							// WorldContactDeltaTangentV
					);

			}
		}

	}

	void FPBDSuspensionConstraints::GatherInput(const FReal Dt, const int32 ConstraintIndex, const int32 Particle0Level, const int32 Particle1Level, FPBDIslandSolverData& SolverData)
	{
	}

	void FPBDSuspensionConstraints::ScatterOutput(FReal Dt, FPBDIslandSolverData& SolverData)
	{
		for (int32 ConstraintIndex : SolverData.GetConstraintIndices(ContainerId))
		{
			if ((CollisionSolvers.Num() > 0) && (CollisionSolvers[ConstraintIndex] != nullptr) && (CollisionSolvers[ConstraintIndex]->NumManifoldPoints() > 0))
			{
				const FPBDCollisionSolver* CollisionSolver = CollisionSolvers[ConstraintIndex];
				const FPBDCollisionSolverManifoldPoint& ManifoldPoint = CollisionSolver->GetManifoldPoint(0);
				ConstraintResults[ConstraintIndex].HardStopNetPushOut = ManifoldPoint.NetPushOutNormal * ManifoldPoint.WorldContactNormal;
				ConstraintResults[ConstraintIndex].HardStopNetImpulse = ManifoldPoint.NetImpulseNormal * ManifoldPoint.WorldContactNormal;
			}

			ConstraintSolverBodies[ConstraintIndex] = nullptr;
			CollisionSolvers[ConstraintIndex]->SetSolverBodies(nullptr, nullptr);
		}
	}

	bool FPBDSuspensionConstraints::ApplyPhase1Serial(const FReal Dt, const int32 It, const int32 NumIts, FPBDIslandSolverData& SolverData)
	{
		if (bChaos_Suspension_Hardstop_Enabled)
		{
			// Suspension Hardstop
			for (int32 ConstraintIndex : SolverData.GetConstraintIndices(ContainerId))
			{
				const FPBDSuspensionSettings& Setting = ConstraintSettings[ConstraintIndex];
				if (Setting.Enabled)
				{
					FPBDCollisionSolver* CollisionSolver = CollisionSolvers[ConstraintIndex];
					if ((CollisionSolver != nullptr) && (CollisionSolver->NumManifoldPoints() > 0))
					{
						FReal MaxPushoutValue = FMath::Min(Chaos_Suspension_MaxPushout, Chaos_Suspension_MaxPushoutVelocity * Dt);
						CollisionSolver->SolvePositionNoFriction(FSolverReal(Dt), FSolverReal(MaxPushoutValue));
					}
				}
			}
		}

		if (bChaos_Suspension_Spring_Enabled)
		{
			// Suspension Spring
			for (int32 ConstraintIndex : SolverData.GetConstraintIndices(ContainerId))
			{
				ApplySingle(Dt, ConstraintIndex);
			}
		}

		// @todo(chaos): early iteration termination in FPBDSuspensionConstraints
		return true;
	}

	bool FPBDSuspensionConstraints::ApplyPhase2Serial(const FReal Dt, const int32 It, const int32 NumIts, FPBDIslandSolverData& SolverData)
	{
		if (bChaos_Suspension_Hardstop_Enabled && bChaos_Suspension_VelocitySolve)
		{
			// Suspension Hardstop
			for (int32 ConstraintIndex : SolverData.GetConstraintIndices(ContainerId))
			{
				const FPBDSuspensionSettings& Setting = ConstraintSettings[ConstraintIndex];
				if (Setting.Enabled)
				{
					FPBDCollisionSolver* CollisionSolver = CollisionSolvers[ConstraintIndex];
					if ((CollisionSolver != nullptr) && (CollisionSolver->NumManifoldPoints() > 0))
					{
						CollisionSolver->SolveVelocity(FSolverReal(Dt), false);
					}
				}
			}
		}

		return true;
	}

	void FPBDSuspensionConstraints::ApplySingle(const FReal Dt, int32 ConstraintIndex)
	{
		check(ConstraintSolverBodies[ConstraintIndex] != nullptr);
		FSolverBody& Body = *ConstraintSolverBodies[ConstraintIndex];
		const FPBDSuspensionSettings& Setting = ConstraintSettings[ConstraintIndex];
		FPBDSuspensionResults& Results = ConstraintResults[ConstraintIndex];

		if (Body.IsDynamic() && Setting.Enabled)
		{
			const FVec3& T = Setting.Target;

			// \todo(chaos): we can cache the CoM-relative connector once per frame rather than recalculate per iteration
			// (we should not be accessing particle state in the solver methods, although this one actually is ok because it only uses frame constrants)
			const FGenericParticleHandle Particle = ConstrainedParticles[ConstraintIndex];
			const FVec3& SuspensionActorOffset = SuspensionLocalOffset[ConstraintIndex];
			const FVec3 SuspensionCoMOffset = Particle->RotationOfMass().UnrotateVector(SuspensionActorOffset - Particle->CenterOfMass());
			const FVec3 SuspensionCoMAxis = Particle->RotationOfMass().UnrotateVector(Setting.Axis);

			// @todo(chaos): use linearized error calculation
			const FRotation3 BodyQ = Body.CorrectedQ();
			const FVec3 BodyP = Body.CorrectedP();
			const FVec3 WorldSpaceX = BodyQ.RotateVector(SuspensionCoMOffset) + BodyP;

			FVec3 AxisWorld = BodyQ.RotateVector(SuspensionCoMAxis);

			const FVec3& SurfaceNormal = Setting.Normal;

			const float MPHToCmS = 100000.f / 2236.94185f;
			const float SpeedThreshold = 10.0f * MPHToCmS;
			const float FortyFiveDegreesThreshold = 0.707f;

			// @todo(chaos): do we need this?
			//if (AxisWorld.Z > FortyFiveDegreesThreshold)
			//{
			//	if (Body.V().SquaredLength() < 1.0f)
			//	{
			//		AxisWorld = FVec3(0.f, 0.f, 1.f);
			//	}
			//	else
			//	{
			//		const FReal Speed = FMath::Abs(Body.V().Length());
			//		if (Speed < SpeedThreshold)
			//		{
			//			AxisWorld = FMath::Lerp(FVec3(0.f, 0.f, 1.f), AxisWorld, Speed / SpeedThreshold);
			//		}
			//	}
			//}

			FReal Distance = FVec3::DotProduct(WorldSpaceX - T, AxisWorld);
			if (Distance >= Setting.MaxLength)
			{
				// do nothing since the target point is further than the longest extension of the suspension spring
				Results.Length = Setting.MaxLength;
				return;
			}

			FVec3 DX = FVec3::ZeroVector;

			// Require the velocity at the WorldSpaceX position - not the velocity of the particle origin
			// NOTE: We are in the position solve phase and velocty is not updated. We must use the implicit 
			// velocity in the damping calculation
			// @todo(chaos): consider moving the damping term to the velocity solve phase
			const FVec3 Diff = WorldSpaceX - BodyP;
			const FVec3 V = FVec3::CalculateVelocity(Body.X(), BodyP, Dt);
			const FVec3 W = FRotation3::CalculateAngularVelocity(Body.R(), BodyQ, Dt);
			FVec3 ArmVelocity = V - FVec3::CrossProduct(Diff, W);


			if (Distance < Setting.MinLength)
			{
				// target point distance is less at min compression limit 
				// - apply distance constraint to try keep a valid min limit
				//FVec3 Ts = WorldSpaceX + AxisWorld * (Setting.MinLength - Distance);
				//DX = (Ts - WorldSpaceX) * Setting.HardstopStiffness;

				Distance = Setting.MinLength;

				//if (PointVelocityAlongAxis < 0.0f)
				//{
				//	const FVec3 SpringVelocity = PointVelocityAlongAxis * AxisWorld;
				//	DX -= SpringVelocity * Setting.HardstopVelocityCompensation;
				//	PointVelocityAlongAxis = 0.0f; //this Dx will cancel velocity, so don't pass PointVelocityAlongAxis on to suspension force calculation 
				//}
			}

			{
				// then the suspension force on top

				FReal DLambda = 0.f;
				{
					// #todo: Preload, better scaled spring damping like other suspension 0 -> 1 range
					const FReal AxisDotNormal = FVec3::DotProduct(AxisWorld, SurfaceNormal);
					FReal SpringCompression = AxisDotNormal * (Setting.MaxLength - Distance) /*+ Setting.SpringPreload*/;
					FReal SpringVelocity = FVec3::DotProduct(ArmVelocity, SurfaceNormal);

					// @todo(chaos): this is not using the correct effective mass
					const bool bAccelerationMode = false;
					const FReal SpringMassScale = (bAccelerationMode) ? FReal(1) / Body.InvM() : FReal(1);
					const FReal S = SpringMassScale * Setting.SpringStiffness * Dt * Dt;
					const FReal D = SpringMassScale * Setting.SpringDamping * Dt;
					
					// @todo(chaos): add missing XPBD term to get iteration-count independent behaviour
					DLambda = (S * SpringCompression - D * SpringVelocity);

					// Suspension springs cannot apply downward forces on the body
					if (DLambda < 0)
					{
						DLambda = 0;
					}

					DX += DLambda * SurfaceNormal;
				}
			}

			const FVec3 Arm = WorldSpaceX - BodyP;

			const FVec3 DP = Body.InvM() * DX;
			const FVec3 DR = Body.InvI() * FVec3::CrossProduct(Arm, DX);
			Body.ApplyTransformDelta(DP, DR);
			Body.UpdateRotationDependentState();

			Results.NetPushOut += DX;
			Results.Length = Distance;
		}
	}
	
	template class TIndexedContainerConstraintHandle<FPBDSuspensionConstraints>;


}
