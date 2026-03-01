[WorkbenchToolAttribute(name: "Auto Camera Screenshot", description: "Automatically create screenshots of an area of a map.", wbModules: {"WorldEditor"}, awesomeFontCode: 0xf447)]
class AutoCameraScreenshotWorldEditorTool: BaseMapMakerTool
{
	// Name: Auto Camera Screenshot Tool
	// Author: Bewilderbeest <bewilder@recoil.org>
	// Refactored for TacOps by Patrick Probst

	// This is a WORLD EDITOR tool, so open up your required map first.

	// The camera will start at m_StartCoords and step by m_StepSize
	// in each axis until it reaches m_EndCoords, generating
	// screenshots into your $profile directory.

	// After starting capture, hit F11 for fullscreen to ensure
	// consistent screenshot sizes.

	// Camera looks straight down
	static const vector CAMERA_LOOK_DOWN = Vector(0, -90, 0);
	
	//------------------------------------------------------------
	// Camera settings
	//------------------------------------------------------------

	[Attribute("200 0 200", UIWidgets.Coords, "Camera start position", "", null, "Camera")]
	vector m_StartCoords;
	
	[Attribute("12800 0 12800", UIWidgets.Coords, "Camera end position", "", null, "Camera")]
	vector m_EndCoords;
	
	[Attribute("950", UIWidgets.Auto, "Camera height", "", null, "Camera")]
	int m_CameraHeight;
	
	[Attribute("0", UIWidgets.CheckBox, "Camera height is absolute, not relative to terrain height", "", null, "Camera")]
	bool m_AbsoluteCameraHeight;

	[Attribute("100", UIWidgets.Auto, "Camera step size (must be > 0)", "", null, "Camera")]
	int m_StepSize;

	[Attribute("15", UIWidgets.Auto, "Camera FOV (in ortho mode this controls vertical extent in world units)", "", null, "Camera")]
	float m_FieldOfView;

	[Attribute("0", UIWidgets.CheckBox, "Use orthographic projection (eliminates perspective distortion for perfect tile stitching)", "", null, "Camera")]
	bool m_UseOrthographic;

	//------------------------------------------------------------
	// Timing
	//------------------------------------------------------------

	[Attribute("700", UIWidgets.Auto, "Sleep after incremental camera movement (ms)", "", null, "Timing")]
	float m_MoveSleep;

	[Attribute("2000", UIWidgets.Auto, "Sleep after a large / discontinuous camera movement (ms)", "", null, "Timing")]
	float m_DiscontinuousMoveSleep;

	[Attribute("200", UIWidgets.Auto, "Sleep after screenshot call (ms)", "", null, "Timing")]
	float m_ScreenshotSleep;
	
	//------------------------------------------------------------
	// Screenshot output
	//------------------------------------------------------------

	[Attribute("mapoutput", UIWidgets.Auto, "Output directory name", "", null, "Screenshot output")]
	string m_OutputDirectory;
	
	[Attribute("eden", UIWidgets.Auto, "Output filename prefix", "", null, "Screenshot output")]
	string m_OutputFilePrefix;

	//------------------------------------------------------------
	// Batch mode (parallel processing)
	//------------------------------------------------------------

	[Attribute("0", UIWidgets.CheckBox, "Enable batch mode: pre-streams multiple tiles simultaneously for faster capture", "", null, "Batch Mode")]
	bool m_EnableBatchMode;

	[Attribute("31", UIWidgets.Slider, "Number of tiles to pre-stream per batch (1-31)\nHigher = faster but more GPU/memory load", "1 31 1", null, "Batch Mode")]
	int m_BatchSize;

	[Attribute("500", UIWidgets.Auto, "Preload radius for SchedulePreload (meters)\nControls how much terrain/LOD is pre-streamed around each tile", "", null, "Batch Mode")]
	float m_PreloadRadius;

	[Attribute("100", UIWidgets.Auto, "Sleep between camera switches within a batch (ms)\nLower = faster but shadows may not settle", "", null, "Batch Mode")]
	float m_CycleMoveSleep;

	//------------------------------------------------------------
	// Advanced
	//------------------------------------------------------------

	[Attribute("_tile.png", UIWidgets.Auto, "Tile filename suffix", "", null, "Advanced")]
	string m_TileFilenameSuffix;

