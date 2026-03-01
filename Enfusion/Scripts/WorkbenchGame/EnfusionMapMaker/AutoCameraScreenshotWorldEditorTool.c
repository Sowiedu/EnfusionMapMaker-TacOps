[WorkbenchToolAttribute(name: "Auto Camera Screenshot", description: "Automatically create screenshots of an area of a map.", wbModules: {"WorldEditor"}, awesomeFontCode: 0xf447)]
class AutoCameraScreenshotWorldEditorTool: WorldEditorTool
{
	// Name: Auto Camera Screenshot Tool
	// Author: Bewilderbeest <bewilder@recoil.org>

	// This is a WORLD EDITOR tool, so open up your required map first.

	// Since we cannot fully control the camera in the World Editor,
	// this requires the user to set the FOV to 15, and the farPlane
	// distance to ~ 5000. This script will yield incorrect results
	// otherwise!

	// The camera will start at m_StartCoords and step by m_StepSize
	// in each axis until it reaches m_EndCoords, generating
	// screenshots into your $profile directory, which is usually
	// C:\Users\<NAME>\Documents\My Games\ArmaReforgerWorkbench\profile\

	// This screenshot capture process has been tested by me to work
	// when you fullscreen the editor application using F11. So to
	// start the process, press the "Start Capture" button, then
	// immediately hit F11 to go into full screen mode, and this
	// gives a consistent screenshot size, otherwise changes to the
	// camera window size will mess with the output.

	// In order to account for LOD streaming and exposure changes,
	// There is a small sleep delay after the camera has moved, and
	// then a small delay after the screenshot has been triggered
	// to allow for async operations to complete. These might need
	// tuning if your screenshots are discontinuous or inconsistent

	// During capture, the escape key will allow you to stop the process,
	// because you cannot access the button if the editor camera is full screen!

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

	[Attribute("15", UIWidgets.Auto, "Camera FOV", "", null, "Camera")]
	float m_FieldOfView;

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
	// Advanced
	//------------------------------------------------------------

	[Attribute("_tile.png", UIWidgets.Auto, "Tile filename suffix (must match the python code)", "", null, "Advanced")]
	string m_TileFilenameSuffix;

	[Attribute("0.025", UIWidgets.Auto, "Custom HDR brightness override (-1 to reset)", "", null, "Advanced")]
	float m_HdrBrightness;

	//------------------------------------------------------------
	// Cached refs & loop state
	//------------------------------------------------------------

	private WorldEditorAPI m_Api;
	private bool m_InCaptureLoop;
	private bool m_CancelCurrentLoop;
	
	//------------------------------------------------------------
	// Editor ref helpers
	//------------------------------------------------------------

	private WorldEditorAPI GetApi()
	{
		if (!m_Api)
		{
			WorldEditor worldEditor = Workbench.GetModule(WorldEditor);
			m_Api = worldEditor.GetApi();
		}
		return m_Api;
	}

	//------------------------------------------------------------
	// Buttons
	//------------------------------------------------------------

	[ButtonAttribute("Move to start")]
	void PositionCameraStart()
	{
		ApplyCameraSettings();
		MoveCamera(m_StartCoords[0], m_StartCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);
	}
	
	[ButtonAttribute("Move to end")]
	void PositionCameraEnd()
	{
		ApplyCameraSettings();
		MoveCamera(m_EndCoords[0], m_EndCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);
	}

	[ButtonAttribute("Stop Capture")]
	void StopCapture()
	{
		if (m_InCaptureLoop)
		{
			if (m_CancelCurrentLoop)
			{
				Print("Halt already in progress");
			}
			else
			{
				m_CancelCurrentLoop = true;
				Print("Halting capture loop ...");
			}
		}
		else
		{
			Print("No capture loop running");
			ResetCustomHDRBrightness();
		}
	}
	
