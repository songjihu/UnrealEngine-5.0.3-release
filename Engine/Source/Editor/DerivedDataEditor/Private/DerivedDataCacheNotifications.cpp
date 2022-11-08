// Copyright Epic Games, Inc. All Rights Reserved.

#include "DerivedDataCacheNotifications.h"
#include "Misc/EngineBuildSettings.h"
#include "Logging/LogMacros.h"
#include "Misc/CoreMisc.h"
#include "Misc/App.h"
#include "Internationalization/Internationalization.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Input/SHyperlink.h"
#include "Framework/Notifications/NotificationManager.h"
#include "DerivedDataCacheInterface.h"
#include "Editor/EditorPerformanceSettings.h"

#define LOCTEXT_NAMESPACE "DerivedDataCacheNotifications"

DEFINE_LOG_CATEGORY_STATIC(DerivedDataCacheNotifications, Log, All);

FDerivedDataCacheNotifications::FDerivedDataCacheNotifications() :
	bSubscribed(false)
{
	Subscribe(true);
}

FDerivedDataCacheNotifications::~FDerivedDataCacheNotifications()
{
	Subscribe(false);
}

void FDerivedDataCacheNotifications::OnDDCNotificationEvent(FDerivedDataCacheInterface::EDDCNotification DDCNotification)
{
	if (IsEngineExitRequested())
	{
		return;
	}

	// TODO : Handle any notificaiton evens here

}

void FDerivedDataCacheNotifications::Subscribe(bool bSubscribe)
{
	if (bSubscribe != bSubscribed)
	{
		FDerivedDataCacheInterface::FOnDDCNotification& DDCNotificationEvent = GetDerivedDataCacheRef().GetDDCNotificationEvent();

		if (bSubscribe)
		{
			DDCNotificationEvent.AddRaw(this, &FDerivedDataCacheNotifications::OnDDCNotificationEvent);
		}
		else
		{
			DDCNotificationEvent.RemoveAll(this);
		}

		bSubscribed = bSubscribe;
	}
}

#undef LOCTEXT_NAMESPACE
