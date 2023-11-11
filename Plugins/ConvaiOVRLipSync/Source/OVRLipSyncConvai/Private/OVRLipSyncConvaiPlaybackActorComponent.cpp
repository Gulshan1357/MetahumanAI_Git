#include "OVRLipSyncConvaiPlaybackActorComponent.h"
#include "OVRLipSyncContextWrapper.h"
#include "OVRLipSyncFrame.h"
#include "Sound/SoundWave.h"
#include "UObject/GarbageCollection.h"


DEFINE_LOG_CATEGORY(ConvaiOVRLipSyncLog);

namespace
{
	UOVRLipSyncFrameSequence *CookAudioData(uint8 *PCMData, int PCMDataSize, int SampleRate, int NumChannels)
		{

			const int16 *PCMDataInt16 = reinterpret_cast<int16*>(PCMData);
			PCMDataSize = PCMDataSize/2;

			// Compute LipSync sequence frames at 100 times a second rate
			constexpr auto LipSyncSequenceUpateFrequency = 100;
			constexpr auto LipSyncSequenceDuration = 1.0f / LipSyncSequenceUpateFrequency;

			auto Sequence = NewObject<UOVRLipSyncFrameSequence>();
			auto ChunkSizeSamples = static_cast<int>(SampleRate * LipSyncSequenceDuration);
			auto ChunkSize = NumChannels * ChunkSizeSamples;


			UOVRLipSyncContextWrapper context(ovrLipSyncContextProvider_Enhanced, SampleRate, 4096, FString());

			float LaughterScore = 0.0f;
			int32_t FrameDelayInMs = 0;
			TArray<float> Visemes;

			TArray<int16_t> samples;
			samples.SetNumZeroed(ChunkSize);
			context.ProcessFrame(samples.GetData(), ChunkSizeSamples, Visemes, LaughterScore, FrameDelayInMs, NumChannels > 1);
			int FrameOffset = (int)(FrameDelayInMs * SampleRate / 1000 * NumChannels);

			for (int offs = 0; offs < PCMDataSize + FrameOffset; offs += ChunkSize)
			{
				int remainingSamples = PCMDataSize - offs;
				if (remainingSamples >= ChunkSize)
				{
					context.ProcessFrame(PCMDataInt16 + offs, ChunkSizeSamples, Visemes, LaughterScore, FrameDelayInMs,
										 NumChannels > 1);
				}
				else
				{
					if (remainingSamples > 0)
					{
						memcpy(samples.GetData(), PCMDataInt16 + offs, sizeof(int16_t) * remainingSamples);
						memset(samples.GetData() + remainingSamples, 0, ChunkSize - remainingSamples);
					}
					else
					{
						memset(samples.GetData(), 0, ChunkSize);
					}
					context.ProcessFrame(samples.GetData(), ChunkSizeSamples, Visemes, LaughterScore, FrameDelayInMs,
										 NumChannels > 1);
				}

				if (offs >= FrameOffset)
				{
					Sequence->Add(Visemes, LaughterScore);
				}

			}
			return Sequence;
		}

	uint32 CalculateSampleRate(USoundWave *SoundWave)
	{
		if (SoundWave == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("CalculateSampleRate: Invalid SoundWave pointer."));
			return -1;
		}

		// USoundWave stores its data in 16-bit PCM format
		const int32 BitDepth = 16;
		const int32 Channels = SoundWave->NumChannels;

		// Get raw PCM data
		// TArray<uint8> RawData;
		// SoundWave->RawData.Lock(LOCK_READ_WRITE);
		// const uint8 *Buffer = (uint8 *)SoundWave->RawData.Realloc(SoundWave->RawData.GetBulkDataSize());
		// RawData.Append(Buffer, SoundWave->RawData.GetBulkDataSize());
		// SoundWave->RawData.Unlock();

		// Total data size in bytes
		const int32 TotalDataSize = SoundWave->RawPCMDataSize;

		// Duration in seconds
		const float Duration = SoundWave->Duration;

		if (Duration == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("CalculateSampleRate: SoundWave duration is zero."));
			return -1;
		}

		// Sample rate calculation
		int32 SampleRate = TotalDataSize / ((BitDepth / 8) * Channels * Duration);

		return SampleRate;
	}

};

UConvaiOVRLipSyncComponent::UConvaiOVRLipSyncComponent() 
{ 
	PrimaryComponentTick.bCanEverTick = true; 
	CurrentSequenceTimePassed = 0;
	IsNeutralPose = true;
}

UConvaiOVRLipSyncComponent::~UConvaiOVRLipSyncComponent() // Implement destructor
{
	if (ConvaiLipSyncWorker != nullptr)
	{
			ConvaiLipSyncWorker->Stop();
			delete ConvaiLipSyncWorker;
			ConvaiLipSyncWorker = nullptr;
	}
}