	[Attribute("0.025", UIWidgets.Auto, "Custom HDR brightness override (-1 to reset)", "", null, "Advanced")]
	float m_HdrBrightness;
	
	//------------------------------------------------------------
	// Buttons
	//------------------------------------------------------------

	[ButtonAttribute("Auto-detect Bounds")]
	void AutoDetectBounds()
	{
		WorldEditorAPI api = GetApi();
		if (!api)
		{
			Print("ERROR: WorldEditorAPI not available");
			return;
		}

		float unitScale = api.GetTerrainUnitScale();
		int resolutionX = api.GetTerrainResolutionX();

		if (unitScale <= 0 || resolutionX <= 0)
		{
			PrintFormat("ERROR: Invalid terrain data (unitScale=%1, resolutionX=%2)", unitScale, resolutionX);
			return;
		}

		float terrainSize = unitScale * resolutionX;

		// Inset by one step to avoid edge artifacts
		int inset = m_StepSize;
		if (inset <= 0)
			inset = 100;

		m_StartCoords = Vector(inset, 0, inset);
		m_EndCoords = Vector(terrainSize - inset, 0, terrainSize - inset);

		PrintFormat("=== Auto-detected Bounds ===");
		PrintFormat("  Terrain size: %1m x %1m (scale=%2m, resolution=%3)", terrainSize, unitScale, resolutionX);
		PrintFormat("  Start: %1, %2", m_StartCoords[0], m_StartCoords[2]);
		PrintFormat("  End:   %1, %2", m_EndCoords[0], m_EndCoords[2]);

		int xDistance = m_EndCoords[0] - m_StartCoords[0];
		int zDistance = m_EndCoords[2] - m_StartCoords[2];
		int stepCountX = xDistance / m_StepSize;
		int stepCountZ = zDistance / m_StepSize;
		PrintFormat("  Tiles: %1 x %2 = %3 (step size %4)", stepCountX, stepCountZ, stepCountX * stepCountZ, m_StepSize);
	}

	[ButtonAttribute("Move to Start")]
	void PositionCameraStart()
	{
		ApplyCameraSettings();
		MoveCamera(m_StartCoords[0], m_StartCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);
	}
	
	[ButtonAttribute("Move to End")]
	void PositionCameraEnd()
	{
		ApplyCameraSettings();
		MoveCamera(m_EndCoords[0], m_EndCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);
	}

	[ButtonAttribute("Stop Capture")]
	void StopCapture()
	{
		if (IsRunning())
		{
			RequestCancel();
		}
		else
		{
			Print("No capture loop running");
			ResetCameraSettings();
		}
	}
	
