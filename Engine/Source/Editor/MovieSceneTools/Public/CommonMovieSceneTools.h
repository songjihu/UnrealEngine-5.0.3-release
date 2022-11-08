// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "FrameNumberNumericInterface.h"
#include "Layout/Geometry.h"
#include "Misc/FrameRate.h"

class FSequencerSectionPainter;

/**
 * Utility for converting time units to slate pixel units and vice versa
 */
struct FTimeToPixel
{
public:
	FTimeToPixel( const FGeometry& AllottedGeometry, const TRange<double>& InLocalViewRange, const FFrameRate& InTickResolution )
		: ViewRangeStartSeconds( InLocalViewRange.GetLowerBoundValue() )
		, TickResolution( InTickResolution )
	{
		double VisibleWidth = InLocalViewRange.Size<double>();

		const float MaxPixelsPerSecond = 1000.f;
		PixelsPerSecond = VisibleWidth > 0 ? AllottedGeometry.GetLocalSize().X / VisibleWidth : MaxPixelsPerSecond;
	}

	/**
	 * Converts a time to a pixel point relative to the geometry of a widget (passed into the constructor)
	 *
	 * @param Time	The time to convert
	 * @return The pixel equivalent of the time
	 */
	float SecondsToPixel( double Time ) const
	{
		return (Time - ViewRangeStartSeconds) * PixelsPerSecond;
	}

	/**
	 * Converts a time delta to a pixel delta
	 *
	 * @param TimeDelta  The time delta to convert
	 * @return           The pixel equivalent of the delta time
	 */
	float SecondsDeltaToPixel( double TimeDelta ) const
	{
		return TimeDelta * PixelsPerSecond;
	}

	/**
	 * Converts a pixel value to time 
	 *
	 * @param PixelX The x value of a pixel coordinate relative to the geometry that was passed into the constructor.
	 * @return The time where the pixel is located
	 */
	double PixelToSeconds( float PixelX ) const
	{
		return (PixelX/PixelsPerSecond) + ViewRangeStartSeconds;
	}

	/**
	 * Converts a frame time to a pixel point relative to the geometry of a widget (passed into the constructor)
	 *
	 * @param Time The time to convert
	 * @return The pixel equivalent of the frame time
	 */
	float FrameToPixel( const FFrameTime& Time ) const
	{
		return (Time / TickResolution - ViewRangeStartSeconds) * PixelsPerSecond;
	}

	/**
	 * Converts a frame delta value to pixel delta
	 *
	 * @param TimeDelta   The time delta to convert
	 * @return            The pixel equivalent of the delta time
	 */
	float FrameDeltaToPixel( const FFrameTime& TimeDelta) const
	{
		return (TimeDelta / TickResolution) * PixelsPerSecond;
	}

	/**
	 * Converts a pixel value to frame time 
	 *
	 * @param PixelX The x value of a pixel coordinate relative to the geometry that was passed into the constructor.
	 * @return The frame time where the pixel is located
	 */
	FFrameTime PixelToFrame( float PixelX ) const
	{
		return ( PixelX / PixelsPerSecond + ViewRangeStartSeconds ) * TickResolution;
	}

	/**
	 * Converts a pixel delta value to delta frame time 
	 *
	 * @param PixelDelta The delta value in pixel space
	 * @return The equivalent delta frame time
	 */
	FFrameTime PixelDeltaToFrame( float PixelDelta ) const
	{
		return ( PixelDelta / PixelsPerSecond ) * TickResolution;
	}

	/**
	 * Retrieve the tick resolution of the current sequence
	 */
	FFrameRate GetTickResolution() const
	{
		return TickResolution;
	}

private:

	/** time range of the sequencer in seconds */
	double ViewRangeStartSeconds;
	/** The tick resolution of the current timeline */
	FFrameRate TickResolution;
	/** The number of pixels in the view range */
	float PixelsPerSecond;
};


/**
 * Draw a frame time next to the scrub handle
 *
 * @param InPainter Structure that affords common painting operations
 * @param CurrentTime Current time of the scrub handle
 * @param FrameTime Frame time to draw
 * @param FrameNumberInterface (optional) Interface to control the display format and/or frame rate conversion of the drawn frame time.
 *            If not provided, the frame time will be drawn as a frame number without any subframe.
 */
void DrawFrameTimeHint(FSequencerSectionPainter& InPainter, const FFrameTime& CurrentTime, const FFrameTime& FrameTime, const FFrameNumberInterface* FrameNumberInterface = nullptr);
