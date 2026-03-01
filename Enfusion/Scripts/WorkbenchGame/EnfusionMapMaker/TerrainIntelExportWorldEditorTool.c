[WorkbenchToolAttribute(
	name: "Terrain Intel Export",
	description: "Export terrain intelligence data (heightmap, roads, forests, water, landmarks, metadata) for TacOps.",
	wbModules: {"WorldEditor"},
	awesomeFontCode: 0xf0ac)]
class TerrainIntelExportWorldEditorTool: BaseMapMakerTool
{
	// Unified terrain intelligence export tool.
	// Exports the following layers to $profile:<mapName>/intel/:
	//   - heightmap.r32 + heightmap_meta.json  (raw float32 binary + metadata)
	//   - roads.geojson                        (road network as LineStrings)
	//   - forests.geojson                      (forest areas as Polygons)
	//   - water.geojson                        (lakes as Polygons, rivers as LineStrings, ocean metadata)
	//   - landmarks.geojson                    (POIs from MapDescriptorComponent as Points)
	//   - terrain_meta.json                    (comprehensive terrain metadata)

	//------------------------------------------------------------
	// State
	//------------------------------------------------------------

	private string m_MapName;
	private string m_OutputDir;
	private string m_WorldPath;

	// Per-file tracking for GeoJSON comma handling
	private bool m_RoadsFirstFeature;
	private bool m_ForestsFirstFeature;
	private bool m_WaterFirstFeature;
	private bool m_LandmarksFirstFeature;

	// Counters for summary
	private int m_RoadCount;
	private int m_ForestCount;
	private int m_LakeCount;
	private int m_RiverCount;
	private int m_LandmarkCount;

	//------------------------------------------------------------
	// Shared: Initialization
	//------------------------------------------------------------

	//! Validate API and world are loaded, set up map name and output dir
	private bool InitExport()
	{
		if (IsRunning())
		{
			Print("Export already in progress");
			return false;
		}

		WorldEditorAPI api = GetApi();
		if (!api)
		{
			Print("ERROR: WorldEditorAPI not available");
			return false;
		}

		api.GetWorldPath(m_WorldPath);
		if (m_WorldPath.IsEmpty())
		{
			Print("ERROR: No world is currently loaded");
			return false;
		}

		m_MapName = FilePath.StripExtension(FilePath.StripPath(m_WorldPath));
		if (m_MapName.IsEmpty())
			m_MapName = "Unknown";

		m_OutputDir = "$profile:" + m_MapName + "/intel";
		FileIO.MakeDirectory("$profile:" + m_MapName);
		FileIO.MakeDirectory(m_OutputDir);

		BeginOperation();

		return true;
	}

	private void FinishExport()
	{
		EndOperation();
	}

	//------------------------------------------------------------
	// Shared: File I/O
	//------------------------------------------------------------

	private FileHandle SafeOpenFile(string path)
	{
		FileHandle fh = FileIO.OpenFile(path, FileMode.WRITE);
		if (!fh)
			PrintFormat("ERROR: Failed to open %1 for writing", path);
		return fh;
	}

	//------------------------------------------------------------
	// Shared: GeoJSON helpers
	//------------------------------------------------------------

	private void WriteGeoJSONHeader(FileHandle fh, string layer)
	{
		fh.WriteLine("{");
		fh.WriteLine("  \"type\": \"FeatureCollection\",");
		fh.WriteLine(string.Format("  \"metadata\": { \"layer\": \"%1\", \"mapName\": \"%2\" },", layer, m_MapName));
		fh.WriteLine("  \"features\": [");
	}

	private void WriteGeoJSONFooter(FileHandle fh)
	{
		fh.WriteLine("");  // newline after last feature
		fh.WriteLine("  ]");
		fh.WriteLine("}");
	}

	private string FormatCoord(vector point)
	{
		return string.Format("[%1, %2]", point[0], point[2]);
	}