	[ButtonAttribute("Start Capture")]
	void StartCapture()
	{
		if (IsRunning())
		{
			Print("Capture loop already in progress");
			return;
		}
		
		// --- Validation ---

		if (m_StepSize <= 0)
		{
			Print("ERROR: StepSize must be greater than 0!");
			return;
		}

		// Clamp batch size
		if (m_EnableBatchMode && (m_BatchSize < 1 || m_BatchSize > 31))
		{
			m_BatchSize = Math.ClampInt(m_BatchSize, 1, 31);
			PrintFormat("WARNING: BatchSize clamped to %1 (valid range: 1-31)", m_BatchSize);
		}
		
		int xDistance = m_EndCoords[0] - m_StartCoords[0];
		int zDistance = m_EndCoords[2] - m_StartCoords[2];
		
		if (xDistance <= 0 || zDistance <= 0)
		{
			PrintFormat("ERROR: End coords must be greater than start coords (xDist=%1, zDist=%2)", xDistance, zDistance);
			return;
		}
		
		// Step size alignment check
		int remainderX = xDistance % m_StepSize;
		int remainderZ = zDistance % m_StepSize;
		if (remainderX != 0 || remainderZ != 0)
		{
			PrintFormat("WARNING: Step size %1 doesn't divide evenly into extent (%2 x %3). Remainder: %4m x %5m will be missed.",
				m_StepSize, xDistance, zDistance, remainderX, remainderZ);
		}

		// FOV check
		if (!m_UseOrthographic && m_FieldOfView != 15)
		{
			PrintFormat("WARNING: FOV is set to %1 — the default tile pipeline expects FOV=15. Tiles may not align correctly.", m_FieldOfView);
		}

		if (m_UseOrthographic)
		{
			PrintFormat("=== ORTHOGRAPHIC MODE: perspective distortion eliminated ===");
			PrintFormat("  FOV value (%1) controls vertical extent in world units", m_FieldOfView);
		}

		int stepCountX = xDistance / m_StepSize;
		int stepCountZ = zDistance / m_StepSize;
		int totalTiles = stepCountX * stepCountZ;

		PrintFormat("Capture plan: %1 x %2 = %3 tiles, step size %4", stepCountX, stepCountZ, totalTiles, m_StepSize);

		BeginOperation();
		
		// --- Resume detection: pre-scan for existing tiles ---
		string outputDirectory = "$profile:" + m_OutputDirectory;
		int existingTiles = CountExistingTiles(outputDirectory, m_StartCoords[0], m_StartCoords[2], m_StepSize, stepCountX, stepCountZ);
		int remainingTiles = totalTiles - existingTiles;

		if (existingTiles > 0)
		{
			PrintFormat("=== Resuming: %1 tiles already exist, %2 remaining ===", existingTiles, remainingTiles);
		}

		Print("Performing initial camera move");
		MoveCamera(m_StartCoords[0], m_StartCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);

		for (int i = 0; i < 5; i++)
		{
			PrintFormat("Starting capture in %1 seconds ...", 5 - i);
			Sleep(1000);

			if (IsCancelled())
			{
				EndOperation();
				Print("Capture loop aborted during countdown");
				return;
			}
		}
		
		if (m_EnableBatchMode)
		{
			PrintFormat("=== BATCH MODE: batch size %1, preload radius %2m, cycle sleep %3ms ===", m_BatchSize, m_PreloadRadius, m_CycleMoveSleep);
			DoBatchLoop(m_StartCoords[0], m_StartCoords[2], m_StepSize, m_CameraHeight, stepCountX, stepCountZ, totalTiles);
		}
		else
		{
			DoLoop(m_StartCoords[0], m_StartCoords[2], m_StepSize, m_CameraHeight, stepCountX, stepCountZ, totalTiles);
		}
		PrintFormat("Capture finished (%1 total tiles)", totalTiles);
	}
	
	//------------------------------------------------------------
	// Resume detection
	//------------------------------------------------------------

	int CountExistingTiles(string outputDirectory, float initialX, float initialZ, int stepSize, int stepCountX, int stepCountZ)
	{
		int count = 0;
		for (int x = 0; x < stepCountX; x++)
		{
			int intMapPositionX = initialX + (x * stepSize);
			string xCoordinateDir = outputDirectory + "/" + string.Format("%1", intMapPositionX) + "/";

			for (int z = 0; z < stepCountZ; z++)
			{
				int intMapPositionZ = initialZ + (z * stepSize);
				string outputPath = xCoordinateDir + m_OutputFilePrefix + "_" + intMapPositionX + "_" + intMapPositionZ;

				if (FileIO.FileExist(outputPath + ".png") || FileIO.FileExist(outputPath + m_TileFilenameSuffix))
				{
					count++;
				}
			}
		}
		return count;
	}

	//------------------------------------------------------------
	// Core capture loop
	//------------------------------------------------------------