void UConvaiOVRLipSyncComponent::TickComponent(float DeltaTime, ELevelTick TickType,
											   FActorComponentTickFunction *ThisTickFunction)
{
	if (Sequences.Num())
	{
		CurrentSequenceTimePassed += DeltaTime;
		//UE_LOG(ConvaiOVRLipSyncLog, Warning, TEXT("CurrentSequenceTimePassed: %f"), CurrentSequenceTimePassed);

		auto PlayPos = CurrentSequenceTimePassed;
		auto IntPos = static_cast<unsigned>(PlayPos * 100);
		if (IntPos >= Sequences[0]->Num())
		{
			//UE_LOG(ConvaiOVRLipSyncLog, Warning, TEXT("Sequence Done at %f"), CurrentSequenceTimePassed);
			CurrentSequenceTimePassed = CurrentSequenceTimePassed - (float) Sequences[0]->Num() / 100.0;
			RemoveSequence();
			InitNeutralPose();
			IsNeutralPose = true;
			OnVisemesDataReady.ExecuteIfBound(); // Related to Convai LipSync Interface
			return;
		}
		const auto &Frame = (*Sequences[0])[IntPos];
		LaughterScore = Frame.LaughterScore;
		Visemes = Frame.VisemeScores;
		OnVisemesReady.Broadcast();
		OnVisemesDataReady.ExecuteIfBound(); // Related to Convai LipSync Interface
	}
	else
	{
		CurrentSequenceTimePassed = 0;
	}
}

void UConvaiOVRLipSyncComponent::ConvaiProcessLipSync(uint8 *InPCMData, uint32 InPCMDataSize, uint32 InSampleRate,
													  uint32 InNumChannels)
{
	const uint32 ChunkSize = InSampleRate * InNumChannels * 2 * 0.5;

	PCMDataCriticalSection.Lock();
	for (uint32 i = 0; i < InPCMDataSize; i += ChunkSize)
	{
		TArray<uint8> Chunk;
		uint32 ThisChunkSize = FMath::Min(ChunkSize, InPCMDataSize - i);
		Chunk.Append(InPCMData + i, ThisChunkSize);
		PCMDataChunkPairs.Enqueue(TPair<TArray<uint8>, uint32>(Chunk, InSampleRate));

	}
	PCMDataCriticalSection.Unlock();

	if (ConvaiLipSyncWorker == nullptr)
	{
		ConvaiLipSyncWorker =
			new FConvaiLipSyncWorker(&PCMDataChunkPairs, &PCMDataCriticalSection, InNumChannels, this);
	}
}

void UConvaiOVRLipSyncComponent::ConvaiStopLipSync()
{ 
	Sequences.Empty();
	InitNeutralPose();
	IsNeutralPose = true;
	CurrentSequenceTimePassed = 0;
}

TArray<float> UConvaiOVRLipSyncComponent::ConvaiGetVisemes()
{ 
	return GetVisemes();
}

TArray<FString> UConvaiOVRLipSyncComponent::ConvaiGetVisemeNames()
{
	return GetVisemeNames();
}

uint32 FConvaiLipSyncWorker::Run()
{
	while (!bStopRequested)
	{
		if (IsGarbageCollecting())
		{
			FPlatformProcess::Sleep(0.01);
			continue;
		}

		CriticalSection->Lock();
		if (PCMDataChunkPairs->IsEmpty())
		{
			CriticalSection->Unlock();
			break;
		}
		TPair<TArray<uint8>, uint32> ChunkPair;
		bool GotChunk = PCMDataChunkPairs->Dequeue(ChunkPair);
		CriticalSection->Unlock();

		if (IsValid(Component)) // Check if Component is valid
		{
			if (GotChunk)
			{
				TArray<uint8> Chunk = ChunkPair.Key;
				uint32 SampleRate = ChunkPair.Value;
				float SequenceDuration =
					float(Chunk.Num()) / float(SampleRate * 2); // Calculate the sequence duration
				UOVRLipSyncFrameSequence *Sequence =
					CookAudioData(Chunk.GetData(), Chunk.Num(), SampleRate, NumChannels);
				Component->AddSequence(Sequence, SequenceDuration);
			}
		}
		else
		{
			bStopRequested = true;
		}

		if (PCMDataChunkPairs->IsEmpty())
			FPlatformProcess::Sleep(0.01); // Avoids spinning CPU too much
	}
	return 0;
}

void FConvaiLipSyncWorker::Stop()
{
	bStopRequested = true;
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
	}
}

void FConvaiLipSyncWorker::Exit()
{
	bStopRequested = true;
	if (IsValid(Component)) // Check if Component is valid
	{
		Component->ConvaiLipSyncWorker = nullptr;
	}
}