	private string FormatCoordArray(array<vector> points)
	{
		string result = "[";
		for (int i = 0; i < points.Count(); i++)
		{
			if (i > 0)
				result += ", ";
			result += FormatCoord(points[i]);
		}
		result += "]";
		return result;
	}

	//! Write a LineString feature to a GeoJSON file
	private void WriteLineStringFeature(FileHandle fh, ref bool firstFeature, array<vector> points, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";
		firstFeature = false;

		string coordsStr = FormatCoordArray(points);
		fh.WriteLine(prefix + "    {");
		fh.WriteLine("      \"type\": \"Feature\",");
		fh.WriteLine("      \"geometry\": {");
		fh.WriteLine("        \"type\": \"LineString\",");
		fh.WriteLine(string.Format("        \"coordinates\": %1", coordsStr));
		fh.WriteLine("      },");
		fh.WriteLine(string.Format("      \"properties\": { %1 }", propsJson));
		fh.WriteLine("    }");
	}

	//! Write a Polygon feature to a GeoJSON file
	private void WritePolygonFeature(FileHandle fh, ref bool firstFeature, array<vector> points, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";
		firstFeature = false;

		// Ensure closure: first point == last point for GeoJSON polygons
		if (points.Count() > 0)
		{
			vector first = points[0];
			vector last = points[points.Count() - 1];
			if (vector.Distance(first, last) > 0.01)
				points.Insert(first);
		}

		string coordsStr = FormatCoordArray(points);
		fh.WriteLine(prefix + "    {");
		fh.WriteLine("      \"type\": \"Feature\",");
		fh.WriteLine("      \"geometry\": {");
		fh.WriteLine("        \"type\": \"Polygon\",");
		fh.WriteLine(string.Format("        \"coordinates\": [%1]", coordsStr));
		fh.WriteLine("      },");
		fh.WriteLine(string.Format("      \"properties\": { %1 }", propsJson));
		fh.WriteLine("    }");
	}

	//! Write a Point feature to a GeoJSON file
	private void WritePointFeature(FileHandle fh, ref bool firstFeature, vector pos, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";
		firstFeature = false;

		fh.WriteLine(prefix + "    {");
		fh.WriteLine("      \"type\": \"Feature\",");
		fh.WriteLine("      \"geometry\": {");
		fh.WriteLine("        \"type\": \"Point\",");
		fh.WriteLine(string.Format("        \"coordinates\": [%1, %2]", pos[0], pos[2]));
		fh.WriteLine("      },");
		fh.WriteLine(string.Format("      \"properties\": { %1 }", propsJson));
		fh.WriteLine("    }");
	}

	//------------------------------------------------------------
	// Shared: Shape extraction
	//------------------------------------------------------------

	//! Extract world-space points from a parent shape entity source.
	//! Returns false if the shape cannot be resolved.
	private bool ExtractShapePoints(IEntitySource parentShapeSrc, out array<vector> points, out bool isClosed)
	{
		points = {};
		isClosed = false;

		WorldEditorAPI api = GetApi();
		IEntity ent = api.SourceToEntity(parentShapeSrc);
		if (!ent)
			return false;

		ShapeEntity shape = ShapeEntity.Cast(ent);
		if (!shape)
		{
			PrintFormat("WARNING: Parent entity is not a ShapeEntity (src class=%1)", parentShapeSrc.GetClassName());
			return false;
		}

		shape.GenerateTesselatedShape(points);
		isClosed = shape.IsClosed();

		// GenerateTesselatedShape returns points in local space; transform to world
		vector mat[4];
		shape.GetWorldTransform(mat);
		for (int i = 0; i < points.Count(); i++)
		{
			points[i] = points[i].Multiply4(mat);
		}

		if (points.Count() == 0)
		{
			PrintFormat("WARNING: Shape has 0 points");
			return false;
		}

		return true;
	}