	// We loop over Z inside X, so we travel vertically in strips, slowly crossing right.
	// Z is North, X is East.
	void DoLoop(int initialX, int initialZ, int stepSize, int camHeight, int stepCountX, int stepCountZ, int totalTiles)
	{
		string outputDirectory = "$profile:" + m_OutputDirectory;
		
		if (!FileIO.MakeDirectory(outputDirectory))
		{
			PrintFormat("WARNING: MakeDirectory failed for %1 (may already exist)", outputDirectory);
		}
		
		bool cameraDiscontinuousMovement = false;
		int currentTile = 0;
		int skippedTiles = 0;
		int capturedTiles = 0;

		// ETA tracking
		int captureStartTime = System.GetTickCount();
		int firstCaptureTime = 0;

		// Set up camera parameters
		ApplyCameraSettings();

		for (int x = 0; x < stepCountX; x++)
		{
			float mapPositionX = initialX + (x * stepSize);
			int intMapPositionX = mapPositionX;
			
			// We use the x coordinate for the output directory structure
			string xCoordinateDir = outputDirectory + "/" + string.Format("%1", intMapPositionX) + "/";
			
			if (!FileIO.MakeDirectory(xCoordinateDir))
			{
				PrintFormat("WARNING: MakeDirectory failed for %1 (may already exist)", xCoordinateDir);
			}
			
			cameraDiscontinuousMovement = true;

			for (int z = 0; z < stepCountZ; z++)
			{
				currentTile++;
				float mapPositionZ = initialZ + (z * stepSize);
				int intMapPositionZ = mapPositionZ;
				
				// Build output paths
				string outputPath = xCoordinateDir + m_OutputFilePrefix + "_" + intMapPositionX + "_" + intMapPositionZ;
				string outputPathWithSuffix = outputPath + ".png";
				
				// Skip if raw screenshot already exists
				if (FileIO.FileExist(outputPathWithSuffix))
				{
					skippedTiles++;
					cameraDiscontinuousMovement = true;
					continue;
				}
			
				// Skip if cropped tile already exists
				string tilePath = outputPath + m_TileFilenameSuffix;
				if (FileIO.FileExist(tilePath))
				{
					skippedTiles++;
					cameraDiscontinuousMovement = true;
					continue;
				}
				
				// Progress report with ETA
				capturedTiles++;
				int percent = (currentTile * 100) / totalTiles;

				string etaStr = "";
				if (capturedTiles > 1 && firstCaptureTime > 0)
				{
					int elapsedMs = System.GetTickCount() - firstCaptureTime;
					int avgMs = elapsedMs / (capturedTiles - 1);
					int remainingCount = totalTiles - currentTile;
					int remainingMs = avgMs * remainingCount;
					int remainingSec = remainingMs / 1000;
					int remainingMin = remainingSec / 60;
					remainingSec = remainingSec % 60;
					etaStr = string.Format(" — ETA %1m %2s", remainingMin, remainingSec);
				}

				PrintFormat("[%1/%2] (%3%%) Capturing x=%4 z=%5%6", currentTile, totalTiles, percent, intMapPositionX, intMapPositionZ, etaStr);
				
				// Record time of first actual capture for ETA baseline
				if (capturedTiles == 1)
					firstCaptureTime = System.GetTickCount();

				MoveCamera(mapPositionX, mapPositionZ, camHeight, m_AbsoluteCameraHeight);
				
				if (cameraDiscontinuousMovement)
				{
					Sleep(m_DiscontinuousMoveSleep);
					cameraDiscontinuousMovement = false;
				}
				else
				{
					Sleep(m_MoveSleep);
				}
				
				// Take the screenshot
				bool success = System.MakeScreenshot(outputPath);
				if (!success)
				{
					PrintFormat("ERROR: Failed to write screenshot at %1", outputPath);
					RequestCancel();
				}
				
				Sleep(m_ScreenshotSleep);

				if (IsCancelled())
					break;
			}
			
			if (IsCancelled())
				break;
		}
		
		// Summary
		int totalElapsedSec = (System.GetTickCount() - captureStartTime) / 1000;
		int elapsedMin = totalElapsedSec / 60;
		int elapsedSec = totalElapsedSec % 60;
		PrintFormat("Capture complete: %1 captured, %2 skipped, %3 total — took %4m %5s", capturedTiles, skippedTiles, totalTiles, elapsedMin, elapsedSec);

		// Reset camera and HDR
		MoveCamera(initialX, initialZ, camHeight, m_AbsoluteCameraHeight);
		ResetCameraSettings();
		EndOperation();
	}
	
	//------------------------------------------------------------
	// Batched capture loop
	//------------------------------------------------------------

