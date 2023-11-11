#pragma once

#include "LipSyncInterface.h"
#include "OVRLipSyncLiveActorComponent.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Components/ActorComponent.h"
#include "Containers/Map.h"
#include "Containers/Queue.h"
#include "HAL/RunnableThread.h"
#include "HAL/Runnable.h"

#include "OVRLipSyncConvaiPlaybackActorComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiOVRLipSyncLog, Log, All);

class UOVRLipSyncFrameSequence;

UCLASS(meta = (BlueprintSpawnableComponent), DisplayName = "Convai OVR LipSync")
class UConvaiOVRLipSyncComponent : public UOVRLipSyncActorComponentBase, public IConvaiLipSyncInterface
{
	GENERATED_BODY()
public:
	UConvaiOVRLipSyncComponent();

	virtual ~UConvaiOVRLipSyncComponent();


	// UActorComponent interface
	//virtual void BeginPlay() override;
	// virtual void OnRegister() override;
	// virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType,
							   FActorComponentTickFunction *ThisTickFunction) override;
	// End UActorComponent interface

	// IConvaiLipSyncInterface
	virtual void ConvaiProcessLipSync(uint8 *PCMData, uint32 PCMDataSize, uint32 SampleRate,
									  uint32 NumChannels) override;

	virtual void ConvaiStopLipSync() override;

	virtual TArray<float> ConvaiGetVisemes() override;

	virtual TArray<FString> ConvaiGetVisemeNames() override;

	void AddSequence(UOVRLipSyncFrameSequence* Sequence, float Duration)
	{ 
		Sequences.Add(Sequence);
		SequenceDurations.Add(Duration);
	}
	void RemoveSequence()
	{ 
		Sequences.RemoveAt(0);
		SequenceDurations.RemoveAt(0);
	}
	
	UPROPERTY()
	TArray<class UOVRLipSyncFrameSequence*> Sequences;
	UPROPERTY()
	TArray<float> SequenceDurations;

	float CurrentSequenceTimePassed;
	bool IsNeutralPose;

	class FConvaiLipSyncWorker *ConvaiLipSyncWorker = nullptr;

	FCriticalSection PCMDataCriticalSection;
	TQueue<TPair<TArray<uint8>, uint32>> PCMDataChunkPairs;
};

class FConvaiLipSyncWorker : public FRunnable
{
public:
	FConvaiLipSyncWorker(TQueue<TPair<TArray<uint8>, uint32>> *InPCMDataChunkPairs, FCriticalSection *InCriticalSection,
						 uint32 InNumChannels, UConvaiOVRLipSyncComponent *InComponent)
		: PCMDataChunkPairs(InPCMDataChunkPairs), CriticalSection(InCriticalSection), NumChannels(InNumChannels),
		  Component(InComponent), bStopRequested(false)
	{
		Thread = FRunnableThread::Create(this, TEXT("ConvaiLipSyncWorker"), 0, TPri_BelowNormal);
	}

	//...

	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	TQueue<TPair<TArray<uint8>, uint32>> *PCMDataChunkPairs;
	FCriticalSection *CriticalSection;
	uint32 NumChannels;
	UConvaiOVRLipSyncComponent *Component;
	FRunnableThread *Thread;
	TAtomic<bool> bStopRequested;
};