	//------------------------------------------------------------
	// Shared: Progress
	//------------------------------------------------------------

	private int m_LastReportedPercent;

	private void ReportProgress(int current, int total, string label)
	{
		if (total <= 0)
			return;

		int percent = (current * 100) / total;
		if (percent >= m_LastReportedPercent + 5)
		{
			PrintFormat("[%1] %2%% (%3/%4)", label, percent, current, total);
			m_LastReportedPercent = percent;
		}
	}

	//------------------------------------------------------------
	// Shared: Landmark filtering
	//------------------------------------------------------------

	//! Returns true if this EMapDescriptorType should be excluded from export
	private bool IsLandmarkFiltered(int baseType)
	{
		// Filter out noise: trees, bushes, fences, walls, lights
		if (baseType == EMapDescriptorType.MDT_TREE) return true;
		if (baseType == EMapDescriptorType.MDT_SMALLTREE) return true;
		if (baseType == EMapDescriptorType.MDT_BUSH) return true;
		if (baseType == EMapDescriptorType.MDT_FENCE) return true;
		if (baseType == EMapDescriptorType.MDT_WALL) return true;
		if (baseType == EMapDescriptorType.MDT_LIGHT) return true;
		if (baseType == EMapDescriptorType.MDT_HIDE) return true;

		// Filter out features already captured by other layers or too dense
		if (baseType == EMapDescriptorType.MDT_ROAD) return true;
		if (baseType == EMapDescriptorType.MDT_FOREST) return true;
		if (baseType == EMapDescriptorType.MDT_TRACK) return true;
		if (baseType == EMapDescriptorType.MDT_MAINROAD) return true;
		if (baseType == EMapDescriptorType.MDT_POWERLINES) return true;
		if (baseType == EMapDescriptorType.MDT_RAILWAY) return true;
		if (baseType == EMapDescriptorType.MDT_FORESTBORDER) return true;
		if (baseType == EMapDescriptorType.MDT_FORESTTRIANGLE) return true;
		if (baseType == EMapDescriptorType.MDT_FORESTSQUARE) return true;
		if (baseType == EMapDescriptorType.MDT_FORESTERLODGE) return true;

		// Filter out debug/functional types
		if (baseType >= EMapDescriptorType.MDT_IMAGE_COUNT) return true;

		return false;
	}

	//------------------------------------------------------------
	// Layer 0: Heightmap Export
	//------------------------------------------------------------

	private void DoHeightmapExport()
	{
		WorldEditorAPI api = GetApi();

		float unitScale = api.GetTerrainUnitScale();
		int resolutionX = api.GetTerrainResolutionX();

		if (unitScale <= 0 || resolutionX <= 0)
		{
			PrintFormat("ERROR: Invalid terrain data (unitScale=%1, resolutionX=%2)", unitScale, resolutionX);
			return;
		}

		float terrainSize = unitScale * resolutionX;
		int samplesPerAxis = resolutionX + 1;
		int totalSamples = samplesPerAxis * samplesPerAxis;

		PrintFormat("=== Heightmap Export ===");
		PrintFormat("  Terrain: %1m x %1m, scale=%2m, %3 samples/axis", terrainSize, unitScale, samplesPerAxis);

		string binPath = m_OutputDir + "/heightmap.r32";
		string metaPath = m_OutputDir + "/heightmap_meta.json";

		FileHandle binFile = SafeOpenFile(binPath);
		if (!binFile)
			return;

		int samplesWritten = 0;
		bool minMaxInit = false;
		float minHeight = 0;
		float maxHeight = 0;
		m_LastReportedPercent = -1;

		for (int z = 0; z < samplesPerAxis; z++)
		{
			array<float> rowData = new array<float>();
			float zPos = z * unitScale;

			for (int x = 0; x < samplesPerAxis; x++)
			{
				float xPos = x * unitScale;
				float height = api.GetTerrainSurfaceY(xPos, zPos);
				rowData.Insert(height);

				if (!minMaxInit)
				{
					minHeight = height;
					maxHeight = height;
					minMaxInit = true;
				}
				else
				{
					if (height < minHeight) minHeight = height;
					if (height > maxHeight) maxHeight = height;
				}
			}

			binFile.WriteArray(rowData, 4, samplesPerAxis);
			samplesWritten += samplesPerAxis;

			ReportProgress(samplesWritten, totalSamples, "Heightmap");

			if (IsCancelled())
			{
				Print("Heightmap export cancelled");
				binFile.Close();
				return;
			}
		}

		binFile.Close();

		// Write metadata sidecar
		WriteHeightmapMetadata(metaPath, samplesPerAxis, unitScale, terrainSize, minHeight, maxHeight);

		PrintFormat("  Heightmap complete: %1 samples, height range [%2, %3]", totalSamples, minHeight, maxHeight);
	}