	// Batch mode: pre-position up to m_BatchSize cameras, call SchedulePreload
	// for all positions during one shared sleep, then rapidly cycle through
	// each position for screenshots.
	void DoBatchLoop(int initialX, int initialZ, int stepSize, int camHeight, int stepCountX, int stepCountZ, int totalTiles)
	{
		WorldEditorAPI api = GetApi();
		BaseWorld baseWorld = api.GetWorld();
		string outputDirectory = "$profile:" + m_OutputDirectory;

		if (!FileIO.MakeDirectory(outputDirectory))
		{
			PrintFormat("WARNING: MakeDirectory failed for %1 (may already exist)", outputDirectory);
		}

		// Set up camera parameters on the editor camera
		ApplyCameraSettings();

		// ETA tracking
		int captureStartTime = System.GetTickCount();
		int firstCaptureTime = 0;
		int overallTileIndex = 0;
		int capturedTiles = 0;
		int skippedTiles = 0;
		int batchCount = 0;

		// Build a flat list of all tile positions to process
		// Each entry: {mapX, mapZ, intMapX, intMapZ}
		ref array<ref array<float>> allTiles = new array<ref array<float>>();
		for (int x = 0; x < stepCountX; x++)
		{
			for (int z = 0; z < stepCountZ; z++)
			{
				float mapX = initialX + (x * stepSize);
				float mapZ = initialZ + (z * stepSize);
				ref array<float> tile = new array<float>();
				tile.Insert(mapX);
				tile.Insert(mapZ);
				allTiles.Insert(tile);
			}
		}

		while (overallTileIndex < allTiles.Count())
		{
			if (IsCancelled())
				break;

			// --- Phase 1: Collect batch of un-skipped tiles ---
			ref array<int> batchIndices = new array<int>();    // indices into allTiles
			ref array<string> batchOutputPaths = new array<string>();
			int scanStart = overallTileIndex;

			while (overallTileIndex < allTiles.Count() && batchIndices.Count() < m_BatchSize)
			{
				float mapX = allTiles[overallTileIndex][0];
				float mapZ = allTiles[overallTileIndex][1];
				int intMapX = mapX;
				int intMapZ = mapZ;

				// Ensure directory exists
				string xCoordinateDir = outputDirectory + "/" + string.Format("%1", intMapX) + "/";
				FileIO.MakeDirectory(xCoordinateDir);

				string outputPath = xCoordinateDir + m_OutputFilePrefix + "_" + intMapX + "_" + intMapZ;

				// Skip if already exists
				if (FileIO.FileExist(outputPath + ".png") || FileIO.FileExist(outputPath + m_TileFilenameSuffix))
				{
					skippedTiles++;
					overallTileIndex++;
					continue;
				}

				batchIndices.Insert(overallTileIndex);
				batchOutputPaths.Insert(outputPath);
				overallTileIndex++;
			}

			// Empty batch (all skipped) — continue to next segment
			if (batchIndices.IsEmpty())
				continue;

			batchCount++;
			int batchTileCount = batchIndices.Count();

			PrintFormat("[Batch %1] Pre-streaming %2 tiles (overall %3-%4 of %5)...",
				batchCount, batchTileCount, scanStart + 1, overallTileIndex, totalTiles);

			// --- Phase 2: Pre-position cameras + SchedulePreload ---
			for (int b = 0; b < batchTileCount; b++)
			{
				int tileIdx = batchIndices[b];
				float mapX = allTiles[tileIdx][0];
				float mapZ = allTiles[tileIdx][1];

				// Compute camera height for this tile
				float height = 0;
				api.TryGetTerrainSurfaceY(mapX, mapZ, height);
				if (m_AbsoluteCameraHeight)
					height = camHeight;
				else
					height += camHeight;

				vector tilePos = Vector(mapX, height, mapZ);

				// Position a secondary camera at this tile (cameras 1-31)
				int camIdx = b + 1;
				baseWorld.SetCamera(camIdx, tilePos, CAMERA_LOOK_DOWN);
				baseWorld.SetCameraVerticalFOV(camIdx, m_FieldOfView);
				if (m_UseOrthographic)
					baseWorld.SetCameraType(camIdx, CameraType.ORTHOGRAPHIC);

				// Trigger terrain/LOD pre-streaming
				baseWorld.SchedulePreload(tilePos, m_PreloadRadius);
			}

			// --- Phase 3: Shared streaming delay ---
			// Use the discontinuous sleep for the first batch (cold start),
			// then the normal move sleep for subsequent batches
			if (batchCount == 1)
				Sleep(m_DiscontinuousMoveSleep);
			else
				Sleep(m_MoveSleep);

			if (IsCancelled())
				break;

			// --- Phase 4: Rapid capture cycle ---
			for (int b = 0; b < batchTileCount; b++)
			{
				if (IsCancelled())
					break;

				int tileIdx = batchIndices[b];
				float mapX = allTiles[tileIdx][0];
				float mapZ = allTiles[tileIdx][1];
				int intMapX = mapX;
				int intMapZ = mapZ;
				string outputPath = batchOutputPaths[b];

				// Move editor camera to this position
				MoveCamera(mapX, mapZ, camHeight, m_AbsoluteCameraHeight);

				// Brief settle for shadows/exposure
				Sleep(m_CycleMoveSleep);

				// Take screenshot
				bool success = System.MakeScreenshot(outputPath);
				if (!success)
				{
					PrintFormat("ERROR: Failed to write screenshot at %1", outputPath);
					RequestCancel();
					break;
				}

				Sleep(m_ScreenshotSleep);

				// Progress report with ETA
				capturedTiles++;
				int currentTileNum = (overallTileIndex - batchTileCount) + b + 1;
				int percent = (currentTileNum * 100) / totalTiles;

				string etaStr = "";
				if (capturedTiles > 1 && firstCaptureTime > 0)
				{
					int elapsedMs = System.GetTickCount() - firstCaptureTime;
					int avgMs = elapsedMs / (capturedTiles - 1);
					int remainingCount = totalTiles - skippedTiles - capturedTiles;
					if (remainingCount < 0)
						remainingCount = 0;
					int remainingMs = avgMs * remainingCount;
					int remainingSec = remainingMs / 1000;
					int remainingMin = remainingSec / 60;
					remainingSec = remainingSec % 60;
					etaStr = string.Format(" — ETA %1m %2s", remainingMin, remainingSec);
				}

				PrintFormat("[%1/%2] (%3%%) Captured x=%4 z=%5%6",
					currentTileNum, totalTiles, percent, intMapX, intMapZ, etaStr);

				// Record time of first actual capture for ETA baseline
				if (capturedTiles == 1)
					firstCaptureTime = System.GetTickCount();
			}
		}

		// Summary
		int totalElapsedSec = (System.GetTickCount() - captureStartTime) / 1000;
		int elapsedMin = totalElapsedSec / 60;
		int elapsedSec = totalElapsedSec % 60;
		PrintFormat("Batch capture complete: %1 captured, %2 skipped, %3 total in %4 batches — took %5m %6s",
			capturedTiles, skippedTiles, totalTiles, batchCount, elapsedMin, elapsedSec);

		// Reset camera and HDR
		MoveCamera(initialX, initialZ, camHeight, m_AbsoluteCameraHeight);
		ResetCameraSettings();
		EndOperation();
	}