	[ButtonAttribute("Start Capture")]
	void StartCapture()
	{
		if (m_InCaptureLoop)
		{
			Print("Capture loop already in progress");
			return;
		}
		
		// Validate step size
		if (m_StepSize <= 0)
		{
			Print("ERROR: StepSize must be greater than 0!");
			return;
		}
		
		// Validate coordinate ranges
		int xDistance = m_EndCoords[0] - m_StartCoords[0];
		int zDistance = m_EndCoords[2] - m_StartCoords[2];
		
		if (xDistance <= 0 || zDistance <= 0)
		{
			PrintFormat("ERROR: End coords must be greater than start coords (xDist=%1, zDist=%2)", xDistance, zDistance);
			return;
		}
		
		int stepCountX = xDistance / m_StepSize;
		int stepCountZ = zDistance / m_StepSize;
		int totalTiles = stepCountX * stepCountZ;

		PrintFormat("Capture plan: %1 x %2 = %3 tiles, step size %4", stepCountX, stepCountZ, totalTiles, m_StepSize);

		m_InCaptureLoop = true;
		m_CancelCurrentLoop = false;
		
		Print("Performing initial camera move");
		MoveCamera(m_StartCoords[0], m_StartCoords[2], m_CameraHeight, m_AbsoluteCameraHeight);

		for (int i = 0; i < 5; i++)
		{
			PrintFormat("Starting capture in %1 seconds ...", 5 - i);
			Sleep(1000);

			if (m_CancelCurrentLoop)
			{
				m_InCaptureLoop = false;
				Print("Capture loop aborted during countdown");
				return;
			}
		}
		
		DoLoop(m_StartCoords[0], m_StartCoords[2], m_StepSize, m_CameraHeight, stepCountX, stepCountZ);
		PrintFormat("Capture finished (%1 total tiles)", totalTiles);
	}
	
	override void OnDeActivate()
	{
		m_CancelCurrentLoop = true;
		m_Api = null;
	}
	
	//------------------------------------------------------------
	// Core capture loop
	//------------------------------------------------------------

	// We loop over Z inside X, so we travel vertically in strips, slowly crossing right.
	// Z is North, X is East.
	void DoLoop(int initialX, int initialZ, int stepSize, int camHeight, int stepCountX, int stepCountZ)
	{
		string outputDirectory = "$profile:" + m_OutputDirectory;
		
		if (!FileIO.MakeDirectory(outputDirectory))
		{
			PrintFormat("WARNING: MakeDirectory failed for %1 (may already exist)", outputDirectory);
		}
		
		bool cameraDiscontinuousMovement = false;
		int totalTiles = stepCountX * stepCountZ;
		int currentTile = 0;
		int skippedTiles = 0;

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
				
				// Progress report
				int percent = (currentTile * 100) / totalTiles;
				PrintFormat("[%1/%2] (%3%%) Capturing x=%4 z=%5", currentTile, totalTiles, percent, intMapPositionX, intMapPositionZ);
				
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
					m_CancelCurrentLoop = true;
				}
				
				Sleep(m_ScreenshotSleep);

				if (m_CancelCurrentLoop)
					break;
			}
			
			if (m_CancelCurrentLoop)
				break;
		}
		
		// Summary
		int capturedTiles = currentTile - skippedTiles;
		PrintFormat("Capture complete: %1 captured, %2 skipped, %3 total", capturedTiles, skippedTiles, totalTiles);

		// Reset camera and HDR
		MoveCamera(initialX, initialZ, camHeight, m_AbsoluteCameraHeight);
		ResetCustomHDRBrightness();
		m_InCaptureLoop = false;
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
	}
	
	void ResetCustomHDRBrightness()
	{
		WorldEditorAPI api = GetApi();
		BaseWorld baseWorld = api.GetWorld();
		int cameraId = baseWorld.GetCurrentCameraId();
		baseWorld.SetCameraHDRBrightness(cameraId, -1);
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

	//------------------------------------------------------------
	// Keyboard input
	//------------------------------------------------------------
	
	override void OnKeyPressEvent(KeyCode key, bool isAutoRepeat)
	{
		if (key == KeyCode.KC_ESCAPE && !isAutoRepeat && m_InCaptureLoop && !m_CancelCurrentLoop)
		{
			m_CancelCurrentLoop = true;
			Print("Escape pressed — halting capture ...");
		}
	}
}