	private void WriteHeightmapMetadata(string filepath, int samplesPerAxis, float stepSize, float terrainSize, float minHeight, float maxHeight)
	{
		FileHandle fh = SafeOpenFile(filepath);
		if (!fh)
			return;

		fh.WriteLine("{");
		fh.WriteLine(string.Format("  \"mapName\": \"%1\",", m_MapName));
		fh.WriteLine(string.Format("  \"width\": %1,", samplesPerAxis));
		fh.WriteLine(string.Format("  \"height\": %1,", samplesPerAxis));
		fh.WriteLine(string.Format("  \"stepSize\": %1,", stepSize));
		fh.WriteLine(string.Format("  \"terrainSize\": %1,", terrainSize));
		fh.WriteLine(string.Format("  \"minHeight\": %1,", minHeight));
		fh.WriteLine(string.Format("  \"maxHeight\": %1,", maxHeight));
		fh.WriteLine("  \"format\": \"float32\",");
		fh.WriteLine("  \"byteOrder\": \"little-endian\",");
		fh.WriteLine("  \"layout\": \"row-major (Z rows, X columns)\"");
		fh.WriteLine("}");
		fh.Close();
	}

	//------------------------------------------------------------
	// Layers 1-4: Single-pass entity export
	//------------------------------------------------------------

