[WorkbenchToolAttribute(
	name: "Terrain Intel Export",
	description: "Export terrain intelligence data (heightmap, roads, forests, water, landmarks, metadata, trees, buildings) for TacOps.",
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
	//   - trees.bin                            (per-tree world-space positions + bounding, binary float32, count = file_size / 16)
	//   - buildings.bin                         (per-building world-space AABB footprints + top, binary float32, count = file_size / 20)

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

	// Surface object export state (used by streaming callbacks)
	// Callbacks collect data into arrays; files are written AFTER the query returns.
	// (QueryEntitiesByAABB invalidates FileHandle member variables during callbacks)
	private int m_TreeCount;
	private int m_BuildingCount;
	private ref array<float> m_TreeData;      // flat: posX, posZ, height, radius per tree
	private ref array<float> m_BuildingData;   // flat: minX, minZ, maxX, maxZ, topY per building

	// Shape extraction state
	private bool m_LastShapeClosed;

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
	private void WriteLineStringFeature(FileHandle fh, bool firstFeature, array<vector> points, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";


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
	private void WritePolygonFeature(FileHandle fh, bool firstFeature, array<vector> points, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";


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
	private void WritePointFeature(FileHandle fh, bool firstFeature, vector pos, string propsJson)
	{
		string prefix = "";
		if (!firstFeature)
			prefix = ",\n";


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
	private bool ExtractShapePoints(IEntitySource parentShapeSrc, out array<vector> points)
	{
		points = {};
		m_LastShapeClosed = false;

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
		m_LastShapeClosed = shape.IsClosed();

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
					m_WaterFirstFeature = false;
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

		int entitiesWithChildren = 0;
		int totalChildren = 0;

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
				if (childCount > 0)
				{
					entitiesWithChildren++;
					totalChildren += childCount;
				}
				for (int c = 0; c < childCount; c++)
				{
					IEntitySource childSrc = entSrc.GetChild(c);
					if (!childSrc)
						continue;

					string childClass = childSrc.GetClassName();

					// Log first few child class names to see what's in the world
					if (totalChildren < 30)
						PrintFormat("    Child[%1]: class=%2 (parent=%3)", c, childClass, entSrc.GetClassName());

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

		PrintFormat("  Scan complete: %1 entities with children, %2 total children", entitiesWithChildren, totalChildren);

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

		if (!ExtractShapePoints(parentShapeSrc, points))
			return;

		float roadWidth = 6.0;
		roadGenSrc.Get("RoadWidth", roadWidth);

		string props = string.Format("\"type\": \"road\", \"width\": %1", roadWidth);
		WriteLineStringFeature(fh, m_RoadsFirstFeature, points, props);
		m_RoadsFirstFeature = false;
		m_RoadCount++;
	}

	private void CollectForest(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource forestGenSrc)
	{
		array<vector> points;

		if (!ExtractShapePoints(parentShapeSrc, points))
			return;

		if (!m_LastShapeClosed)
		{
			PrintFormat("WARNING: Forest shape is not closed, skipping");
			return;
		}

		WritePolygonFeature(fh, m_ForestsFirstFeature, points, "\"type\": \"forest\"");
		m_ForestsFirstFeature = false;
		m_ForestCount++;
	}

	private void CollectLake(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource lakeGenSrc)
	{
		array<vector> points;

		if (!ExtractShapePoints(parentShapeSrc, points))
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
		m_WaterFirstFeature = false;
		m_LakeCount++;
	}

	private void CollectRiver(FileHandle fh, IEntitySource parentShapeSrc, IEntitySource riverSrc)
	{
		array<vector> points;

		if (!ExtractShapePoints(parentShapeSrc, points))
			return;

		float clearance = 10.0;
		riverSrc.Get("Clearance", clearance);

		string props = string.Format("\"type\": \"river\", \"width\": %1", clearance);
		WriteLineStringFeature(fh, m_WaterFirstFeature, points, props);
		m_WaterFirstFeature = false;
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
		m_LandmarksFirstFeature = false;
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
	// Layer 6: Tree Export (runtime entity query)
	//------------------------------------------------------------

	//! Export all standing tree entities as a compact binary file.
	//! Callbacks collect data into m_TreeData; file is written after the query.
	//! (QueryEntitiesByAABB invalidates member FileHandle variables during callbacks)
	//!
	//! Format: N * 16-byte records (float32 posX, posZ, height, radius)
	//! Reader derives count from file_size / 16.
	private void ExportTrees()
	{
		WorldEditorAPI api = GetApi();
		BaseWorld world = api.GetWorld();
		if (!world)
		{
			Print("ERROR: No world available for tree export");
			return;
		}

		vector worldMins, worldMaxs;
		world.GetBoundBox(worldMins, worldMaxs);

		// Prepare collection array
		m_TreeCount = 0;
		m_TreeData = new array<float>();

		Print("  Scanning world for tree entities...");

		// Query — callback only appends to m_TreeData, no file I/O
		world.QueryEntitiesByAABB(worldMins, worldMaxs, OnTreeEntityFound, null, EQueryEntitiesFlags.STATIC);

		PrintFormat("  Query complete. Collected %1 trees (%2 floats).", m_TreeCount, m_TreeData.Count());

		// Now write everything to file at once
		string path = m_OutputDir + "/trees.bin";
		FileHandle fh = SafeOpenFile(path);
		if (!fh)
			return;

		// Write all records — no footer; reader derives count from file_size / 16
		if (m_TreeData.Count() > 0)
			fh.WriteArray(m_TreeData, 4, m_TreeData.Count());
		fh.Close();

		PrintFormat("  Trees: %1 entities -> trees.bin", m_TreeCount);

		// Free collection memory
		m_TreeData = null;
	}

	//! Callback for tree entity query — collects data, no file I/O.
	private bool OnTreeEntityFound(IEntity ent)
	{
		if (IsCancelled())
			return false;

		// Skip tree parts (sub-components of destructible trees)
		if (ent.IsInherited(SCR_TreePartV2))
			return true;

		// Skip fallen trees (lie horizontal, don't block LOS)
		if (ent.IsInherited(FallenTree))
			return true;

		// Match standing trees and forest generator trees
		bool isTree = ent.IsInherited(Tree) || ent.IsInherited(ForestGeneratorTree);
		// Also match cluster vegetation objects
		isTree = isTree || ent.IsInherited(SmallForestGeneratorClusterObject);

		if (!isTree)
			return true;

		vector mins, maxs;
		ent.GetWorldBounds(mins, maxs);

		float height = maxs[1] - mins[1];
		if (height < 1.0)
			return true;  // skip ground debris / saplings

		float posX = (mins[0] + maxs[0]) * 0.5;
		float posZ = (mins[2] + maxs[2]) * 0.5;
		float radius = Math.Max(maxs[0] - mins[0], maxs[2] - mins[2]) * 0.5;

		// Collect into flat array — no file I/O here
		m_TreeData.Insert(posX);
		m_TreeData.Insert(posZ);
		m_TreeData.Insert(height);
		m_TreeData.Insert(radius);
		m_TreeCount++;

		// Progress reporting every 10000 trees
		if (m_TreeCount % 10000 == 0)
			PrintFormat("    ... %1 trees found so far", m_TreeCount);

		return true;
	}

	//------------------------------------------------------------
	// Layer 7: Building Export (runtime entity query)
	//------------------------------------------------------------

	//! Export all building entities as a compact binary file.
	//! Same collect-then-write pattern as tree export.
	//!
	//! Format: N * 20-byte records (float32 minX, minZ, maxX, maxZ, topY)
	//! Reader derives count from file_size / 20.
	private void ExportBuildings()
	{
		WorldEditorAPI api = GetApi();
		BaseWorld world = api.GetWorld();
		if (!world)
		{
			Print("ERROR: No world available for building export");
			return;
		}

		vector worldMins, worldMaxs;
		world.GetBoundBox(worldMins, worldMaxs);

		// Prepare collection array
		m_BuildingCount = 0;
		m_BuildingData = new array<float>();

		Print("  Scanning world for building entities...");

		// Query — callback only appends to m_BuildingData, no file I/O
		world.QueryEntitiesByAABB(worldMins, worldMaxs, OnBuildingEntityFound, null, EQueryEntitiesFlags.STATIC);

		PrintFormat("  Query complete. Collected %1 buildings (%2 floats).", m_BuildingCount, m_BuildingData.Count());

		// Now write everything to file at once
		string path = m_OutputDir + "/buildings.bin";
		FileHandle fh = SafeOpenFile(path);
		if (!fh)
			return;

		// Write all records — no footer; reader derives count from file_size / 20
		if (m_BuildingData.Count() > 0)
			fh.WriteArray(m_BuildingData, 4, m_BuildingData.Count());
		fh.Close();

		PrintFormat("  Buildings: %1 entities -> buildings.bin", m_BuildingCount);

		// Free collection memory
		m_BuildingData = null;
	}

	//! Callback for building entity query — collects data, no file I/O.
	private bool OnBuildingEntityFound(IEntity ent)
	{
		if (IsCancelled())
			return false;

		// Match Building class specifically (not BaseBuilding — that includes
		// SCR_BuildingRegionEntity, SCR_FiringRangeTarget, SCR_FragmentEntity)
		if (!ent.IsInherited(Building))
			return true;

		vector mins, maxs;
		ent.GetWorldBounds(mins, maxs);

		float height = maxs[1] - mins[1];
		if (height < 1.0)
			return true;  // skip very small structures

		// Collect into flat array — no file I/O here
		m_BuildingData.Insert(mins[0]);
		m_BuildingData.Insert(mins[2]);
		m_BuildingData.Insert(maxs[0]);
		m_BuildingData.Insert(maxs[2]);
		m_BuildingData.Insert(maxs[1]);
		m_BuildingCount++;

		if (m_BuildingCount % 1000 == 0)
			PrintFormat("    ... %1 buildings found so far", m_BuildingCount);

		return true;
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

		// Layer 6: Trees (runtime entity query)
		if (!IsCancelled())
			ExportTrees();

		// Layer 7: Buildings (runtime entity query)
		if (!IsCancelled())
			ExportBuildings();

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

	[ButtonAttribute("Export Trees")]
	void BtnExportTrees()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Trees Export: %1 ===", m_MapName);
		ExportTrees();
		FinishExport();
	}

	[ButtonAttribute("Export Buildings")]
	void BtnExportBuildings()
	{
		if (!InitExport())
			return;

		PrintFormat("=== Buildings Export: %1 ===", m_MapName);
		ExportBuildings();
		FinishExport();
	}

	[ButtonAttribute("Cancel Export")]
	void BtnCancelExport()
	{
		RequestCancel();
	}
}