	//------------------------------------------------------------
	// Camera helpers
	//------------------------------------------------------------

	void ApplyCameraSettings()
	{
		WorldEditorAPI api = GetApi();
		BaseWorld baseWorld = api.GetWorld();
		int cameraId = baseWorld.GetCurrentCameraId();
		baseWorld.SetCameraHDRBrightness(cameraId, m_HdrBrightness);
		baseWorld.SetCameraVerticalFOV(cameraId, m_FieldOfView);
		if (m_UseOrthographic)
			baseWorld.SetCameraType(cameraId, CameraType.ORTHOGRAPHIC);
	}
	
	void ResetCameraSettings()
	{
		WorldEditorAPI api = GetApi();
		BaseWorld baseWorld = api.GetWorld();
		int cameraId = baseWorld.GetCurrentCameraId();
		baseWorld.SetCameraHDRBrightness(cameraId, -1);
		baseWorld.SetCameraType(cameraId, CameraType.PERSPECTIVE);
	}
	
	void MoveCamera(float xPos, float zPos, float camHeight, bool camHeightAbsolute)
	{
		WorldEditorAPI api = GetApi();
		
		float height = 0;
		api.TryGetTerrainSurfaceY(xPos, zPos, height);
		
		if (camHeightAbsolute)
			height = camHeight;
		else
			height += camHeight;
		
		vector newCamPos = Vector(xPos, height, zPos);
		api.SetCamera(newCamPos, CAMERA_LOOK_DOWN);
	}
}