	private void ExportEntityLayers(bool doRoads, bool doForests, bool doWater, bool doLandmarks)
	{
		WorldEditorAPI api = GetApi();

		// Reset counters
		m_RoadCount = 0;
		m_ForestCount = 0;
		m_LakeCount = 0;
		m_RiverCount = 0;
		m_LandmarkCount = 0;
		m_LastReportedPercent = -1;

		// Open file handles for requested layers
		FileHandle fhRoads, fhForests, fhWater, fhLandmarks;

		if (doRoads)
		{
			fhRoads = SafeOpenFile(m_OutputDir + "/roads.geojson");
			if (!fhRoads) doRoads = false;
			else { WriteGeoJSONHeader(fhRoads, "roads"); m_RoadsFirstFeature = true; }
		}

		if (doForests)
		{
			fhForests = SafeOpenFile(m_OutputDir + "/forests.geojson");
			if (!fhForests) doForests = false;
			else { WriteGeoJSONHeader(fhForests, "forests"); m_ForestsFirstFeature = true; }
		}

		if (doWater)
		{
			fhWater = SafeOpenFile(m_OutputDir + "/water.geojson");
			if (!fhWater) doWater = false;
			else
			{
				WriteGeoJSONHeader(fhWater, "water");
				m_WaterFirstFeature = true;

				// Write ocean metadata as first feature if applicable
				BaseWorld world = api.GetWorld();
				if (world && world.IsOcean())
				{
					float oceanHeight = world.GetOceanBaseHeight();
					string oceanProps = string.Format("\"type\": \"ocean\", \"baseHeight\": %1", oceanHeight);
					// Ocean is a metadata-only point at origin
					WritePointFeature(fhWater, m_WaterFirstFeature, vector.Zero, oceanProps);
				}
			}
		}

		if (doLandmarks)
		{
			fhLandmarks = SafeOpenFile(m_OutputDir + "/landmarks.geojson");
			if (!fhLandmarks) doLandmarks = false;
			else { WriteGeoJSONHeader(fhLandmarks, "landmarks"); m_LandmarksFirstFeature = true; }
		}

		// Nothing to export?
		if (!doRoads && !doForests && !doWater && !doLandmarks)
		{
			Print("No entity layers to export");
			return;
		}

		// --- Single-pass entity iteration ---
		int entityCount = api.GetEditorEntityCount();
		PrintFormat("  Scanning %1 editor entities ...", entityCount);

		for (int i = 0; i < entityCount; i++)
		{
			if (IsCancelled())
				break;

			IEntitySource entSrc = api.GetEditorEntity(i);
			if (!entSrc)
				continue;

			// --- Shape children scan (roads, forests, water) ---
			if (doRoads || doForests || doWater)
			{
				int childCount = entSrc.GetNumChildren();
				for (int c = 0; c < childCount; c++)
				{
					IEntitySource childSrc = entSrc.GetChild(c);
					if (!childSrc)
						continue;

					string childClass = childSrc.GetClassName();

					if (doRoads && childClass == "RoadGeneratorEntity")
						CollectRoad(fhRoads, entSrc, childSrc);
					else if (doForests && childClass == "ForestGeneratorEntity")
						CollectForest(fhForests, entSrc, childSrc);
					else if (doWater && childClass == "LakeGeneratorEntity")
						CollectLake(fhWater, entSrc, childSrc);
					else if (doWater && childClass == "RiverEntity")
						CollectRiver(fhWater, entSrc, childSrc);
				}
			}

			// --- Landmark scan ---
			if (doLandmarks)
				CollectLandmark(fhLandmarks, entSrc);

			ReportProgress(i, entityCount, "Entity scan");
		}

		// --- Close all files ---
		if (doRoads && fhRoads)
		{
			WriteGeoJSONFooter(fhRoads);
			fhRoads.Close();
			PrintFormat("  Roads: %1 features → roads.geojson", m_RoadCount);
		}

		if (doForests && fhForests)
		{
			WriteGeoJSONFooter(fhForests);
			fhForests.Close();
			PrintFormat("  Forests: %1 features → forests.geojson", m_ForestCount);
		}

		if (doWater && fhWater)
		{
			WriteGeoJSONFooter(fhWater);
			fhWater.Close();
			PrintFormat("  Water: %1 lakes, %2 rivers → water.geojson", m_LakeCount, m_RiverCount);
		}

		if (doLandmarks && fhLandmarks)
		{
			WriteGeoJSONFooter(fhLandmarks);
			fhLandmarks.Close();
			PrintFormat("  Landmarks: %1 features → landmarks.geojson", m_LandmarkCount);
		}

		if (IsCancelled())
			Print("Entity export cancelled by user");
	}

	//------------------------------------------------------------
	// Collectors (called from the single loop)
	//------------------------------------------------------------

	private void CollectRoad(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource roadGenSrc)
	{
		array<vector> points;
		bool isClosed;

		if (!ExtractShapePoints(parentShapeSrc, points, isClosed))
			return;

		float roadWidth = 6.0;
		roadGenSrc.Get("RoadWidth", roadWidth);

		string props = string.Format("\"type\": \"road\", \"width\": %1", roadWidth);
		WriteLineStringFeature(fh, m_RoadsFirstFeature, points, props);
		m_RoadCount++;
	}

	private void CollectForest(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource forestGenSrc)
	{
		array<vector> points;
		bool isClosed;

		if (!ExtractShapePoints(parentShapeSrc, points, isClosed))
			return;

		if (!isClosed)
		{
			PrintFormat("WARNING: Forest shape is not closed, skipping");
			return;
		}

		WritePolygonFeature(fh, m_ForestsFirstFeature, points, "\"type\": \"forest\"");
		m_ForestCount++;
	}

