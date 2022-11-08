// Copyright Epic Games, Inc. All Rights Reserved.

#include "VirtualizationFileBackend.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "VirtualizationUtilities.h"

namespace UE::Virtualization
{

FFileSystemBackend::FFileSystemBackend(FStringView ConfigName, FStringView DebugName)
	: IVirtualizationBackend(ConfigName, DebugName, EOperations::Both)
{
}

bool FFileSystemBackend::Initialize(const FString& ConfigEntry)
{
	if (!FParse::Value(*ConfigEntry, TEXT("Path="), RootDirectory))
	{
		UE_LOG(LogVirtualization, Error, TEXT("[%s] 'Path=' not found in the config file"), *GetDebugName());
		return false;
	}

	FPaths::NormalizeDirectoryName(RootDirectory);

	if (RootDirectory.IsEmpty())
	{
		UE_LOG(LogVirtualization, Error, TEXT("[%s] Config file entry 'Path=' was empty"), *GetDebugName());
		return false;
	}

	// TODO: Validate that the given path is usable?

	int32 RetryCountIniFile = INDEX_NONE;
	if (FParse::Value(*ConfigEntry, TEXT("RetryCount="), RetryCountIniFile))
	{
		RetryCount = RetryCountIniFile;
	}

	int32 RetryWaitTimeMSIniFile = INDEX_NONE;
	if (FParse::Value(*ConfigEntry, TEXT("RetryWaitTime="), RetryWaitTimeMSIniFile))
	{
		RetryWaitTimeMS = RetryWaitTimeMSIniFile;
	}

	// Now log a summary of the backend settings to make issues easier to diagnose
	UE_LOG(LogVirtualization, Log, TEXT("[%s] Using path: '%s'"), *GetDebugName(), *RootDirectory);
	UE_LOG(LogVirtualization, Log, TEXT("[%s] Will retry failed read attempts %d times with a gap of %dms betwen them"), *GetDebugName(), RetryCount, RetryWaitTimeMS);

	return true;
}

EPushResult FFileSystemBackend::PushData(const FIoHash& Id, const FCompressedBuffer& Payload, const FString& PackageContext)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFileSystemBackend::PushData);

	if (DoesPayloadExist(Id))
	{
		UE_LOG(LogVirtualization, Verbose, TEXT("[%s] Already has a copy of the payload '%s'."), *GetDebugName(), *LexToString(Id));
		return EPushResult::PayloadAlreadyExisted;
	}

	// Make sure to log any disk write failures to the user, even if this backend will often be optional as they are
	// not expected and could indicate bigger problems.
	// 
	// First we will write out the payload to a temp file, after which we will move it to the correct storage location
	// this helps reduce the chance of leaving corrupted data on disk in the case of a power failure etc.
	const FString TempFilePath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("miragepayload"));
	
	TUniquePtr<FArchive> FileAr(IFileManager::Get().CreateFileWriter(*TempFilePath));

	if (FileAr == nullptr)
	{
		TStringBuilder<MAX_SPRINTF> SystemErrorMsg;
		Utils::GetFormattedSystemError(SystemErrorMsg);

		UE_LOG(LogVirtualization, Error, TEXT("[%s] Failed to write payload '%s' to '%s' due to system error: %s"), 
			*GetDebugName(),
			*LexToString(Id),
			*TempFilePath,
			SystemErrorMsg.ToString());

		return EPushResult::Failed;
	}

	for (const FSharedBuffer& Buffer : Payload.GetCompressed().GetSegments())
	{
		// Const cast because FArchive requires a non-const pointer!
		FileAr->Serialize(const_cast<void*>(Buffer.GetData()), static_cast<int64>(Buffer.GetSize()));
	}

	if (!FileAr->Close())
	{
		TStringBuilder<MAX_SPRINTF> SystemErrorMsg;
		Utils::GetFormattedSystemError(SystemErrorMsg);

		UE_LOG(LogVirtualization, Error, TEXT("[%s] Failed to write payload '%s' contents to '%s' due to system error: %s"),
			*GetDebugName(),
			*LexToString(Id),
			*TempFilePath,
			SystemErrorMsg.ToString());

		IFileManager::Get().Delete(*TempFilePath, true, false, true);  // Clean up the temp file if it is still around but do not failure cases to the user
		
		return EPushResult::Failed;
	}

	TStringBuilder<512> FilePath;
	CreateFilePath(Id, FilePath);

	// If the file already exists we don't need to replace it, we will also do our own error logging.
	if (!IFileManager::Get().Move(FilePath.ToString(), *TempFilePath, /*Replace*/ false, /*EvenIfReadOnly*/ false, /*Attributes*/ false, /*bDoNotRetryOrError*/ true))
	{
		// Store the error message in case we need to display it
		TStringBuilder<MAX_SPRINTF> SystemErrorMsg;
		Utils::GetFormattedSystemError(SystemErrorMsg);

		IFileManager::Get().Delete(*TempFilePath, true, false, true); // Clean up the temp file if it is still around but do not failure cases to the user

		// Check if another thread or process was writing out the payload at the same time, if so we 
		// don't need to give an error message.
		if (DoesPayloadExist(Id))
		{
			UE_LOG(LogVirtualization, Verbose, TEXT("[%s] Already has a copy of the payload '%s'."), *GetDebugName(), *LexToString(Id));
			return EPushResult::PayloadAlreadyExisted;
		}
		else
		{
			UE_LOG(	LogVirtualization, Error, TEXT("[%s] Failed to move payload '%s' to it's final location '%s' due to system error: %s"),
					*GetDebugName(),
					*LexToString(Id),
					*FilePath,
					SystemErrorMsg.ToString());

			return EPushResult::Failed;
		}
	}

	return EPushResult::Success;
}

