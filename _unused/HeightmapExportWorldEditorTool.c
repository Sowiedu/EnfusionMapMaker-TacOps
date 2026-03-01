[WorkbenchToolAttribute(
	name: "Heightmap Export",
	description: "Export terrain heightmap data as raw float32 binary (.r32) at native resolution.",
	wbModules: {"WorldEditor"},
	awesomeFontCode: 0xf6ec)]
class HeightmapExportWorldEditorTool: WorldEditorTool
{
	// Name: Heightmap Export Tool
	// Exports the terrain heightmap as a raw float32 binary file (.r32)
	// and a JSON metadata sidecar. The output is written to the
	// $profile directory under a folder named after the current map.
	//
	// The .r32 file contains row-major float32 values (iterate Z rows, then X columns).
	// This can be loaded in Python with:
	//   import numpy as np
	//   data = np.fromfile('heightmap.r32', dtype=np.float32).reshape(height, width)
	//
	// Or opened in GIMP as raw data (float32, little-endian).
	//
	// NOTE: For large terrains (e.g. 12800m at 1m scale = ~164M samples),
	// the export can take several minutes as each height is individually
	// queried via GetTerrainSurfaceY. Progress is reported every 5%.

	//------------------------------------------------------------
	// State
	//------------------------------------------------------------

	private WorldEditorAPI m_Api;
	private bool m_InExportLoop;
	private bool m_CancelExport;

	//------------------------------------------------------------
	// Cached API access
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

	[ButtonAttribute("Export Heightmap")]
	void ExportHeightmap()
	{
		if (m_InExportLoop)
		{
			Print("Export already in progress");
			return;
		}

		WorldEditorAPI api = GetApi();
		if (!api)
		{
			Print("ERROR: WorldEditorAPI not available");
			return;
		}

		// Get terrain info
		float unitScale = api.GetTerrainUnitScale();
		int resolutionX = api.GetTerrainResolutionX();

		if (unitScale <= 0 || resolutionX <= 0)
		{
			PrintFormat("ERROR: Invalid terrain data (unitScale=%1, resolutionX=%2)", unitScale, resolutionX);
			return;
		}

		float terrainSize = unitScale * resolutionX;

		// Get map name for output directory
		string worldPath;
		api.GetWorldPath(worldPath);
		if (worldPath.IsEmpty())
		{
			Print("ERROR: No world is currently loaded");
			return;
		}

		string mapName = FilePath.StripExtension(FilePath.StripPath(worldPath));
		if (mapName.IsEmpty())
			mapName = "Unknown";

		// Grid dimensions: we sample at every grid point (native resolution)
		// resolutionX is the number of grid cells, so we have resolutionX + 1 sample points per axis
		int samplesPerAxis = resolutionX + 1;
		int totalSamples = samplesPerAxis * samplesPerAxis;

		PrintFormat("=== Heightmap Export ===");
		PrintFormat("  Map: %1", mapName);
		PrintFormat("  Terrain size: %1m x %1m", terrainSize);
		PrintFormat("  Unit scale: %1m", unitScale);
		PrintFormat("  Grid resolution: %1 cells (%2 samples per axis)", resolutionX, samplesPerAxis);
		PrintFormat("  Total samples: %1", totalSamples);
		PrintFormat("  Output: $profile:%1/", mapName);

		m_InExportLoop = true;
		m_CancelExport = false;

		DoExport(api, mapName, unitScale, samplesPerAxis, terrainSize);
	}

	[ButtonAttribute("Cancel Export")]
	void CancelExport()
	{
		if (m_InExportLoop)
		{
			m_CancelExport = true;
			Print("Cancelling export ...");
		}
		else
		{
			Print("No export in progress");
		}
	}

	//------------------------------------------------------------
	// Core export
	//------------------------------------------------------------