	private void CollectLake(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource lakeGenSrc)
	{
		array<vector> points;
		bool isClosed;

		if (!ExtractShapePoints(parentShapeSrc, points, isClosed))
			return;

		// Derive surface Y from the average Y of shape points
		float surfaceY = 0;
		if (points.Count() > 0)
		{
			for (int i = 0; i < points.Count(); i++)
				surfaceY += points[i][1];
			surfaceY /= points.Count();
		}

		string props = string.Format("\"type\": \"lake\", \"surfaceY\": %1", surfaceY);
		WritePolygonFeature(fh, m_WaterFirstFeature, points, props);
		m_LakeCount++;
	}

	private void CollectRiver(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource riverSrc)
	{
		array<vector> points;
		bool isClosed;

		if (!ExtractShapePoints(parentShapeSrc, points, isClosed))
			return;

		float clearance = 10.0;
		riverSrc.Get("Clearance", clearance);

		string props = string.Format("\"type\": \"river\", \"width\": %1", clearance);
		WriteLineStringFeature(fh, m_WaterFirstFeature, points, props);
		m_RiverCount++;
	}

	private void CollectLandmark(FileHandle fh, IEntitySource entSrc)
	{
		WorldEditorAPI api = GetApi();
		IEntity entity = api.SourceToEntity(entSrc);
		if (!entity)
			return;

		MapDescriptorComponent desc = MapDescriptorComponent.Cast(entity.FindComponent(MapDescriptorComponent));
		if (!desc)
			return;

		int baseType = desc.GetBaseType();
		if (IsLandmarkFiltered(baseType))
			return;

		vector pos = entity.GetOrigin();
		string name = entity.GetName();
		string typeName = typename.EnumToString(EMapDescriptorType, baseType);

		// Strip quotes from name for JSON safety (Enforce Script has limited escape support)
		name.Replace("\"", "");

		float elevation = api.GetTerrainSurfaceY(pos[0], pos[2]);

		string props = string.Format(
			"\"type\": \"landmark\", \"descriptorType\": \"%1\", \"descriptorTypeId\": %2, \"name\": \"%3\", \"elevation\": %4",
			typeName, baseType, name, elevation);

		WritePointFeature(fh, m_LandmarksFirstFeature, pos, props);
		m_LandmarkCount++;
	}

	//------------------------------------------------------------
	// Layer 5: Metadata Export
	//------------------------------------------------------------