FCompressedBuffer FFileSystemBackend::PullData(const FIoHash& Id)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFileSystemBackend::PullData);

	TStringBuilder<512> FilePath;
	CreateFilePath(Id, FilePath);

	// TODO: Should we allow the error severity to be configured via ini or just not report this case at all?
	if (!IFileManager::Get().FileExists(FilePath.ToString()))
	{
		UE_LOG(LogVirtualization, Verbose, TEXT("[%s] Does not contain the payload '%s'"), *GetDebugName(), *LexToString(Id));
		return FCompressedBuffer();
	}

	TUniquePtr<FArchive> FileAr = OpenFileForReading(FilePath.ToString());

	if (FileAr == nullptr)
	{
		TStringBuilder<MAX_SPRINTF> SystemErrorMsg;
		Utils::GetFormattedSystemError(SystemErrorMsg);

		UE_LOG(LogVirtualization, Error, TEXT("[%s] Failed to load payload '%s' from file '%s' due to system error: %s"),
			*GetDebugName(),
			*LexToString(Id),
			FilePath.ToString(),
			SystemErrorMsg.ToString());

		return FCompressedBuffer();
	}

	return FCompressedBuffer::Load(*FileAr);
}

bool FFileSystemBackend::DoesPayloadExist(const FIoHash& Id)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFileSystemBackend::DoesPayloadExist);

	TStringBuilder<512> FilePath;
	CreateFilePath(Id, FilePath);

	return IFileManager::Get().FileExists(FilePath.ToString());
}

void FFileSystemBackend::CreateFilePath(const FIoHash& PayloadId, FStringBuilderBase& OutPath)
{
	TStringBuilder<52> PayloadPath;
	Utils::PayloadIdToPath(PayloadId, PayloadPath);

	OutPath << RootDirectory << TEXT("/") << PayloadPath;
}

TUniquePtr<FArchive> FFileSystemBackend::OpenFileForReading(const TCHAR* FilePath)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFileSystemBackend::OpenFileForReading);

	int32 Retries = 0;

	while (Retries < RetryCount)
	{
		TUniquePtr<FArchive> FileAr(IFileManager::Get().CreateFileReader(FilePath));
		if (FileAr)
		{
			return FileAr;
		}
		else
		{
			UE_LOG(LogVirtualization, Warning, TEXT("[%s] Failed to open '%s' for reading attempt retrying (%d/%d) in %dms..."), *GetDebugName(), FilePath, Retries, RetryCount, RetryWaitTimeMS);
			FPlatformProcess::SleepNoStats(RetryWaitTimeMS * 0.001f);

			Retries++;
		}
	}

	return nullptr;
}

UE_REGISTER_VIRTUALIZATION_BACKEND_FACTORY(FFileSystemBackend, FileSystem);

} // namespace UE::Virtualization