	void DoExport(WorldEditorAPI api, string mapName, float stepSize, int samplesPerAxis, float terrainSize)
	{
		// Create output directory
		string outputDir = "$profile:" + mapName;
		FileIO.MakeDirectory(outputDir);

		string binPath = outputDir + "/heightmap.r32";
		string metaPath = outputDir + "/heightmap_meta.json";

		// Open binary file for writing
		FileHandle binFile = FileIO.OpenFile(binPath, FileMode.WRITE);
		if (!binFile)
		{
			PrintFormat("ERROR: Failed to open %1 for writing", binPath);
			m_InExportLoop = false;
			return;
		}

		int totalSamples = samplesPerAxis * samplesPerAxis;
		int samplesWritten = 0;
		bool minMaxInitialized = false;
		float minHeight = 0;
		float maxHeight = 0;

		int lastReportedPercent = -1;

		// Row-major order: outer loop Z (rows), inner loop X (columns)
		// This matches image convention: row 0 = Z=0 (south), last row = Z=max (north)
		for (int z = 0; z < samplesPerAxis; z++)
		{
			// Collect one full row of floats
			array<float> rowData = new array<float>();
			float zPos = z * stepSize;

			for (int x = 0; x < samplesPerAxis; x++)
			{
				float xPos = x * stepSize;
				float height = api.GetTerrainSurfaceY(xPos, zPos);

				rowData.Insert(height);

				// Track min/max
				if (!minMaxInitialized)
				{
					minHeight = height;
					maxHeight = height;
					minMaxInitialized = true;
				}
				else
				{
					if (height < minHeight)
						minHeight = height;
					if (height > maxHeight)
						maxHeight = height;
				}
			}

			// Write the entire row as float32 array
			binFile.WriteArray(rowData, 4, samplesPerAxis);
			samplesWritten += samplesPerAxis;

			// Progress reporting (every 5%)
			int percent = (samplesWritten * 100) / totalSamples;
			if (percent >= lastReportedPercent + 5)
			{
				PrintFormat("[row %1/%2] (%3%%) min=%4 max=%5", z + 1, samplesPerAxis, percent, minHeight, maxHeight);
				lastReportedPercent = percent;
			}

			// Check for cancellation
			if (m_CancelExport)
			{
				Print("Export cancelled by user");
				binFile.Close();
				m_InExportLoop = false;
				return;
			}
		}

		binFile.Close();

		// Write metadata JSON
		WriteMetadata(metaPath, mapName, samplesPerAxis, stepSize, terrainSize, minHeight, maxHeight);

		// Summary
		PrintFormat("=== Export Complete ===");
		PrintFormat("  Samples: %1 (%2 x %2)", totalSamples, samplesPerAxis);
		PrintFormat("  Height range: %1m to %2m", minHeight, maxHeight);
		PrintFormat("  Binary: %1", binPath);
		PrintFormat("  Metadata: %1", metaPath);

		m_InExportLoop = false;
	}

	void WriteMetadata(string filepath, string mapName, int samplesPerAxis, float stepSize, float terrainSize, float minHeight, float maxHeight)
	{
		FileHandle metaFile = FileIO.OpenFile(filepath, FileMode.WRITE);
		if (!metaFile)
		{
			PrintFormat("ERROR: Failed to open %1 for writing", filepath);
			return;
		}

		metaFile.WriteLine("{");
		metaFile.WriteLine(string.Format("  \"mapName\": \"%1\",", mapName));
		metaFile.WriteLine(string.Format("  \"width\": %1,", samplesPerAxis));
		metaFile.WriteLine(string.Format("  \"height\": %1,", samplesPerAxis));
		metaFile.WriteLine(string.Format("  \"stepSize\": %1,", stepSize));
		metaFile.WriteLine(string.Format("  \"terrainSize\": %1,", terrainSize));
		metaFile.WriteLine(string.Format("  \"minHeight\": %1,", minHeight));
		metaFile.WriteLine(string.Format("  \"maxHeight\": %1,", maxHeight));
		metaFile.WriteLine(string.Format("  \"format\": \"float32\","));
		metaFile.WriteLine(string.Format("  \"byteOrder\": \"little-endian\","));
		metaFile.WriteLine(string.Format("  \"layout\": \"row-major (Z rows, X columns)\""));
		metaFile.WriteLine("}");
		metaFile.Close();

		PrintFormat("Metadata written to %1", filepath);
	}

	//------------------------------------------------------------
	// Lifecycle
	//------------------------------------------------------------

	override void OnDeActivate()
	{
		m_CancelExport = true;
		m_Api = null;
	}

	//------------------------------------------------------------
	// Keyboard input
	//------------------------------------------------------------

	override void OnKeyPressEvent(KeyCode key, bool isAutoRepeat)
	{
		if (key == KeyCode.KC_ESCAPE && !isAutoRepeat && m_InExportLoop && !m_CancelExport)
		{
			m_CancelExport = true;
			Print("Escape pressed — cancelling export ...");
		}
	}
}