	private void ExportMetadata()
	{
		WorldEditorAPI api = GetApi();

		string path = m_OutputDir + "/terrain_meta.json";
		FileHandle fh = SafeOpenFile(path);
		if (!fh)
			return;

		float unitScale = api.GetTerrainUnitScale();
		int resX = api.GetTerrainResolutionX();
		int resZ = resX; // square terrains in Enfusion
		float sizeX = unitScale * resX;
		float sizeZ = sizeX;

		// Ocean info
		BaseWorld world = api.GetWorld();
		bool hasOcean = false;
		float oceanHeight = 0;
		if (world)
		{
			hasOcean = world.IsOcean();
			if (hasOcean)
				oceanHeight = world.GetOceanBaseHeight();
		}

		// Geolocation from weather manager
		float latitude = 0;
		float longitude = 0;
		bool hasGeo = false;
		ChimeraWorld chimeraWorld = ChimeraWorld.CastFrom(world);
		if (chimeraWorld)
		{
			TimeAndWeatherManagerEntity weatherMgr = chimeraWorld.GetTimeAndWeatherManager();
			if (weatherMgr)
			{
				latitude = weatherMgr.GetCurrentLatitude();
				longitude = weatherMgr.GetCurrentLongitude();
				hasGeo = true;
			}
		}

		int entityCount = api.GetEditorEntityCount();

		fh.WriteLine("{");
		fh.WriteLine(string.Format("  \"mapName\": \"%1\",", m_MapName));
		fh.WriteLine(string.Format("  \"worldPath\": \"%1\",", m_WorldPath));

		// Terrain section
		fh.WriteLine("  \"terrain\": {");
		fh.WriteLine(string.Format("    \"sizeX\": %1,", sizeX));
		fh.WriteLine(string.Format("    \"sizeZ\": %1,", sizeZ));
		fh.WriteLine(string.Format("    \"unitScale\": %1,", unitScale));
		fh.WriteLine(string.Format("    \"resolutionX\": %1,", resX));
		fh.WriteLine(string.Format("    \"resolutionZ\": %1,", resZ));
		fh.WriteLine(string.Format("    \"gridCellCount\": %1", resX * resZ));
		fh.WriteLine("  },");

		// Ocean section
		fh.WriteLine("  \"ocean\": {");
		fh.WriteLine(string.Format("    \"hasOcean\": %1", hasOcean));
		if (hasOcean)
		{
			fh.WriteLine(",");
			fh.WriteLine(string.Format("    \"baseHeight\": %1", oceanHeight));
		}
		fh.WriteLine("  },");

		// Geolocation
		if (hasGeo)
		{
			fh.WriteLine("  \"geolocation\": {");
			fh.WriteLine(string.Format("    \"latitude\": %1,", latitude));
			fh.WriteLine(string.Format("    \"longitude\": %1", longitude));
			fh.WriteLine("  },");
		}

		// Entities
		fh.WriteLine("  \"entities\": {");
		fh.WriteLine(string.Format("    \"totalEditorEntities\": %1", entityCount));
		fh.WriteLine("  }");

		fh.WriteLine("}");
		fh.Close();

		PrintFormat("  Metadata written → terrain_meta.json");
	}

	//------------------------------------------------------------
	// Buttons
	//------------------------------------------------------------

	[ButtonAttribute("Export All")]
	void ExportAll()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Terrain Intel Export: %1 ===", m_MapName);
		PrintFormat("  Output: %1", m_OutputDir);

		// Layer 0: Heightmap
		if (!IsCancelled())
			DoHeightmapExport();

		// Layers 1-4: Single-pass entity scan
		if (!IsCancelled())
			ExportEntityLayers(true, true, true, true);

		// Layer 5: Metadata
		if (!IsCancelled())
			ExportMetadata();

		PrintFormat("=== Export Complete ===");
		FinishExport();
	}

	[ButtonAttribute("Export Heightmap")]
	void BtnExportHeightmap()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Heightmap Export: %1 ===", m_MapName);
		DoHeightmapExport();
		FinishExport();
	}

	[ButtonAttribute("Export Roads")]
	void BtnExportRoads()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Roads Export: %1 ===", m_MapName);
		ExportEntityLayers(true, false, false, false);
		FinishExport();
	}

	[ButtonAttribute("Export Forests")]
	void BtnExportForests()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Forests Export: %1 ===", m_MapName);
		ExportEntityLayers(false, true, false, false);
		FinishExport();
	}

	[ButtonAttribute("Export Water Bodies")]
	void BtnExportWaterBodies()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Water Bodies Export: %1 ===", m_MapName);
		ExportEntityLayers(false, false, true, false);
		FinishExport();
	}

	[ButtonAttribute("Export Landmarks")]
	void BtnExportLandmarks()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Landmarks Export: %1 ===", m_MapName);
		ExportEntityLayers(false, false, false, true);
		FinishExport();
	}

	[ButtonAttribute("Export Metadata")]
	void BtnExportMetadata()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Metadata Export: %1 ===", m_MapName);
		ExportMetadata();
		FinishExport();
	}

	[ButtonAttribute("Cancel Export")]
	void BtnCancelExport()
	{
		RequestCancel();
	}
}
